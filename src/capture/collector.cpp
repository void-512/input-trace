#include "inputtrace/collector.hpp"
#include <wtsapi32.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <climits>
#include <vector>

namespace inputtrace {
namespace {
std::atomic<Collector*> active{nullptr};
std::string utf8(const wchar_t* s,int length=-1) {
    int n=WideCharToMultiByte(CP_UTF8,0,s,length,nullptr,0,nullptr,nullptr);
    if(n<=0) return {};
    std::string out(std::size_t(n),0);
    WideCharToMultiByte(CP_UTF8,0,s,length,out.data(),n,nullptr,nullptr);
    if(length==-1 && !out.empty()) out.pop_back();
    return out;
}
std::int64_t wall_us() {
    FILETIME f; GetSystemTimePreciseAsFileTime(&f);
    ULARGE_INTEGER v; v.LowPart=f.dwLowDateTime;v.HighPart=f.dwHighDateTime;
    return std::int64_t(v.QuadPart/10-11644473600000000ull);
}
std::uint32_t modifier_bit(const RAWKEYBOARD& k) {
    switch(k.VKey) {
    case VK_SHIFT: return k.MakeCode==0x36?8:4;
    case VK_LSHIFT: return 4; case VK_RSHIFT: return 8;
    case VK_CONTROL: return k.Flags&RI_KEY_E0?2:1;
    case VK_LCONTROL: return 1; case VK_RCONTROL: return 2;
    case VK_MENU: return k.Flags&RI_KEY_E0?32:16;
    case VK_LMENU: return 16; case VK_RMENU: return 32;
    case VK_LWIN: return 64; case VK_RWIN: return 128;
    default: return 0;
    }
}
int floor_cell(int x) { return x>=0?x/32:-((31-x)/32); }
}

Collector::Collector(Storage& storage,int flush_ms):storage_(storage),flush_ms_(flush_ms) {
    QueryPerformanceFrequency(&frequency_);
    QueryPerformanceCounter(&anchor_qpc_);
    anchor_us_=wall_us();
}
Collector::~Collector(){stop();}
void Collector::run_self_test() {
    HWND test_window=CreateWindowExW(0,L"STATIC",L"InputTrace self-test",WS_OVERLAPPED,
                                      0,0,0,0,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    foreground(test_window);
    const auto t=now_us();
    RAWKEYBOARD control{};control.MakeCode=0x1d;control.VKey=VK_CONTROL;
    RAWKEYBOARD c{};c.MakeCode=0x2e;c.VKey='C';
    keyboard(control,t);keyboard(c,t+1000);keyboard(c,t+2000); // repeat is not another press
    c.Flags=RI_KEY_BREAK;keyboard(c,t+21000);
    control.Flags=RI_KEY_BREAK;keyboard(control,t+30000);
    RAWMOUSE m{};m.usButtonFlags=RI_MOUSE_LEFT_BUTTON_DOWN;mouse(m,t+40000);
    m.usButtonFlags=RI_MOUSE_LEFT_BUTTON_UP;mouse(m,t+70000);
    m.usButtonFlags=RI_MOUSE_WHEEL;m.usButtonData=120;mouse(m,t+80000);
    m.usButtonFlags=0;m.lLastX=10;m.lLastY=5;mouse(m,t+100000);
    flush(true);
    if(test_window) DestroyWindow(test_window);
}
void Collector::run_benchmark(std::uint64_t packets) {
    const auto t=now_us();
    RAWKEYBOARD key{};key.MakeCode=0x1e;key.VKey='A';
    RAWMOUSE move{};move.lLastX=1;move.lLastY=1;
    for(std::uint64_t i=0;i<packets;++i) {
        auto at=t+std::int64_t(i*125);
        key.Flags=0;keyboard(key,at);
        key.Flags=RI_KEY_BREAK;keyboard(key,at+60);
        mouse(move,at+80);
        if((i+1)%(std::uint64_t(flush_ms_)*8)==0) flush();
    }
    flush(true);
}
bool Collector::start() {
    thread_=std::thread(&Collector::run,this);
    std::unique_lock lock(mutex_);
    cv_.wait(lock,[this]{return ready_;});
    return started_;
}
void Collector::stop() {
    HWND w=window_.load();
    if(w) PostMessageW(w,WM_CLOSE,0,0);
    if(thread_.joinable()) thread_.join();
}
std::string Collector::error() const {std::lock_guard lock(mutex_);return error_;}
std::int64_t Collector::now_us() const {
    LARGE_INTEGER t;QueryPerformanceCounter(&t);
    std::int64_t d=t.QuadPart-anchor_qpc_.QuadPart;
    return anchor_us_+(d/frequency_.QuadPart)*1000000+
        (d%frequency_.QuadPart)*1000000/frequency_.QuadPart;
}
void Collector::add(int kind,std::int64_t code,std::int64_t context,std::int64_t us,
                    int x,int y,int monitor,std::int64_t duration,
                    std::int64_t raw_distance,std::int64_t pixel_distance,std::int64_t wheel) {
    StatKey k{us/60000000*60000000,context,code,kind,x,y,monitor};
    auto it=batch_.stats.find(k);
    if(it==batch_.stats.end()) {
        if(batch_.stats.size()>=100000) {++aggregate_drops_;return;}
        it=batch_.stats.try_emplace(k).first;
    }
    auto& v=it->second;++v.count;v.duration_us+=duration;
    v.raw_distance+=raw_distance;v.pixel_distance+=pixel_distance;v.wheel_delta+=wheel;
}
void Collector::release_all(std::int64_t us) {
    for(std::size_t i=0;i<keys_.size();++i) if(keys_[i].down) {
        add(key_release,i,keys_[i].context,us,0,0,0,std::max<std::int64_t>(0,us-keys_[i].at));
        keys_[i]={};
    }
    POINT p{};bool has_position=GetPhysicalCursorPos(&p)!=0;
    for(std::size_t i=0;i<buttons_.size();++i) if(buttons_[i].down) {
        add(button_release,i+1,buttons_[i].context,us,
            has_position?floor_cell(p.x):INT_MIN,has_position?floor_cell(p.y):INT_MIN,0,
            std::max<std::int64_t>(0,us-buttons_[i].at));
        buttons_[i]={};
    }
    modifiers_=button_mask_=0;
}
void Collector::foreground(HWND hwnd) {
    if(hwnd==foreground_window_) return;
    release_all(now_us());
    foreground_window_=hwnd;context_=0;pid_=0;
    if(!hwnd) return;
    DWORD pid=0;GetWindowThreadProcessId(hwnd,&pid);
    if(!pid) return;
    pid_=pid;
    if(batch_.contexts.size()>=8192) flush();
    if(batch_.contexts.size()>=8192) {++context_drops_;return;}
    Context c; c.id=next_context_++;c.pid=pid;c.hwnd=reinterpret_cast<std::uintptr_t>(hwnd);c.confidence=1;
    HANDLE process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);
    if(process) {
        wchar_t path[4096]{};DWORD length=4096;
        if(QueryFullProcessImageNameW(process,0,path,&length)) {
            c.path=utf8(path,int(length));
            std::size_t slash=c.path.find_last_of("\\/");
            c.name=slash==std::string::npos?c.path:c.path.substr(slash+1);
            c.confidence=2;
        }
        CloseHandle(process);
    }
    context_=c.id;batch_.contexts.push_back(std::move(c));
}
void Collector::keyboard(const RAWKEYBOARD& k,std::int64_t us) {
    if(k.MakeCode==KEYBOARD_OVERRUN_MAKE_CODE || k.VKey==255) {++raw_errors_;return;}
    unsigned id=(k.MakeCode?unsigned(k.MakeCode):unsigned(k.VKey)|0x8000u)
              |((k.Flags&RI_KEY_E0)?0x100u:0u)|((k.Flags&RI_KEY_E1)?0x200u:0u);
    if(id>=keys_.size()) {++raw_errors_;return;}
    ++keyboard_events_;
    auto& state=keys_[id];
    bool up=(k.Flags&RI_KEY_BREAK)!=0;
    if(up) {
        if(!state.down) return;
        add(key_release,id,state.context,us,0,0,0,std::max<std::int64_t>(0,us-state.at));
        modifiers_ &= ~state.modifier;
        state={};
    } else {
        if(state.down) return; // Typematic repeat.
        std::uint32_t bit=modifier_bit(k);
        state={true,us,context_,bit};
        add(key_press,id,context_,us);
        if(bit) modifiers_|=bit;
        else if(modifiers_) add(combo,(std::int64_t(modifiers_)<<32)|id,context_,us);
    }
}
void Collector::mouse(const RAWMOUSE& m,std::int64_t us) {
    ++mouse_events_;
    const std::int64_t dx=m.lLastX,dy=m.lLastY;
    pending_raw_distance_+=std::llabs(dx)+std::llabs(dy);
    bool sample=(dx || dy || (m.usFlags&MOUSE_MOVE_ABSOLUTE)) &&
                (!last_move_us_ || us-last_move_us_>=16000);
    if(!sample && !m.usButtonFlags) return;
    POINT p{};bool has_position=GetPhysicalCursorPos(&p)!=0;
    int monitor=0;
    if(has_position) {
        HMONITOR h=MonitorFromPoint(p,MONITOR_DEFAULTTONEAREST);
        auto it=monitors_.find(h);
        if(it==monitors_.end() && monitors_.size()<256 && next_monitor_<4096) {
            int id=next_monitor_++;
            it=monitors_.try_emplace(h,id).first;
            MONITORINFOEXW info{};info.cbSize=sizeof(info);
            if(GetMonitorInfoW(h,&info)) {
                batch_.monitors.push_back({id,info.rcMonitor.left,info.rcMonitor.top,
                    info.rcMonitor.right,info.rcMonitor.bottom,utf8(info.szDevice)});
            }
        }
        if(it!=monitors_.end()) monitor=it->second;
    }
    if(sample && has_position) {
        std::int64_t pixels=have_point_?std::llabs(std::int64_t(p.x)-last_point_.x)+
                                  std::llabs(std::int64_t(p.y)-last_point_.y):0;
        add(movement,0,context_,us,floor_cell(p.x),floor_cell(p.y),monitor,0,
            pending_raw_distance_,pixels);
        pending_raw_distance_=0;last_move_us_=us;last_point_=p;have_point_=true;
    }
    const int cx=has_position?floor_cell(p.x):INT_MIN;
    const int cy=has_position?floor_cell(p.y):INT_MIN;
    const USHORT down[]={RI_MOUSE_LEFT_BUTTON_DOWN,RI_MOUSE_RIGHT_BUTTON_DOWN,
        RI_MOUSE_MIDDLE_BUTTON_DOWN,RI_MOUSE_BUTTON_4_DOWN,RI_MOUSE_BUTTON_5_DOWN};
    const USHORT up[]={RI_MOUSE_LEFT_BUTTON_UP,RI_MOUSE_RIGHT_BUTTON_UP,
        RI_MOUSE_MIDDLE_BUTTON_UP,RI_MOUSE_BUTTON_4_UP,RI_MOUSE_BUTTON_5_UP};
    for(int i=0;i<5;++i) {
        if(m.usButtonFlags&down[i] && !buttons_[i].down) {
            buttons_[i]={true,us,context_,0};button_mask_|=1u<<i;
            add(button_press,i+1,context_,us,cx,cy,monitor);
            if((button_mask_&(button_mask_-1))!=0) add(mouse_combo,button_mask_,context_,us,cx,cy,monitor);
        }
        if(m.usButtonFlags&up[i] && buttons_[i].down) {
            auto& b=buttons_[i];
            add(button_release,i+1,b.context,us,cx,cy,monitor,std::max<std::int64_t>(0,us-b.at));
            b={};button_mask_ &= ~(1u<<i);
        }
    }
    if(m.usButtonFlags&RI_MOUSE_WHEEL)
        add(vertical_wheel,0,context_,us,cx,cy,monitor,0,0,0,SHORT(m.usButtonData));
    if(m.usButtonFlags&RI_MOUSE_HWHEEL)
        add(horizontal_wheel,0,context_,us,cx,cy,monitor,0,0,0,SHORT(m.usButtonData));
}
void Collector::raw(HRAWINPUT handle) {
    alignas(RAWINPUT) std::array<std::byte,256> local{};
    UINT bytes=UINT(local.size());
    UINT result=GetRawInputData(handle,RID_INPUT,local.data(),&bytes,sizeof(RAWINPUTHEADER));
    if(result==UINT(-1) && GetLastError()==ERROR_INSUFFICIENT_BUFFER) {
        std::vector<std::byte> large(bytes);
        result=GetRawInputData(handle,RID_INPUT,large.data(),&bytes,sizeof(RAWINPUTHEADER));
        if(result==UINT(-1)) {++raw_errors_;return;}
        const auto* r=reinterpret_cast<const RAWINPUT*>(large.data());
        std::int64_t us=now_us();
        if(r->header.dwType==RIM_TYPEKEYBOARD) keyboard(r->data.keyboard,us);
        else if(r->header.dwType==RIM_TYPEMOUSE) mouse(r->data.mouse,us);
        return;
    }
    if(result==UINT(-1)) {++raw_errors_;return;}
    const auto* r=reinterpret_cast<const RAWINPUT*>(local.data());
    std::int64_t us=now_us();
    if(r->header.dwType==RIM_TYPEKEYBOARD) keyboard(r->data.keyboard,us);
    else if(r->header.dwType==RIM_TYPEMOUSE) mouse(r->data.mouse,us);
}
void Collector::flush(bool final) {
    std::int64_t us=now_us();
    if(final) release_all(us);
    if(final && pending_raw_distance_) {
        add(movement,0,context_,us,have_point_?floor_cell(last_point_.x):INT_MIN,
            have_point_?floor_cell(last_point_.y):INT_MIN,0,0,
            pending_raw_distance_);
        pending_raw_distance_=0;
    }
    if(storage_.submit(std::move(batch_),final)) batch_=Batch{};
    else if(batch_.stats.size()>=100000) {aggregate_drops_+=batch_.stats.size();batch_.stats.clear();}
    if(!final) {QueryPerformanceCounter(&anchor_qpc_);anchor_us_=wall_us();}
}
void CALLBACK Collector::foreground_proc(HWINEVENTHOOK,DWORD,HWND hwnd,LONG,LONG,DWORD,DWORD) {
    if(auto* c=active.load()) c->foreground(hwnd);
}
LRESULT CALLBACK Collector::window_proc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp) {
    auto* c=reinterpret_cast<Collector*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));
    if(msg==WM_NCCREATE) {
        c=reinterpret_cast<Collector*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(c));
    }
    if(c) {
        switch(msg) {
        case WM_INPUT: c->raw(reinterpret_cast<HRAWINPUT>(lp));break;
        case WM_INPUT_DEVICE_CHANGE: c->release_all(c->now_us());break;
        case WM_DISPLAYCHANGE: c->monitors_.clear();break;
        case WM_POWERBROADCAST:
            if(wp==PBT_APMSUSPEND) c->release_all(c->now_us());
            else if(wp==PBT_APMRESUMEAUTOMATIC || wp==PBT_APMRESUMESUSPEND) {
                QueryPerformanceCounter(&c->anchor_qpc_);c->anchor_us_=wall_us();
                c->foreground(GetForegroundWindow());
            }
            break;
        case WM_WTSSESSION_CHANGE:
            if(wp==WTS_SESSION_LOCK) {c->release_all(c->now_us());c->foreground(nullptr);}
            else if(wp==WTS_SESSION_UNLOCK) c->foreground(GetForegroundWindow());
            break;
        case WM_TIMER:
            if(!c->foreground_hook_) c->foreground(GetForegroundWindow());
            c->flush();return 0;
        case WM_CLOSE: KillTimer(hwnd,1);WTSUnRegisterSessionNotification(hwnd);DestroyWindow(hwnd);return 0;
        case WM_DESTROY: PostQuitMessage(0);return 0;
        }
    }
    return DefWindowProcW(hwnd,msg,wp,lp);
}
void Collector::run() {
    HINSTANCE instance=GetModuleHandleW(nullptr);
    WNDCLASSW cls{};cls.lpfnWndProc=&Collector::window_proc;cls.hInstance=instance;
    cls.lpszClassName=L"InputTraceRawCapture";
    if(!RegisterClassW(&cls)) {
        std::lock_guard lock(mutex_);error_="RegisterClass failed";ready_=true;cv_.notify_all();return;
    }
    HWND w=CreateWindowExW(0,cls.lpszClassName,L"InputTrace capture",WS_OVERLAPPED,
                           0,0,0,0,nullptr,nullptr,instance,this);
    if(!w) {
        std::lock_guard lock(mutex_);error_="CreateWindow failed";ready_=true;cv_.notify_all();
        UnregisterClassW(cls.lpszClassName,instance);return;
    }
    window_=w;
    RAWINPUTDEVICE devices[2]={{1,6,RIDEV_INPUTSINK|RIDEV_DEVNOTIFY,w},
                               {1,2,RIDEV_INPUTSINK|RIDEV_DEVNOTIFY,w}};
    if(!RegisterRawInputDevices(devices,2,sizeof(RAWINPUTDEVICE))) {
        std::lock_guard lock(mutex_);error_="RegisterRawInputDevices failed";ready_=true;cv_.notify_all();
        DestroyWindow(w);window_=nullptr;UnregisterClassW(cls.lpszClassName,instance);return;
    }
    WTSRegisterSessionNotification(w,NOTIFY_FOR_THIS_SESSION);
    active=this;
    foreground_hook_=SetWinEventHook(EVENT_SYSTEM_FOREGROUND,EVENT_SYSTEM_FOREGROUND,
                    nullptr,&Collector::foreground_proc,0,0,WINEVENT_OUTOFCONTEXT);
    foreground(GetForegroundWindow());
    SetTimer(w,1,UINT(flush_ms_),nullptr);
    {
        std::lock_guard lock(mutex_);started_=true;ready_=true;
    }
    cv_.notify_all();
    MSG message;
    while(GetMessageW(&message,nullptr,0,0)>0) {TranslateMessage(&message);DispatchMessageW(&message);}
    flush(true);
    if(foreground_hook_) UnhookWinEvent(foreground_hook_);
    active=nullptr;
    RAWINPUTDEVICE remove[2]={{1,6,RIDEV_REMOVE,nullptr},{1,2,RIDEV_REMOVE,nullptr}};
    RegisterRawInputDevices(remove,2,sizeof(RAWINPUTDEVICE));
    window_=nullptr;
    UnregisterClassW(cls.lpszClassName,instance);
}
}
