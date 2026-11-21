#include "net/crawler.h"

#include "net/librats_convert.h"
#include "net/p2p_transport.h"
#include "common/performance_metrics.h"

// librats' EventBus exposes a method named emit(), which collides with Qt's
// `emit` keyword macro. Neutralise the macro across all librats includes, then
// restore it so the crawler's own `emit signal` statements keep working.
#pragma push_macro("emit")
#pragma push_macro("slots")
#pragma push_macro("signals")
#undef emit
#undef slots
#undef signals
#ifdef RATS_SEARCH_FEATURES
#include "librats/bittorrent/torrent_info.h"
#include "librats/core/address.h"
#include "librats/dht/dht.h"
#include "librats/subsystems/bittorrent.h"
#endif
#pragma pop_macro("signals")
#pragma pop_macro("slots")
#pragma pop_macro("emit")

#include <QDebug>
#include <QVector>
#include <iterator>
#include <QPointer>
#include <QRandomGenerator>
#include <QScopeGuard>
#include <QCoreApplication>
#include <QEvent>

namespace rats::net {

Crawler::Crawler(P2PTransport* transport, QObject* parent)
    : QObject(parent)
    , transport_(transport)
    , running_(false)
    , discoveredCount_(0)
    , activeFetches_(0)
    , walkIntervalMs_(DEFAULT_WALK_INTERVAL_MS)
{
    gate_ = std::make_shared<CallbackGate>();
    gate_->owner = this;
    walkTimer_ = new QTimer(this);
    ignoreTimer_ = new QTimer(this);
    metadataQueueTimer_ = new QTimer(this);

    connect(walkTimer_, &QTimer::timeout, this, &Crawler::onSpiderWalk);
    connect(ignoreTimer_, &QTimer::timeout, this, &Crawler::onIgnoreToggle);
    connect(metadataQueueTimer_, &QTimer::timeout, this, &Crawler::processMetadataQueue);
}

Crawler::~Crawler()
{
    stop();
    std::lock_guard<std::mutex> lock(gate_->mutex);
    gate_->owner = nullptr;
}

librats::Bittorrent* Crawler::bittorrent() const
{
    if (!transport_) {
        return nullptr;
    }
    return transport_->bittorrent();
}

bool Crawler::start()
{
    if (running_) {
        return true;
    }

    if (!transport_) {
        qWarning() << "Crawler: P2P transport not available";
        return false;
    }

    if (!transport_->isRunning()) {
        qWarning() << "Crawler: P2P transport is not running - start it first";
        return false;
    }

    librats::Bittorrent* bt = bittorrent();
    if (!bt) {
        qWarning() << "Crawler: BitTorrent subsystem not available from transport";
        return false;
    }

    if (!transport_->isDhtRunning()) {
        qWarning() << "Crawler: DHT is not running";
        return false;
    }

    qInfo() << "Starting DHT crawler...";

    ++generation_;
    const auto generation = generation_;
    try {
#ifdef RATS_SEARCH_FEATURES
        // Enable spider mode with an announce callback. The core delivers the
        // info-hash already decoded and the peer as a structured Address, so no
        // string parsing is needed. The callback fires on a librats worker
        // thread, so marshal it onto the Qt thread before touching our state.
        bt->set_spider_mode(true);

        bt->set_spider_announce_callback([gate = gate_, generation](const librats::InfoHash& info_hash, const librats::Address& peer) {
            std::lock_guard<std::mutex> lock(gate->mutex);
            auto* self = gate->owner;
            if (!self) return;
            self->enqueueAnnouncement(generation, info_hash, peer.ip.to_string(), peer.port);
        });

        qInfo() << "Spider mode enabled with announce callback";
#endif

        running_ = true;

        walkTimer_->start(walkIntervalMs_);
        ignoreTimer_->start(DEFAULT_IGNORE_INTERVAL_MS);
        metadataQueueTimer_->start(METADATA_QUEUE_INTERVAL_MS);

        emit statusChanged("Active");

        qInfo() << "DHT crawler started successfully";
        qInfo() << "DHT nodes:" << transport_->dhtNodeCount();
        return true;
    } catch (const std::exception& e) {
        qWarning() << "Crawler: failed to start:" << e.what();
        return false;
    }
}

// Called with the callback gate locked, including on the librats thread.
void Crawler::enqueueAnnouncement(quint64 generation, const std::array<uint8_t, 20>& hash, std::string ip, uint16_t port)
{
    auto& m = common::PerformanceMetrics::instance();
    m.increment(QStringLiteral("crawler.announcements"));
    int usedSlots = gate_->discoverySlots.load();
    while (usedSlots < common::CrawlerLimits::Discovery && ip.size() <= 64 && !gate_->discoverySlots.compare_exchange_weak(usedSlots, usedSlots + 1)) {}
    if (usedSlots >= common::CrawlerLimits::Discovery || ip.size() > 64) {
        m.increment(QStringLiteral("crawler.announce_rejected_full")); return;
    }
    gate_->mailbox.push({hash, std::move(ip), port, generation});
    ++gate_->dispatches;
    m.gauge(QStringLiteral("crawler.discovery_slots"), gate_->discoverySlots.load());
    m.increment(QStringLiteral("crawler.announce_accepted"));
    m.gauge(QStringLiteral("crawler.announce_dispatch_pending"), gate_->dispatches.load());
    if (gate_->wakePending) return;
    gate_->wakePending = true;
    QMetaObject::invokeMethod(this, [this] { drainAnnouncements(generation_); }, Qt::QueuedConnection);
}

void Crawler::drainAnnouncements(quint64)
{
    std::queue<Announcement> batch;
    {
        std::lock_guard<std::mutex> lock(gate_->mutex);
        // Drain at most 100 per turn so metadata/SQL completion and timers progress.
        for (int i = 0; i < 100 && !gate_->mailbox.empty(); ++i) {
            batch.push(std::move(gate_->mailbox.front())); gate_->mailbox.pop();
        }
        gate_->wakePending = !gate_->mailbox.empty();
        if (gate_->wakePending) QMetaObject::invokeMethod(this, [this] { drainAnnouncements(generation_); }, Qt::QueuedConnection);
    }
    while (!batch.empty()) {
        const auto item = std::move(batch.front()); batch.pop();
        if (running_ && generation_ == item.generation) onAnnounce(item.hash, item.ip, item.port, true);
        else --gate_->discoverySlots;
        --gate_->dispatches;
        common::PerformanceMetrics::instance().increment(QStringLiteral("crawler.announce_processed"));
        common::PerformanceMetrics::instance().gauge(QStringLiteral("crawler.announce_dispatch_pending"), gate_->dispatches.load());
    }
}

void Crawler::stop()
{
    if (!running_) {
        return;
    }

    qInfo() << "Stopping DHT crawler...";

    walkTimer_->stop();
    ignoreTimer_->stop();
    metadataQueueTimer_->stop();

#ifdef RATS_SEARCH_FEATURES
    librats::Bittorrent* bt = bittorrent();
    if (bt) {
        bt->set_spider_mode(false);
        bt->set_spider_announce_callback({});
    }
#endif

    // Note: we never stop the BitTorrent subsystem here - the transport owns it.

    running_ = false; metadataBlocked_ = false;
    ++generation_;
    // Requests cannot be aborted in librats. Keep their result reservations until
    // their callbacks settle (or the transport joins), including across restart.
    {
        std::lock_guard<std::mutex> lock(gate_->mutex);
        common::PerformanceMetrics::instance().increment(QStringLiteral("crawler.announce_cancelled"), gate_->mailbox.size());
        gate_->discoverySlots -= int(gate_->mailbox.size());
        gate_->dispatches -= int(gate_->mailbox.size()); gate_->mailbox = {};
        common::PerformanceMetrics::instance().gauge(QStringLiteral("crawler.announce_dispatch_pending"), 0);
    }
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        common::PerformanceMetrics::instance().increment(QStringLiteral("crawler.metadata_cancelled_queued"), metadataQueue_.size());
        metadataQueue_ = {};
        queuedBytes_ = 0;
    }
    { std::lock_guard<std::mutex> lock(recentHashesMutex_);
      common::PerformanceMetrics::instance().increment(QStringLiteral("crawler.candidate_cancelled"), gate_->activeCandidates.load());
      gate_->discoverySlots -= gate_->activeCandidates.load();
      candidates_.clear(); gate_->activeCandidates = 0; }
    refreshMetrics();

    emit statusChanged("Stopped");

    qInfo() << "DHT crawler stopped. Total discovered:" << discoveredCount_.load();
}

void Crawler::releaseStoppedFetches()
{
    Q_ASSERT(!running_);
    // Transport has joined: no callback can enqueue another model. Destroy
    // queued deliveries before returning their reservations to a future run.
    QCoreApplication::removePostedEvents(this, QEvent::MetaCall);
    {
        std::lock_guard<std::mutex> lock(gate_->mutex);
        // An announce callback already running when stop() unregistered it may
        // have enqueued after stop() cleared the mailbox. Transport has now joined.
        const int cancelled=int(gate_->mailbox.size());
        common::PerformanceMetrics::instance().increment(QStringLiteral("crawler.announce_cancelled"),cancelled);
        gate_->discoverySlots-=cancelled;gate_->dispatches-=cancelled;gate_->mailbox={};gate_->wakePending=false;
        common::PerformanceMetrics::instance().gauge(QStringLiteral("crawler.announce_dispatch_pending"),gate_->dispatches.load());
    }
    common::PerformanceMetrics::instance().increment(QStringLiteral("crawler.metadata_cancelled_inflight"), fetchTasks_.size());
    fetchTasks_.clear();
    if (releaseResult_) for (const auto id : reservedFetches_) releaseResult_(fetchHashes_.value(id));
    reservedFetches_.clear();
    fetchHashes_.clear();
    activeFetches_ = 0;
    common::PerformanceMetrics::instance().gauge(QStringLiteral("crawler.metadata_inflight"), 0);
    common::PerformanceMetrics::instance().gauge(QStringLiteral("crawler.delivery_pending"), 0);
    common::PerformanceMetrics::instance().gauge(QStringLiteral("crawler.delivery_estimated_bytes"), 0);
    refreshMetrics();
}

bool Crawler::isRunning() const
{
    return running_;
}

void Crawler::setWalkInterval(int intervalMs)
{
    walkIntervalMs_ = intervalMs;
    if (running_ && walkTimer_) {
        walkTimer_->setInterval(intervalMs);
    }
}

void Crawler::setMetadataConcurrency(int limit)
{
    const int bounded = qBound(1, limit, common::CrawlerLimits::MaxConcurrency);
    if (bounded == metadataConcurrency_) return;
    const bool increased = bounded > metadataConcurrency_; metadataConcurrency_ = bounded;
    refreshMetrics(); emit metadataActivityChanged();
    if (running_ && increased) QMetaObject::invokeMethod(this, &Crawler::processMetadataQueue, Qt::QueuedConnection);
}

void Crawler::setKnownHashFilter(KnownHashFilter filter)
{
    knownHashFilter_ = std::move(filter);
}

void Crawler::onSpiderWalk()
{
    if (!running_) {
        return;
    }

#ifdef RATS_SEARCH_FEATURES
    // Trigger a spider walk to expand the DHT routing table.
    librats::Bittorrent* bt = bittorrent();
    if (bt) {
        bt->spider_walk();
    }
#endif
}

void Crawler::onIgnoreToggle()
{
#ifdef RATS_SEARCH_FEATURES
    // Rate limiting: toggle spider ignore mode to throttle incoming DHT requests.
    librats::Bittorrent* bt = bittorrent();
    if (bt && bt->is_spider_mode()) {
        bool currentIgnore = bt->is_spider_ignoring();
        bt->set_spider_ignore(!currentIgnore);
    }
#endif
}

void Crawler::processMetadataQueue()
{
    if (!running_) {
        return;
    }

    const bool wasBlocked = metadataBlocked_; metadataBlocked_ = false;
    retryCandidates();
    while (activeFetches_.load() < metadataConcurrency_) {
        MetadataRequest request;
        {
            std::lock_guard<std::mutex> lock(queueMutex_);
            if (metadataQueue_.empty()) {
                break;
            }
            request = metadataQueue_.front();
            if (reserveResult_ && !reserveResult_(request.infoHash)) { metadataBlocked_ = true; break; }
            metadataQueue_.pop();
            queuedBytes_ -= request.estimatedBytes;
        }
        common::PerformanceMetrics::instance().observe(QStringLiteral("crawler.metadata_queue_wait_ms"),
            (timing_.nowNs() - request.queuedNs) / 1000000.0);
        refreshMetrics();
        fetchMetadata(request);
    }
    if (wasBlocked != metadataBlocked_) emit metadataActivityChanged();
    refreshMetrics();
}

void Crawler::refreshMetrics()
{
    std::lock_guard<std::mutex> lock(queueMutex_);
    auto& m = common::PerformanceMetrics::instance();
    m.gauge(QStringLiteral("crawler.running"), running_.load() ? 1 : 0);
    m.gauge(QStringLiteral("crawler.discovery_active"), gate_->activeCandidates.load());
    m.gauge(QStringLiteral("crawler.discovery_slots"), gate_->discoverySlots.load());
    m.gauge(QStringLiteral("crawler.metadata_configured_limit"), metadataConcurrency_);
    m.gauge(QStringLiteral("crawler.metadata_budget_blocked"), metadataBlocked_);
    m.gauge(QStringLiteral("crawler.legacy_active_fetches"), activeFetches_.load());
    m.gauge(QStringLiteral("crawler.metadata_queue_items"), metadataQueue_.size());
    m.gauge(QStringLiteral("crawler.metadata_queue_estimated_bytes"), queuedBytes_);
    const qint64 age = metadataQueue_.empty() ? 0
        : (timing_.nowNs() - metadataQueue_.front().queuedNs) / 1000000;
    m.gauge(QStringLiteral("crawler.metadata_queue_oldest_ms"), age);
}

void Crawler::onAnnounce(const std::array<uint8_t, 20>& infoHash, const std::string& ip, uint16_t port, bool reserved)
{
    bool transferred = false;
    const auto release = qScopeGuard([&] { if (reserved && !transferred) --gate_->discoverySlots; });
    // Convert info-hash to a lower-case hex string.
    QString hashHex;
    for (uint8_t byte : infoHash) {
        hashHex += QString("%1").arg(byte, 2, 16, QChar('0'));
    }

    const auto now = timing_.nowNs();
    const auto peer = qMakePair(QString::fromStdString(ip), port);
    QList<QPair<QString, uint16_t>> rememberedPeers;
    auto existing = candidates_.find(hashHex);
    if (existing != candidates_.end()) {
        if (!existing->peers.contains(peer) && existing->peers.size() < 3) existing->peers.append(peer);
        if (existing->phase != CandidatePhase::Cached || existing->expiresNs > now) {
            common::PerformanceMetrics::instance().increment(QStringLiteral("crawler.recent_duplicates"));
            return;
        }
        rememberedPeers = existing->peers;
        candidates_.erase(existing);
    }
    if (candidates_.size() >= int(MAX_RECENT_HASHES)) {
        // Evict expired/completed entries only. Never forget an active hash.
        auto victim = candidates_.end();
        for (auto it = candidates_.begin(); it != candidates_.end(); ++it)
            if (it->phase == CandidatePhase::Cached && (victim == candidates_.end() || it->expiresNs < victim->expiresNs)) victim = it;
        if (victim != candidates_.end()) candidates_.erase(victim);
        else { common::PerformanceMetrics::instance().increment(QStringLiteral("crawler.candidate_rejected_full")); return; }
    }
    if (gate_->activeCandidates.load() >= int(MAX_RECENT_HASHES)) {
        common::PerformanceMetrics::instance().increment(QStringLiteral("crawler.candidate_rejected_full")); return;
    }
    if (!reserved) {
        int usedSlots = gate_->discoverySlots.load();
        while (usedSlots < common::CrawlerLimits::Discovery && !gate_->discoverySlots.compare_exchange_weak(usedSlots, usedSlots + 1)) {}
        if (usedSlots >= common::CrawlerLimits::Discovery) { common::PerformanceMetrics::instance().increment(QStringLiteral("crawler.candidate_rejected_full")); return; }
        reserved = true;
    }
    Candidate candidate; candidate.createdNs = now; candidate.token = ++nextCandidateToken_; candidate.peers = rememberedPeers;
    if (!candidate.peers.contains(peer) && candidate.peers.size() < 3) candidate.peers.append(peer);
    const auto token = candidate.token;
    candidates_.insert(hashHex, candidate);
    ++gate_->activeCandidates;
    common::PerformanceMetrics::instance().gauge(QStringLiteral("crawler.discovery_slots"), gate_->discoverySlots.load());
    transferred = true;

    auto& metrics = common::PerformanceMetrics::instance();
    metrics.increment(QStringLiteral("crawler.candidates"));

    MetadataRequest request;
    request.infoHash = hashHex;
    request.peerIp = QString::fromStdString(ip);
    request.peerPort = port;
    request.queuedNs = timing_.nowNs();
    request.estimatedBytes = sizeof(request) + (request.infoHash.capacity() + request.peerIp.capacity()) * sizeof(QChar);
    const auto generation = generation_;
    QPointer<Crawler> self(this);
    if (hashLookup_) {
        if (!hashLookup_(hashHex, [self, generation, token, request](domain::HashState state) {
                if (self) self->applyHashState(request, generation, token, state);
            })) {
            candidates_[hashHex].retryLookup = true;
            metrics.increment(QStringLiteral("crawler.known_rejected")); indexResult(hashHex, true);
        }
        return;
    }
    if (knownHashLookup_) {
        if (!knownHashLookup_(hashHex, [self, generation, token, request](bool known) {
                if (self) self->applyHashState(request, generation, token,
                    known ? domain::HashState::Known : domain::HashState::Missing);
            })) { metrics.increment(QStringLiteral("crawler.known_rejected")); indexResult(hashHex, true); }
        return;
    }
    bool known = false;
    if (knownHashFilter_) {
        common::PerformanceTimer lookup(QStringLiteral("crawler.known_lookup_ms"));
        metrics.increment(QStringLiteral("crawler.known_lookup_calls")); known = knownHashFilter_(hashHex);
    }
    if (known) { metrics.increment(QStringLiteral("crawler.already_indexed")); indexResult(hashHex, false); return; }
    queueMetadata(request);
}

void Crawler::applyHashState(const MetadataRequest& request, quint64 generation, quint64 token, domain::HashState state)
{
    if (!running_ || generation_ != generation || !candidates_.contains(request.infoHash)
        || candidates_[request.infoHash].token != token
        || candidates_[request.infoHash].phase != CandidatePhase::Checking) return;
    auto& m = common::PerformanceMetrics::instance();
    switch (state) {
    case domain::HashState::Known:
        m.increment(QStringLiteral("crawler.already_indexed")); indexResult(request.infoHash, false); break;
    case domain::HashState::Collision:
        m.increment(QStringLiteral("crawler.known_collision")); indexResult(request.infoHash, false); break;
    case domain::HashState::Failed:
        candidates_[request.infoHash].retryLookup = true;
        m.increment(QStringLiteral("crawler.known_lookup_failed")); indexResult(request.infoHash, true); break;
    case domain::HashState::Incomplete:
        m.increment(QStringLiteral("crawler.details_repair_candidates"));
        [[fallthrough]];
    case domain::HashState::Missing: queueMetadata(request); break;
    }
}

void Crawler::indexResult(const QString& hash, bool retryable)
{
    auto it = candidates_.find(hash.toLower());
    if (it == candidates_.end()) return;
    const auto now = timing_.nowNs();
    if (retryable && (it->retries >= 3 || now - it->createdNs >= 120000000000LL)) {
        common::PerformanceMetrics::instance().increment(QStringLiteral("crawler.network_retry_exhausted")); retryable = false;
    }
    if (!retryable && it->phase != CandidatePhase::Cached) {
        --gate_->activeCandidates;
        --gate_->discoverySlots;
        common::PerformanceMetrics::instance().gauge(QStringLiteral("crawler.discovery_slots"), gate_->discoverySlots.load());
        common::PerformanceMetrics::instance().increment(QStringLiteral("crawler.candidate_completed"));
    }
    it->phase = retryable ? CandidatePhase::RetryWaiting : CandidatePhase::Cached;
    it->expiresNs = now + (retryable
        ? ((500LL << it->retries++) + timing_.jitterMs(250)) * 1000000
        : 300000000000LL);
}

void Crawler::retryCandidates()
{
    const auto now = timing_.nowNs();
    // Snapshot keys: lookup callbacks can complete synchronously and mutate entries.
    const auto hashes = candidates_.keys();
    for (const auto& hash : hashes) {
        auto it = candidates_.find(hash);
        if (it == candidates_.end() || it->phase != CandidatePhase::RetryWaiting || it->expiresNs > now) continue;
        if (now - it->createdNs >= 120000000000LL) { indexResult(hash, false); continue; }
        MetadataRequest request; request.infoHash = hash; request.queuedNs = now;
        request.estimatedBytes = sizeof(request) + 256;
        if (!it->peers.isEmpty()) {
            const auto peer = it->peers.at(it->retries % it->peers.size());
            request.peerIp = peer.first; request.peerPort = peer.second;
        }
        common::PerformanceMetrics::instance().increment(QStringLiteral("crawler.network_retries"));
        if (it->retryLookup && hashLookup_) {
            it->phase = CandidatePhase::Checking; it->token = ++nextCandidateToken_;
            const auto token = it->token, generation = generation_;
            QPointer<Crawler> self(this);
            if (!hashLookup_(hash, [self, request, generation, token](domain::HashState state) {
                if (self) self->applyHashState(request, generation, token, state);
            })) indexResult(hash, true);
        } else queueMetadata(request);
    }
}

void Crawler::queueMetadata(MetadataRequest request)
{
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        if (metadataQueue_.size() >= 1024 || queuedBytes_ + request.estimatedBytes > 1024 * 1024) {
            common::PerformanceMetrics::instance().increment(QStringLiteral("crawler.metadata_rejected_full"));
            std::lock_guard<std::mutex> recentLock(recentHashesMutex_);
            indexResult(request.infoHash, true);
            return;
        }
        queuedBytes_ += request.estimatedBytes;
        if (candidates_.contains(request.infoHash)) candidates_[request.infoHash].phase = CandidatePhase::Queued;
        metadataQueue_.push(std::move(request));
    }
    common::PerformanceMetrics::instance().increment(QStringLiteral("crawler.metadata_queued"));
    refreshMetrics();
}

void Crawler::fetchMetadata(const MetadataRequest& request)
{
#ifdef RATS_SEARCH_FEATURES
    librats::Bittorrent* bt = bittorrent();
    if (!bt && !metadataFetcher_) {
        common::PerformanceMetrics::instance().increment(QStringLiteral("crawler.metadata_unavailable"));
        if (releaseResult_) releaseResult_(request.infoHash);
        indexResult(request.infoHash, true);
        return;
    }

    activeFetches_++;
    const auto taskId = ++nextTaskId_, generation = generation_;
    fetchTasks_.insert(taskId);
    if (reserveResult_) reservedFetches_.insert(taskId);
    fetchHashes_.insert(taskId, request.infoHash);
    if (candidates_.contains(request.infoHash)) candidates_[request.infoHash].phase = CandidatePhase::Fetching;
    auto& metrics = common::PerformanceMetrics::instance();
    metrics.increment(QStringLiteral("crawler.metadata_attempts"));
    metrics.adjustGauge(QStringLiteral("crawler.metadata_inflight"), 1);
    emit metadataActivityChanged();
    const qint64 startedNs = common::PerformanceMetrics::nowNs();

    const QString infoHash = request.infoHash;
    const QString peerIp = request.peerIp;
    const uint16_t peerPort = request.peerPort;

    const auto completed = std::make_shared<std::atomic<bool>>(false);
    if (metadataFetcher_) {
        auto completion = [gate = gate_, generation, taskId, infoHash, startedNs, completed](bool valid, domain::Torrent torrent, bool retryable) {
            if (completed->exchange(true)) return;
            auto& m = common::PerformanceMetrics::instance();
            m.observe(QStringLiteral("crawler.metadata_fetch_ms"), (common::PerformanceMetrics::nowNs() - startedNs) / 1000000.0);
            qint64 bytes = 1024 + (torrent.name.capacity() + torrent.hash.capacity()) * sizeof(QChar);
            for (const auto& f : torrent.fileList) bytes += sizeof(f) + f.path.capacity() * sizeof(QChar);
            if (bytes > common::CrawlerLimits::MetadataBytes || (valid && torrent.hash.toLower() != infoHash)) { valid = false; retryable = false; torrent = {}; }
            m.increment(valid ? QStringLiteral("crawler.metadata_succeeded") : QStringLiteral("crawler.metadata_failed"));
            if (!valid && retryable) m.increment(QStringLiteral("crawler.metadata_timeouts"));
            std::lock_guard<std::mutex> lock(gate->mutex);
            if (auto* self = gate->owner) QMetaObject::invokeMethod(self, [self, taskId, generation, valid, torrent = std::move(torrent), retryable] {
                self->finishMetadata(taskId, generation, valid, torrent, retryable);
            }, Qt::QueuedConnection);
        };
        try { metadataFetcher_(infoHash, peerIp, peerPort, std::move(completion)); }
        catch (...) { if (!completed->exchange(true)) finishMetadata(taskId, generation, false, {}, true); }
        return;
    }

    // librats manages the temporary torrent, the BEP 9 fetch and the timeout; it
    // invokes this callback exactly once (success or timeout) on a worker thread.
    auto onResult = [gate = gate_, generation, taskId, infoHash, peerIp, peerPort, startedNs, completed](
                        const librats::bittorrent::TorrentInfo& torrentInfo, bool success, const std::string& err) {
        if (completed->exchange(true)) return;
        auto& m = common::PerformanceMetrics::instance();
        bool valid = success && torrentInfo.is_valid();
        // Bound conversion before allocating Qt paths/models. Librats owns its raw metadata.
        qint64 estimate = 1024;
        if (valid) {
            estimate += qint64(torrentInfo.name().size()) * 2;
            for (const auto& file : torrentInfo.files().files()) estimate += 128 + qint64(file.path.size()) * 2;
            if (estimate > common::CrawlerLimits::MetadataBytes) {
                valid = false; m.increment(QStringLiteral("crawler.metadata_rejected_large"));
            }
        }
        const bool retryable = !success && QString::fromStdString(err).contains("timeout", Qt::CaseInsensitive);
        m.observe(QStringLiteral("crawler.metadata_fetch_ms"), (common::PerformanceMetrics::nowNs() - startedNs) / 1000000.0);
        m.increment(valid ? QStringLiteral("crawler.metadata_succeeded") : QStringLiteral("crawler.metadata_failed"));
        if (!valid && err.find("timeout") != std::string::npos) m.increment(QStringLiteral("crawler.metadata_timeouts"));
        domain::Torrent torrent = valid ? toDomainTorrent(infoHash, torrentInfo, peerIp, static_cast<int>(peerPort)) : domain::Torrent{};
        std::lock_guard<std::mutex> lock(gate->mutex);
        auto* self = gate->owner;
        if (!self) return;
        qint64 bytes = sizeof(torrent) + (torrent.name.capacity() + torrent.hash.capacity()) * sizeof(QChar);
        for (const auto& file : torrent.fileList) bytes += sizeof(file) + file.path.capacity() * sizeof(QChar);
        if (bytes > common::CrawlerLimits::MetadataBytes) { valid = false; torrent = {}; m.increment(QStringLiteral("crawler.metadata_rejected_large")); }
        const auto deliveredNs = common::PerformanceMetrics::nowNs();
        if (valid) {
            m.adjustGauge(QStringLiteral("crawler.delivery_pending"), 1);
            m.adjustGauge(QStringLiteral("crawler.delivery_estimated_bytes"), bytes);
        }
        QMetaObject::invokeMethod(self, [self, generation, taskId, valid, torrent, bytes, deliveredNs, retryable] {
            if (valid && self->fetchTasks_.contains(taskId)) {
                auto& metrics = common::PerformanceMetrics::instance();
                metrics.adjustGauge(QStringLiteral("crawler.delivery_pending"), -1);
                metrics.adjustGauge(QStringLiteral("crawler.delivery_estimated_bytes"), -bytes);
                metrics.observe(QStringLiteral("crawler.delivery_wait_ms"),
                    (common::PerformanceMetrics::nowNs() - deliveredNs) / 1000000.0);
            }
            self->finishMetadata(taskId, generation, valid, torrent, retryable);
        }, Qt::QueuedConnection);
    };

    try {
        if (!peerIp.isEmpty() && peerPort > 0) {
            // Fast path: fetch directly from the announcing peer (no DHT search).
            qCDebug(common::crawlerDetail) << "Fetching metadata for" << infoHash.left(8) << "directly from" << peerIp << ":" << peerPort;
            bt->get_torrent_metadata_from_peer(infoHash.toStdString(), peerIp.toStdString(), peerPort, onResult);
        } else {
            // Slow path: let librats find peers via the DHT.
            qCDebug(common::crawlerDetail) << "Fetching metadata for" << infoHash.left(8) << "via DHT search";
            bt->get_torrent_metadata(infoHash.toStdString(), onResult);
        }
    } catch (...) {
        common::PerformanceMetrics::instance().increment(QStringLiteral("crawler.metadata_launch_failed"));
        if (!completed->exchange(true)) finishMetadata(taskId, generation, false, {}, true);
    }
#else
    if (releaseResult_) releaseResult_(request.infoHash);
    indexResult(request.infoHash, true);
    qWarning() << "BitTorrent features not enabled, cannot fetch metadata";
#endif
}

void Crawler::finishMetadata(quint64 taskId, quint64 generation, bool valid, const domain::Torrent& torrent, bool retryable)
{
    if (!fetchTasks_.remove(taskId)) return;
    const auto hash = fetchHashes_.take(taskId);
    const bool reserved = reservedFetches_.remove(taskId);
    if (reserved && (!valid || !running_ || generation_ != generation) && releaseResult_) releaseResult_(hash);
    --activeFetches_; emit metadataActivityChanged();
    common::PerformanceMetrics::instance().adjustGauge(QStringLiteral("crawler.metadata_inflight"), -1);
    if (running_ && generation_ == generation) {
        if (valid) onMetadataReceived(torrent);
        else indexResult(hash, retryable);
    }
}

void Crawler::onMetadataReceived(const rats::domain::Torrent& torrent)
{
    common::PerformanceMetrics::instance().increment(QStringLiteral("crawler.delivered"));
    qCDebug(common::crawlerDetail) << "Discovered torrent metadata:" << torrent.name << "(" << torrent.hash.left(8) << ")";

    if (candidates_.contains(torrent.hash)) candidates_[torrent.hash].phase = CandidatePhase::Indexing;
    discoveredCount_++;
    emit discovered(torrent);
}

} // namespace rats::net
