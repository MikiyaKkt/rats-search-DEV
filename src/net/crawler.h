#ifndef RATS_NET_CRAWLER_H
#define RATS_NET_CRAWLER_H

#include "domain/torrent.h"
#include "common/crawler_limits.h"
#include "common/crawler_timing.h"
#include "domain/hash_state.h"
#include <QHash>

#include <QObject>
#include <QString>
#include <QTimer>
#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <queue>
#include <set>
#include <string>
#include <memory>
#include <QSet>

namespace librats {
class Bittorrent;
}

namespace rats::net {

class P2PTransport;

// DHT crawler for the rats::net layer.
//
// Borrows the librats BitTorrent subsystem from a P2PTransport (non-owning)
// and:
//   1. Walks the DHT to expand the routing table.
//   2. Captures announce_peer messages from other clients (spider mode).
//   3. Fetches BEP 9 metadata for freshly discovered info-hashes.
//   4. Emits the resulting domain::Torrent — and nothing more.
//
// The crawler's responsibility ENDS at discovery: it never touches a database
// and never inserts anything. Persistence, filtering and content classification
// are the caller's job, driven off the discovered() signal.
class Crawler : public QObject {
    Q_OBJECT

public:
    // transport is borrowed (non-owning) and must outlive the crawler.
    explicit Crawler(P2PTransport* transport, QObject* parent = nullptr);
    ~Crawler() override;

    // Lifecycle
    bool start();
    void stop();
    bool isRunning() const;
    // Refresh queue age even when no new announcements arrive. Qt thread only.
    void refreshMetrics();

    // Tuning
    void setWalkInterval(int intervalMs);
    void setMetadataConcurrency(int limit);
    int metadataConcurrency() const { return metadataConcurrency_; }
    int activeMetadataFetches() const { return activeFetches_.load(); }
    bool metadataBackpressured() const { return metadataBlocked_; }
    // Transport seam for deterministic tests and controlled pipeline profiling.
    // Completion can arrive from any thread; lifetime/generation checks are shared.
    using MetadataCompletion = std::function<void(bool, domain::Torrent, bool)>;
    using MetadataFetcher = std::function<void(const QString&, const QString&, uint16_t, MetadataCompletion)>;
    void setMetadataFetcher(MetadataFetcher fetcher) { metadataFetcher_ = std::move(fetcher); }
    void setTiming(common::CrawlerTiming timing) { Q_ASSERT(!running_ && activeFetches_ == 0); timing_ = std::move(timing); }

    // Predicate consulted for each freshly seen info-hash before a BEP 9 metadata
    // fetch is queued: returning true skips the fetch (e.g. the torrent is
    // already indexed). Keeps the crawler DB-free — the caller injects the
    // lookup. Called on the Qt thread (onAnnounce is already marshalled there).
    using KnownHashFilter = std::function<bool(const QString& infoHash)>;
    void setKnownHashFilter(KnownHashFilter filter);
    // Production uses this asynchronous lookup; the synchronous predicate above
    // is retained for isolated baseline/test callers only.
    using KnownHashLookup = std::function<bool(const QString&, std::function<void(bool)>)>;
    void setKnownHashLookup(KnownHashLookup lookup) { knownHashLookup_ = std::move(lookup); }
    using HashLookup = std::function<bool(const QString&, std::function<void(domain::HashState)>)>;
    void setHashLookup(HashLookup lookup) { hashLookup_ = std::move(lookup); }
    void indexResult(const QString& hash, bool retryable);
    void setResultBudget(std::function<bool(const QString&)> reserve,
                         std::function<void(const QString&)> release) {
        reserveResult_ = std::move(reserve); releaseResult_ = std::move(release);
    }
    // Transport has joined all metadata callbacks. Invalidate any undelivered results.
    void releaseStoppedFetches();

signals:
    void statusChanged(const QString& status);
    void metadataActivityChanged();
    // Emitted once metadata has been fetched and a full torrent model built.
    void discovered(const rats::domain::Torrent& torrent);

private slots:
    void onSpiderWalk();
    void onIgnoreToggle();
    void processMetadataQueue();

private:
    friend class CrawlerMetricsTest;
    friend class CrawlerBenchmark;
    // A pending metadata fetch.
    // An empty peerIp means "no announcing peer known" — fall back to a DHT
    // search (slow path) instead of a direct peer connection (fast path).
    struct MetadataRequest {
        QString infoHash;
        QString peerIp;
        uint16_t peerPort = 0;
        qint64 queuedNs = 0;
        qint64 estimatedBytes = 0;
    };

    // Borrowed BitTorrent subsystem (null if the transport has none).
    librats::Bittorrent* bittorrent() const;

    // announce_peer callback, already marshalled onto the Qt thread.
    void onAnnounce(const std::array<uint8_t, 20>& infoHash, const std::string& ip, uint16_t port, bool reserved = false);

    // Kick off a BEP 9 metadata fetch (fast path if peer is known, else DHT).
    void fetchMetadata(const MetadataRequest& request);
    void queueMetadata(MetadataRequest request);
    void finishMetadata(quint64 taskId, quint64 generation, bool valid, const domain::Torrent& torrent, bool retryable = true);
    void drainAnnouncements(quint64 generation);
    void enqueueAnnouncement(quint64 generation, const std::array<uint8_t, 20>& hash, std::string ip, uint16_t port);
    void retryCandidates();
    void applyHashState(const MetadataRequest& request, quint64 generation, quint64 token, domain::HashState state);

    // Handle a completed metadata fetch (runs on the Qt thread).
    void onMetadataReceived(const rats::domain::Torrent& torrent);

    // Tuning defaults — no magic numbers scattered through the logic.
    static constexpr int DEFAULT_WALK_INTERVAL_MS = 100;
    static constexpr int DEFAULT_IGNORE_INTERVAL_MS = 1000;
    static constexpr int METADATA_QUEUE_INTERVAL_MS = 100;
    static constexpr int MAX_CONCURRENT_METADATA_FETCHES = common::CrawlerLimits::DefaultConcurrency;
    static constexpr size_t MAX_RECENT_HASHES = common::CrawlerLimits::Discovery;

    P2PTransport* transport_; // borrowed, non-owning

    std::atomic<bool> running_;
    std::atomic<int> discoveredCount_;
    std::atomic<int> activeFetches_;

    QTimer* walkTimer_;
    QTimer* ignoreTimer_;
    QTimer* metadataQueueTimer_;

    int walkIntervalMs_;
    int metadataConcurrency_ = MAX_CONCURRENT_METADATA_FETCHES;
    bool metadataBlocked_ = false;
    MetadataFetcher metadataFetcher_;
    common::CrawlerTiming timing_;

    std::queue<MetadataRequest> metadataQueue_;
    qint64 queuedBytes_ = 0;
    std::mutex queueMutex_;

    enum class CandidatePhase { Checking, Queued, Fetching, Indexing, Cached, RetryWaiting };
    struct Candidate {
        CandidatePhase phase = CandidatePhase::Checking;
        QList<QPair<QString, uint16_t>> peers;
        qint64 expiresNs = 0;
        quint64 token = 0;
        int retries = 0;
        qint64 createdNs = 0;
        bool retryLookup = false;
    };
    QHash<QString, Candidate> candidates_;
    quint64 nextCandidateToken_ = 0;
    std::mutex recentHashesMutex_;

    KnownHashFilter knownHashFilter_;
    KnownHashLookup knownHashLookup_;
    HashLookup hashLookup_;
    std::function<bool(const QString&)> reserveResult_;
    std::function<void(const QString&)> releaseResult_;
    struct Announcement {
        std::array<uint8_t, 20> hash;
        std::string ip;
        uint16_t port;
        quint64 generation;
    };
    struct CallbackGate {
        std::mutex mutex;
        Crawler* owner = nullptr;
        std::atomic<int> dispatches {0};
        std::atomic<int> activeCandidates {0};
        std::atomic<int> discoverySlots {0}; // mailbox + active/retrying candidates
        std::queue<Announcement> mailbox;
        bool wakePending = false;
    };
    std::shared_ptr<CallbackGate> gate_;
    quint64 generation_ = 0, nextTaskId_ = 0;
    QSet<quint64> fetchTasks_;
    QHash<quint64, QString> fetchHashes_;
    QSet<quint64> reservedFetches_;
};

} // namespace rats::net

#endif // RATS_NET_CRAWLER_H
