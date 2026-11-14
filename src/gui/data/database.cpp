#include "database.hpp"
#include <sqlite3.h>
#include <QFileInfo>
#include <QFile>
#include <QStringList>
#include <algorithm>

namespace gui {
namespace {
struct Stmt {
    sqlite3_stmt* p=nullptr;
    ~Stmt(){sqlite3_finalize(p);}
};
QString field(sqlite3_stmt* s,int i) {
    auto* t=sqlite3_column_text(s,i);
    return t?QString::fromUtf8(reinterpret_cast<const char*>(t)):QString();
}
bool prepare(sqlite3* db,Stmt& s,const char* sql,QString& error) {
    if(sqlite3_prepare_v2(db,sql,-1,&s.p,nullptr)==SQLITE_OK) return true;
    error=QString::fromUtf8(sqlite3_errmsg(db)); return false;
}
}
QString applicationIdentity(const QString& path,const QString& name) {
    if(!path.trimmed().isEmpty()) return path.trimmed().toCaseFolded();
    if(!name.trimmed().isEmpty()) return QStringLiteral("name:")+name.trimmed().toCaseFolded();
    return QStringLiteral("unknown");
}
Snapshot load(const Request& r) {
    Snapshot out; out.database=r.database;
    sqlite3* db=nullptr;
    QByteArray bytes=QFile::encodeName(r.database);
    if(sqlite3_open_v2(bytes.constData(),&db,SQLITE_OPEN_READONLY|SQLITE_OPEN_URI,nullptr)!=SQLITE_OK) {
        out.error=db?QString::fromUtf8(sqlite3_errmsg(db)):QStringLiteral("Unable to open database");
        if(db) sqlite3_close(db); return out;
    }
    sqlite3_busy_timeout(db,1500);
    auto close=[&]{sqlite3_close(db);};
    {
        Stmt s;
        if(!prepare(db,s,"SELECT version FROM schema_info LIMIT 1",out.error)) {close();return out;}
        if(sqlite3_step(s.p)!=SQLITE_ROW || sqlite3_column_int(s.p,0)!=1) {
            out.error="Unsupported or missing InputTrace schema version";close();return out;
        }
    }
    if(sqlite3_exec(db,"BEGIN",nullptr,nullptr,nullptr)!=SQLITE_OK) {
        out.error=QString::fromUtf8(sqlite3_errmsg(db));close();return out;
    }
    {
        Stmt s;
        if(!prepare(db,s,"SELECT id,started_us,COALESCE(stopped_us,0) FROM sessions ORDER BY started_us DESC LIMIT 1000",out.error)) {sqlite3_exec(db,"ROLLBACK",nullptr,nullptr,nullptr);close();return out;}
        while(sqlite3_step(s.p)==SQLITE_ROW) out.sessions.push_back({sqlite3_column_int64(s.p,0),sqlite3_column_int64(s.p,1),sqlite3_column_int64(s.p,2)});
    }
    {
        Stmt s;
        if(!prepare(db,s,"SELECT session_id,id,device,left_px,top_px,right_px,bottom_px FROM monitors ORDER BY session_id,id LIMIT 4096",out.error)) {sqlite3_exec(db,"ROLLBACK",nullptr,nullptr,nullptr);close();return out;}
        while(sqlite3_step(s.p)==SQLITE_ROW) out.monitors.push_back({sqlite3_column_int64(s.p,0),sqlite3_column_int(s.p,1),sqlite3_column_int(s.p,3),sqlite3_column_int(s.p,4),sqlite3_column_int(s.p,5),sqlite3_column_int(s.p,6),field(s.p,2)});
    }
    {
        Stmt s;
        const char* sql=R"SQL(SELECT a.session_id,a.minute_us,a.context_id,a.kind,a.code,a.cell_x,a.cell_y,a.monitor,
          a.count,a.duration_us,a.raw_distance,a.pixel_distance,a.wheel_delta,
          COALESCE(c.exe_name,''),COALESCE(c.exe_path,'')
          FROM activity_aggregates a LEFT JOIN app_contexts c
          ON c.session_id=a.session_id AND c.id=a.context_id
          WHERE a.minute_us>=?1 AND a.minute_us<?2
          AND (?3='' OR (CASE WHEN TRIM(COALESCE(c.exe_path,''))<>'' THEN LOWER(TRIM(c.exe_path))
             WHEN TRIM(COALESCE(c.exe_name,''))<>'' THEN 'name:'||LOWER(TRIM(c.exe_name)) ELSE 'unknown' END)=?3)
          ORDER BY a.minute_us DESC LIMIT 150001)SQL";
        if(!prepare(db,s,sql,out.error)) {sqlite3_exec(db,"ROLLBACK",nullptr,nullptr,nullptr);close();return out;}
        sqlite3_bind_int64(s.p,1,r.from);sqlite3_bind_int64(s.p,2,r.to);
        auto filter=r.app.toUtf8();sqlite3_bind_text(s.p,3,filter.constData(),-1,SQLITE_TRANSIENT);
        int rc=SQLITE_ROW;
        while((rc=sqlite3_step(s.p))==SQLITE_ROW) {
            auto name=field(s.p,13),path=field(s.p,14);
            if(out.rows.size()==150000) {out.truncated=true;break;}
            out.rows.push_back({sqlite3_column_int64(s.p,0),sqlite3_column_int64(s.p,1),sqlite3_column_int64(s.p,4),
                sqlite3_column_int64(s.p,8),sqlite3_column_int64(s.p,9),sqlite3_column_int64(s.p,10),
                sqlite3_column_int64(s.p,11),sqlite3_column_int64(s.p,12),
                sqlite3_column_int(s.p,2),sqlite3_column_int(s.p,3),sqlite3_column_int(s.p,5),
                sqlite3_column_int(s.p,6),sqlite3_column_int(s.p,7),name,path});
        }
        if(rc!=SQLITE_DONE && !out.truncated) out.error=QString::fromUtf8(sqlite3_errmsg(db));
    }
    sqlite3_exec(db,"COMMIT",nullptr,nullptr,nullptr);
    close(); return out;
}
QString keyName(qint64 code) {
    const bool fallback=(code&0x8000)!=0;
    const int scan=int(code&0xff), flags=int(code&0x300);
    if(fallback) return (QString("VK 0x%1").arg(scan,2,16,QChar('0'))+
        (flags==0x100?" E0":flags==0x200?" E1":"")).toUpper();
    static const char* names[]={"","Esc","1","2","3","4","5","6","7","8","9","0","-","=","Backspace","Tab",
        "Q","W","E","R","T","Y","U","I","O","P","[","]","Enter","Ctrl","A","S","D","F","G","H","J","K","L",";","'","`","Shift","\\","Z","X","C","V","B","N","M",",",".","/","Shift","Num *","Alt","Space","Caps"};
    if(flags==0 && scan>0 && scan<int(sizeof(names)/sizeof(names[0])) && *names[scan]) return QString::fromLatin1(names[scan]);
    if(flags==0x100) {
        switch(scan) {case 0x1c:return "Numpad Enter";case 0x1d:return "Right Ctrl";case 0x35:return "Numpad /";
        case 0x38:return "Right Alt";case 0x47:return "Home";case 0x48:return "Up";case 0x49:return "Page Up";
        case 0x4b:return "Left";case 0x4d:return "Right";case 0x4f:return "End";case 0x50:return "Down";
        case 0x51:return "Page Down";case 0x52:return "Insert";case 0x53:return "Delete";}
    }
    return QString("Scan %1%2").arg(scan,2,16,QChar('0')).arg(flags==0x100?" E0":flags==0x200?" E1":"").toUpper();
}
QString shortcutName(qint64 code) {
    const quint32 mod=quint64(code)>>32;
    QStringList parts;
    const char* names[]={"L Ctrl","R Ctrl","L Shift","R Shift","L Alt","R Alt","L Win","R Win"};
    for(int i=0;i<8;++i) if(mod&(1u<<i)) parts<<names[i];
    parts<<keyName(quint32(code));return parts.join(" + ");
}
}
