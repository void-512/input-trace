#include "visuals.hpp"
#include <QPainter>
#include <QMouseEvent>
#include <QToolTip>
#include <QDateTime>
#include <QTimeZone>
#include <QHash>
#include <cmath>
#include <algorithm>

namespace gui {
namespace {
QColor surface(){return QColor("#141e2c");}
QColor muted(){return QColor("#8190a5");}
QColor hot(qreal t){t=std::clamp(t,0.0,1.0);return QColor::fromRgbF(.14+.31*t,.28+.47*t,.40+.43*t);}
void title(QPainter& p,const QRectF& r,const QString& text){p.setPen(QColor("#e7edf5"));QFont f=p.font();f.setBold(true);f.setPointSize(12);p.setFont(f);p.drawText(r,text);}
}
KeyboardView::KeyboardView(QWidget* p):QWidget(p){setMinimumHeight(300);setMouseTracking(true);
    struct Spec {int scan;const char* label;double width;};
    const QVector<QVector<Spec>> rows={
        {{1,"Esc",1},{2,"1",1},{3,"2",1},{4,"3",1},{5,"4",1},{6,"5",1},{7,"6",1},{8,"7",1},{9,"8",1},{10,"9",1},{11,"0",1},{12,"-",1},{13,"=",1},{14,"Back",2}},
        {{15,"Tab",1.5},{16,"Q",1},{17,"W",1},{18,"E",1},{19,"R",1},{20,"T",1},{21,"Y",1},{22,"U",1},{23,"I",1},{24,"O",1},{25,"P",1},{26,"[",1},{27,"]",1},{28,"Enter",1.5}},
        {{58,"Caps",1.8},{30,"A",1},{31,"S",1},{32,"D",1},{33,"F",1},{34,"G",1},{35,"H",1},{36,"J",1},{37,"K",1},{38,"L",1},{39,";",1},{40,"'",1},{43,"\\",1.7}},
        {{42,"Shift",2.2},{44,"Z",1},{45,"X",1},{46,"C",1},{47,"V",1},{48,"B",1},{49,"N",1},{50,"M",1},{51,",",1},{52,".",1},{53,"/",1},{54,"Shift",2.8}},
        {{29,"Ctrl",1.5},{56,"Alt",1.5},{57,"Space",7},{0x138,"Alt",1.5},{0x11d,"Ctrl",1.5},{0x14b,"←",1},{0x150,"↓",1},{0x14d,"→",1}}
    };
    for(int y=0;y<rows.size();++y){double x=0;for(const auto& s:rows[y]){keys_.push_back({s.scan,QString::fromUtf8(s.label),QRectF(x,y,s.width,1)});x+=s.width;}}
}
void KeyboardView::setRows(const QVector<Row>& rows){counts_.clear();holds_.clear();releases_.clear();total_=0;max_=1;
    for(const auto& r:rows){if(r.kind==1){counts_[r.code]+=r.count;total_+=r.count;max_=std::max(max_,counts_[r.code]);}
        if(r.kind==2){holds_[r.code]+=r.duration;releases_[r.code]+=r.count;}} update();}
void KeyboardView::paintEvent(QPaintEvent*){QPainter p(this);p.setRenderHint(QPainter::Antialiasing);p.fillRect(rect(),surface());
    QRectF area=QRectF(rect()).adjusted(20,48,-20,-18); qreal sx=area.width()/16.5,sy=area.height()/5.0;
    title(p,QRectF(20,14,width()-40,24),"PHYSICAL KEYBOARD  ·  PRESS FREQUENCY");
    for(const auto& k:keys_){QRectF r(area.left()+k.rect.x()*sx+3,area.top()+k.rect.y()*sy+3,k.rect.width()*sx-6,sy-6);
        qreal intensity=counts_.value(k.code)?std::log1p(double(counts_.value(k.code)))/std::log1p(double(max_)):0;
        p.setBrush(counts_.value(k.code)?hot(intensity):QColor("#253246"));p.setPen(QColor("#3b4d63"));p.drawRoundedRect(r,5,5);
        p.setPen(Qt::white);QFont f=p.font();f.setPointSize(std::max(7,int(std::min(sx,sy)/5)));f.setBold(false);p.setFont(f);p.drawText(r,Qt::AlignCenter,k.label);
    }}
void KeyboardView::mouseMoveEvent(QMouseEvent* e){QRectF area=QRectF(rect()).adjusted(20,48,-20,-18);qreal sx=area.width()/16.5,sy=area.height()/5.0;
    for(const auto& k:keys_){QRectF r(area.left()+k.rect.x()*sx,area.top()+k.rect.y()*sy,k.rect.width()*sx,sy);if(r.contains(e->position())){
        qint64 c=counts_.value(k.code), n=releases_.value(k.code);QString tip=QString("%1 · scan 0x%2\n%3 presses · %4% of total\nAverage completed hold: %5 ms")
            .arg(keyName(k.code)).arg(k.code,2,16,QChar('0')).arg(c).arg(total_?QString::number(100.0*c/total_,'f',1):"0")
            .arg(n?QString::number(double(holds_.value(k.code))/n/1000,'f',1):"—");QToolTip::showText(e->globalPosition().toPoint(),tip,this);return;}}
    QToolTip::hideText();}

MouseView::MouseView(QWidget* p):QWidget(p){setMinimumHeight(380);setMouseTracking(true);}
void MouseView::setData(const QVector<Row>& rows,const QVector<Monitor>& monitors,bool movement,qint64 selectedSession,int selectedMonitor){
    movement_=movement;cells_.clear();screens_.clear();max_=1;extent_={};
    for(const auto& m:monitors) if(m.session==selectedSession && (selectedMonitor==0 || m.id==selectedMonitor)){
        QRectF r(m.left,m.top,m.right-m.left,m.bottom-m.top);screens_.push_back({r,m.device.isEmpty()?QString("Monitor %1").arg(m.id):m.device});extent_=extent_.isNull()?r:extent_.united(r);}
    QMap<QPair<int,int>,qint64> sums;
    for(const auto& r:rows) if(r.session==selectedSession && (selectedMonitor==0 || r.monitor==selectedMonitor)
         && r.x!=INT32_MIN && r.y!=INT32_MIN && (movement?r.kind==6:r.kind==4)) sums[{r.x,r.y}]+=r.count;
    for(auto it=sums.cbegin();it!=sums.cend();++it){QRectF r(it.key().first*32,it.key().second*32,32,32);
        cells_.push_back({r,it.value(),QString("Cell (%1, %2) · %3 %4").arg(it.key().first).arg(it.key().second).arg(it.value()).arg(movement?"samples":"clicks")});
        max_=std::max(max_,it.value());if(extent_.isNull()) extent_=r;else extent_=extent_.united(r);}
    update();}
QPointF MouseView::transform(const QPointF& point) const {QRectF a=QRectF(rect()).adjusted(32,60,-32,-42);
    if(extent_.isNull()||extent_.width()<=0||extent_.height()<=0)return a.center();
    qreal scale=std::min(a.width()/extent_.width(),a.height()/extent_.height());
    QPointF origin=a.center()-QPointF(extent_.center().x()*scale,extent_.center().y()*scale);return origin+point*scale;}
void MouseView::paintEvent(QPaintEvent*){QPainter p(this);p.setRenderHint(QPainter::Antialiasing);p.fillRect(rect(),surface());
    title(p,QRectF(22,16,width()-44,28),movement_?"MOVEMENT SAMPLES  ·  32 PX CELLS":"MOUSE CLICKS  ·  32 PX CELLS");
    if(extent_.isNull()){p.setPen(muted());p.drawText(rect(),Qt::AlignCenter,"No spatial activity in this selection");return;}
    for(const auto& s:screens_){QRectF r(transform(s.first.topLeft()),transform(s.first.bottomRight()));p.setBrush(QColor("#1d2b3e"));p.setPen(QPen(QColor("#66819c"),2));p.drawRect(r);p.setPen(QColor("#9fb5ca"));p.drawText(r.adjusted(8,6,-4,-4),Qt::AlignTop|Qt::AlignLeft,s.second);}
    p.setPen(Qt::NoPen);for(const auto& c:cells_){QRectF r(transform(c.rect.topLeft()),transform(c.rect.bottomRight()));qreal t=std::log1p(double(c.value))/std::log1p(double(max_));
        QColor color=hot(t);color.setAlpha(95+int(t*145));p.setBrush(color);p.drawRect(r);}
    p.setPen(muted());p.drawText(QRectF(20,height()-28,width()-40,20),"Aggregated spatial samples · no precise cursor path is recorded");}
void MouseView::mouseMoveEvent(QMouseEvent* e){for(const auto& c:cells_){QRectF r(transform(c.rect.topLeft()),transform(c.rect.bottomRight()));if(r.contains(e->position())){QToolTip::showText(e->globalPosition().toPoint(),c.detail,this);return;}}QToolTip::hideText();}

TimelineView::TimelineView(QWidget* p):QWidget(p){setMinimumHeight(300);setMouseTracking(true);}
void TimelineView::setRows(const QVector<Row>& rows,int grouping,bool keyboard,bool mouse){QMap<qint64,Bucket> map;max_=1;
    bucketUs_=grouping==0?60000000ll:grouping==1?3600000000ll:86400000000ll;
    for(const auto& r:rows){qint64 t=r.minute;if(grouping==1)t=t/3600000000ll*3600000000ll;
        else if(grouping==2){auto dt=QDateTime::fromMSecsSinceEpoch(t/1000).toLocalTime();auto day=QDateTime(dt.date(),QTime(0,0),QTimeZone::systemTimeZone());t=day.toMSecsSinceEpoch()*1000;}
        auto& b=map[t];b.time=t;if(keyboard&&r.kind==1)b.keys+=r.count;if(mouse&&(r.kind==4||r.kind==6||r.kind==7))b.mouse+=r.count;}
    buckets_.clear();for(auto b:map)if(b.keys||b.mouse){buckets_.push_back(b);max_=std::max(max_,b.keys+b.mouse);}update();}
void TimelineView::paintEvent(QPaintEvent*){QPainter p(this);p.setRenderHint(QPainter::Antialiasing);p.fillRect(rect(),surface());title(p,QRectF(22,14,width()-44,26),"ACTIVITY OVER TIME");
    QRectF area=QRectF(rect()).adjusted(48,62,-20,-48);p.setPen(QColor("#34465c"));for(int i=0;i<=4;++i){qreal y=area.bottom()-area.height()*i/4;p.drawLine(QPointF(area.left(),y),QPointF(area.right(),y));}
    if(buckets_.isEmpty()){p.setPen(muted());p.drawText(area,Qt::AlignCenter,"No activity in this range");return;}
    qint64 first=buckets_.first().time,last=buckets_.last().time+bucketUs_;qreal span=double(std::max<qint64>(bucketUs_,last-first));
    qreal bar=std::max(1.0,std::min(24.0,area.width()*double(bucketUs_)/span*.72));
    for(const auto& b:buckets_){qreal x=area.left()+area.width()*double(b.time-first)/span;
        qreal kh=area.height()*b.keys/max_,mh=area.height()*b.mouse/max_;
        p.fillRect(QRectF(x,area.bottom()-kh,bar,kh),QColor("#52b5d2"));p.fillRect(QRectF(x,area.bottom()-kh-mh,bar,mh),QColor("#9d86f3"));}
    p.setPen(muted());for(int i=0;i<5;++i){qint64 t=first+qint64(double(last-first)*i/4);QString label=QDateTime::fromMSecsSinceEpoch(t/1000).toLocalTime().toString("MMM d hh:mm");
        qreal left=std::clamp(area.left()+area.width()*i/4-45,area.left(),area.right()-90);
        p.drawText(QRectF(left,area.bottom()+8,90,22),Qt::AlignCenter,label);}}
void TimelineView::mouseMoveEvent(QMouseEvent* e){QRectF area=QRectF(rect()).adjusted(48,62,-20,-48);if(buckets_.isEmpty()||!area.contains(e->position()))return;
    qint64 first=buckets_.first().time,last=buckets_.last().time+bucketUs_;
    qint64 t=first+qint64((e->position().x()-area.left())/area.width()*double(last-first));
    auto it=std::upper_bound(buckets_.begin(),buckets_.end(),t,[](qint64 value,const Bucket& b){return value<b.time;});
    if(it==buckets_.begin()){QToolTip::hideText();return;}--it;
    if(t>=it->time+bucketUs_){QToolTip::hideText();return;}
    const auto& b=*it;
    QString tip=QString("%1 local\n%2 key presses · %3 mouse events/samples")
        .arg(QDateTime::fromMSecsSinceEpoch(b.time/1000).toLocalTime().toString("yyyy-MM-dd hh:mm t")).arg(b.keys).arg(b.mouse);
    QToolTip::showText(e->globalPosition().toPoint(),tip,this);}
}
