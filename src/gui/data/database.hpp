#pragma once
#include <QString>
#include <QVector>
#include <QDateTime>
#include <cstdint>

namespace gui {
struct Row {
    qint64 session=0, minute=0, code=0, count=0, duration=0, raw=0, pixels=0, wheel=0;
    int context=0, kind=0, x=0, y=0, monitor=0;
    QString app, path;
};
struct Monitor { qint64 session=0; int id=0,left=0,top=0,right=0,bottom=0; QString device; };
struct Session { qint64 id=0,started=0,stopped=0; };
struct Snapshot {
    QVector<Row> rows;
    QVector<Monitor> monitors;
    QVector<Session> sessions;
    QString error, database;
    bool truncated=false;
};
struct Request { QString database, app; qint64 from=0,to=0; };
Snapshot load(const Request& request);
QString keyName(qint64 code);
QString shortcutName(qint64 code);
QString applicationIdentity(const QString& path,const QString& name);
}
