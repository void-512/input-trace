#include "data/database.hpp"
#include "widgets/visuals.hpp"
#include <QApplication>
#include <QMainWindow>
#include <QStackedWidget>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QLabel>
#include <QPushButton>
#include <QComboBox>
#include <QDateTimeEdit>
#include <QLineEdit>
#include <QFileDialog>
#include <QTableWidget>
#include <QHeaderView>
#include <QFrame>
#include <QTimer>
#include <QDir>
#include <QPointer>
#include <QCommandLineParser>
#include <QStandardPaths>
#include <QMessageBox>
#include <QCheckBox>
#include <QSpinBox>
#include <QPainter>
#include <QProcess>
#include <QFileInfo>
#include <QLocale>
#include <QStyle>
#include <QTimeZone>
#include <QMetaObject>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <map>
#include <algorithm>

using namespace gui;
namespace {
QString number(qint64 n){return QLocale().toString(n);}
QLabel* label(const QString& t,const QString& cls="") {auto* l=new QLabel(t);if(!cls.isEmpty())l->setProperty("class",cls);return l;}
QFrame* panel(QLayout* content){auto* f=new QFrame;f->setProperty("class","panel");f->setLayout(content);return f;}
QVBoxLayout* column(int gap=12){auto* l=new QVBoxLayout;l->setContentsMargins(18,16,18,16);l->setSpacing(gap);return l;}
QTableWidget* table(const QStringList& columns){auto* t=new QTableWidget(0,columns.size());t->setHorizontalHeaderLabels(columns);t->setEditTriggers(QAbstractItemView::NoEditTriggers);t->setSelectionBehavior(QAbstractItemView::SelectRows);t->setAlternatingRowColors(true);t->verticalHeader()->hide();t->horizontalHeader()->setStretchLastSection(true);t->setShowGrid(false);t->setMinimumHeight(260);return t;}
void item(QTableWidget* t,int row,int col,const QString& value){auto* i=new QTableWidgetItem(value);t->setItem(row,col,i);}
QWidget* scrollPage(QVBoxLayout* content){auto* inner=new QWidget;inner->setLayout(content);auto* scroll=new QScrollArea;scroll->setWidgetResizable(true);scroll->setFrameShape(QFrame::NoFrame);scroll->setWidget(inner);return scroll;}
QString appName(const Row& r){return r.app.isEmpty()?(r.path.isEmpty()?"Unknown application":QFileInfo(r.path).fileName()):r.app;}
}

class MainWindow final : public QMainWindow {
    QStackedWidget* pages_=new QStackedWidget;
    QVector<QPushButton*> nav_;
    QComboBox *range_=new QComboBox,*app_=new QComboBox,*mode_=new QComboBox,*monitor_=new QComboBox,*mouseSession_=new QComboBox,*group_=new QComboBox;
    QDateTimeEdit *from_=new QDateTimeEdit,*to_=new QDateTimeEdit;
    QLineEdit* path_=new QLineEdit;
    QLabel *status_=label("Ready"),*banner_=label(""),*sessionInfo_=label(""),*dashboardApps_=label(""),*dashboardKeys_=label("");
    QLabel *keyMetric_=label("0"),*clickMetric_=label("0"),*scrollMetric_=label("0"),*moveMetric_=label("0");
    QLabel* pageTitle_=label("Overview  /  recorded activity","topTitle");
    KeyboardView* keyboard_=new KeyboardView;
    MouseView* mouse_=new MouseView;
    TimelineView *dashTimeline_=new TimelineView,*timeline_=new TimelineView;
    QTableWidget *keys_=table({"Physical key","Presses","Share","Avg hold","Shortcuts"}),
                 *shortcuts_=table({"Shortcut","Count"}),*apps_=table({"Observed foreground app","Keys","Clicks","Movement samples","Shortcuts"});
    QCheckBox *timelineKeys_=new QCheckBox("Key presses"),*timelineMouse_=new QCheckBox("Mouse activity");
    Snapshot snapshot_;
    bool demo_=false,suppress_=false;
    quint64 generation_=0;
    std::thread worker_;
    std::mutex mutex_;std::condition_variable cv_;
    bool stopping_=false,pending_=false;
    Request request_;
    QString screenshotDir_;
    int screenshotStage_=0;
public:
    MainWindow(QString db,bool demo,QString screenshots):demo_(demo),screenshotDir_(std::move(screenshots)) {
        setWindowTitle("InputTrace  •  Activity Studio");resize(1500,900);setMinimumSize(980,670);
        auto* root=new QWidget;auto* body=new QHBoxLayout(root);body->setContentsMargins(0,0,0,0);body->setSpacing(0);
        auto* sidebar=new QFrame;sidebar->setObjectName("sidebar");sidebar->setFixedWidth(205);auto* side=new QVBoxLayout(sidebar);
        side->setContentsMargins(18,25,18,18);side->setSpacing(8);
        auto* brand=label("◈  INPUTTRACE","brand");side->addWidget(brand);side->addWidget(label("ACTIVITY STUDIO","eyebrow"));side->addSpacing(28);
        QStringList sections={"Dashboard","Keyboard","Mouse","Applications","Timeline","Settings"};
        for(int i=0;i<sections.size();++i){auto* b=new QPushButton(sections[i]);b->setProperty("class","nav");b->setCheckable(true);b->setCursor(Qt::PointingHandCursor);nav_<<b;side->addWidget(b);connect(b,&QPushButton::clicked,this,[this,i]{selectPage(i);});}
        side->addStretch();side->addWidget(label("READ-ONLY ANALYSIS","eyebrow"));side->addWidget(status_);
        body->addWidget(sidebar);
        auto* right=new QWidget;auto* layout=new QVBoxLayout(right);layout->setContentsMargins(28,20,28,20);layout->setSpacing(16);
        auto* top=new QHBoxLayout;top->addWidget(pageTitle_);top->addStretch();
        range_->addItems({"Today","Last 24 hours","Last 7 days","Last 30 days","Custom range"});range_->setCurrentIndex(2);
        top->addWidget(label("Period","muted"));top->addWidget(range_);
        from_->setCalendarPopup(true);to_->setCalendarPopup(true);from_->setDisplayFormat("yyyy-MM-dd HH:mm");to_->setDisplayFormat("yyyy-MM-dd HH:mm");
        from_->setDateTime(QDateTime::currentDateTime().addDays(-7));to_->setDateTime(QDateTime::currentDateTime());top->addWidget(from_);top->addWidget(to_);
        top->addWidget(label("App","muted"));app_->setMinimumWidth(160);app_->addItem("All applications",QString());top->addWidget(app_);
        auto* refreshButton=new QPushButton("↻  Refresh");refreshButton->setProperty("class","accent");top->addWidget(refreshButton);layout->addLayout(top);
        banner_->setProperty("class",demo?"demo":"note");banner_->setText(demo?"DEMO DATA  ·  Synthetic records in an isolated database":"Foreground application is observed context; counts are aggregated by UTC minute.");layout->addWidget(banner_);
        buildPages();layout->addWidget(pages_,1);body->addWidget(right,1);setCentralWidget(root);
        setStyleSheet(R"QSS(
            QWidget{background:#0c1420;color:#e7edf5;font-family:'Segoe UI';font-size:10pt}
            QLabel{background:transparent}
            #sidebar{background:#111c2b;border-right:1px solid #28364a}
            QLabel[class="brand"]{font-size:16pt;font-weight:700;color:#f5f8fd}
            QLabel[class="eyebrow"]{font-size:8pt;color:#67869d;font-weight:700;letter-spacing:1px}
            QLabel[class="topTitle"]{font-size:15pt;font-weight:700}
            QLabel[class="muted"]{color:#91a4ba}
            QLabel[class="demo"]{background:#203a3d;color:#8de1d2;border:1px solid #356765;border-radius:8px;padding:9px 14px;font-weight:600}
            QLabel[class="note"]{background:#192a3b;color:#a8bbd1;border:1px solid #2d4258;border-radius:8px;padding:9px 14px}
            QLabel[class="error"]{background:#41282d;color:#ffc6c6;border:1px solid #9c5056;border-radius:8px;padding:9px 14px}
            QFrame[class="panel"]{background:#141e2c;border:1px solid #2a3b50;border-radius:11px}
            QLabel[class="metric"]{font-size:22pt;font-weight:700;color:#f5fbff}
            QLabel[class="metricLabel"]{color:#95a7b9;font-size:9pt}
            QPushButton[class="nav"]{text-align:left;padding:11px 14px;border:0;border-radius:7px;color:#a7b6c8}
            QPushButton[class="nav"]:hover{background:#1a2a3e;color:white}
            QPushButton[class="nav"]:checked{background:#204054;color:#8de5e0;font-weight:700}
            QPushButton[class="accent"]{background:#267f93;color:white;border:0;border-radius:6px;padding:8px 14px;font-weight:700}
            QPushButton[class="accent"]:hover{background:#319ab1}
            QComboBox,QDateTimeEdit,QLineEdit,QSpinBox{background:#1b2a3d;border:1px solid #344a60;border-radius:5px;padding:6px;color:#edf5fd}
            QComboBox::drop-down{border:0;width:20px} QAbstractItemView{background:#1a293b;selection-background-color:#2c6575}
            QTableWidget{background:#141e2c;alternate-background-color:#192638;border:0;selection-background-color:#205b6d;gridline-color:#26394d}
            QHeaderView::section{background:#1b2b3d;color:#a4bed0;border:0;border-bottom:1px solid #34485e;padding:8px;font-weight:700}
            QScrollArea{border:0} QScrollBar:vertical{background:#111e2c;width:10px} QScrollBar::handle:vertical{background:#36506b;border-radius:5px}
            QCheckBox{spacing:7px}
        )QSS");
        path_->setText(db);selectPage(0);
        connect(range_,qOverload<int>(&QComboBox::currentIndexChanged),this,[this]{updateRangeVisibility();refresh();});
        connect(app_,qOverload<int>(&QComboBox::currentIndexChanged),this,[this]{refresh();});
        connect(from_,&QDateTimeEdit::dateTimeChanged,this,[this]{if(range_->currentIndex()==4)refresh();});
        connect(to_,&QDateTimeEdit::dateTimeChanged,this,[this]{if(range_->currentIndex()==4)refresh();});
        connect(refreshButton,&QPushButton::clicked,this,[this]{refresh();});
        connect(mode_,qOverload<int>(&QComboBox::currentIndexChanged),this,[this]{renderMouse();});
        connect(monitor_,qOverload<int>(&QComboBox::currentIndexChanged),this,[this]{renderMouse();});
        connect(mouseSession_,qOverload<int>(&QComboBox::currentIndexChanged),this,[this]{renderMouse();});
        connect(group_,qOverload<int>(&QComboBox::currentIndexChanged),this,[this]{renderTimelines();});
        connect(timelineKeys_,&QCheckBox::toggled,this,[this]{renderTimelines();});
        connect(timelineMouse_,&QCheckBox::toggled,this,[this]{renderTimelines();});
        connect(apps_,&QTableWidget::cellDoubleClicked,this,[this](int row,int){auto key=apps_->item(row,0)->data(Qt::UserRole).toString();int i=app_->findData(key);if(i>=0){app_->setCurrentIndex(i);selectPage(1);}});
        updateRangeVisibility();
        worker_=std::thread([this]{workerLoop();});refresh();
    }
    ~MainWindow() override {{std::lock_guard lock(mutex_);stopping_=true;pending_=false;}cv_.notify_one();if(worker_.joinable())worker_.join();}
private:
    void addHeading(QVBoxLayout* l,const QString& name,const QString& subtitle){l->addWidget(label(name,"topTitle"));l->addWidget(label(subtitle,"muted"));}
    QFrame* metricCard(const QString& name,QLabel* value,const QString& hint){auto* c=column(4);c->addWidget(label(name,"metricLabel"));value->setProperty("class","metric");c->addWidget(value);c->addWidget(label(hint,"muted"));return panel(c);}
    void buildPages(){
        auto* dash=column(16);dash->setContentsMargins(2,2,2,2);addHeading(dash,"Dashboard","A clear view of recorded activity in the selected interval.");
        auto* cards=new QHBoxLayout;cards->setSpacing(12);cards->addWidget(metricCard("KEY PRESSES",keyMetric_,"physical down transitions"));cards->addWidget(metricCard("MOUSE CLICKS",clickMetric_,"button down transitions"));cards->addWidget(metricCard("SCROLL",scrollMetric_,"wheel events"));cards->addWidget(metricCard("MOUSE MOVEMENT",moveMetric_,"raw delta units"));dash->addLayout(cards);
        dashTimeline_->setMinimumHeight(270);dash->addWidget(panel(wrap(dashTimeline_)));
        auto* lower=new QHBoxLayout;auto* a=column();a->addWidget(label("MOST ACTIVE APPLICATIONS","eyebrow"));a->addWidget(dashboardApps_);lower->addWidget(panel(a),1);
        auto* k=column();k->addWidget(label("MOST USED KEYS","eyebrow"));k->addWidget(dashboardKeys_);lower->addWidget(panel(k),1);dash->addLayout(lower);
        auto* info=column();info->addWidget(label("DATA SOURCE","eyebrow"));info->addWidget(sessionInfo_);dash->addWidget(panel(info));dash->addStretch();pages_->addWidget(scrollPage(dash));
        auto* key=column();key->setContentsMargins(2,2,2,2);addHeading(key,"Keyboard","Physical scan codes; key presses, releases, and shortcut records are separate.");key->addWidget(panel(wrap(keyboard_)));
        auto* keyTables=new QHBoxLayout;auto* kt=column();kt->addWidget(label("KEY STATISTICS","eyebrow"));kt->addWidget(keys_);keyTables->addWidget(panel(kt),2);auto* st=column();st->addWidget(label("MODIFIER SHORTCUTS","eyebrow"));st->addWidget(shortcuts_);keyTables->addWidget(panel(st),1);key->addLayout(keyTables);key->addStretch();pages_->addWidget(scrollPage(key));
        auto* mouse=column();mouse->setContentsMargins(2,2,2,2);addHeading(mouse,"Mouse","Spatial aggregates are drawn in physical virtual-desktop coordinates.");auto* controls=new QHBoxLayout;
        mode_->addItems({"Click heatmap","Movement samples"});monitor_->addItem("Full virtual desktop",0);controls->addWidget(label("Mode","muted"));controls->addWidget(mode_);controls->addSpacing(16);controls->addWidget(label("Session","muted"));controls->addWidget(mouseSession_);controls->addSpacing(16);controls->addWidget(label("Monitor","muted"));controls->addWidget(monitor_);controls->addStretch();mouse->addLayout(controls);
        mouse->addWidget(panel(wrap(mouse_)));mouse->addWidget(label("Movement is sampled at most every 16 ms. Raw delta units and sampled cursor pixels are different measures.","muted"));mouse->addStretch();pages_->addWidget(scrollPage(mouse));
        auto* apps=column();apps->setContentsMargins(2,2,2,2);addHeading(apps,"Applications","Activity attributed to observed foreground context, grouped by executable path when available.");apps_->setMinimumHeight(440);apps->addWidget(panel(wrap(apps_)));apps->addWidget(label("Double-click an application to filter its keyboard, mouse, and timeline views.","muted"));apps->addStretch();pages_->addWidget(scrollPage(apps));
        auto* timeline=column();timeline->setContentsMargins(2,2,2,2);addHeading(timeline,"Timeline","UTC minute buckets displayed in local time. Stacked bars show recorded bucket totals.");auto* opts=new QHBoxLayout;group_->addItems({"Minute","Hour","Day"});group_->setCurrentIndex(1);timelineKeys_->setChecked(true);timelineMouse_->setChecked(true);opts->addWidget(label("Group by","muted"));opts->addWidget(group_);opts->addSpacing(20);opts->addWidget(timelineKeys_);opts->addWidget(timelineMouse_);opts->addStretch();timeline->addLayout(opts);timeline_->setMinimumHeight(480);timeline->addWidget(panel(wrap(timeline_)));timeline->addWidget(label("Hour and day grouping uses local civil time; daylight-saving changes can repeat or skip an hour.","muted"));timeline->addStretch();pages_->addWidget(scrollPage(timeline));
        auto* settings=column();settings->setContentsMargins(2,2,2,2);addHeading(settings,"Settings","Choose an existing InputTrace database. Analysis opens it read-only.");auto* box=column();box->addWidget(label("DATABASE FILE","eyebrow"));auto* browseRow=new QHBoxLayout;browseRow->addWidget(path_,1);auto* browse=new QPushButton("Browse…");browse->setProperty("class","accent");browseRow->addWidget(browse);box->addLayout(browseRow);auto* open=new QPushButton("Open database");open->setProperty("class","accent");box->addWidget(open,0,Qt::AlignLeft);box->addWidget(label("No synthetic data is inserted into a recording database. Demo mode uses a separate build/test database.","muted"));settings->addWidget(panel(box));settings->addStretch();pages_->addWidget(scrollPage(settings));
        connect(browse,&QPushButton::clicked,this,[this]{auto f=QFileDialog::getOpenFileName(this,"Open InputTrace database",path_->text(),"SQLite database (*.db *.sqlite);;All files (*)");if(!f.isEmpty())path_->setText(f);});
        connect(open,&QPushButton::clicked,this,[this]{demo_=false;suppress_=true;app_->clear();app_->addItem("All applications",QString());suppress_=false;
            banner_->setProperty("class","note");banner_->setText("Foreground application is observed context; counts are aggregated by UTC minute.");banner_->style()->unpolish(banner_);banner_->style()->polish(banner_);refresh();selectPage(0);});
    }
    QVBoxLayout* wrap(QWidget* w){auto* l=column();l->setContentsMargins(10,10,10,10);l->addWidget(w);return l;}
    void selectPage(int i){pages_->setCurrentIndex(i);for(int n=0;n<nav_.size();++n)nav_[n]->setChecked(n==i);
        const QStringList names={"Overview  /  recorded activity","Keyboard  /  physical keys","Mouse  /  spatial activity","Applications  /  foreground context","Timeline  /  activity buckets","Settings  /  data source"};pageTitle_->setText(names.value(i));}
    void updateRangeVisibility(){bool custom=range_->currentIndex()==4;from_->setVisible(custom);to_->setVisible(custom);}
    void refresh(){if(suppress_)return;auto now=QDateTime::currentDateTime();QDateTime from=now.addDays(-7),to=now;
        switch(range_->currentIndex()){case 0:from=QDateTime(now.date(),QTime(0,0),QTimeZone::systemTimeZone());break;
        case 1:from=now.addSecs(-86400);break;case 2:from=now.addDays(-7);break;case 3:from=now.addDays(-30);break;
        case 4:from=from_->dateTime();to=to_->dateTime();break;}
        Request r{path_->text(),app_->currentData().toString(),from.toMSecsSinceEpoch()*1000,to.toMSecsSinceEpoch()*1000+1};
        status_->setText("Loading…");{std::lock_guard lock(mutex_);++generation_;request_=r;pending_=true;}cv_.notify_one();}
    void workerLoop(){for(;;){Request r;quint64 gen;{std::unique_lock lock(mutex_);cv_.wait(lock,[this]{return stopping_||pending_;});if(stopping_)return;r=request_;gen=generation_;pending_=false;}
            Snapshot result=load(r);QPointer<MainWindow> self(this);QMetaObject::invokeMethod(this,[self,result=std::move(result),gen]() mutable {if(self&&gen==self->generation_)self->apply(std::move(result));},Qt::QueuedConnection);}}
    void apply(Snapshot value){snapshot_=std::move(value);if(!snapshot_.error.isEmpty()){status_->setText("Database error");banner_->setProperty("class","error");banner_->setText("Database error  ·  "+snapshot_.error);
            banner_->style()->unpolish(banner_);banner_->style()->polish(banner_);
            suppress_=true;app_->clear();app_->addItem("All applications",QString());suppress_=false;
            updateMouseSessions();renderDashboard();renderKeyboard();renderMouse();renderApplications();renderTimelines();
            if(!screenshotDir_.isEmpty())QTimer::singleShot(250,this,[this]{captureScreenshots();});return;}
        status_->setText(snapshot_.truncated?"Result capped":"Ready");banner_->setText(snapshot_.truncated?"Showing the newest 150,000 aggregate rows; narrow the range for complete totals.":
            demo_?"DEMO DATA  ·  Synthetic records in an isolated database":"Foreground application is observed context; counts are aggregated by UTC minute.");
        banner_->setProperty("class",demo_?"demo":"note");banner_->style()->unpolish(banner_);banner_->style()->polish(banner_);
        updateAppFilter();updateMouseSessions();renderDashboard();renderKeyboard();renderMouse();renderApplications();renderTimelines();
        if(!screenshotDir_.isEmpty())QTimer::singleShot(250,this,[this]{captureScreenshots();});}
    void updateAppFilter(){QString current=app_->currentData().toString();if(!current.isEmpty())return;QMap<QString,QString> names;
        for(const auto& r:snapshot_.rows)if(r.context!=0)names[applicationIdentity(r.path,r.app)]=appName(r);
        suppress_=true;app_->clear();app_->addItem("All applications",QString());for(auto i=names.cbegin();i!=names.cend();++i)app_->addItem(i.value(),i.key());int idx=app_->findData(current);if(idx>=0)app_->setCurrentIndex(idx);suppress_=false;}
    void renderDashboard(){qint64 keys=0,clicks=0,wheel=0,raw=0;QMap<QString,qint64> apps;QMap<qint64,qint64> keyCounts;
        for(const auto& r:snapshot_.rows){if(r.kind==1){keys+=r.count;keyCounts[r.code]+=r.count;}if(r.kind==4)clicks+=r.count;if(r.kind==7||r.kind==8)wheel+=r.count;if(r.kind==6)raw+=r.raw;
            if(r.kind==1||r.kind==4||r.kind==6)apps[appName(r)]+=r.count;}
        keyMetric_->setText(number(keys));clickMetric_->setText(number(clicks));scrollMetric_->setText(number(wheel));moveMetric_->setText(number(raw));
        QVector<QPair<QString,qint64>> av;for(auto i=apps.cbegin();i!=apps.cend();++i)av.push_back({i.key(),i.value()});std::sort(av.begin(),av.end(),[](auto a,auto b){return a.second>b.second;});
        QStringList lines;for(int i=0;i<std::min(5,int(av.size()));++i)lines<<QString("%1    %2").arg(av[i].first).arg(number(av[i].second));dashboardApps_->setText(lines.isEmpty()?"No recorded activity":lines.join("\n"));
        QVector<QPair<qint64,qint64>> kv;for(auto i=keyCounts.cbegin();i!=keyCounts.cend();++i)kv.push_back({i.key(),i.value()});std::sort(kv.begin(),kv.end(),[](auto a,auto b){return a.second>b.second;});lines.clear();for(int i=0;i<std::min(5,int(kv.size()));++i)lines<<QString("%1    %2").arg(keyName(kv[i].first)).arg(number(kv[i].second));dashboardKeys_->setText(lines.isEmpty()?"No recorded keys":lines.join("\n"));
        sessionInfo_->setText(QString("%1\n%2 collection session(s)  ·  %3 aggregate rows in view")
            .arg(snapshot_.database).arg(snapshot_.sessions.size()).arg(number(snapshot_.rows.size())));}
    void renderKeyboard(){keyboard_->setRows(snapshot_.rows);QMap<qint64,qint64> press,release,hold,combo;
        qint64 total=0;for(const auto& r:snapshot_.rows){if(r.kind==1){press[r.code]+=r.count;total+=r.count;}if(r.kind==2){release[r.code]+=r.count;hold[r.code]+=r.duration;}if(r.kind==3)combo[r.code]+=r.count;}
        QMap<qint64,qint64> comboKey;for(auto i=combo.cbegin();i!=combo.cend();++i)comboKey[quint32(i.key())]+=i.value();
        QVector<qint64> codes=press.keys();std::sort(codes.begin(),codes.end(),[&](qint64 a,qint64 b){return press[a]>press[b];});keys_->setRowCount(std::min(100,int(codes.size())));
        for(int i=0;i<keys_->rowCount();++i){qint64 c=codes[i];item(keys_,i,0,keyName(c));item(keys_,i,1,number(press[c]));item(keys_,i,2,total?QString::number(100.0*press[c]/total,'f',1)+"%":"0%");item(keys_,i,3,release[c]?QString::number(double(hold[c])/release[c]/1000,'f',1)+" ms":"—");item(keys_,i,4,number(comboKey[c]));}keys_->resizeColumnsToContents();
        QVector<qint64> shortcuts=combo.keys();std::sort(shortcuts.begin(),shortcuts.end(),[&](qint64 a,qint64 b){return combo[a]>combo[b];});shortcuts_->setRowCount(std::min(100,int(shortcuts.size())));
        for(int i=0;i<shortcuts_->rowCount();++i){item(shortcuts_,i,0,shortcutName(shortcuts[i]));item(shortcuts_,i,1,number(combo[shortcuts[i]]));}shortcuts_->resizeColumnsToContents();}
    void updateMouseSessions(){auto old=mouseSession_->currentData();mouseSession_->blockSignals(true);mouseSession_->clear();for(const auto& s:snapshot_.sessions){
        auto when=QDateTime::fromMSecsSinceEpoch(s.started/1000).toLocalTime().toString("MMM d, yyyy");mouseSession_->addItem(QString("%1 · session %2").arg(when).arg(s.id),s.id);}
        int index=mouseSession_->findData(old);mouseSession_->setCurrentIndex(index<0?0:index);mouseSession_->blockSignals(false);}
    void renderMouse(){if(!mouse_)return;auto old=monitor_->currentData();monitor_->blockSignals(true);monitor_->clear();monitor_->addItem("Full virtual desktop",0);
        qint64 session=mouseSession_->currentData().toLongLong();
        for(const auto& m:snapshot_.monitors)if(m.session==session)monitor_->addItem(m.device.isEmpty()?QString("Monitor %1").arg(m.id):m.device,m.id);
        int index=monitor_->findData(old);monitor_->setCurrentIndex(index<0?0:index);monitor_->blockSignals(false);
        mouse_->setData(snapshot_.rows,snapshot_.monitors,mode_->currentIndex()==1,session,monitor_->currentData().toInt());}
    void renderApplications(){struct Agg{QString name,path; qint64 keys=0,clicks=0,move=0,shortcuts=0;};QMap<QString,Agg> map;
        for(const auto& r:snapshot_.rows){auto id=applicationIdentity(r.path,r.app);auto& a=map[id];a.name=appName(r);a.path=r.path;if(r.kind==1)a.keys+=r.count;if(r.kind==4)a.clicks+=r.count;if(r.kind==6)a.move+=r.count;if(r.kind==3)a.shortcuts+=r.count;}
        QVector<QPair<QString,Agg>> values;for(auto i=map.cbegin();i!=map.cend();++i)values.push_back({i.key(),i.value()});std::sort(values.begin(),values.end(),[](auto a,auto b){return a.second.keys+a.second.clicks+a.second.move>b.second.keys+b.second.clicks+b.second.move;});
        apps_->setRowCount(values.size());for(int i=0;i<values.size();++i){const auto& a=values[i].second;item(apps_,i,0,a.name);apps_->item(i,0)->setData(Qt::UserRole,values[i].first);apps_->item(i,0)->setToolTip(a.path.isEmpty()?"Path unavailable":a.path);item(apps_,i,1,number(a.keys));item(apps_,i,2,number(a.clicks));item(apps_,i,3,number(a.move));item(apps_,i,4,number(a.shortcuts));}apps_->resizeColumnsToContents();}
    void renderTimelines(){dashTimeline_->setRows(snapshot_.rows,1,true,true);timeline_->setRows(snapshot_.rows,group_->currentIndex(),timelineKeys_->isChecked(),timelineMouse_->isChecked());}
    void captureScreenshots(){QDir().mkpath(screenshotDir_);
        if(screenshotStage_==0){const QStringList names={"dashboard","keyboard","mouse","applications","timeline"};
            for(int i=0;i<names.size();++i){selectPage(i);QApplication::processEvents();grab().save(screenshotDir_+"/"+names[i]+".png");}selectPage(0);
            if(snapshot_.error.isEmpty()&&app_->count()>1){screenshotStage_=1;app_->setCurrentIndex(1);return;}}
        else if(screenshotStage_==1){selectPage(0);QApplication::processEvents();grab().save(screenshotDir_+"/filtered-dashboard.png");screenshotStage_=2;range_->setCurrentIndex(0);return;}
        else {selectPage(0);QApplication::processEvents();grab().save(screenshotDir_+"/today-dashboard.png");}
        QTimer::singleShot(0,qApp,&QApplication::quit);}
};

int main(int argc,char** argv){QApplication app(argc,argv);QApplication::setApplicationName("InputTrace Activity Studio");
    QCommandLineParser parser;parser.addHelpOption();parser.addOption({"db","Open an existing database read-only.","path"});parser.addOption({"demo","Use the isolated synthetic demonstration database."});parser.addOption({"screenshots","Capture all main views as PNG and exit.","directory"});parser.process(app);
    QString db=parser.value("db");bool demo=parser.isSet("demo");
    if(demo){db=QCoreApplication::applicationDirPath()+"/test-data/inputtrace-demo.db";QString script=QDir(QCoreApplication::applicationDirPath()).absoluteFilePath("../../scripts/generate_demo.py");
        if(!QFileInfo::exists(script))script=QDir::current().absoluteFilePath("scripts/generate_demo.py");
        QString python=qEnvironmentVariable("INPUTTRACE_PYTHON","python");QProcess process;process.start(python,{script,db});process.waitForFinished(30000);
        if(process.exitStatus()!=QProcess::NormalExit||process.exitCode()!=0){QMessageBox::critical(nullptr,"Demo data",QString("Could not generate demo database: %1\n%2").arg(script,QString::fromUtf8(process.readAllStandardError())));return 1;}}
    if(db.isEmpty())db=QDir::current().absoluteFilePath("inputtrace.db");
    MainWindow window(db,demo,parser.value("screenshots"));window.show();
    if(parser.isSet("screenshots"))QTimer::singleShot(30000,&app,&QApplication::quit);
    return app.exec();}
