#pragma once
#include "inputtrace/model.hpp"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <windows.h>

namespace inputtrace {
class Storage {
public:
    explicit Storage(std::string path, int retention_days=90);
    ~Storage();
    bool start(std::int64_t start_us);
    bool submit(Batch&& batch,bool wait_for_space=false);
    void stop(std::int64_t stop_us);
    std::int64_t session() const { return session_id_; }
    std::uint64_t transactions() const { return transactions_; }
    std::uint64_t rows() const { return rows_; }
    std::uint64_t dropped_batches() const { return dropped_; }
    std::uint64_t backpressure() const { return backpressure_; }
    std::size_t depth() const;
    std::string error() const;
    HANDLE failure_event() const { return failure_event_; }
    static int query(const std::string& path);
    static bool verify_self_test(const std::string& path,std::int64_t session_id);
private:
    void run();
    std::string path_;
    int retention_days_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Batch> queue_;
    std::thread thread_;
    HANDLE failure_event_=nullptr;
    bool closing_=false, ready_=false;
    std::string error_;
    std::int64_t start_us_=0, stop_us_=0, session_id_=0;
    std::atomic<std::uint64_t> transactions_{0}, rows_{0}, dropped_{0}, backpressure_{0};
};
}
