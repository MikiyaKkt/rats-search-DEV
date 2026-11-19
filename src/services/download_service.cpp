#include "services/download_service.h"

#include "common/infohash.h"
#include "services/torrent_session_store.h"

#include <algorithm>
#include <QDebug>
#include <QDir>
#include <QMutexLocker>
#include <QStandardPaths>
#include <QUuid>
#include <QPointer>

namespace rats::service {

// ============================================================================
// DownloadFile / Download — JSON
// ============================================================================

QJsonObject DownloadFile::toJson() const
{
    QJsonObject obj;
    obj["path"] = path;
    obj["size"] = size;
    obj["index"] = index;
    obj["selected"] = selected;
    obj["priority"] = priority;
    obj["progress"] = progress;
    obj["verifiedBytes"] = verifiedBytes;
    return obj;
}

QJsonObject Download::toJson() const
{
    QJsonObject obj;
    obj["hash"] = hash;
    obj["name"] = name;
    obj["savePath"] = savePath;
    obj["totalSize"] = totalSize;
    obj["downloadedBytes"] = downloadedBytes;
    obj["progress"] = progress;
    obj["progressPending"] = progressPending;
    obj["verifiedBytes"] = verifiedBytes;
    obj["downloadSpeed"] = downloadSpeed;
    obj["receivedPayloadBytes"] = downloadedBytes;
    obj["verifiedSpeed"] = verifiedSpeed;
    obj["queuedUploadBytes"] = queuedUploadBytes;
    obj["uploadQueueSpeed"] = uploadSpeed;
    obj["phase"] = phaseName(phase);
    obj["enginePaused"] = enginePaused;
    obj["hashFailures"] = hashFailures;
    obj["error"] = error.toJson();
    obj["peersConnected"] = peersConnected;
    obj["paused"] = paused;
    obj["removeOnDone"] = removeOnDone;
    obj["ready"] = ready;
    obj["completed"] = completed;
    obj["fullTorrentComplete"]=completed;
    obj["selectionApplying"]=selectionApplying;
    obj["wantedBytes"]=wantedBytes; obj["wantedVerifiedBytes"]=wantedVerifiedBytes;
    obj["selectedComplete"]=selectedComplete; obj["selectionRevision"]=selectionRevision;
    obj["selectionConfirmed"]=selectionConfirmed;
    obj["taskPolicy"] = policy.toJson();
    obj["queuePosition"] = queuePosition;
    obj["schedulePaused"] = schedulePaused;
    obj["queueReason"] = queueReason;
    obj["seededMilliseconds"]=seededMilliseconds;obj["seedingUploadedBytes"]=seedingUploadedBytes;obj["seedingStopped"]=seedingStopped;
    obj["uploadedBytes"] = queuedUploadBytes;
    obj["uploadSpeed"] = uploadSpeed;

    QJsonArray filesArr;
    for (const DownloadFile& f : files) {
        filesArr.append(f.toJson());
    }
    obj["files"] = filesArr;
    return obj;
}

// ============================================================================
// Construction
// ============================================================================

DownloadService::DownloadService(net::TorrentEngine* engine, QObject* parent, Clock clock,UtcClock utcClock)
    : QObject(parent)
    , engine_(engine)
    , sessionStore_(std::make_unique<TorrentSessionStore>())
    , defaultDownloadPath_(QStandardPaths::writableLocation(QStandardPaths::DownloadLocation))
{
    utcClock_=utcClock?std::move(utcClock):UtcClock([]{return QDateTime::currentDateTimeUtc();});
    elapsed_.start();
    clock_ = clock ? std::move(clock) : Clock([this] { return elapsed_.elapsed(); });
    updateTimer_ = new QTimer(this);
    updateTimer_->setInterval(1000); // poll once per second
    connect(updateTimer_, &QTimer::timeout, this, &DownloadService::onUpdateTimer);
    // Safe to start now: the poll is a no-op until the engine is ready.
    updateTimer_->start();
}

DownloadService::~DownloadService()
{
    if (updateTimer_) {
        updateTimer_->stop();
    }

    // Persist resume data for every active download so downloaded pieces survive
    // a restart. librats keeps ownership of the torrents; we only ask it to save.
    const auto snapshot = allDownloads();
    if (engine_) for (const Download& d : snapshot) engine_->saveResumeData(d.hash);

    QMutexLocker lock(&mutex_);
    downloads_.clear();
}

bool DownloadService::isReady() const
{
    return engine_ && engine_->isReady();
}

// ============================================================================
// Download lifecycle
// ============================================================================

bool DownloadService::add(const QString& magnetLink, const QString& savePath)
{
    if (!isReady()) {
        qWarning() << "DownloadService: Not ready";
        return false;
    }

    QString hash = net::TorrentEngine::parseInfoHash(magnetLink);
    if (!infohash::isValid(hash)) {
        qWarning() << "DownloadService: Invalid magnet link or hash:" << magnetLink;
        return false;
    }
    hash = infohash::normalize(hash);

    if (contains(hash)) {
        qInfo() << "DownloadService: Already downloading:" << hash;
        return false;
    }

    const QString path = resolveSavePath(savePath);
    if (!ensureDir(path)) {
        qWarning() << "DownloadService: Cannot create download directory:" << path;
        return false;
    }

    qInfo() << "DownloadService: Adding torrent" << hash << "to" << path;
    if (!engine_->createDownload(hash, path, {net::TorrentEngine::StartMode::Inactive, false, true, false}).applied()) {
        qWarning() << "DownloadService: Failed to add magnet:" << hash;
        return false;
    }

    Download d;
    d.hash = hash;
    d.name = hash; // placeholder until metadata arrives
    d.savePath = path;
    d.ready = false;
    {
        QMutexLocker lock(&mutex_);
        d.queuePosition = nextQueuePosition_++;
        downloads_[hash] = d;
    }

    enginePolicyDirty_ = true;
    reconcileSchedule();

    emit downloadStarted(hash);
    return true;
}

bool DownloadService::addWithInfo(const domain::Torrent& info, const QString& savePath)
{
    if (!infohash::isValid(info.hash)) {
        qWarning() << "DownloadService: Invalid hash for download:" << info.hash;
        return false;
    }
    if (!isReady()) {
        qWarning() << "DownloadService: Not ready";
        return false;
    }

    const QString hash = infohash::normalize(info.hash);
    if (contains(hash)) {
        qInfo() << "DownloadService: Already downloading:" << hash;
        return false;
    }

    const QString path = resolveSavePath(savePath);
    if (!ensureDir(path)) {
        qWarning() << "DownloadService: Cannot create download directory:" << path;
        return false;
    }

    qInfo() << "DownloadService: Adding torrent with info" << hash << info.name.left(50);
    if (!engine_->createDownload(hash, path, {net::TorrentEngine::StartMode::Inactive, false, true, false}).applied()) {
        qWarning() << "DownloadService: Failed to add magnet:" << hash;
        return false;
    }

    Download d;
    d.hash = hash;
    d.name = info.name.isEmpty() ? hash : info.name;
    d.totalSize = info.size;
    d.savePath = path;
    d.ready = false; // metadata (authoritative name/files) not yet known
    d.completed = false;
    {
        QMutexLocker lock(&mutex_);
        d.queuePosition = nextQueuePosition_++;
        downloads_[hash] = d;
    }

    enginePolicyDirty_ = true;
    reconcileSchedule();

    emit downloadStarted(hash);
    return true;
}

bool DownloadService::addFromFile(const QString& torrentFile, const QString& savePath)
{
    if (!isReady()) {
        qWarning() << "DownloadService: Not ready";
        return false;
    }

    // Parse up front so we know the hash (for dedup) and the file list.
    net::TorrentMetadata meta = engine_->readTorrentFile(torrentFile);
    if (!meta.valid) {
        qWarning() << "DownloadService: Failed to parse torrent file:" << torrentFile;
        return false;
    }

    const QString hash = infohash::normalize(meta.hash);
    if (contains(hash)) {
        qInfo() << "DownloadService: Already downloading:" << hash;
        return false;
    }

    const QString path = resolveSavePath(savePath);
    if (!ensureDir(path)) {
        qWarning() << "DownloadService: Cannot create download directory:" << path;
        return false;
    }

    qInfo() << "DownloadService: Adding torrent file" << torrentFile << "to" << path;
    if (!engine_->createDownload(torrentFile, path, {net::TorrentEngine::StartMode::Inactive, false, true, true}).applied()) {
        qWarning() << "DownloadService: Failed to add torrent file:" << torrentFile;
        return false;
    }

    Download d;
    d.hash = hash;
    d.name = meta.name;
    d.savePath = path;
    d.totalSize = meta.totalSize;
    d.ready = true;
    for (int i = 0; i < meta.files.size(); ++i) {
        DownloadFile f;
        f.path = meta.files[i].path;
        f.size = meta.files[i].size;
        f.index = i;
        f.selected = true;
        d.files.append(f);
    }
    {
        QMutexLocker lock(&mutex_);
        d.queuePosition = nextQueuePosition_++;
        downloads_[hash] = d;
    }

    enginePolicyDirty_ = true;
    reconcileSchedule();

    emit downloadStarted(hash);
    emit filesReady(hash, filesToJson(d.files));
    if(!d.selectionConfirmed) emit selectionRequired(hash);
    return true;
}

bool DownloadService::restore(const Download& entry)
{
    if (!isReady()) {
        qWarning() << "DownloadService: Not ready for restore";
        return false;
    }

    const QString h = infohash::normalize(entry.hash);
    if (contains(h)) {
        qInfo() << "DownloadService: Already active:" << h.left(8);
        return false;
    }

    const QString path = resolveSavePath(entry.savePath);
    if (!ensureDir(path)) {
        qWarning() << "DownloadService: Cannot create download directory:" << path;
        return false;
    }

    qInfo() << "DownloadService: Restoring torrent" << h.left(8) << entry.name.left(30) << "from" << path;
    if (!engine_->createDownload(h, path, {net::TorrentEngine::StartMode::Inactive, true, true, false}).applied()) {
        qWarning() << "DownloadService: Failed to restore torrent:" << h.left(8);
        return false;
    }

    Download d;
    d.hash = h;
    d.paused = entry.paused;
    d.selectionConfirmed=entry.selectionConfirmed;
    d.selectionRevision=entry.selectionRevision;
    d.restoredTask=true;
    d.progressPending=true;
    d.seededMilliseconds=entry.seededMilliseconds;d.seedingUploadedBytes=entry.seedingUploadedBytes;d.seedingTransportBaseline=-1;d.seedingStopped=entry.seedingStopped;
    d.policy = entry.policy;
    d.queuePosition = entry.queuePosition;
    d.savePath = path;
    d.name = entry.name.isEmpty() ? h : entry.name;
    d.ready = false;

    d.downloadedBytes = entry.downloadedBytes;
    d.totalSize = entry.totalSize;
    d.files = entry.files;
    d.progress=entry.progress;
    d.verifiedBytes=entry.verifiedBytes;
    d.wantedBytes=entry.wantedBytes;
    d.wantedVerifiedBytes=entry.wantedVerifiedBytes;
    d.completed = false;
    d.phase = DownloadPhase::Checking;
    {
        QMutexLocker lock(&mutex_);
        nextQueuePosition_ = qMax(nextQueuePosition_, d.queuePosition + 1);
        downloads_[h] = d;
    }

    enginePolicyDirty_ = true;
    reconcileSchedule();

    emit downloadStarted(h);
    if (!d.files.isEmpty()) {
        emit filesReady(h, filesToJson(d.files));
    }
    if (d.completed) {
        emit downloadCompleted(h);
    }
    return true;
}

void DownloadService::remove(const QString& hash, bool saveResumeData)
{
    const QString h = infohash::normalize(hash);

    {
        QMutexLocker lock(&mutex_);
        auto it = downloads_.find(h);
        if (it == downloads_.end()) {
            qWarning() << "DownloadService: Torrent not found:" << h;
            return;
        }
        downloads_.erase(it);
    }

    if (engine_) {
        if (saveResumeData) {
            qInfo() << "DownloadService: Saving resume data for:" << h;
            engine_->saveResumeData(h);
        }
        engine_->remove(h);
    }

    reconcileSchedule();
    persistPolicyChange();
    emit torrentRemoved(h);
    qInfo() << "DownloadService: Stopped and removed torrent:" << h;
}

bool DownloadService::pause(const QString& hash)
{
    if (!engine_) {
        return false;
    }
    const QString h = infohash::normalize(hash);

    {
        QMutexLocker lock(&mutex_);
        auto it = downloads_.find(h);
        if (it == downloads_.end() || it->paused) {
            return false;
        }
        it->paused = true;
    }

    engine_->pause(h);
    reconcileSchedule();
    persistPolicyChange();
    return true;
}

bool DownloadService::resume(const QString& hash)
{
    if (!engine_) {
        return false;
    }
    const QString h = infohash::normalize(hash);
    if (engine_->status(h).error.isError()) return false;

    {
        QMutexLocker lock(&mutex_);
        auto it = downloads_.find(h);
        if (it == downloads_.end() || (!it->paused && !it->seedingStopped)) {
            return false;
        }
    }


    {
        QMutexLocker lock(&mutex_);
        auto it = downloads_.find(h);
        if (it == downloads_.end()) return false;
        if(it->seedingStopped){it->seedingStopped=false;it->seededMilliseconds=0;it->seedingUploadedBytes=0;it->seedingTransportBaseline=it->queuedUploadBytes;}
        it->paused = false;
    }
    reconcileSchedule();
    persistPolicyChange();
    return true;
}

bool DownloadService::togglePause(const QString& hash)
{
    const QString h = infohash::normalize(hash);

    bool isPaused = false;
    {
        QMutexLocker lock(&mutex_);
        auto it = downloads_.find(h);
        if (it == downloads_.end()) {
            return false;
        }
        isPaused = it->paused || it->seedingStopped;
    }

    return isPaused ? resume(h) : pause(h);
}

bool DownloadService::selectFiles(const QString& hash,const QVector<bool>& selection) {
    const auto d=getDownload(hash);
    if(!d.ready || selection.size()!=d.files.size()) return false;
    QVector<int> priorities;for(bool selected:selection) priorities.append(selected?1:0);
    const auto result=setFilePriorities(hash,priorities,d.selectionRevision);
    return result.outcome==OperationResult::Outcome::Accepted || result.applied();
}
bool DownloadService::selectFilesJson(const QString& hash,const QJsonValue& value) {
    const auto d=getDownload(hash);QVector<bool> selection;
    for(const auto& f:d.files) selection.append(f.selected);
    if(value.isArray()) {
        const auto array=value.toArray();if(array.size()!=selection.size()) return false;
        for(int i=0;i<array.size();++i) {if(!array[i].isBool()) return false;selection[i]=array[i].toBool();}
    } else if(value.isObject()) {
        const auto object=value.toObject();
        for(auto i=object.begin();i!=object.end();++i) {
            bool ok=false;const int index=i.key().toInt(&ok);
            if(!ok || index<0 || index>=selection.size() || !i.value().isBool()) return false;
            selection[index]=i.value().toBool();
        }
    } else return false;
    return selectFiles(hash,selection);
}
OperationResult DownloadService::setFilePriorities(const QString& hash,const QVector<int>& priorities,qint64 expected,bool confirm) {
    OperationResult r; r.hash=infohash::normalize(hash);
    const auto d=getDownload(r.hash); r.revision=d.selectionRevision;
    if(!isReady() || d.hash.isEmpty() || !d.ready) r.error={"metadataRequired","Download metadata is required"};
    else if(d.selectionApplying) r.error={"operationBusy","A file operation is already running"};
    else if(expected!=d.selectionRevision) r.error={"revisionConflict","File selection changed"};
    else if(priorities.size()!=d.files.size() || std::any_of(priorities.begin(),priorities.end(),[](int p){return p<0 || p>2;}))
        r.error={"invalidSelection","Provide one priority (0, 1 or 2) for every file"};
    if(r.error.isError()) return completeOperation(r);
    r.outcome=OperationResult::Outcome::Accepted; r=completeOperation(r);
    {QMutexLocker lock(&mutex_); downloads_[r.hash].selectionApplying=true;}
    QPointer<DownloadService> self(this);
    engine_->setFilePriorities(r.hash,priorities,expected,[self,id=r.operationId,confirm](OperationResult result){
        if(!self) return;
        QMetaObject::invokeMethod(self,[self,id,confirm,result]() mutable {
            if(!self) return;
            result.operationId=id;
            const auto hash=self->operations_.value(id).hash; result.hash=hash;
            const auto snapshot=self->engine_->status(hash);
            {QMutexLocker lock(&self->mutex_); auto it=self->downloads_.find(hash);
                if(it==self->downloads_.end()) {result.outcome=OperationResult::Outcome::Rejected;result.error={"taskRemoved","Download was removed"};}
                else {it->selectionApplying=false;if(result.applied()) {self->applySnapshot(*it,snapshot);it->selectionConfirmed=confirm || it->selectionConfirmed;}}}
            if(result.applied() && !self->persistPolicyChange()) {result.outcome=OperationResult::Outcome::AppliedNotPersisted;result.error={"persistenceFailed","Selection applied; saving failed"};}
            self->operations_[id]=result; self->reconcileSchedule();
            emit self->filesChanged(hash,filesToJson(self->getDownload(hash).files));
              const auto current=self->getDownload(hash);
              emit self->progressUpdated(hash,self->progressJson(current));
            emit self->operationFinished(id,result.toJson());
        },Qt::QueuedConnection);
    });
    return r;
}

OperationResult DownloadService::recheck(const QString& hash,bool retry) {
    OperationResult r; r.hash=infohash::normalize(hash);
    const auto d=getDownload(r.hash);
    if(!isReady() || d.hash.isEmpty() || !d.ready) r.error={"metadataRequired","Download metadata is required"};
    else if(d.selectionApplying) r.error={"operationBusy","A file operation is already running"};
    if(r.error.isError()) return completeOperation(r);
    r.outcome=OperationResult::Outcome::Accepted;r=completeOperation(r);
    {QMutexLocker lock(&mutex_);downloads_[r.hash].selectionApplying=true;}
    QPointer<DownloadService> self(this);
    engine_->recheck(r.hash,retry,[self,id=r.operationId](OperationResult result){
        if(!self) return;
        QMetaObject::invokeMethod(self,[self,id,result]() mutable {
            if(!self) return;result.operationId=id;result.hash=self->operations_.value(id).hash;
            {QMutexLocker lock(&self->mutex_);auto it=self->downloads_.find(result.hash);if(it!=self->downloads_.end())it->selectionApplying=false;}
            self->operations_[id]=result;self->onUpdateTimer();emit self->operationFinished(id,result.toJson());
        },Qt::QueuedConnection);
    });return r;
}

void DownloadService::setRemoveOnDone(const QString& hash, bool removeOnDone)
{
    QMutexLocker lock(&mutex_);
    auto it = downloads_.find(infohash::normalize(hash));
    if (it != downloads_.end()) {
        it->removeOnDone = removeOnDone;
    }
}

QString DownloadService::registerSeed(const net::SeedResult& seed)
{
    const QString hash = infohash::normalize(seed.hash);

    Download d;
    d.hash = hash;
    d.name = seed.name;
    d.savePath = seed.savePath;
    d.totalSize = seed.totalSize;
    d.downloadedBytes = seed.totalSize; // already complete for seeding
    d.verifiedBytes = seed.totalSize;
    d.phase = DownloadPhase::Seeding;
    d.progress = 1.0;
    d.ready = true;
    d.selectionConfirmed=true;
    d.completed = true;
    d.engineStarted = true;
    d.contentStarted = true;
    for (int i = 0; i < seed.files.size(); ++i) {
        DownloadFile f;
        f.path = seed.files[i].path;
        f.size = seed.files[i].size;
        f.index = i;
        f.selected = true;
        f.progress = 1.0;
        f.verifiedBytes = f.size;
        d.files.append(f);
    }

    {
        QMutexLocker lock(&mutex_);
        if (downloads_.contains(hash)) {
            qInfo() << "DownloadService: Torrent already exists:" << hash;
            return hash; // existing hash — not an error
        }
        d.queuePosition = nextQueuePosition_++;
        downloads_[hash] = d;
    }

    qInfo() << "DownloadService: Seeding torrent:" << d.name << "hash:" << hash.left(8);
    enginePolicyDirty_=true;
    reconcileSchedule();
    emit downloadStarted(hash);
    emit filesReady(hash, filesToJson(d.files));
    if(!d.selectionConfirmed) emit selectionRequired(hash);
    emit downloadCompleted(hash);
    return hash;
}

// ============================================================================
// Queries
// ============================================================================

bool DownloadService::isDownloading(const QString& hash) const
{
    return contains(infohash::normalize(hash));
}

Download DownloadService::getDownload(const QString& hash) const
{
    const QString h = infohash::normalize(hash);
    QMutexLocker lock(&mutex_);
    return downloads_.value(h);
}

QVector<Download> DownloadService::allDownloads() const
{
    QMutexLocker lock(&mutex_);
    QVector<Download> result;
    for (const Download& d : downloads_) {
        result.append(d);
    }
    return result;
}

QJsonArray DownloadService::toJsonArray() const
{
    QMutexLocker lock(&mutex_);
    QJsonArray arr;
    for (const Download& d : downloads_) {
        auto json=d.toJson();
        json["effectivePolicy"]=resolveEffectivePolicy(scheduledPolicy(globalPolicy_,utcClock_()),d.policy).toJson();
        arr.append(json);
    }
    return arr;
}

// ============================================================================
// Configuration
// ============================================================================

void DownloadService::setDefaultDownloadPath(const QString& path)
{
    defaultDownloadPath_ = path;
}

// ============================================================================
// Session persistence
// ============================================================================

bool DownloadService::saveSession(const QString& filePath)
{
    if (!sessionWritable_) return false;
    QVector<Download> snapshot;
    {
        QMutexLocker lock(&mutex_);
        for (const Download& d : downloads_) {
            snapshot.append(d);
        }
    }

    // Ask librats to persist resume data (downloaded pieces) for each torrent.
    if (engine_) {
        for (const Download& d : snapshot) {
            qInfo() << "DownloadService: Saving resume data for" << d.hash.left(8);
            engine_->saveResumeData(d.hash);
        }
    }

    QString error;
    if (!sessionStore_->saveDocument(filePath, {2, sessionRevision_ + 1, snapshot}, &error)) {
        qWarning() << "DownloadService: Session save failed:" << error;
        return false;
    }
    ++sessionRevision_;
    return true;
}

int DownloadService::loadSession(const QString& filePath)
{
    sessionPath_ = filePath;
    const auto loaded = sessionStore_->loadDocument(filePath);
    if (!loaded.ok()) {
        sessionWritable_ = false;
        qWarning() << "DownloadService: Session requires recovery:" << loaded.error;
        emit downloadFailed({}, DownloadError{"sessionInvalid", loaded.error}.toJson());
        return 0;
    }
    sessionWritable_ = true;
    sessionRevision_ = loaded.document.revision;
    const QVector<Download>& entries = loaded.document.downloads;
    int restored = 0;
    restoring_ = true;

    for (const Download& e : entries) {
        if (!infohash::isValid(e.hash)) {
            continue;
        }

        qInfo() << "DownloadService: Restoring torrent:" << e.hash.left(8) << e.name.left(30)
                << (e.completed ? "(completed/seeding)" : "(downloading)");

        if (restore(e)) {
            if (e.paused) {
                pause(e.hash);
            }
            setRemoveOnDone(e.hash, e.removeOnDone);

            restored++;
        }
    }

    restoring_ = false;
    reconcileSchedule();
    if (restored > 0) {
        qInfo() << "DownloadService: Restored" << restored << "torrents from session";
    }
    return restored;
}

// ============================================================================
// Progress polling
// ============================================================================

void DownloadService::onUpdateTimer()
{
    if (!isReady()) {
        return;
    }
    Transitions t = pollStatus();
    reconcileSchedule();
    flushTransitions(t);
    const auto now=clock_();
    if(!sessionPath_.isEmpty() && (checkpointDirty_ || !t.newlyCompleted.isEmpty() || !t.newlySelectedComplete.isEmpty())
        && (now-lastCheckpointMs_>=10000 || !t.newlyCompleted.isEmpty() || !t.newlySelectedComplete.isEmpty())) {
        lastCheckpointMs_=now;
        if(saveSession(sessionPath_))checkpointDirty_=false;
    }
}

DownloadService::Transitions DownloadService::pollStatus()
{
    Transitions t;

    // Snapshot the hashes to poll (don't hold the mutex across engine calls).
    QStringList hashes;
    {
        QMutexLocker lock(&mutex_);
        hashes = downloads_.keys();
    }

    for (const QString& hash : hashes) {
        // Thread-safe snapshot from the reactor thread (no mutex held).
        net::TorrentSnapshot snap = engine_->status(hash);

        QJsonObject progress;
        bool emitProgress = false;
        {
            QMutexLocker lock(&mutex_);
            auto it = downloads_.find(hash);
            if (it == downloads_.end()) {
                continue;
            }
            Download& d = *it;

            if (!snap.exists) {
                continue; // librats no longer tracks it — leave as-is
            }

            const bool wasReady = d.ready;


            const auto previousError = d.error.toJson();
            const auto previousPhase = d.phase;
            applySnapshot(d, snap);
            if (snap.hasMetadata && !wasReady) t.newlyReady.append(hash);
            updateRates(d, clock_());
            if (d.error.isError() && d.error.toJson() != previousError)
                t.failures.append({hash, d.error.toJson()});
            if (d.phase != previousPhase) t.states.append({hash, phaseName(d.phase)});
            if (d.selectionConfirmed && d.selectedComplete && d.lastSelectedCompletionRevision!=d.selectionRevision) {d.lastSelectedCompletionRevision=d.selectionRevision;t.newlySelectedComplete.append(hash);}
            if (!d.completed) d.completionNotified=false;
            if (d.completed && d.selectionConfirmed && !d.completionNotified) {
                d.completionNotified=true;
    d.completed = true;
                d.progress = 1.0;
                d.downloadSpeed = 0.0;
                t.newlyCompleted.append(hash);
                if (d.removeOnDone) {
                    t.toRemove.append(hash);
                }
            }

            progress = progressJson(d);
            progress["effectivePolicy"] = resolveEffectivePolicy(scheduledPolicy(globalPolicy_,utcClock_()),d.policy).toJson();
            // Only broadcast when the snapshot actually changed; an idle, paused
            // or finished-seeding download otherwise re-emits an identical payload
            // to every WebSocket client every second.
            if (progress != d.lastProgress) {
                d.lastProgress = progress;
                checkpointDirty_=true;
                emitProgress = true;
            }
        }

        if (emitProgress) {
            emit progressUpdated(hash, progress);
        }
    }

    return t;
}

void DownloadService::updateRates(Download& d, qint64 nowMs)
{
    if(d.lastSampledMs>=0 && nowMs>d.lastSampledMs && d.selectedComplete && !d.paused && !d.enginePaused && !d.schedulePaused && !d.seedingStopped)
        d.seededMilliseconds=qMin<qint64>(9007199254740991LL,d.seededMilliseconds+qMin<qint64>(nowMs-d.lastSampledMs,315360000000LL));
    if(d.seedingTransportBaseline>=0) {
        const qint64 delta=d.queuedUploadBytes>=d.seedingTransportBaseline?d.queuedUploadBytes-d.seedingTransportBaseline:d.queuedUploadBytes;
        d.seedingUploadedBytes=qMin<qint64>(9007199254740991LL,d.seedingUploadedBytes+qMax<qint64>(0,delta));
    }
    d.seedingTransportBaseline=d.queuedUploadBytes;
    const double previousVerifiedSpeed = d.verifiedSpeed;
    d.downloadSpeed = d.verifiedSpeed = d.uploadSpeed = 0;
    if (d.lastSampledMs >= 0 && nowMs > d.lastSampledMs && !d.enginePaused && !d.error.isError()) {
        const double seconds = (nowMs - d.lastSampledMs) / 1000.0;
        d.downloadSpeed = std::max<qint64>(0, d.downloadedBytes - d.lastSampledBytes) / seconds;
        d.uploadSpeed = std::max<qint64>(0, d.queuedUploadBytes - d.lastSampledUploaded) / seconds;
        if (d.phase == DownloadPhase::Downloading && d.wantedVerifiedBytes >= d.lastSampledVerified) {
            const double sample = (d.wantedVerifiedBytes - d.lastSampledVerified) / seconds;
            d.verifiedSpeed = previousVerifiedSpeed > 0 ? 0.5 * sample + 0.5 * previousVerifiedSpeed : sample;
        }
    }
    d.lastSampledBytes = d.downloadedBytes;
    d.lastSampledVerified = d.wantedVerifiedBytes;
    d.lastSampledUploaded = d.queuedUploadBytes;
    d.lastSampledMs = nowMs;
}
void DownloadService::flushTransitions(const Transitions& t)
{
    for (const auto& failure : t.failures) emit downloadFailed(failure.first, failure.second);
    for (const auto& state : t.states) emit stateChanged(state.first, state.second);
    for (const QString& hash : t.newlyReady) {
        QVector<DownloadFile> filesCopy;
        {
            QMutexLocker lock(&mutex_);
            auto it = downloads_.find(hash);
            if (it != downloads_.end()) {
                filesCopy = it->files;
            }
        }
        qInfo() << "DownloadService: Metadata received for:" << hash.left(8);
        emit downloadStarted(hash);
        emit filesReady(hash, filesToJson(filesCopy));
        const auto current=getDownload(hash);
        if(!current.selectionConfirmed && !current.restoredTask) emit selectionRequired(hash);
    }

    for(const auto& hash:t.newlySelectedComplete) emit selectedCompleted(hash);
    for (const QString& hash : t.newlyCompleted) {
        qInfo() << "DownloadService: Download completed:" << hash.left(8);
        emit downloadCompleted(hash);
    }

    for (const QString& hash : t.toRemove) {
        remove(hash);
    }
}

// ============================================================================
// Private helpers
// ============================================================================

void DownloadService::applySnapshot(Download& d, const net::TorrentSnapshot& snap)
{
    const bool settled=snap.hasMetadata && d.engineStarted && snap.phase!=DownloadPhase::Checking
        && snap.phase!=DownloadPhase::Stopped && snap.phase!=DownloadPhase::Metadata && !snap.error.isError();
    // A stopped/checking engine has not loaded its storage manifest yet. Never
    // replace remembered intent with its initial defaults. Replay a stale or
    // missing native resume selection before allowing any content requests.
    if(d.restoredTask && d.selectionConfirmed && settled && !d.selectionRestorePending) {
        bool matchingFiles=snap.files.size()==d.files.size();
        bool differs=snap.selectionRevision<d.selectionRevision;
        for(int i=0;i<snap.files.size() && i<d.files.size();++i) {
            matchingFiles=matchingFiles && snap.files[i].path==d.files[i].path && snap.files[i].size==d.files[i].size && d.files[i].index==i;
            if(snap.selectionRevision==d.selectionRevision)differs=differs || snap.files[i].priority!=d.files[i].priority;
        }
        if(snap.selectionRevision<=d.selectionRevision && !matchingFiles) {
            // Do not apply positional choices to a different file list. A newer
            // native manifest, however, is already durable authoritative intent.
            d.paused=true;d.selectionConfirmed=false;
        } else if(differs) {
            d.selectionRestorePending=d.selectionApplying=true;
            QVector<int> priorities;for(const auto& file:d.files)priorities.append(file.priority);
            const auto hash=d.hash;
            QTimer::singleShot(0,this,[this,hash,priorities,revision=snap.selectionRevision]{
                engine_->setFilePriorities(hash,priorities,revision,[self=QPointer<DownloadService>(this),hash](OperationResult result){
                    if(!self)return;
                    QMetaObject::invokeMethod(self,[self,hash,result]{
                        if(!self)return;
                        {QMutexLocker lock(&self->mutex_);auto it=self->downloads_.find(hash);if(it==self->downloads_.end())return;
                            it->selectionRestorePending=it->selectionApplying=false;
                            if(result.applied()){it->selectionRevision=result.revision;it->restoredTask=false;}
                            else {it->paused=true;it->selectionConfirmed=false;it->restoredTask=false;it->error=result.error;}}
                        if(!result.applied())emit self->downloadFailed(hash,result.error.toJson());
                        self->onUpdateTimer();self->persistPolicyChange();
                    },Qt::QueuedConnection);
                });
            });
        } else if(!differs) d.restoredTask=false;
    }
    const bool keepSelection=d.restoredTask && (!settled || d.selectionRestorePending);
    const bool keepProgress=d.progressPending && (!settled || d.selectionRestorePending);
    if(!keepProgress)d.progressPending=false;
    if (!snap.name.isEmpty()) {
        d.name = snap.name;
    }
    if (snap.hasMetadata || snap.totalSize > 0) d.totalSize = snap.totalSize;
    if(!keepProgress){d.downloadedBytes = snap.downloaded;d.verifiedBytes = snap.verifiedBytes;}
    d.queuedUploadBytes = snap.queuedUploadBytes;
    d.phase = snap.phase;
    d.error = snap.error;
    d.enginePaused = snap.paused;
    d.hashFailures = snap.hashFailures;
    if(!keepProgress)d.progress = snap.progress;
    d.peersConnected = snap.numPeers;
    d.completed = !keepProgress && (snap.fullTorrentComplete || snap.isComplete);
    if(!keepProgress){d.wantedBytes=snap.wantedBytes;d.wantedVerifiedBytes=snap.wantedVerifiedBytes;}
    d.selectedComplete=!keepProgress && snap.selectedComplete;
    if(snap.hasMetadata && !keepSelection) {
        if(d.selectionConfirmed && snap.selectionRevision<d.selectionRevision) {d.selectionConfirmed=false;d.paused=true;}
        d.selectionRevision=snap.selectionRevision;
    }
    d.ready = snap.hasMetadata;
    // Resume entries may carry the user's selection before BEP 9 completes.
    // An empty metadata-less snapshot must not erase it or the preview size.
    if (!snap.hasMetadata || keepSelection || keepProgress) return;

    const auto previousFiles = d.files;
    d.files.clear();
    for (int i = 0; i < snap.files.size(); ++i) {
        DownloadFile f;
        f.path = snap.files[i].path;
        f.size = snap.files[i].size;
        f.index = i;
        f.priority=snap.files[i].priority;f.selected=f.priority!=0;
        f.verifiedBytes = snap.files[i].verifiedBytes;
        f.progress = f.size > 0 ? static_cast<double>(f.verifiedBytes) / f.size : (snap.isComplete ? 1.0 : 0.0);
        d.files.append(f);
    }
}

QString DownloadService::resolveSavePath(const QString& savePath) const
{
    return savePath.isEmpty() ? defaultDownloadPath_ : savePath;
}

bool DownloadService::ensureDir(const QString& path)
{
    QDir dir(path);
    if (dir.exists()) {
        return true;
    }
    return dir.mkpath(".");
}

bool DownloadService::contains(const QString& hash) const
{
    QMutexLocker lock(&mutex_);
    return downloads_.contains(hash);
}

QJsonArray DownloadService::filesToJson(const QVector<DownloadFile>& files)
{
    QJsonArray arr;
    for (const DownloadFile& f : files) {
        arr.append(f.toJson());
    }
    return arr;
}

QJsonObject DownloadService::progressJson(const Download& d)
{
    QJsonObject o;
    o["wantedBytes"]=d.wantedBytes;o["wantedVerifiedBytes"]=d.wantedVerifiedBytes;
    o["selectedComplete"]=d.selectedComplete;o["selectionRevision"]=d.selectionRevision;o["selectionConfirmed"]=d.selectionConfirmed;
    o["downloaded"] = d.downloadedBytes;
    o["total"] = d.totalSize;
    o["receivedPayloadBytes"] = d.downloadedBytes;
    o["verifiedBytes"] = d.verifiedBytes;
    o["verifiedSpeed"] = d.verifiedSpeed;
    o["queuedUploadBytes"] = d.queuedUploadBytes;
    o["uploadQueueSpeed"] = d.uploadSpeed;
    o["uploadSpeed"] = d.uploadSpeed;
    o["uploadedBytes"] = d.queuedUploadBytes;
    o["taskPolicy"] = d.policy.toJson();
    o["queuePosition"] = d.queuePosition;
    o["schedulePaused"] = d.schedulePaused;
    o["queueReason"] = d.queueReason;
    o["seededMilliseconds"]=d.seededMilliseconds;o["seedingStopped"]=d.seedingStopped;o["seedingUploadedBytes"]=d.seedingUploadedBytes;
    o["phase"] = phaseName(d.phase);
    o["enginePaused"] = d.enginePaused;
    o["hashFailures"] = d.hashFailures;
    o["error"] = d.error.toJson();
    o["progress"] = d.progress;
    o["progressPending"] = d.progressPending;
    o["downloadSpeed"] = static_cast<int>(d.downloadSpeed);
    o["paused"] = d.paused;
    o["completed"] = d.completed; // so a progress update alone can render "completed"
    o["fullTorrentComplete"]=d.completed;
    o["selectionApplying"]=d.selectionApplying;
    o["removeOnDone"] = d.removeOnDone;
    if (!d.paused && !d.enginePaused && !d.error.isError() && d.phase == DownloadPhase::Downloading && d.verifiedSpeed > 0 && d.wantedBytes > d.wantedVerifiedBytes) {
        o["timeRemaining"] = static_cast<qint64>((d.wantedBytes - d.wantedVerifiedBytes) / d.verifiedSpeed);
    } else {
        o["timeRemaining"] = 0;
    }
    return o;
}

EffectivePolicy DownloadService::effectivePolicy(const QString& hash) const {
    return resolveEffectivePolicy(scheduledPolicy(globalPolicy_,utcClock_()),getDownload(hash).policy);
}
void DownloadService::configurePolicy(const GlobalDownloadPolicy& p) {
    if(!p.validate().isEmpty()) return;
    globalPolicy_=p; enginePolicyDirty_=true; reconcileSchedule();
}
void DownloadService::applyEnginePolicies() {
    if(!enginePolicyDirty_ || !isReady()) return;
    const auto active=scheduledPolicy(globalPolicy_,utcClock_());
    if(!engine_->setGlobalPolicy(active.downloadRate,active.uploadRate,globalPolicy_.maxConnections,
        globalPolicy_.taskConnections,globalPolicy_.checkingSlots)) return;
    engine_->setCacheBudget(globalPolicy_.cacheBytes);
    for(const auto& d:allDownloads()) {
        const auto effective=resolveEffectivePolicy(scheduledPolicy(globalPolicy_,utcClock_()),d.policy);
        engine_->setTaskPolicy(d.hash,d.policy.downloadRate.value_or(0),d.policy.uploadRate.value_or(0),effective.maxConnections);
        engine_->setAdvancedPolicy(d.hash,d.policy.strategy,d.policy.automaticWindow,d.policy.requestWindow,d.policy.windowMin,d.policy.windowMax);
    }
    enginePolicyDirty_=false;
}
void DownloadService::reconcileSchedule() {
    if(restoring_ || !isReady()) return;
    const auto resolution=globalPolicy_.schedule.resolve(utcClock_());
    if(!(resolution==scheduled_)){scheduled_=resolution;enginePolicyDirty_=true;}
    bool stopped=false;
    {QMutexLocker lock(&mutex_);for(auto& d:downloads_) {
        if(!d.ready || d.phase==DownloadPhase::Checking)continue;
        const bool reached=d.selectionConfirmed && d.selectedComplete && d.policy.seeding.value_or(globalPolicy_.seeding).shouldStop(d.seedingUploadedBytes,d.wantedBytes,d.seededMilliseconds/1000);
        if(reached!=d.seedingStopped){d.seedingStopped=reached;stopped=true;}
    }}
    if(stopped)persistPolicyChange();
    applyEnginePolicies();
    const auto tasks=allDownloads();
    QVector<ScheduleTask> input;
    for(const auto& d:tasks) input.append({d.hash,d.phase,d.paused || (d.seedingStopped && d.ready && d.engineStarted) || (scheduled_.pause && d.ready && d.engineStarted) || d.selectionApplying || (d.ready && d.engineStarted && (!d.selectionConfirmed || std::none_of(d.files.begin(),d.files.end(),[](const auto& f){return f.selected;}))),d.ready && d.selectionConfirmed,d.completed || d.selectedComplete,d.error.isError(),
        d.queuePosition,d.policy.priority,d.verifiedBytes});
    const auto decision=scheduler_.reconcile(input,globalPolicy_,clock_());
    const auto allowed=decision.content|decision.metadata|decision.checking;
    // Revoke first so reducing a limit cannot briefly over-admit new content.
    for(const auto& d:tasks) if(!allowed.contains(d.hash) || d.paused) {
        if(d.engineStarted && (d.phase!=DownloadPhase::Checking || d.paused)) {
            engine_->allowContent(d.hash,false); engine_->pause(d.hash);
        }
        QMutexLocker lock(&mutex_);
        auto i=downloads_.find(d.hash); if(i==downloads_.end()) continue;
        i->schedulePaused=!d.paused; i->contentStarted=false;
        i->queueReason=d.paused?"userPaused":d.error.isError()?"error":d.seedingStopped?"seedingGoal":scheduled_.pause?"timeSchedule":d.selectionApplying?"fileOperation":!d.selectionConfirmed?"selectionRequired":std::none_of(d.files.begin(),d.files.end(),[](const auto& f){return f.selected;})?"noFilesSelected":"queueSlot";
        if(d.phase!=DownloadPhase::Checking && !d.error.isError()) i->phase=DownloadPhase::Waiting;
    }
    for(const auto& d:tasks) if(allowed.contains(d.hash) && !d.paused && !d.error.isError()) {
        bool ok=true,content=d.contentStarted;
        if(!d.engineStarted) { ok=engine_->startMetadata(d.hash); content=false; }
        else if(decision.content.contains(d.hash) && d.phase!=DownloadPhase::Checking && (!content || d.enginePaused || d.schedulePaused)) {
            ok=engine_->startContent(d.hash); content=ok;
        } else if(decision.metadata.contains(d.hash) && (d.enginePaused || d.schedulePaused)) {
            engine_->resume(d.hash); ok=engine_->startMetadata(d.hash);
        }
        QMutexLocker lock(&mutex_);
        auto i=downloads_.find(d.hash); if(i==downloads_.end()) continue;
        if(ok) { i->engineStarted=true; i->contentStarted=content; i->schedulePaused=false; i->queueReason.clear(); }
        else { i->error={"engineRejected","Engine rejected queue admission"}; i->phase=DownloadPhase::Error; }
    }
}
bool DownloadService::persistPolicyChange() {
    return restoring_ || sessionPath_.isEmpty() || saveSession(sessionPath_);
}
OperationResult DownloadService::completeOperation(OperationResult r) {
    r.operationId=QUuid::createUuid().toString(QUuid::WithoutBraces);
    if(operations_.size()>=128) operations_.erase(operations_.begin());
    operations_[r.operationId]=r; return r;
}
OperationResult DownloadService::operationStatus(const QString& id) const {
    if(operations_.contains(id)) return operations_.value(id);
    OperationResult r; r.error={"operationNotFound","Operation is unknown or expired"}; return r;
}
OperationResult DownloadService::setGlobalPolicy(GlobalDownloadPolicy p,qint64 expected) {
    OperationResult r; r.revision=globalPolicy_.revision;
    const auto error=p.validate();
    if(globalPolicy_.revision>=9007199254740991LL) { r.error={"revisionExhausted","Policy revision exhausted"}; return completeOperation(r); }
    if(!error.isEmpty()) { r.error={"invalidPolicy",error}; return completeOperation(r); }
    if(expected>=0 && expected!=globalPolicy_.revision) { r.error={"revisionConflict","Global policy changed"}; return completeOperation(r); }
    const auto active=scheduledPolicy(p,utcClock_());
    if(!isReady() || !engine_->setGlobalPolicy(active.downloadRate,active.uploadRate,p.maxConnections,p.taskConnections,p.checkingSlots)) {
        r.error={"engineRejected","Download engine is not ready"}; return completeOperation(r);
    }
    p.revision=globalPolicy_.revision+1; globalPolicy_=p; enginePolicyDirty_=true;
    reconcileSchedule(); r.revision=p.revision;
    const bool saved=(!policyWriter_ || policyWriter_(p)) && persistPolicyChange();
    r.outcome=saved?OperationResult::Outcome::Applied:OperationResult::Outcome::AppliedNotPersisted;
    if(!saved) r.error={"persistenceFailed","Policy applied; saving failed"};
    emit policyChanged(p.toJson()); return completeOperation(r);
}
OperationResult DownloadService::setTaskPolicy(const QString& hash,TaskDownloadPolicy p,qint64 expected) {
    OperationResult r; r.hash=infohash::normalize(hash);
    const auto error=p.validate();
    if(!error.isEmpty()) { r.error={"invalidPolicy",error}; return completeOperation(r); }
    if(!contains(r.hash)) { r.error={"taskNotFound","Download does not exist"}; return completeOperation(r); }
    const auto old=getDownload(r.hash); r.revision=old.policy.revision;
    if(r.revision>=9007199254740991LL) { r.error={"revisionExhausted","Task revision exhausted"}; return completeOperation(r); }
    if(expected>=0 && expected!=r.revision) { r.error={"revisionConflict","Task policy changed"}; return completeOperation(r); }
    const auto effective=resolveEffectivePolicy(scheduledPolicy(globalPolicy_,utcClock_()),p);
    if(!engine_->setTaskPolicy(r.hash,p.downloadRate.value_or(0),p.uploadRate.value_or(0),effective.maxConnections)) {
        r.error={"engineRejected","Task policy was not applied"}; return completeOperation(r);
    }
    if(!engine_->setAdvancedPolicy(r.hash,p.strategy,p.automaticWindow,p.requestWindow,p.windowMin,p.windowMax)) {
        const auto previous=resolveEffectivePolicy(scheduledPolicy(globalPolicy_,utcClock_()),old.policy);
        engine_->setTaskPolicy(r.hash,old.policy.downloadRate.value_or(0),old.policy.uploadRate.value_or(0),previous.maxConnections);
        r.error={"engineRejected","Advanced policy was not applied"};return completeOperation(r);
    }
    p.revision=old.policy.revision+1;
    { QMutexLocker lock(&mutex_); downloads_[r.hash].policy=p;downloads_[r.hash].seedingStopped=false; }
    reconcileSchedule(); r.revision=p.revision;
    const bool saved=persistPolicyChange();
    r.outcome=saved?OperationResult::Outcome::Applied:OperationResult::Outcome::AppliedNotPersisted;
    if(!saved) r.error={"persistenceFailed","Task policy applied; saving failed"};
    emit queueChanged(toJsonArray()); return completeOperation(r);
}
OperationResult DownloadService::moveQueueItem(const QString& hash,const QString& before,qint64 expected) {
    OperationResult r; r.hash=infohash::normalize(hash); r.revision=globalPolicy_.revision;
    if(r.revision>=9007199254740991LL) { r.error={"revisionExhausted","Queue revision exhausted"}; return completeOperation(r); }
    if(expected>=0 && expected!=r.revision) { r.error={"revisionConflict","Queue changed"}; return completeOperation(r); }
    const auto target=infohash::normalize(before);
    if(!contains(r.hash) || (!before.isEmpty() && (!contains(target) || target==r.hash))) {
        r.error={"invalidQueueMove","Queue target is invalid"}; return completeOperation(r);
    }
    auto tasks=allDownloads();
    std::sort(tasks.begin(),tasks.end(),[](const auto& a,const auto& b){ return a.queuePosition<b.queuePosition; });
    Download moving=getDownload(r.hash);
    tasks.erase(std::remove_if(tasks.begin(),tasks.end(),[&](const auto& d){return d.hash==r.hash;}),tasks.end());
    auto position=std::find_if(tasks.begin(),tasks.end(),[&](const auto& d){return d.hash==target;});
    tasks.insert(position,moving);
    { QMutexLocker lock(&mutex_); qint64 n=0; for(const auto& d:tasks) downloads_[d.hash].queuePosition=n++; nextQueuePosition_=n; }
    ++globalPolicy_.revision; r.revision=globalPolicy_.revision; reconcileSchedule();
    const bool saved=(!policyWriter_ || policyWriter_(globalPolicy_)) && persistPolicyChange();
    r.outcome=saved?OperationResult::Outcome::Applied:OperationResult::Outcome::AppliedNotPersisted;
    if(!saved) r.error={"persistenceFailed","Queue moved; saving failed"};
    emit queueChanged(toJsonArray()); return completeOperation(r);
}
QJsonObject DownloadService::pieceMap(const QString& hash,int maximum) const {return engine_?engine_->pieceMap(infohash::normalize(hash),maximum):QJsonObject{};}
OperationResult DownloadService::setSchedule(const BandwidthSchedule& schedule,qint64 expected) {
    auto policy=globalPolicy_;policy.schedule=schedule;return setGlobalPolicy(policy,expected);
}
OperationResult DownloadService::setSeedingPolicy(const QString& hash,std::optional<SeedingPolicy> seeding,qint64 expected) {
    if(hash.isEmpty()){auto policy=globalPolicy_;policy.seeding=seeding.value_or(SeedingPolicy{});return setGlobalPolicy(policy,expected);}
    auto policy=getDownload(hash).policy;policy.seeding=seeding;return setTaskPolicy(hash,policy,expected);
}
QJsonObject DownloadService::scheduleStatus() const {
    const auto now=utcClock_();const auto resolution=globalPolicy_.schedule.resolve(now);
    return {{"schedule",globalPolicy_.schedule.toJson()},{"revision",globalPolicy_.revision},{"pause",resolution.pause},
        {"downloadLimitBps",resolution.downloadRate},{"uploadLimitBps",resolution.uploadRate},
        {"nextChangeUtc",globalPolicy_.schedule.nextChange(now).toString(Qt::ISODate)}};
}
} // namespace rats::service
