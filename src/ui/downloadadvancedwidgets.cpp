#include "downloadadvancedwidgets.h"
#include "downloadtextbinder.h"
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QComboBox>
#include <QLineEdit>
#include <QTableWidget>
#include <QHeaderView>
#include <QPushButton>
#include <QToolButton>
#include <QMenu>
#include <QTimeEdit>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QJsonArray>

SeedingPolicyWidget::SeedingPolicyWidget(bool allowInherit,QWidget* parent):QWidget(parent) {
    auto* texts=new DownloadTextBinder(this,"SeedingPolicyWidget");auto tr=[texts](const char* source){return texts->text(source);};
    auto* layout=new QFormLayout(this);layout->setContentsMargins(0,0,0,0);
    inherit_=new QCheckBox(tr("Inherit global seeding conditions"),this);inherit_->setVisible(allowInherit);layout->addRow(inherit_);
    ratio_=new QDoubleSpinBox(this);ratio_->setDecimals(3);ratio_->setRange(0,1000);ratio_->setSpecialValueText(tr("Unlimited"));
    minutes_=new QSpinBox(this);minutes_->setRange(0,315360000);minutes_->setSpecialValueText(tr("Unlimited"));minutes_->setSuffix(tr(" s"));
    mode_=new QComboBox(this);mode_->addItems({tr("Stop when any condition is reached"),tr("Stop when all enabled conditions are reached")});
    layout->addRow(tr("Share ratio"),ratio_);layout->addRow(tr("Active seeding time"),minutes_);layout->addRow(mode_);
    connect(inherit_,&QCheckBox::toggled,this,[this](bool inherit){ratio_->setEnabled(!inherit);minutes_->setEnabled(!inherit);mode_->setEnabled(!inherit);});
    setValue(allowInherit?std::nullopt:std::optional<rats::service::SeedingPolicy>(rats::service::SeedingPolicy{}));
    texts->bind();
}
void SeedingPolicyWidget::setValue(std::optional<rats::service::SeedingPolicy> p) {
    inherit_->setChecked(!p);const auto value=p.value_or(rats::service::SeedingPolicy{});
    ratio_->setValue(value.ratioPermille/1000.0);minutes_->setValue(int(value.durationSeconds));mode_->setCurrentIndex(value.all?1:0);
}
std::optional<rats::service::SeedingPolicy> SeedingPolicyWidget::value() const {
    if(inherit_->isChecked())return std::nullopt;
    return rats::service::SeedingPolicy{qRound64(ratio_->value()*1000),qint64(minutes_->value()),mode_->currentIndex()==1};
}
DownloadScheduleWidget::DownloadScheduleWidget(QWidget* parent):QWidget(parent) {
    auto* texts=new DownloadTextBinder(this,"DownloadScheduleWidget");auto tr=[texts](const char* source){return texts->text(source);};
    auto* layout=new QVBoxLayout(this);layout->setContentsMargins(0,0,0,0);
    enabled_=new QCheckBox(tr("Enable weekly bandwidth schedule"),this);zone_=new QLineEdit("UTC",this);zone_->setPlaceholderText(tr("IANA time zone, e.g. Asia/Tokyo"));
    layout->addWidget(enabled_);auto* form=new QFormLayout;form->addRow(tr("Time zone"),zone_);layout->addLayout(form);
    table_=new QTableWidget(0,6,this);table_->setHorizontalHeaderLabels({tr("Days"),tr("From"),tr("Until"),tr("Download KiB/s"),tr("Upload KiB/s"),tr("Pause content")});
    table_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);table_->setMinimumHeight(150);layout->addWidget(table_);
    auto* row=new QHBoxLayout;auto* add=new QPushButton(tr("Add interval"),this);auto* remove=new QPushButton(tr("Remove interval"),this);row->addWidget(add);row->addWidget(remove);layout->addLayout(row);
    connect(add,&QPushButton::clicked,this,[this]{if(table_->rowCount()<64)addRule({});});
    connect(remove,&QPushButton::clicked,this,[this]{if(table_->currentRow()>=0)table_->removeRow(table_->currentRow());});
    auto* note=new QLabel(tr("Intervals use local wall time and may cross midnight. End 00:00 means midnight. Overlaps are rejected. Zero adds no rate cap; global and task caps still apply. DST repeated minutes use the same rule."),this);note->setWordWrap(true);layout->addWidget(note);
    texts->bind();
}
void DownloadScheduleWidget::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if(event->type()==QEvent::LanguageChange && table_)setValue(value());
}
void DownloadScheduleWidget::addRule(const rats::service::ScheduleRule& r) {
    const int row=table_->rowCount();table_->insertRow(row);
    auto* days=new QToolButton(table_);days->setPopupMode(QToolButton::InstantPopup);auto* menu=new QMenu(days);days->setMenu(menu);days->setProperty("days",r.days);
    const QStringList names={tr("Mon"),tr("Tue"),tr("Wed"),tr("Thu"),tr("Fri"),tr("Sat"),tr("Sun")};
    auto update=[days,names]{const int mask=days->property("days").toInt();QStringList selected;for(int d=0;d<7;++d)if(mask&(1<<d))selected<<names[d];days->setText(selected.join(' '));};
    for(int d=0;d<7;++d){auto* action=menu->addAction(names[d]);action->setCheckable(true);action->setChecked(r.days&(1<<d));connect(action,&QAction::toggled,days,[days,d,update](bool checked){int mask=days->property("days").toInt();days->setProperty("days",checked?(mask|(1<<d)):(mask&~(1<<d)));update();});}update();table_->setCellWidget(row,0,days);
    for(int column=1;column<=2;++column){const int minute=(column==1?r.startMinute:r.endMinute)%1440;auto* time=new QTimeEdit(QTime(minute/60,minute%60),table_);time->setDisplayFormat("HH:mm");table_->setCellWidget(row,column,time);}
    for(int column=3;column<=4;++column){auto* rate=new QDoubleSpinBox(table_);rate->setDecimals(6);rate->setRange(0,8796093022207.0);rate->setValue((column==3?r.downloadRate:r.uploadRate)/1024.0);table_->setCellWidget(row,column,rate);}
    auto* pause=new QCheckBox(table_);pause->setChecked(r.pause);table_->setCellWidget(row,5,pause);
}
void DownloadScheduleWidget::setValue(const rats::service::BandwidthSchedule& p) {enabled_->setChecked(p.enabled);zone_->setText(p.timeZone);table_->setRowCount(0);for(const auto& r:p.rules)addRule(r);}
rats::service::BandwidthSchedule DownloadScheduleWidget::value() const {
    rats::service::BandwidthSchedule p;p.enabled=enabled_->isChecked();p.timeZone=zone_->text().trimmed();
    for(int row=0;row<table_->rowCount();++row){rats::service::ScheduleRule r;r.days=table_->cellWidget(row,0)->property("days").toInt();
        auto time=[&](int col){const auto t=qobject_cast<QTimeEdit*>(table_->cellWidget(row,col))->time();return t.hour()*60+t.minute();};
        r.startMinute=time(1);r.endMinute=time(2);if(!r.endMinute)r.endMinute=1440;
        r.downloadRate=qRound64(qobject_cast<QDoubleSpinBox*>(table_->cellWidget(row,3))->value()*1024);r.uploadRate=qRound64(qobject_cast<QDoubleSpinBox*>(table_->cellWidget(row,4))->value()*1024);
        r.pause=qobject_cast<QCheckBox*>(table_->cellWidget(row,5))->isChecked();p.rules.append(r);
    }return p;
}
PieceMapWidget::PieceMapWidget(QWidget* parent):QWidget(parent){
    auto* texts=new DownloadTextBinder(this,"PieceMapWidget");auto tr=[texts](const char* source){return texts->text(source);};
    setMinimumHeight(60);setAccessibleName(tr("Piece map"));setToolTip(tr("Green: verified; blue: active; gray: wanted; dark: skipped. Each cell may aggregate multiple pieces."));texts->bind();
}
void PieceMapWidget::setRanges(const QJsonObject& map){map_=map;update();}
void PieceMapWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);const auto ranges=map_["ranges"].toArray();if(ranges.isEmpty()){p.drawText(rect(),Qt::AlignCenter,tr("Piece map is available after metadata and storage startup"));return;}
    const int columns=qMax(1,width()/12),rows=(ranges.size()+columns-1)/columns;const double h=double(height())/rows;
    for(int i=0;i<ranges.size();++i){const auto r=ranges[i].toObject();const auto count=r["end"].toInteger()-r["begin"].toInteger();
        const QRectF cell((i%columns)*width()/double(columns),(i/columns)*h,width()/double(columns)-1,h-1);
        p.fillRect(cell,r["active"].toInteger()>0?QColor("#3084cc"):r["wanted"].toInteger()>0?QColor("#999999"):QColor("#444444"));
        if(count>0){auto verified=cell;verified.setWidth(cell.width()*r["verified"].toDouble()/count);p.fillRect(verified,QColor("#35995a"));}
    }
}
