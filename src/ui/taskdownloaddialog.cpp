#include "taskdownloaddialog.h"
#include "downloadadvancedwidgets.h"
#include "downloadtextbinder.h"
#include <QCheckBox>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QTimer>
#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QMessageBox>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QPushButton>
#include <QLineEdit>

TaskDownloadDialog::TaskDownloadDialog(rats::service::DownloadService* service,const QString& hash,QWidget* parent)
    :QDialog(parent),service_(service),hash_(hash),policy_(service->getDownload(hash).policy) {
    auto* texts=new DownloadTextBinder(this,"TaskDownloadDialog");auto tr=[texts](const char* source){return texts->text(source);};
    setWindowTitle(tr("Task download settings")); resize(450,300);
    auto* outer=new QVBoxLayout(this);auto* scroll=new QScrollArea(this);scroll->setWidgetResizable(true);
    auto* body=new QWidget(scroll);auto* layout=new QFormLayout(body);scroll->setWidget(body);outer->addWidget(scroll);
    auto rate=[&] { auto* spin=new QDoubleSpinBox(this); spin->setDecimals(6); spin->setRange(-1,8796093022207.0);
        spin->setSuffix(" KiB/s"); spin->setSpecialValueText(tr("Inherit")); return spin; };
    down_=rate(); up_=rate();
    down_->setValue(policy_.downloadRate?*policy_.downloadRate/1024.0:-1);
    up_->setValue(policy_.uploadRate?*policy_.uploadRate/1024.0:-1);
    peers_=new QSpinBox(this); peers_->setRange(0,10000); peers_->setSpecialValueText(tr("Inherit"));
    peers_->setValue(policy_.maxConnections.value_or(0));
    priority_=new QComboBox(this); priority_->addItems({tr("Low"),tr("Normal"),tr("High")}); priority_->setCurrentIndex(policy_.priority+1);
    layout->addRow(tr("Download limit"),down_); layout->addRow(tr("Upload limit"),up_);
    layout->addRow(tr("BT connections"),peers_); layout->addRow(tr("Queue priority"),priority_);
    strategy_=new QComboBox(this);strategy_->addItems({tr("Rarest first"),tr("Sequential"),tr("Selected file first/last pieces")});
    strategy_->setCurrentIndex(policy_.strategy=="sequential"?1:policy_.strategy=="firstLast"?2:0);layout->addRow(tr("Download strategy"),strategy_);
    autoWindow_=new QCheckBox(tr("Automatically adapt request window"),this);autoWindow_->setChecked(policy_.automaticWindow);layout->addRow(autoWindow_);
    auto window=[this](int value){auto* spin=new QSpinBox(this);spin->setRange(1,128);spin->setValue(value);return spin;};
    manualWindow_=window(policy_.requestWindow);minWindow_=window(policy_.windowMin);maxWindow_=window(policy_.windowMax);
    layout->addRow(tr("Manual requests per peer"),manualWindow_);layout->addRow(tr("Automatic minimum"),minWindow_);layout->addRow(tr("Automatic maximum"),maxWindow_);
    seeding_=new SeedingPolicyWidget(true,this);seeding_->setValue(policy_.seeding);layout->addRow(tr("Seeding conditions"),seeding_);
    auto* map=new PieceMapWidget(this);layout->addRow(map);map->setRanges(service_->pieceMap(hash_));
    auto* timer=new QTimer(this);timer->setInterval(3000);connect(timer,&QTimer::timeout,this,[this,map]{if(isVisible())map->setRanges(service_->pieceMap(hash_));});timer->start();
    auto* resetSeed=new QPushButton(tr("Restart seeding after goal"),this);layout->addRow(resetSeed);
    connect(resetSeed,&QPushButton::clicked,this,[this]{service_->resume(hash_);});
    auto* note=new QLabel(tr("-1 inherits; 0 removes the task cap. Global caps always apply. Confirm files before downloading content. Skipped boundary data is kept in .parts for piece verification."));
    note->setWordWrap(true); layout->addRow(note);
    const auto download=service_->getDownload(hash_);selectionRevision_=download.selectionRevision;
    files_=new QTreeWidget(this);files_->setHeaderLabels({tr("File"),tr("Priority"),tr("Verified")});
    QMap<QString,QTreeWidgetItem*> folders;
    for(const auto& f:download.files) {
        const auto parts=f.path.split('/');QString path;QTreeWidgetItem* parent=nullptr;
        for(int i=0;i<parts.size()-1;++i) {path+=parts[i]+"/";if(!folders.contains(path)) {
            auto* folder=parent?new QTreeWidgetItem(parent):new QTreeWidgetItem(files_);folder->setText(0,parts[i]);
            folder->setFlags(folder->flags()|Qt::ItemIsUserCheckable|Qt::ItemIsAutoTristate);folder->setCheckState(0,Qt::Checked);folders[path]=folder;}parent=folders[path];}
        auto* item=parent?new QTreeWidgetItem(parent):new QTreeWidgetItem(files_);
        item->setText(0,parts.last());item->setData(0,Qt::UserRole,f.index);item->setToolTip(0,f.path);
        item->setFlags(item->flags()|Qt::ItemIsUserCheckable);item->setCheckState(0,f.selected?Qt::Checked:Qt::Unchecked);
        auto* priority=new QComboBox(files_);priority->addItems({tr("Normal"),tr("High")});priority->setCurrentIndex(f.priority==2?1:0);files_->setItemWidget(item,1,priority);
        item->setText(2,QString::number(f.progress*100,'f',1)+"%");
    }
    auto* filter=new QLineEdit(this);filter->setPlaceholderText(tr("Filter files by name or extension"));layout->addRow(filter);
    connect(filter,&QLineEdit::textChanged,this,[this](const QString& value){for(QTreeWidgetItemIterator it(files_);*it;++it) if((*it)->childCount()==0)(*it)->setHidden(!(*it)->toolTip(0).contains(value,Qt::CaseInsensitive));});
    auto* selectMatching=new QPushButton(tr("Select matching files"),this);auto* skipMatching=new QPushButton(tr("Skip matching files"),this);
    layout->addRow(selectMatching,skipMatching);
    for(auto* button:{selectMatching,skipMatching})connect(button,&QPushButton::clicked,this,[this,button,selectMatching]{
        for(QTreeWidgetItemIterator it(files_);*it;++it)if((*it)->data(0,Qt::UserRole).isValid() && !(*it)->isHidden())(*it)->setCheckState(0,button==selectMatching?Qt::Checked:Qt::Unchecked);
    });
    layout->addRow(files_);files_->expandAll();resize(720,640);
    auto* apply=new QPushButton(tr("Confirm file selection"),this);apply->setEnabled(download.ready);layout->addRow(apply);
    connect(apply,&QPushButton::clicked,this,[this,apply,tr]{
        QVector<int> priorities(service_->getDownload(hash_).files.size(),0);
        for(QTreeWidgetItemIterator it(files_);*it;++it) {const auto index=(*it)->data(0,Qt::UserRole);if(!index.isValid())continue;
            auto* combo=qobject_cast<QComboBox*>(files_->itemWidget(*it,1));priorities[index.toInt()]=(*it)->checkState(0)==Qt::Checked?(combo->currentIndex()+1):0;}
        const auto result=service_->setFilePriorities(hash_,priorities,selectionRevision_);
        if(result.outcome==rats::OperationResult::Outcome::Accepted)apply->setEnabled(false);else QMessageBox::warning(this,tr("File selection"),result.error.message);
    });
    connect(service_,&rats::service::DownloadService::operationFinished,this,[this,apply,tr](const QString& id,const QJsonObject& value){
        const auto result=rats::OperationResult::fromJson(value);if(result.hash!=hash_)return;apply->setEnabled(true);
        if(result.applied())selectionRevision_=result.revision;else QMessageBox::warning(this,tr("File selection"),result.error.message);
        if(id==pendingSaveOperation_) {pendingSaveOperation_.clear();setEnabled(true);if(result.applied())submitPolicy();}
    });
    connect(service_,&rats::service::DownloadService::filesChanged,this,[this](const QString& hash,const QJsonArray& files){
        if(hash!=hash_)return;
        for(QTreeWidgetItemIterator it(files_);*it;++it) {const auto index=(*it)->data(0,Qt::UserRole);if(!index.isValid() || index.toInt()>=files.size())continue;
            (*it)->setText(2,QString::number(files[index.toInt()].toObject()["progress"].toDouble()*100,'f',1)+"%");}
    });
    auto* check=new QPushButton(tr("Recheck / retry files"),this);layout->addRow(check);
    connect(check,&QPushButton::clicked,this,[this,tr]{const auto r=service_->recheck(hash_,true);if(r.outcome!=rats::OperationResult::Outcome::Accepted)QMessageBox::warning(this,tr("Recheck"),r.error.message);});
    auto* buttons=new QDialogButtonBox(QDialogButtonBox::Save|QDialogButtonBox::Cancel,this); layout->addRow(buttons);
    connect(buttons,&QDialogButtonBox::accepted,this,[this]{submitPolicy();});
    connect(buttons,&QDialogButtonBox::rejected,this,&QDialog::reject);
    texts->bind();
}
QVector<int> TaskDownloadDialog::selectedPriorities() const {
    QVector<int> priorities(service_->getDownload(hash_).files.size(),0);
    for(QTreeWidgetItemIterator it(files_);*it;++it) {const auto index=(*it)->data(0,Qt::UserRole);if(!index.isValid())continue;
        auto* combo=qobject_cast<QComboBox*>(files_->itemWidget(*it,1));priorities[index.toInt()]=(*it)->checkState(0)==Qt::Checked?(combo->currentIndex()+1):0;}
    return priorities;
}
void TaskDownloadDialog::submitPolicy() {
    const auto current=service_->getDownload(hash_);const auto priorities=selectedPriorities();
    QVector<int> existing;for(const auto& file:current.files)existing.append(file.priority);
    // Save means save the whole task, including the visible file selection.
    if(!current.selectionConfirmed || priorities!=existing) {
        const auto result=service_->setFilePriorities(hash_,priorities,selectionRevision_);
        if(result.outcome==rats::OperationResult::Outcome::Accepted){pendingSaveOperation_=result.operationId;setEnabled(false);}
        else QMessageBox::warning(this,tr("File selection"),result.error.message);
        return;
    }
    auto next=policy_;
    next.downloadRate=down_->value()<0?std::nullopt:std::optional<qint64>(qRound64(down_->value()*1024));
    next.uploadRate=up_->value()<0?std::nullopt:std::optional<qint64>(qRound64(up_->value()*1024));
    next.maxConnections=peers_->value()?std::optional<int>(peers_->value()):std::nullopt;
    next.priority=priority_->currentIndex()-1;
    next.strategy=strategy_->currentIndex()==1?"sequential":strategy_->currentIndex()==2?"firstLast":"rarestFirst";
    next.automaticWindow=autoWindow_->isChecked();next.requestWindow=manualWindow_->value();next.windowMin=minWindow_->value();next.windowMax=maxWindow_->value();next.seeding=seeding_->value();
    const auto result=service_->setTaskPolicy(hash_,next,policy_.revision);
    if(result.outcome==rats::OperationResult::Outcome::Applied) accept();
    else { if(result.applied()) policy_=service_->getDownload(hash_).policy;
        QMessageBox::warning(this,tr("Task settings"),result.error.message); }
}
