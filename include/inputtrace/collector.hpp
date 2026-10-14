#pragma once
#include "inputtrace/storage.hpp"
#include <atomic>
#include <array>
#include <unordered_map>
#include <vector>
#include <thread>
#include <windows.h>

namespace inputtrace {
class Collector {
public:
    explicit Collector(Storage& storage,int flush_ms=2000);
    ~Collector();
    bool start();
    void stop();
    void run_self_test();
    void run_benchmark(std::uint64_t packets);
    std::uint64_t keyboard_events() const { return keyboard_events_; }
    std::uint64_t mouse_events() const { return mouse_events_; }
    std::uint64_t raw_errors() const { return raw_errors_; }
    std::uint64_t aggregate_drops() const { return aggregate_drops_; }
    std::uint64_t context_drops() const { return context_drops_; }
    std::string error() const;
private:
    struct Press { bool down=false; std::int64_t at=0, context=0; std::uint32_t modifier=0; };
    static LRESULT CALLBACK window_proc(HWND, UINT, WPARAM, LPARAM);
    static void CALLBACK foreground_proc(HWINEVENTHOOK,DWORD,HWND,LONG,LONG,DWORD,DWORD);
    void run();
    void raw(HRAWINPUT);
    void keyboard(const RAWKEYBOARD&, std::int64_t);
    void mouse(const RAWMOUSE&, std::int64_t);
    void foreground(HWND);
    void flush(bool final=false);
    void release_all(std::int64_t);
    void add(int kind, std::int64_t code, std::int64_t context, std::int64_t us,
             int x=0,int y=0,int monitor=0,std::int64_t duration=0,
             std::int64_t raw_distance=0,std::int64_t pixel_distance=0,std::int64_t wheel=0);
    std::int64_t now_us() const;
    Storage& storage_;
    int flush_ms_;
    std::thread thread_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    bool ready_=false, started_=false;
    std::string error_;
    std::atomic<HWND> window_{nullptr};
    HWINEVENTHOOK foreground_hook_=nullptr;
    std::vector<Press> keys_{65536};
    std::array<Press,5> buttons_{};
    std::uint32_t modifiers_=0, button_mask_=0;
    std::int64_t context_=0, next_context_=1;
    std::uint32_t pid_=0;
    HWND foreground_window_=nullptr;
    Batch batch_;
    LARGE_INTEGER frequency_{}, anchor_qpc_{};
    std::int64_t anchor_us_=0, last_move_us_=0;
    POINT last_point_{};
    bool have_point_=false;
    std::int64_t pending_raw_distance_=0;
    std::unordered_map<HMONITOR,int> monitors_;
    int next_monitor_=1;
    std::atomic<std::uint64_t> keyboard_events_{0}, mouse_events_{0}, raw_errors_{0}, aggregate_drops_{0}, context_drops_{0};
};
}
