#include <algorithm>
#include <limits>
#include "downloadswidget.h"
#include "taskdownloaddialog.h"
#include <QMessageBox>
#include "app/application.h"
#include "app/favorites_store.h"
#include "domain/torrent.h"
#include "format.h"
#include "services/download_service.h"
#include "services/search_service.h"
#include <QDesktopServices>
#include <QDir>
#include <QJsonArray>
#include <QJsonObject>
#include <QScrollArea>
#include <QStyle>
#include <QUrl>

// DownloadItemWidget implementation

DownloadItemWidget::DownloadItemWidget(const QString& hash, const QString& name, qint64 size, QWidget* parent)
    : QWidget(parent)
    , hash_(hash)
    , nameLabel_(nullptr)
    , statusLabel_(nullptr)
    , speedLabel_(nullptr)
    , progressBar_(nullptr)
    , pauseButton_(nullptr)
    , cancelButton_(nullptr)
{
    setupUi(name, size);
}

void DownloadItemWidget::setupUi(const QString& name, qint64 size)
{
    setObjectName("downloadItemWidget");
    setMinimumHeight(100);

    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(16, 12, 16, 12);
    mainLayout->setSpacing(8);

    // Top row: name and size
    QHBoxLayout* topRow = new QHBoxLayout();

    nameLabel_ = new QLabel(name, this);
    nameLabel_->setObjectName("downloadItemName");
    nameLabel_->setWordWrap(true);
    topRow->addWidget(nameLabel_, 1);

    sizeLabel_ = new QLabel(rats::ui::formatSize(size), this);
    sizeLabel_->setObjectName("downloadItemSize");
    topRow->addWidget(sizeLabel_);

    mainLayout->addLayout(topRow);

    // Middle row: progress bar
    progressBar_ = new QProgressBar(this);
    progressBar_->setMinimum(0);
    progressBar_->setMaximum(100);
    progressBar_->setValue(0);
    progressBar_->setTextVisible(true);
    progressBar_->setObjectName("downloadProgress");
    mainLayout->addWidget(progressBar_);

    // Bottom row: status, speed, and buttons
    QHBoxLayout* bottomRow = new QHBoxLayout();

    statusLabel_ = new QLabel(tr("Waiting..."), this);
    statusLabel_->setObjectName("downloadItemStatus");
    bottomRow->addWidget(statusLabel_);

    speedLabel_ = new QLabel(this);
    speedLabel_->setObjectName("downloadItemSpeed");
    bottomRow->addWidget(speedLabel_);

    bottomRow->addStretch();

    favoriteButton_ = new QPushButton(tr("⭐ Favorite"), this);
    favoriteButton_->setObjectName("secondaryButton");
    connect(favoriteButton_, &QPushButton::clicked, this, [this]() { emit favoriteRequested(hash_); });
    bottomRow->addWidget(favoriteButton_);

    pauseButton_ = new QPushButton(tr("Pause"), this);
    pauseButton_->setObjectName("secondaryButton");
    connect(pauseButton_, &QPushButton::clicked, this, [this]() { emit pauseToggled(hash_); });
    bottomRow->addWidget(pauseButton_);
    auto* properties=new QPushButton(tr("Settings"),this);
    connect(properties,&QPushButton::clicked,this,[this]{emit policyRequested(hash_);});
    bottomRow->addWidget(properties);
    auto* first=new QPushButton(tr("Queue first"),this);
    connect(first,&QPushButton::clicked,this,[this]{emit queueFirstRequested(hash_);});
    bottomRow->addWidget(first);

    cancelButton_ = new QPushButton(tr("Cancel"), this);
    cancelButton_->setObjectName("dangerButton");
    connect(cancelButton_, &QPushButton::clicked, this, [this]() { emit cancelRequested(hash_); });
    bottomRow->addWidget(cancelButton_);

    mainLayout->addLayout(bottomRow);
}

void DownloadItemWidget::updateProgress(qint64 downloaded, qint64 total, int speed, double progress)
{
    // A completed torrent keeps its "Completed" presentation; a routine progress
    // tick must not rewrite the status back to "size / size" with an active look.
    if (completed_) {
        return;
    }

    progressBar_->setValue(static_cast<int>(progress * 100));

    QString status = QString("%1 / %2").arg(rats::ui::formatSize(downloaded), rats::ui::formatSize(total));
    if (total > 0 && speed > 0) {
        qint64 remaining = (total - downloaded) / speed;
        status += QString(" - %1 remaining").arg(formatTime(remaining));
    }
    statusLabel_->setText(status);

    if (speed > 0) {
        speedLabel_->setText(rats::ui::formatSpeed(speed));
    } else {
        speedLabel_->clear();
    }
}

void DownloadItemWidget::updateInfo(const QString& name, qint64 size)
{
    if (!name.isEmpty())
        nameLabel_->setText(name);
    if (size > 0)
        sizeLabel_->setText(rats::ui::formatSize(size));
}

void DownloadItemWidget::setCompleted()
{
    if (completed_) {
        return; // already presented as completed — keep it sticky and idempotent
    }
    completed_ = true;

    progressBar_->setValue(100);
    progressBar_->setObjectName("downloadProgress"); // clear any "paused" style
    progressBar_->style()->unpolish(progressBar_);
    progressBar_->style()->polish(progressBar_);
    statusLabel_->setText(tr("Completed"));
    speedLabel_->clear();
    pauseButton_->setText(tr("Open"));
    pauseButton_->setObjectName("successButton");
    pauseButton_->style()->unpolish(pauseButton_);
    pauseButton_->style()->polish(pauseButton_);
    disconnect(pauseButton_, &QPushButton::clicked, nullptr, nullptr);
    connect(pauseButton_, &QPushButton::clicked, this, [this]() { emit openRequested(hash_); });

    // A completed torrent can no longer be "cancelled", but it can still be
    // removed from the list (and stops seeding); relabel rather than hide so the
    // action stays reachable.
    cancelButton_->setText(tr("Remove"));
}

void DownloadItemWidget::applyProgress(const QJsonObject& snapshot)
{
    const bool complete = snapshot["completed"].toBool();
    if (!complete && completed_) {
        completed_ = false;
        disconnect(pauseButton_, &QPushButton::clicked, nullptr, nullptr);
        connect(pauseButton_, &QPushButton::clicked, this, [this] { emit pauseToggled(hash_); });
        pauseButton_->setObjectName("secondaryButton");
        pauseButton_->style()->unpolish(pauseButton_);
        pauseButton_->style()->polish(pauseButton_);
        cancelButton_->setText(tr("Cancel"));
    }
    const auto error = snapshot["error"].toObject();
    pauseButton_->setEnabled(error["code"].toString().isEmpty());
    if (complete) setCompleted();
    const qint64 verified = snapshot.contains("wantedVerifiedBytes") ? snapshot["wantedVerifiedBytes"].toVariant().toLongLong()
        : snapshot["downloaded"].toVariant().toLongLong();
    const qint64 total = snapshot.contains("wantedBytes") ? snapshot["wantedBytes"].toVariant().toLongLong()
        : snapshot["totalSize"].toVariant().toLongLong();
    const double rate = snapshot.contains("verifiedSpeed") ? snapshot["verifiedSpeed"].toDouble() : snapshot["downloadSpeed"].toDouble();
    updateProgress(verified, total, static_cast<int>(std::clamp(rate, 0.0, double(std::numeric_limits<int>::max()))), snapshot["progress"].toDouble());
    setPaused(snapshot["paused"].toBool());
    const auto phase = snapshot["phase"].toString();
    if (!error["code"].toString().isEmpty())
        statusLabel_->setText(tr("Disk error: %1 (%2)").arg(error["message"].toString(), error["code"].toString()));
    else if (phase == "checking") statusLabel_->setText(tr("Checking files"));
    else if (phase == "waiting" && !snapshot["paused"].toBool() && !snapshot["enginePaused"].toBool()) statusLabel_->setText(tr("Waiting to start"));
    else if (phase == "metadata") statusLabel_->setText(tr("Fetching metadata"));
    if(snapshot["schedulePaused"].toBool() && !snapshot["paused"].toBool() && error["code"].toString().isEmpty())
        statusLabel_->setText(tr("Queued: %1").arg(snapshot["queueReason"].toString()));
    if(snapshot["selectedComplete"].toBool() && !complete)statusLabel_->setText(tr("Selected files complete (partial seeding)"));
    if(!snapshot["selectionConfirmed"].toBool() && phase!="metadata")statusLabel_->setText(tr("Confirm files in task settings"));
    if(snapshot["seedingStopped"].toBool())statusLabel_->setText(tr("Seeding goal reached"));
    else if(snapshot["queueReason"].toString()=="timeSchedule")statusLabel_->setText(tr("Paused by bandwidth schedule"));
    const auto effective=snapshot["effectivePolicy"].toObject();
    auto limit=[](QJsonValue value){ return value.toInteger()==0?QString("∞"):rats::ui::formatSize(value.toInteger())+"/s"; };
    speedLabel_->setText(tr("↓ %1/s  ↑ %2/s").arg(rats::ui::formatSize(qint64(rate)),
        rats::ui::formatSize(snapshot["uploadSpeed"].toInteger())));
    speedLabel_->setToolTip(tr("Effective limits: ↓ %1  ↑ %2").arg(limit(effective["downloadLimitBps"]),limit(effective["uploadLimitBps"])));
    statusLabel_->setToolTip(tr("Received: %1\nVerified: %2\nError path: %3")
        .arg(rats::ui::formatSize(snapshot["receivedPayloadBytes"].toVariant().toLongLong()),
             rats::ui::formatSize(verified), error["path"].toString()));
}

void DownloadItemWidget::setPaused(bool paused)
{
    if (completed_) {
        return; // a completed torrent is not a pausable download
    }

    if (paused) {
        pauseButton_->setText(tr("Resume"));
        statusLabel_->setText(tr("Paused"));
        progressBar_->setObjectName("downloadProgressPaused");
    } else {
        pauseButton_->setText(tr("Pause"));
        progressBar_->setObjectName("downloadProgress");
    }
    progressBar_->style()->unpolish(progressBar_);
    progressBar_->style()->polish(progressBar_);
}

void DownloadItemWidget::setFavorite(bool favorite)
{
    if (favorite) {
        favoriteButton_->setText(tr("★ Favorited"));
        favoriteButton_->setObjectName("warningButton");
    } else {
        favoriteButton_->setText(tr("⭐ Favorite"));
        favoriteButton_->setObjectName("secondaryButton");
    }
    favoriteButton_->style()->unpolish(favoriteButton_);
    favoriteButton_->style()->polish(favoriteButton_);
}

QString DownloadItemWidget::formatTime(qint64 seconds) const
{
    if (seconds < 60)
        return QString::number(seconds) + "s";
    if (seconds < 3600)
        return QString("%1m %2s").arg(seconds / 60).arg(seconds % 60);
    return QString("%1h %2m").arg(seconds / 3600).arg((seconds % 3600) / 60);
}

// DownloadsWidget implementation

DownloadsWidget::DownloadsWidget(QWidget* parent)
    : QWidget(parent)
    , app_(nullptr)
    , listLayout_(nullptr)
    , emptyLabel_(nullptr)
    , statusLabel_(nullptr)
    , listContainer_(nullptr)
{
    setupUi();
}

DownloadsWidget::~DownloadsWidget() { }

void DownloadsWidget::setupUi()
{
    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    // Header
    QWidget* headerRow = new QWidget(this);
    headerRow->setObjectName("headerRow");
    QHBoxLayout* headerLayout = new QHBoxLayout(headerRow);
    headerLayout->setContentsMargins(16, 12, 16, 12);

    QLabel* titleLabel = new QLabel(tr("📥 Downloads"), this);
    titleLabel->setObjectName("headerLabel");
    headerLayout->addWidget(titleLabel);

    headerLayout->addStretch();

    statusLabel_ = new QLabel(this);
    statusLabel_->setObjectName("statusLabel");
    headerLayout->addWidget(statusLabel_);

    mainLayout->addWidget(headerRow);

    // Empty state
    emptyLabel_ = new QLabel(tr("No active downloads.\nStart downloading "
                                "torrents from the search results!"),
        this);
    emptyLabel_->setAlignment(Qt::AlignCenter);
    emptyLabel_->setObjectName("emptyStateLabel");
    mainLayout->addWidget(emptyLabel_);

    // Scroll area for downloads list
    QScrollArea* scrollArea = new QScrollArea(this);
    scrollArea->setWidgetResizable(true);
    scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scrollArea->setObjectName("downloadsScrollArea");

    listContainer_ = new QWidget(this);
    listContainer_->setObjectName("downloadsListContainer");
    listLayout_ = new QVBoxLayout(listContainer_);
    listLayout_->setContentsMargins(12, 12, 12, 12);
    listLayout_->setSpacing(8);
    listLayout_->addStretch();

    scrollArea->setWidget(listContainer_);
    listContainer_->hide();
    mainLayout->addWidget(scrollArea, 1);
}

void DownloadsWidget::setApplication(rats::app::Application* app)
{
    app_ = app;

    if (app_ && app_->downloads()) {
        rats::service::DownloadService* downloads = app_->downloads();
        connect(downloads, &rats::service::DownloadService::downloadStarted, this, &DownloadsWidget::onDownloadStarted);
        connect(downloads, &rats::service::DownloadService::progressUpdated, this, &DownloadsWidget::onProgressUpdated);
        connect(
            downloads, &rats::service::DownloadService::downloadCompleted, this, &DownloadsWidget::onDownloadCompleted);
        connect(
            downloads, &rats::service::DownloadService::torrentRemoved, this, &DownloadsWidget::onDownloadCancelled);
        connect(downloads,&rats::service::DownloadService::selectionRequired,this,[this](const QString& hash){
            auto* dialog=new TaskDownloadDialog(app_->downloads(),hash,this);dialog->setAttribute(Qt::WA_DeleteOnClose);dialog->open();
        });
        loadDownloads();
    }

    if (app_ && app_->favorites()) {
        connect(
            app_->favorites(), &rats::app::FavoritesStore::favoritesChanged, this, &DownloadsWidget::refreshFavorites);
    }
}

void DownloadsWidget::loadDownloads()
{
    if (!app_ || !app_->downloads())
        return;

    // Clear existing items
    for (auto it = downloadItems_.begin(); it != downloadItems_.end(); ++it) {
        (*it)->deleteLater();
    }
    downloadItems_.clear();

    // Load current downloads
    const QVector<rats::service::Download> downloads = app_->downloads()->allDownloads();
    for (const rats::service::Download& dl : downloads) {
        addDownloadItem(dl.hash, dl.name, dl.totalSize);

        downloadItems_[dl.hash]->applyProgress(dl.toJson());

        if (dl.completed) {
            downloadItems_[dl.hash]->setCompleted();
        } else if (dl.paused) {
            downloadItems_[dl.hash]->setPaused(true);
        }
    }

    if (downloadItems_.isEmpty()) {
        emptyLabel_->show();
        listContainer_->hide();
        statusLabel_->clear();
    } else {
        emptyLabel_->hide();
        listContainer_->show();
        statusLabel_->setText(tr("%n download(s)", nullptr, static_cast<int>(downloadItems_.size())));
    }
}

void DownloadsWidget::addDownloadItem(const QString& hash, const QString& name, qint64 size)
{
    if (downloadItems_.contains(hash))
        return;

    DownloadItemWidget* item = new DownloadItemWidget(hash, name, size, this);

    connect(item, &DownloadItemWidget::pauseToggled, this, &DownloadsWidget::onPauseToggled);
    connect(item, &DownloadItemWidget::cancelRequested, this, &DownloadsWidget::onCancelRequested);
    connect(item, &DownloadItemWidget::openRequested, this, &DownloadsWidget::onOpenRequested);
    connect(item, &DownloadItemWidget::favoriteRequested, this, &DownloadsWidget::onFavoriteRequested);
    connect(item,&DownloadItemWidget::policyRequested,this,[this](const QString& hash){
        TaskDownloadDialog dialog(app_->downloads(),hash,this); dialog.exec();
    });
    connect(item,&DownloadItemWidget::queueFirstRequested,this,[this](const QString& hash){
        auto tasks=app_->downloads()->allDownloads();
        std::sort(tasks.begin(),tasks.end(),[](const auto& a,const auto& b){return a.queuePosition<b.queuePosition;});
        if(tasks.isEmpty() || tasks.front().hash==hash) return;
        const auto r=app_->downloads()->moveQueueItem(hash,tasks.front().hash,app_->downloads()->globalPolicy().revision);
        if(r.outcome!=rats::OperationResult::Outcome::Applied) QMessageBox::warning(this,tr("Queue"),r.error.message);
    });

    if (app_ && app_->favorites())
        item->setFavorite(app_->favorites()->isFavorite(hash));

    // Insert before the stretch
    listLayout_->insertWidget(listLayout_->count() - 1, item);
    downloadItems_[hash] = item;

    emptyLabel_->hide();
    listContainer_->show();
    statusLabel_->setText(tr("%n download(s)", nullptr, static_cast<int>(downloadItems_.size())));
}

void DownloadsWidget::removeDownloadItem(const QString& hash)
{
    if (!downloadItems_.contains(hash))
        return;

    DownloadItemWidget* item = downloadItems_.take(hash);
    listLayout_->removeWidget(item);
    item->deleteLater();

    if (downloadItems_.isEmpty()) {
        emptyLabel_->show();
        listContainer_->hide();
        statusLabel_->clear();
    } else {
        statusLabel_->setText(tr("%n download(s)", nullptr, static_cast<int>(downloadItems_.size())));
    }
}

void DownloadsWidget::onDownloadStarted(const QString& hash)
{
    if (!app_ || !app_->downloads())
        return;

    const rats::service::Download info = app_->downloads()->getDownload(hash);
    if (info.hash.isEmpty())
        return;

    // downloadStarted fires twice: once on add() (name is the info-hash
    // placeholder, size 0) and again when a magnet's metadata arrives (real name
    // and size known). Refresh the existing item on the second call instead of
    // returning early, otherwise it keeps showing the hash as its name.
    if (downloadItems_.contains(hash))
        downloadItems_[hash]->updateInfo(info.name, info.totalSize);
    else
        addDownloadItem(hash, info.name, info.totalSize);
}

void DownloadsWidget::onProgressUpdated(const QString& hash, const QJsonObject& progress)
{
    if (!downloadItems_.contains(hash)) {
        return;
    }
    downloadItems_[hash]->applyProgress(progress);
}

void DownloadsWidget::onDownloadCompleted(const QString& hash)
{
    if (downloadItems_.contains(hash)) {
        downloadItems_[hash]->setCompleted();
    }
}

void DownloadsWidget::onDownloadCancelled(const QString& hash)
{
    removeDownloadItem(hash);
}

void DownloadsWidget::onPauseToggled(const QString& hash)
{
    if (!app_ || !app_->downloads())
        return;

    rats::service::DownloadService* downloads = app_->downloads();
    downloads->togglePause(hash);

    const rats::service::Download info = downloads->getDownload(hash);
    if (downloadItems_.contains(hash)) {
        downloadItems_[hash]->setPaused(info.paused);
    }
}

void DownloadsWidget::onCancelRequested(const QString& hash)
{
    if (app_ && app_->downloads()) {
        app_->downloads()->remove(hash);
    }
}

void DownloadsWidget::onOpenRequested(const QString& hash)
{
    if (!app_ || !app_->downloads())
        return;

    const QString savePath = app_->downloads()->getDownload(hash).savePath;
    if (savePath.isEmpty() || !QDir(savePath).exists()) {
        statusLabel_->setText(tr("Download folder not found"));
        return;
    }
    QDesktopServices::openUrl(QUrl::fromLocalFile(savePath));
}

void DownloadsWidget::onFavoriteRequested(const QString& hash)
{
    if (!app_ || !app_->favorites() || !app_->downloads())
        return;

    rats::app::FavoritesStore* fav = app_->favorites();
    if (fav->isFavorite(hash)) {
        fav->remove(hash);
        return;
    }

    // Prefer the fully indexed torrent (poster, votes, trackers). Fall back to
    // the live download metadata when it hasn't been indexed.
    rats::domain::Torrent torrent;
    if (app_->search()) {
        if (auto indexed = app_->search()->get(hash))
            torrent = *indexed;
    }

    if (!torrent.isValid()) {
        const rats::service::Download dl = app_->downloads()->getDownload(hash);
        torrent.hash = dl.hash;
        torrent.name = dl.name;
        torrent.size = dl.totalSize;
        torrent.files = dl.files.size();
        for (const rats::service::DownloadFile& f : dl.files)
            torrent.fileList.append({ f.path, f.size });
    }

    fav->add(torrent);
    // The button refresh is driven by FavoritesStore::favoritesChanged →
    // refreshFavorites(), so no direct setFavorite() call is needed here.
}

void DownloadsWidget::refreshFavorites()
{
    if (!app_ || !app_->favorites())
        return;

    for (auto it = downloadItems_.begin(); it != downloadItems_.end(); ++it) {
        (*it)->setFavorite(app_->favorites()->isFavorite(it.key()));
    }
}
