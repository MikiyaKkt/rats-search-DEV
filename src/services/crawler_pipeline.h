#ifndef RATS_SERVICE_CRAWLER_PIPELINE_H
#define RATS_SERVICE_CRAWLER_PIPELINE_H

#include "domain/torrent.h"
#include "common/crawler_limits.h"
#include "common/crawler_timing.h"
#include "services/filter_policy.h"
#include "services/indexing_service.h"
#include <QObject>
#include <QThread>
#include <QHash>
#include <QTimer>
#include "domain/hash_state.h"
#include <atomic>
#include <memory>
#include <functional>

namespace rats::data { class Manticore; class TorrentRepository; }
class CrawlerPipelineTest;
namespace rats::service {

// Controller lives on the application thread. Only the bounded job mailbox is
// crossed; the worker never waits for this thread, including during shutdown.
class CrawlerPipeline : public QObject {
    Q_OBJECT
public:
    enum class State { Stopped, Starting, Running, Draining };
    Q_ENUM(State)
    enum class Admission { Accepted, Stopped, Full, TooLarge };
    Q_ENUM(Admission)
    // Called for successful queries only. Errors settle the task and are counted;
    // they never masquerade as a missing hash or launch a metadata fetch.
    using KnownCallback = std::function<void(bool)>;
    using StateCallback = std::function<void(domain::HashState)>;
    // Test seam also allows exercising slow/failing work without a live network.
    struct Backend {
        std::function<bool(const QString&)> known;
        std::function<IndexingService::Result(domain::Torrent, FilterSettings)> insert;
        std::function<void()> close;
        std::function<QHash<QString, domain::HashState>(const QStringList&)> knownBatch;
        std::function<IndexingService::BatchResult(QVector<domain::Torrent>, FilterSettings)> insertBatch;
        common::CrawlerTiming timing;
    };
    CrawlerPipeline(data::TorrentRepository* repository, data::Manticore* manticore, QObject* parent = nullptr);
    explicit CrawlerPipeline(Backend backend, QObject* parent = nullptr);
    ~CrawlerPipeline() override;
    void start();
    void stop();
    void setFilterSettings(FilterSettings settings);
    void configure(int pendingLimit, int batchSize);
    int pendingLimit() const { return pendingLimit_; }
    int batchSize() const { return batchSize_; }
    Admission checkKnown(QString hash, KnownCallback callback);
    Admission checkState(QString hash, StateCallback callback);
    Admission insert(domain::Torrent torrent);
    bool reserveResult(const QString& hash);
    void releaseResult(const QString& hash);
    bool metadataPaused() const { return paused_; }
    static constexpr qint64 ResultBytes = common::CrawlerLimits::MetadataBytes;
    State state() const { return state_; }
    int pending() const { return ledger_.size(); }
    static constexpr int MaxJobs = common::CrawlerLimits::Pending;
    static constexpr qint64 MaxBytes = common::CrawlerLimits::PendingBytes;
    static constexpr qint64 MaxJobBytes = 16 * 1024 * 1024;
    static constexpr int BatchSize = 100;
    static constexpr int BatchWaitMs = 100;
signals:
    void torrentIndexed(const rats::domain::Torrent& torrent);
    void settled(quint64 taskId, bool cancelled);
    void batchIndexed(const QVector<rats::domain::Torrent>& torrents);
    void indexFinished(const QString& hash, rats::service::IndexingService::Outcome outcome);
private:
    friend class ::CrawlerPipelineTest;
    struct Pending {
        quint64 id = 0, version = 0;
        qint64 submittedNs = 0;
        QString hash;
        domain::Torrent torrent;
        FilterSettings settings;
        StateCallback callback;
        int retries = 0;
        qint64 retryNs = 0;
    };
    struct Notification { Pending task; IndexingService::ItemResult result; };
    Admission admit(Pending& task, qint64 bytes);
    void flushReady(bool due);
    void dispatch(QList<Pending> tasks, bool inserting);
    void scheduleFlush();
    void release(const Pending& task, bool cancelled = false);
    void notifyReady();
    void refreshMetrics();
    bool retry(Pending task, bool inserting);
    void retryReady();
    Backend backend_;
    QThread thread_;
    QObject* worker_ = nullptr;
    State state_ = State::Stopped;
    FilterSettings filter_;
    quint64 filterVersion_ = 0;
    std::shared_ptr<std::atomic<bool>> cancel_;
    quint64 generation_ = 0, nextId_ = 0;
    QHash<quint64, qint64> ledger_;
    qint64 bytes_ = 0;
    QList<Pending> lookups_, inserts_;
    QList<Notification> notifications_;
    QTimer flushTimer_, notificationTimer_;
    struct Retry { Pending task; bool inserting; };
    QList<Retry> retries_;
    QHash<QString, Pending> reservations_;
    QTimer retryTimer_;
    bool paused_ = false;
    int pendingLimit_ = MaxJobs, batchSize_ = BatchSize;
};
}
#endif
