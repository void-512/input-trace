#include "inputtrace/collector.hpp"
#include <QCoreApplication>
#include <sqlite3.h>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <windows.h>
#include <psapi.h>

namespace {
HANDLE stop_event=nullptr;
BOOL WINAPI console_handler(DWORD kind) {
    if(kind==CTRL_C_EVENT || kind==CTRL_BREAK_EVENT || kind==CTRL_CLOSE_EVENT) {
        if(stop_event) SetEvent(stop_event);
        return TRUE;
    }
    return FALSE;
}
std::int64_t utc_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}
std::uint64_t process_cpu_100ns() {
    FILETIME created{},exited{},kernel{},user{};
    if(!GetProcessTimes(GetCurrentProcess(),&created,&exited,&kernel,&user)) return 0;
    ULARGE_INTEGER k{},u{};
    k.LowPart=kernel.dwLowDateTime;k.HighPart=kernel.dwHighDateTime;
    u.LowPart=user.dwLowDateTime;u.HighPart=user.dwHighDateTime;
    return k.QuadPart+u.QuadPart;
}
void report_resources(const std::string& db,std::chrono::steady_clock::time_point started,
                      std::uint64_t initial_cpu) {
    double elapsed=std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count();
    double cpu_ms=double(process_cpu_100ns()-initial_cpu)/10000.0;
    SYSTEM_INFO system{};GetSystemInfo(&system);
    PROCESS_MEMORY_COUNTERS_EX memory{};memory.cb=sizeof(memory);
    GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),sizeof(memory));
    std::uintmax_t bytes=0;
    std::error_code error;
    if(std::filesystem::exists(db,error)) bytes=std::filesystem::file_size(db,error);
    std::cout<<"elapsed_s="<<elapsed<<" cpu_ms="<<cpu_ms
             <<" one_core_pct="<<(elapsed>0?cpu_ms/elapsed/10:0)
             <<" system_pct="<<(elapsed>0?cpu_ms/elapsed/10/system.dwNumberOfProcessors:0)
             <<" private_kib="<<memory.PrivateUsage/1024<<" db_bytes="<<bytes<<'\n';
}
}
int main(int argc,char** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    QCoreApplication app(argc,argv);
    QCoreApplication::setApplicationName("InputTrace");
    QCoreApplication::setApplicationVersion("0.1");
    std::string db="inputtrace.db";
    int duration=0,stats_every=5,retention_days=90,flush_ms=2000;
    std::uint64_t benchmark=0,storage_benchmark=0;
    bool query=false,self_test=false,benchmark_requested=false,storage_benchmark_requested=false;
    auto args=QCoreApplication::arguments();
    try {
    for(int i=1;i<args.size();++i) {
        std::string a=args[i].toStdString();
        if(a=="--db" && i+1<args.size()) db=args[++i].toUtf8().toStdString();
        else if(a=="--duration" && i+1<args.size()) duration=std::stoi(args[++i].toStdString());
        else if(a=="--stats-every" && i+1<args.size()) stats_every=std::stoi(args[++i].toStdString());
        else if(a=="--retain-days" && i+1<args.size()) retention_days=std::stoi(args[++i].toStdString());
        else if(a=="--flush-ms" && i+1<args.size()) flush_ms=std::stoi(args[++i].toStdString());
        else if(a=="--query") query=true;
        else if(a=="--version") {
            std::cout<<"InputTrace 0.1 Qt "<<QT_VERSION_STR<<" SQLite "<<sqlite3_libversion()<<'\n';
            return 0;
        }
        else if(a=="--self-test") self_test=true;
        else if(a=="--benchmark" && i+1<args.size()) {
            benchmark=std::stoull(args[++i].toStdString());benchmark_requested=true;
        }
        else if(a=="--storage-benchmark" && i+1<args.size()) {
            storage_benchmark=std::stoull(args[++i].toStdString());storage_benchmark_requested=true;
        }
        else if(a=="--help") {
            std::cout<<"InputTrace --db FILE [--duration SECONDS] [--stats-every SECONDS] [--retain-days DAYS] [--flush-ms MS] [--query|--self-test|--benchmark PACKETS|--storage-benchmark ROWS]\n"
                       "Ctrl+C stops capture and flushes the database.\n";
            return 0;
        } else {std::cerr<<"Unknown or incomplete option: "<<a<<'\n';return 2;}
    }
    } catch(const std::exception& e) {std::cerr<<"Invalid option value: "<<e.what()<<'\n';return 2;}
    if(query) return inputtrace::Storage::query(db);
    if(duration<0 || stats_every<0 || retention_days<0 || flush_ms<100 || flush_ms>10000 ||
       (benchmark_requested && !benchmark) || (storage_benchmark_requested && !storage_benchmark)) {
        std::cerr<<"Invalid interval (flush-ms must be 100..10000)\n";return 2;
    }
    inputtrace::Storage storage(db,retention_days);
    if(!storage.start(utc_us())) {std::cerr<<"Storage: "<<storage.error()<<'\n';return 1;}
    inputtrace::Collector collector(storage,flush_ms);
    auto measurement_start=std::chrono::steady_clock::now();
    auto initial_cpu=process_cpu_100ns();
    if(self_test) {
        collector.run_self_test();
        std::this_thread::sleep_for(std::chrono::milliseconds(110));
        auto session_id=storage.session();
        storage.stop(utc_us());
        report_resources(db,measurement_start,initial_cpu);
        if(!storage.error().empty()) {std::cerr<<storage.error()<<'\n';return 1;}
        bool okay=inputtrace::Storage::verify_self_test(db,session_id);
        std::cout<<"self_test="<<(okay?"PASS":"FAIL")<<'\n';
        return okay?0:1;
    }
    if(benchmark) {
        collector.run_benchmark(benchmark);
        storage.stop(utc_us());
        report_resources(db,measurement_start,initial_cpu);
        std::cout<<"synthetic_packets="<<benchmark<<" key_transitions="<<collector.keyboard_events()
                 <<" mouse_packets="<<collector.mouse_events()<<" transactions="<<storage.transactions()
                 <<" rows="<<storage.rows()<<" backpressure="<<storage.backpressure()
                 <<" drops="<<storage.dropped_batches()+collector.aggregate_drops()+collector.context_drops()<<'\n';
        return storage.error().empty()?0:1;
    }
    if(storage_benchmark) {
        auto minute=utc_us()/60000000*60000000;
        inputtrace::Batch batch;
        for(std::uint64_t i=0;i<storage_benchmark;++i) {
            inputtrace::StatKey key{minute,0,0,inputtrace::movement,int(i%1000),int(i/1000),0};
            batch.stats.emplace(key,inputtrace::StatValue{1,0,1,1,0});
            if(batch.stats.size()==5000 || i+1==storage_benchmark) {
                while(!storage.submit(std::move(batch))) {
                    if(!storage.error().empty()) break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                batch=inputtrace::Batch{};
            }
        }
        storage.stop(utc_us());
        report_resources(db,measurement_start,initial_cpu);
        std::cout<<"storage_rows="<<storage.rows()<<" transactions="<<storage.transactions()
                 <<" backpressure="<<storage.backpressure()<<'\n';
        return storage.error().empty()?0:1;
    }
    if(!collector.start()) {
        std::cerr<<"Capture: "<<collector.error()<<'\n';
        storage.stop(utc_us());return 1;
    }
    stop_event=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    SetConsoleCtrlHandler(console_handler,TRUE);
    std::cout<<"InputTrace collecting; session "<<storage.session()<<", database "<<db<<'\n';
    auto started=std::chrono::steady_clock::now();
    auto next_stats=started+std::chrono::seconds(stats_every?stats_every:86400);
    for(;;) {
        auto now=std::chrono::steady_clock::now();
        if(duration && now-started>=std::chrono::seconds(duration)) break;
        auto deadline=next_stats;
        if(duration) deadline=std::min(deadline,started+std::chrono::seconds(duration));
        auto wait=std::chrono::duration_cast<std::chrono::milliseconds>(deadline-now).count();
        if(wait<0) wait=0;
        HANDLE events[2]={stop_event,storage.failure_event()};
        DWORD result=WaitForMultipleObjects(2,events,FALSE,DWORD(std::min<std::int64_t>(wait,3600000)));
        if(result==WAIT_OBJECT_0 || result==WAIT_OBJECT_0+1) break;
        now=std::chrono::steady_clock::now();
        if(stats_every && now>=next_stats) {
            std::cout<<"keyboard="<<collector.keyboard_events()<<" mouse="<<collector.mouse_events()
                     <<" transactions="<<storage.transactions()<<" rows="<<storage.rows()
                     <<" queue="<<storage.depth()<<" raw_errors="<<collector.raw_errors()
                     <<" backpressure="<<storage.backpressure()
                     <<" dropped_batches="<<storage.dropped_batches()
                     <<" aggregate_drops="<<collector.aggregate_drops()
                     <<" context_drops="<<collector.context_drops()<<'\n';
            next_stats=now+std::chrono::seconds(stats_every);
        }
    }
    collector.stop();
    storage.stop(utc_us());
    SetConsoleCtrlHandler(console_handler,FALSE);
    CloseHandle(stop_event);stop_event=nullptr;
    std::cout<<"Stopped. keyboard="<<collector.keyboard_events()<<" mouse="<<collector.mouse_events()
             <<" transactions="<<storage.transactions()<<" rows="<<storage.rows()
             <<" raw_errors="<<collector.raw_errors()<<" dropped_batches="<<storage.dropped_batches()
             <<" backpressure="<<storage.backpressure()
             <<" aggregate_drops="<<collector.aggregate_drops()
             <<" context_drops="<<collector.context_drops()<<'\n';
    report_resources(db,measurement_start,initial_cpu);
    if(!storage.error().empty()) {std::cerr<<"Storage: "<<storage.error()<<'\n';return 1;}
    return 0;
}
