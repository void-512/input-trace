#pragma once
#include "data/database.hpp"
#include <QWidget>
#include <QMap>

namespace gui {
class KeyboardView : public QWidget {
public:
    explicit KeyboardView(QWidget* parent=nullptr);
    void setRows(const QVector<Row>& rows);
protected:
    void paintEvent(QPaintEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
private:
    struct Key { int code; QString label; QRectF rect; };
    QVector<Key> keys_;
    QMap<qint64,qint64> counts_,holds_;
    QMap<qint64,qint64> releases_;
    qint64 total_=0,max_=1;
};
class MouseView : public QWidget {
public:
    explicit MouseView(QWidget* parent=nullptr);
    void setData(const QVector<Row>& rows,const QVector<Monitor>& monitors,bool movement,qint64 selectedSession,int selectedMonitor);
protected:
    void paintEvent(QPaintEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
private:
    struct Cell { QRectF rect; qint64 value=0; QString detail; };
    QVector<Cell> cells_;
    QVector<QPair<QRectF,QString>> screens_;
    QRectF extent_;
    qint64 max_=1;
    bool movement_=false;
    QPointF transform(const QPointF&) const;
};
class TimelineView : public QWidget {
public:
    explicit TimelineView(QWidget* parent=nullptr);
    void setRows(const QVector<Row>& rows,int grouping,bool keyboard,bool mouse);
protected:
    void paintEvent(QPaintEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
private:
    struct Bucket { qint64 time=0,keys=0,mouse=0; };
    QVector<Bucket> buckets_;
    qint64 max_=1;
    qint64 bucketUs_=3600000000ll;
};
}
