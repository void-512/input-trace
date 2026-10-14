#include "inputtrace/storage.hpp"
#include <sqlite3.h>
#include <iostream>
#include <stdexcept>
#include <chrono>

namespace inputtrace {
namespace {
void check(int rc, sqlite3* db) {
    if (rc!=SQLITE_OK && rc!=SQLITE_DONE && rc!=SQLITE_ROW)
        throw std::runtime_error(sqlite3_errmsg(db));
}
void sql(sqlite3* db, const char* statement) {
    char* message=nullptr;
    if (sqlite3_exec(db,statement,nullptr,nullptr,&message)!=SQLITE_OK) {
        std::string error=message?message:"SQLite error";
        sqlite3_free(message);
        throw std::runtime_error(error);
    }
}
struct Statement {
    sqlite3* db;
    sqlite3_stmt* p=nullptr;
    Statement(sqlite3* d,const char* text):db(d) { check(sqlite3_prepare_v2(db,text,-1,&p,nullptr),db); }
    ~Statement(){sqlite3_finalize(p);}
    void step(){check(sqlite3_step(p),db);sqlite3_reset(p);sqlite3_clear_bindings(p);}
};
}

Storage::Storage(std::string path,int retention_days):path_(std::move(path)),retention_days_(retention_days) {
    failure_event_=CreateEventW(nullptr,TRUE,FALSE,nullptr);
}
Storage::~Storage(){
    if(thread_.joinable()) stop(stop_us_);
    if(failure_event_) CloseHandle(failure_event_);
}
bool Storage::start(std::int64_t start_us) {
    if(!failure_event_) {error_="CreateEvent failed";return false;}
    start_us_=start_us;
    thread_=std::thread(&Storage::run,this);
    std::unique_lock lock(mutex_);
    cv_.wait(lock,[this]{return ready_;});
    return error_.empty();
}
bool Storage::submit(Batch&& batch,bool wait_for_space) {
    if(batch.stats.empty() && batch.contexts.empty() && batch.monitors.empty()) return true;
    std::unique_lock lock(mutex_);
    if(wait_for_space && queue_.size()>=8) {
        ++backpressure_;
        cv_.wait(lock,[this]{return queue_.size()<8 || closing_ || !error_.empty();});
    }
    if(closing_ || !error_.empty()) { ++dropped_; return false; }
    if(queue_.size()>=8) { ++backpressure_; return false; }
    queue_.push_back(std::move(batch));
    cv_.notify_one();
    return true;
}
void Storage::stop(std::int64_t stop_us) {
    { std::lock_guard lock(mutex_); closing_=true; stop_us_=stop_us; }
    cv_.notify_one();
    if(thread_.joinable()) thread_.join();
}
std::size_t Storage::depth() const { std::lock_guard lock(mutex_); return queue_.size(); }
std::string Storage::error() const { std::lock_guard lock(mutex_); return error_; }

void Storage::run() {
    sqlite3* db=nullptr;
    try {
        if(sqlite3_open_v2(path_.c_str(),&db,SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE,nullptr)!=SQLITE_OK)
            throw std::runtime_error(db?sqlite3_errmsg(db):"Cannot open SQLite database");
        sqlite3_busy_timeout(db,5000);
        sql(db,"PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; PRAGMA foreign_keys=ON;");
        sql(db,R"SQL(
            CREATE TABLE IF NOT EXISTS schema_info(version INTEGER NOT NULL);
            INSERT INTO schema_info SELECT 1 WHERE NOT EXISTS(SELECT 1 FROM schema_info);
            CREATE TABLE IF NOT EXISTS sessions(
                id INTEGER PRIMARY KEY, started_us INTEGER NOT NULL, stopped_us INTEGER);
            CREATE TABLE IF NOT EXISTS app_contexts(
                session_id INTEGER NOT NULL, id INTEGER NOT NULL, pid INTEGER NOT NULL,
                hwnd INTEGER NOT NULL, exe_path TEXT NOT NULL, exe_name TEXT NOT NULL,
                confidence INTEGER NOT NULL, PRIMARY KEY(session_id,id));
            CREATE TABLE IF NOT EXISTS monitors(
                session_id INTEGER NOT NULL, id INTEGER NOT NULL, device TEXT NOT NULL,
                left_px INTEGER NOT NULL, top_px INTEGER NOT NULL,
                right_px INTEGER NOT NULL, bottom_px INTEGER NOT NULL,
                PRIMARY KEY(session_id,id));
            CREATE TABLE IF NOT EXISTS activity_aggregates(
                session_id INTEGER NOT NULL, minute_us INTEGER NOT NULL,
                context_id INTEGER NOT NULL, kind INTEGER NOT NULL, code INTEGER NOT NULL,
                cell_x INTEGER NOT NULL, cell_y INTEGER NOT NULL, monitor INTEGER NOT NULL,
                count INTEGER NOT NULL, duration_us INTEGER NOT NULL,
                raw_distance INTEGER NOT NULL, pixel_distance INTEGER NOT NULL,
                wheel_delta INTEGER NOT NULL,
                PRIMARY KEY(session_id,minute_us,context_id,kind,code,cell_x,cell_y,monitor));
            CREATE INDEX IF NOT EXISTS ix_activity_kind_time
                ON activity_aggregates(kind,minute_us);
            CREATE INDEX IF NOT EXISTS ix_activity_context_time
                ON activity_aggregates(context_id,minute_us);
        )SQL");
        {
            Statement version(db,"SELECT version FROM schema_info LIMIT 1");
            if(sqlite3_step(version.p)!=SQLITE_ROW || sqlite3_column_int(version.p,0)!=1)
                throw std::runtime_error("Unsupported database schema version");
        }
        Statement session(db,"INSERT INTO sessions(started_us) VALUES(?1)");
        sqlite3_bind_int64(session.p,1,start_us_); session.step();
        session_id_=sqlite3_last_insert_rowid(db);
        {
            Statement unknown(db,"INSERT INTO app_contexts(session_id,id,pid,hwnd,exe_path,exe_name,confidence) VALUES(?1,0,0,0,'','',0)");
            sqlite3_bind_int64(unknown.p,1,session_id_);unknown.step();
        }
        {
            std::lock_guard lock(mutex_); ready_=true;
        }
        cv_.notify_all();
        Statement context(db,R"SQL(
            INSERT OR REPLACE INTO app_contexts
            (session_id,id,pid,hwnd,exe_path,exe_name,confidence)
            VALUES(?1,?2,?3,?4,?5,?6,?7))SQL");
        Statement monitor(db,R"SQL(
            INSERT OR REPLACE INTO monitors
            (session_id,id,device,left_px,top_px,right_px,bottom_px)
            VALUES(?1,?2,?3,?4,?5,?6,?7))SQL");
        Statement stat(db,R"SQL(
            INSERT INTO activity_aggregates
            (session_id,minute_us,context_id,kind,code,cell_x,cell_y,monitor,
             count,duration_us,raw_distance,pixel_distance,wheel_delta)
            VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13)
            ON CONFLICT DO UPDATE SET
              count=count+excluded.count,
              duration_us=duration_us+excluded.duration_us,
              raw_distance=raw_distance+excluded.raw_distance,
              pixel_distance=pixel_distance+excluded.pixel_distance,
              wheel_delta=wheel_delta+excluded.wheel_delta)SQL");
        auto cleanup=[&] {
            if(retention_days_<=0) return;
            auto now=std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            auto cutoff=now-std::int64_t(retention_days_)*86400000000ll;
            Statement prune(db,"DELETE FROM activity_aggregates WHERE minute_us < ?1");
            sqlite3_bind_int64(prune.p,1,cutoff);prune.step();
            Statement old_context(db,"DELETE FROM app_contexts WHERE session_id IN (SELECT id FROM sessions WHERE id<>?2 AND COALESCE(stopped_us,started_us) < ?1)");
            sqlite3_bind_int64(old_context.p,1,cutoff);
            sqlite3_bind_int64(old_context.p,2,session_id_);old_context.step();
            Statement old_sessions(db,"DELETE FROM sessions WHERE id<>?2 AND COALESCE(stopped_us,started_us) < ?1");
            sqlite3_bind_int64(old_sessions.p,1,cutoff);
            sqlite3_bind_int64(old_sessions.p,2,session_id_);old_sessions.step();
            Statement old_monitors(db,"DELETE FROM monitors WHERE session_id NOT IN (SELECT id FROM sessions)");
            old_monitors.step();
        };
        cleanup();
        auto last_cleanup=std::chrono::steady_clock::now();
        for(;;) {
            Batch batch;
            {
                std::unique_lock lock(mutex_);
                cv_.wait(lock,[this]{return closing_ || !queue_.empty();});
                if(queue_.empty()) break;
                batch=std::move(queue_.front());queue_.pop_front();
                cv_.notify_all();
            }
            sql(db,"BEGIN IMMEDIATE");
            try {
                for(const auto& c:batch.contexts) {
                    sqlite3_bind_int64(context.p,1,session_id_);
                    sqlite3_bind_int64(context.p,2,c.id);
                    sqlite3_bind_int64(context.p,3,c.pid);
                    sqlite3_bind_int64(context.p,4,c.hwnd);
                    sqlite3_bind_text(context.p,5,c.path.c_str(),-1,SQLITE_TRANSIENT);
                    sqlite3_bind_text(context.p,6,c.name.c_str(),-1,SQLITE_TRANSIENT);
                    sqlite3_bind_int(context.p,7,c.confidence);context.step();
                }
                for(const auto& m:batch.monitors) {
                    sqlite3_bind_int64(monitor.p,1,session_id_);
                    sqlite3_bind_int(monitor.p,2,m.id);
                    sqlite3_bind_text(monitor.p,3,m.device.c_str(),-1,SQLITE_TRANSIENT);
                    sqlite3_bind_int(monitor.p,4,m.left);
                    sqlite3_bind_int(monitor.p,5,m.top);
                    sqlite3_bind_int(monitor.p,6,m.right);
                    sqlite3_bind_int(monitor.p,7,m.bottom);
                    monitor.step();
                }
                for(const auto& [k,v]:batch.stats) {
                    sqlite3_bind_int64(stat.p,1,session_id_);
                    sqlite3_bind_int64(stat.p,2,k.minute);
                    sqlite3_bind_int64(stat.p,3,k.context);
                    sqlite3_bind_int(stat.p,4,k.kind);
                    sqlite3_bind_int64(stat.p,5,k.code);
                    sqlite3_bind_int(stat.p,6,k.x);sqlite3_bind_int(stat.p,7,k.y);
                    sqlite3_bind_int(stat.p,8,k.monitor);
                    sqlite3_bind_int64(stat.p,9,v.count);
                    sqlite3_bind_int64(stat.p,10,v.duration_us);
                    sqlite3_bind_int64(stat.p,11,v.raw_distance);
                    sqlite3_bind_int64(stat.p,12,v.pixel_distance);
                    sqlite3_bind_int64(stat.p,13,v.wheel_delta);stat.step();
                }
                sql(db,"COMMIT");
                ++transactions_;rows_+=batch.stats.size();
                if(std::chrono::steady_clock::now()-last_cleanup>=std::chrono::hours(24)) {
                    cleanup();last_cleanup=std::chrono::steady_clock::now();
                }
            } catch(...) { try {sql(db,"ROLLBACK");} catch(...) {} throw; }
        }
        if(stop_us_>0) {
            Statement end(db,"UPDATE sessions SET stopped_us=?1 WHERE id=?2");
            sqlite3_bind_int64(end.p,1,stop_us_);sqlite3_bind_int64(end.p,2,session_id_);end.step();
        }
    } catch(const std::exception& e) {
        std::lock_guard lock(mutex_);
        error_=e.what();ready_=true;
        dropped_+=queue_.size();queue_.clear();
        if(failure_event_) SetEvent(failure_event_);
        cv_.notify_all();
    }
    if(db) sqlite3_close(db);
}

int Storage::query(const std::string& path) {
    sqlite3* db=nullptr;
    if(sqlite3_open_v2(path.c_str(),&db,SQLITE_OPEN_READONLY,nullptr)!=SQLITE_OK) {
        std::cerr << "Cannot open database\n"; if(db) sqlite3_close(db);return 1;
    }
    const char* q="SELECT kind,COUNT(*),SUM(count),SUM(duration_us),SUM(raw_distance),"
                  "SUM(pixel_distance),SUM(wheel_delta) FROM activity_aggregates GROUP BY kind ORDER BY kind";
    sqlite3_stmt* s=nullptr;
    if(sqlite3_prepare_v2(db,q,-1,&s,nullptr)!=SQLITE_OK){std::cerr<<sqlite3_errmsg(db)<<'\n';sqlite3_close(db);return 1;}
    std::cout<<"kind rows events hold_us raw_distance pixel_distance wheel_delta\n";
    while(sqlite3_step(s)==SQLITE_ROW) {
        for(int i=0;i<7;++i) std::cout<<sqlite3_column_int64(s,i)<<(i==6?'\n':' ');
    }
    sqlite3_finalize(s);sqlite3_close(db);return 0;
}
bool Storage::verify_self_test(const std::string& path,std::int64_t session_id) {
    sqlite3* db=nullptr;
    if(sqlite3_open_v2(path.c_str(),&db,SQLITE_OPEN_READONLY,nullptr)!=SQLITE_OK) {
        if(db) sqlite3_close(db);return false;
    }
    const char* q="SELECT kind,SUM(count),SUM(duration_us),SUM(raw_distance),SUM(wheel_delta) "
                  "FROM activity_aggregates WHERE session_id=?1 GROUP BY kind ORDER BY kind";
    sqlite3_stmt* s=nullptr;
    if(sqlite3_prepare_v2(db,q,-1,&s,nullptr)!=SQLITE_OK){sqlite3_close(db);return false;}
    sqlite3_bind_int64(s,1,session_id);
    const std::int64_t expected[7][5]={{1,2,0,0,0},{2,2,50000,0,0},{3,1,0,0,0},
        {4,1,0,0,0},{5,1,30000,0,0},{6,1,0,15,0},{7,1,0,0,120}};
    bool okay=true;
    for(const auto& row:expected) {
        if(sqlite3_step(s)!=SQLITE_ROW) {okay=false;break;}
        for(int i=0;i<5;++i) if(sqlite3_column_int64(s,i)!=row[i]) okay=false;
    }
    if(sqlite3_step(s)!=SQLITE_DONE) okay=false;
    sqlite3_finalize(s);
    const char* context_query="SELECT confidence,pid FROM app_contexts WHERE session_id=?1 AND id=1";
    if(sqlite3_prepare_v2(db,context_query,-1,&s,nullptr)!=SQLITE_OK) {sqlite3_close(db);return false;}
    sqlite3_bind_int64(s,1,session_id);
    if(sqlite3_step(s)!=SQLITE_ROW || sqlite3_column_int(s,0)!=2 || sqlite3_column_int(s,1)<=0)
        okay=false;
    sqlite3_finalize(s);
    const char* attribution_query="SELECT COUNT(*) FROM activity_aggregates WHERE session_id=?1 AND context_id<>1";
    if(sqlite3_prepare_v2(db,attribution_query,-1,&s,nullptr)!=SQLITE_OK) {sqlite3_close(db);return false;}
    sqlite3_bind_int64(s,1,session_id);
    if(sqlite3_step(s)!=SQLITE_ROW || sqlite3_column_int(s,0)!=0) okay=false;
    sqlite3_finalize(s);sqlite3_close(db);return okay;
}
}
