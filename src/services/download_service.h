#ifndef RATS_SERVICE_DOWNLOAD_SERVICE_H
#define RATS_SERVICE_DOWNLOAD_SERVICE_H

#include "domain/torrent.h"
#include "net/torrent_engine.h"
#include "services/download_policy.h"
#include "services/download_scheduler.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QMutex>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QElapsedTimer>
#include <QVector>
#include <memory>

namespace rats::service {

class TorrentSessionStore;

// Per-file download state. Extends the pure domain::File (path + size) with the
// download-runtime bits — selection and per-file progress.
struct DownloadFile {
    QString path;
    qint64 size = 0;
    int index = 0;
    bool selected = true;
    double progress = 0.0;
    qint64 verifiedBytes = 0;
    int priority=1;

    QJsonObject toJson() const;
};

// The live state of one active download (or a completed torrent kept for
// seeding).
struct Download {
    qint64 wantedBytes=0,wantedVerifiedBytes=0,selectionRevision=0;
    bool selectedComplete=false,selectionConfirmed=false,selectionApplying=false;
    bool restoredTask=false, progressPending=false, selectionRestorePending=false; // runtime only
    qint64 lastSelectedCompletionRevision=-1;
    qint64 seededMilliseconds=0,seedingUploadedBytes=0;
    qint64 seedingTransportBaseline=0; // runtime transport epoch, excluded from persistence
    bool seedingStopped=false;
    QString hash;
    QString name;
    QString savePath;
    qint64 totalSize = 0;
    qint64 downloadedBytes = 0;
    double progress = 0.0;
    qint64 verifiedBytes = 0;
    double downloadSpeed = 0.0;
    qint64 queuedUploadBytes = 0;
    double verifiedSpeed = 0.0;
    double uploadSpeed = 0.0; // queue rate until transport accounting in S1
    DownloadPhase phase = DownloadPhase::Stopped;
    DownloadError error;
    bool enginePaused = false;
    qint64 hashFailures = 0;
    int peersConnected = 0;
    bool paused = false;
    bool removeOnDone = false;
    bool ready = false; // metadata (name/files/size) known
    bool completed = false;
    bool completionNotified = false;
    TaskDownloadPolicy policy;
    qint64 queuePosition = 0;
    bool schedulePaused = false;
    bool engineStarted = false;
    bool contentStarted = false;
    QString queueReason;
    QVector<DownloadFile> files;

    // Rolling sample used to derive downloadSpeed from the cumulative byte
    // counter between polls (librats exposes total bytes, not a per-torrent
    // rate).
    qint64 lastSampledBytes = 0;
    qint64 lastSampledMs = -1;
    qint64 lastSampledVerified = 0;
    qint64 lastSampledUploaded = 0;

    // Last progress payload emitted, so the 1s poll can skip re-broadcasting an
    // unchanged snapshot for idle/paused/seeding downloads.
    QJsonObject lastProgress;

    QJsonObject toJson() const;
};

// Owns the active-download registry, the 1s progress-poll timer and the
// add/pause/resume/remove/select lifecycle. All librats work is delegated to
// TorrentEngine and all persistence to TorrentSessionStore, so this class holds
// no librats include and no file I/O of its own.
//
// Async disk failures are surfaced once per changed error via downloadFailed.
// The legacy bool add/restore methods retain synchronous failure semantics.
class DownloadService : public QObject {
    Q_OBJECT

public:
    // engine is borrowed (non-owning) and must outlive the service.
    using Clock = std::function<qint64()>;
    using UtcClock=std::function<QDateTime()>;
    explicit DownloadService(net::TorrentEngine* engine, QObject* parent = nullptr, Clock clock = {},UtcClock utcClock={});
    ~DownloadService() override;

    // --- Download lifecycle -------------------------------------------------
    // Add by magnet link or 40-char hex hash.
    bool add(const QString& magnetLink, const QString& savePath = QString());
    // Add by known metadata (hash + optional name/size), fetching content via
    // DHT.
    bool addWithInfo(const domain::Torrent& info, const QString& savePath = QString());
    // Add from a .torrent file (metadata is available immediately).
    bool addFromFile(const QString& torrentFile, const QString& savePath = QString());

    // Stop and remove a torrent. saveResumeData preserves downloaded pieces.
    void remove(const QString& hash, bool saveResumeData = false);
    bool pause(const QString& hash);
    bool resume(const QString& hash);
    bool togglePause(const QString& hash);
    bool selectFiles(const QString& hash, const QVector<bool>& selection);
    bool selectFilesJson(const QString& hash, const QJsonValue& selection);
    OperationResult setFilePriorities(const QString& hash,const QVector<int>& priorities,qint64 expectedRevision,bool confirm=true);
    OperationResult recheck(const QString& hash,bool retry=false);
    QJsonObject pieceMap(const QString& hash,int maximum=256) const;
    OperationResult setSchedule(const BandwidthSchedule&,qint64 expectedRevision);
    OperationResult setSeedingPolicy(const QString& hash,std::optional<SeedingPolicy>,qint64 expectedRevision);
    QJsonObject scheduleStatus() const;

    // Register an already-created, seeding torrent in the registry (used by
    // TorrentCreator). Emits the started/files/completed/created signals. Returns
    // the (normalised) info hash.
    QString registerSeed(const net::SeedResult& seed);

    // --- Queries ------------------------------------------------------------
    bool isDownloading(const QString& hash) const;
    Download getDownload(const QString& hash) const;
    QVector<Download> allDownloads() const;
    QJsonArray toJsonArray() const;

    // --- Configuration ------------------------------------------------------
    void setDefaultDownloadPath(const QString& path);
    GlobalDownloadPolicy globalPolicy() const { return globalPolicy_; }
    EffectivePolicy effectivePolicy(const QString& hash) const;
    OperationResult setGlobalPolicy(GlobalDownloadPolicy policy, qint64 expectedRevision = -1);
    OperationResult setTaskPolicy(const QString& hash, TaskDownloadPolicy policy, qint64 expectedRevision = -1);
    OperationResult moveQueueItem(const QString& hash, const QString& beforeHash, qint64 expectedRevision = -1);
    OperationResult operationStatus(const QString& id) const;
    void setPolicyPersistence(std::function<bool(const GlobalDownloadPolicy&)> writer) { policyWriter_ = std::move(writer); }
    void configurePolicy(const GlobalDownloadPolicy&);

    // --- Session persistence (delegated to TorrentSessionStore) -------------
    bool saveSession(const QString& filePath);
    int loadSession(const QString& filePath);

signals:
    void downloadFailed(const QString& hash, const QJsonObject& error);
    void stateChanged(const QString& hash, const QString& state);
    void downloadStarted(const QString& hash);
    // File list available (after metadata), as an API-compatible JSON array.
    void filesReady(const QString& hash, const QJsonArray& files);
    // Carries the full snapshot — including `paused` and `removeOnDone` — so a
    // state flip needs no separate signal; the next poll re-broadcasts it.
    void progressUpdated(const QString& hash, const QJsonObject& progress);
    void downloadCompleted(const QString& hash);
    void torrentRemoved(const QString& hash);
    void policyChanged(const QJsonObject& policy);
    void queueChanged(const QJsonArray& tasks);
    void selectionRequired(const QString& hash);
    void selectedCompleted(const QString& hash);
    void filesChanged(const QString& hash,const QJsonArray& files);
    void operationFinished(const QString& id,const QJsonObject& result);

private slots:
    void onUpdateTimer();

private:
    bool isReady() const;
    void reconcileSchedule();
    void applyEnginePolicies();
    OperationResult completeOperation(OperationResult result);
    bool persistPolicyChange();

    // Restore a torrent from a previous session, loading any resume data. The
    // persisted entry supplies history and policy. Completion and progress are
    // rebuilt from the engine's asynchronous recheck.
    bool restore(const Download& entry);
    void setRemoveOnDone(const QString& hash, bool removeOnDone);

    // State changes detected during a poll, flushed as signals afterwards.
    struct Transitions {
        QVector<QPair<QString, QJsonObject>> failures;
        QVector<QPair<QString, QString>> states;
        QStringList newlyReady;
        QStringList newlyCompleted;
        QStringList newlySelectedComplete;
        QStringList toRemove;
    };

    // onUpdateTimer split into its three concerns.
    Transitions pollStatus();
    void updateRates(Download& d, qint64 monotonicMs);
    void flushTransitions(const Transitions& t);

    // Copy metadata (name/size/files) from a librats snapshot into a Download.
    void applySnapshot(Download& d, const net::TorrentSnapshot& snap);

    QString resolveSavePath(const QString& savePath) const;
    static bool ensureDir(const QString& path);
    bool contains(const QString& hash) const;
    static QJsonArray filesToJson(const QVector<DownloadFile>& files);
    static QJsonObject progressJson(const Download& d);

    net::TorrentEngine* engine_; // borrowed, non-owning
    std::unique_ptr<TorrentSessionStore> sessionStore_;
    QString defaultDownloadPath_;

    QHash<QString, Download> downloads_;
    mutable QMutex mutex_;

    QTimer* updateTimer_ = nullptr;
    QElapsedTimer elapsed_;
    Clock clock_;
    UtcClock utcClock_;
    ScheduleResolution scheduled_;
    // Refuse automatic writes after a corrupt/unsupported session load, so a
    // clean shutdown cannot overwrite recoverable user data with an empty list.
    bool sessionWritable_ = true;
    qint64 sessionRevision_ = 0;
    qint64 nextQueuePosition_ = 0;
    GlobalDownloadPolicy globalPolicy_;
    DownloadScheduler scheduler_;
    bool restoring_ = false;
    bool enginePolicyDirty_ = true;
    bool checkpointDirty_=false;
    qint64 lastCheckpointMs_=0;
    QString sessionPath_;
    std::function<bool(const GlobalDownloadPolicy&)> policyWriter_;
    QHash<QString, OperationResult> operations_;
};

} // namespace rats::service

#endif // RATS_SERVICE_DOWNLOAD_SERVICE_H
