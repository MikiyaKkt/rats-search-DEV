#include "services/crawler_pipeline.h"
#include "common/performance_metrics.h"
#include "data/torrent_repository.h"
#include "data/manticore.h"
#include <QElapsedTimer>
#include <QDebug>
#include <QJsonDocument>
#include <QRandomGenerator>
#include <QCoreApplication>
#include <QEvent>

namespace rats::service {
using common::PerformanceMetrics;
using domain::HashState;

CrawlerPipeline::CrawlerPipeline(data::TorrentRepository* repository, data::Manticore* manticore, QObject* parent)
    : CrawlerPipeline(Backend {{}, {}, [manticore] { manticore->closeCurrentThreadConnection(); },
          [repository](const QStringList& hashes) { return repository->checkMany(hashes); },
          [repository](QVector<domain::Torrent> torrents, FilterSettings settings) {
              FilterPolicy policy(std::move(settings));
              IndexingService indexing(repository, &policy);
              return indexing.insertBatch(std::move(torrents));
          }}, parent) {}

CrawlerPipeline::CrawlerPipeline(Backend backend, QObject* parent)
    : QObject(parent), backend_(std::move(backend))
{
    thread_.setObjectName(QStringLiteral("crawler-database"));
    flushTimer_.setSingleShot(true);
    connect(&flushTimer_, &QTimer::timeout, this, [this] { flushReady(true); });
    retryTimer_.setInterval(50);
    connect(&retryTimer_, &QTimer::timeout, this, &CrawlerPipeline::retryReady);
    notificationTimer_.setInterval(50);
    connect(&notificationTimer_, &QTimer::timeout, this, &CrawlerPipeline::notifyReady);
}
CrawlerPipeline::~CrawlerPipeline() { stop(); }

void CrawlerPipeline::start()
{
    Q_ASSERT(QThread::currentThread() == this->thread());
    if (state_ != State::Stopped) return;
    state_ = State::Starting; ++generation_;
    cancel_ = std::make_shared<std::atomic<bool>>(false);
    worker_ = new QObject;
    worker_->setObjectName(QStringLiteral("crawler-database-worker"));
    worker_->moveToThread(&thread_);
    connect(&thread_, &QThread::finished, worker_, &QObject::deleteLater);
    thread_.start(); state_ = State::Running; retryTimer_.start(); refreshMetrics();
}

void CrawlerPipeline::stop()
{
    Q_ASSERT(QThread::currentThread() == this->thread());
    if (state_ == State::Stopped) return;
    state_ = State::Draining; flushTimer_.stop(); notificationTimer_.stop(); retryTimer_.stop();
    PerformanceMetrics::instance().increment(QStringLiteral("crawler.db_cancelled"), lookups_.size() + inserts_.size());
    lookups_.clear(); inserts_.clear(); notifications_.clear(); retries_.clear(); cancel_->store(true);
    QElapsedTimer elapsed; elapsed.start();
    QMetaObject::invokeMethod(worker_, [this] { if (backend_.close) backend_.close(); }, Qt::BlockingQueuedConnection);
    thread_.quit(); thread_.wait(); worker_ = nullptr; ++generation_;
    // The worker has joined. Destroy any undelivered result captures before
    // returning their capacity to an immediate restart.
    QCoreApplication::removePostedEvents(this, QEvent::MetaCall);
    PerformanceMetrics::instance().increment(QStringLiteral("crawler.db_shutdown_unreported"), ledger_.size() - reservations_.size());
    PerformanceMetrics::instance().increment(QStringLiteral("crawler.capacity_cancelled"), ledger_.size() - reservations_.size());
    // Network callbacks can still own models. Carry their tokens through stop/start
    // rather than making their bytes available to a new generation prematurely.
    ledger_.clear(); bytes_ = 0;
    for (const auto& task : reservations_) { ledger_.insert(task.id, ResultBytes); bytes_ += ResultBytes; }
    state_ = State::Stopped; refreshMetrics();
    if (elapsed.elapsed() > 10000) qWarning() << "[CrawlerPipeline] drain exceeded 10s; safely joined after" << elapsed.elapsed() << "ms";
}

void CrawlerPipeline::configure(int pendingLimit, int batchSize)
{
    Q_ASSERT(QThread::currentThread() == this->thread());
    pendingLimit_ = qBound(1, pendingLimit, MaxJobs);
    batchSize_ = qBound(1, batchSize, BatchSize);
    refreshMetrics(); flushReady(false);
}

void CrawlerPipeline::setFilterSettings(FilterSettings settings)
{
    Q_ASSERT(QThread::currentThread() == this->thread());
    filter_ = std::move(settings); ++filterVersion_;
}

CrawlerPipeline::Admission CrawlerPipeline::admit(Pending& task, qint64 bytes)
{
    Q_ASSERT(QThread::currentThread() == this->thread());
    auto& m = PerformanceMetrics::instance();
    if (state_ != State::Running) return Admission::Stopped;
    if (bytes > MaxJobBytes) { m.increment(QStringLiteral("crawler.db_rejected_large")); return Admission::TooLarge; }
    if (ledger_.size() >= pendingLimit_ || bytes_ + bytes > MaxBytes) {
        m.increment(QStringLiteral("crawler.db_rejected_full")); return Admission::Full;
    }
    task.id = ++nextId_; task.submittedNs = backend_.timing.nowNs();
    task.version = filterVersion_; task.settings = filter_;
    ledger_.insert(task.id, bytes); bytes_ += bytes;
    m.increment(QStringLiteral("crawler.capacity_accepted")); refreshMetrics();
    return Admission::Accepted;
}

CrawlerPipeline::Admission CrawlerPipeline::checkKnown(QString hash, KnownCallback callback)
{
    return checkState(std::move(hash), [callback = std::move(callback)](HashState state) {
        if (state != HashState::Failed) callback(state == HashState::Known || state == HashState::Collision);
    });
}

CrawlerPipeline::Admission CrawlerPipeline::checkState(QString hash, StateCallback callback)
{
    Pending task; task.hash = hash.toLower(); task.callback = std::move(callback);
    const auto admission = admit(task, sizeof(Pending) + task.hash.capacity() * sizeof(QChar));
    if (admission == Admission::Accepted) { lookups_.append(std::move(task)); flushReady(false); }
    return admission;
}

CrawlerPipeline::Admission CrawlerPipeline::insert(domain::Torrent torrent)
{
    Q_ASSERT(QThread::currentThread() == this->thread());
    if (state_ != State::Running) return Admission::Stopped;
    torrent.hash = torrent.hash.toLower();
    qint64 bytes = sizeof(Pending) + (torrent.hash.capacity() + torrent.name.capacity() + torrent.ipv4.capacity()) * sizeof(QChar);
    for (const auto& f : torrent.fileList) bytes += sizeof(f) + f.path.capacity() * sizeof(QChar);
    bytes += QJsonDocument(torrent.info).toJson(QJsonDocument::Compact).size();
    Pending task; task.hash = torrent.hash; task.torrent = std::move(torrent);
    Admission admission;
    auto reserved = reservations_.find(task.hash);
    if (reserved != reservations_.end()) {
        auto reservation = reserved.value(); reservations_.erase(reserved);
        if (bytes > ResultBytes) {
            PerformanceMetrics::instance().increment(QStringLiteral("crawler.db_rejected_large"));
            release(reservation); return Admission::TooLarge;
        }
        task.id = reservation.id; task.submittedNs = reservation.submittedNs;
        task.version = reservation.version; task.settings = reservation.settings;
        bytes_ += bytes - ledger_.value(task.id); ledger_[task.id] = bytes;
        admission = Admission::Accepted; refreshMetrics();
    } else admission = admit(task, bytes);
    if (admission == Admission::Accepted) { inserts_.append(std::move(task)); flushReady(false); }
    return admission;
}

void CrawlerPipeline::scheduleFlush()
{
    qint64 oldest = backend_.timing.nowNs();
    if (!lookups_.isEmpty()) oldest = qMin(oldest, lookups_.first().submittedNs);
    if (!inserts_.isEmpty()) oldest = qMin(oldest, inserts_.first().submittedNs);
    if (lookups_.isEmpty() && inserts_.isEmpty()) { flushTimer_.stop(); return; }
    flushTimer_.start(qMax(0, BatchWaitMs - int((backend_.timing.nowNs() - oldest) / 1000000)));
}

void CrawlerPipeline::flushReady(bool due)
{
    if (state_ != State::Running) return;
    for (bool inserting : {true, false}) {
        auto& queue = inserting ? inserts_ : lookups_;
        while (!queue.isEmpty() && (queue.size() >= batchSize_
            || (due && backend_.timing.nowNs() - queue.first().submittedNs >= BatchWaitMs * 1000000LL))) {
            QList<Pending> batch;
            const auto version = queue.first().version;
            while (!queue.isEmpty() && batch.size() < batchSize_ && (!inserting || queue.first().version == version))
                batch.append(queue.takeFirst());
            dispatch(std::move(batch), inserting);
        }
    }
    scheduleFlush();
}

void CrawlerPipeline::dispatch(QList<Pending> tasks, bool inserting)
{
    const auto generation = generation_; const auto cancel = cancel_;
    QMetaObject::invokeMethod(worker_, [this, tasks = std::move(tasks), inserting, generation, cancel] {
        QHash<QString, HashState> known;
        IndexingService::BatchResult result;
        const bool cancelled = cancel->load();
        auto& m = PerformanceMetrics::instance();
        m.observe(QStringLiteral("crawler.db_batch_size"), tasks.size());
        for (const auto& task : tasks) m.observe(QStringLiteral("crawler.db_queue_wait_ms"),
            (backend_.timing.nowNs() - task.submittedNs) / 1000000.0);
        const bool expired = tasks.first().retries > 0
            && backend_.timing.nowNs() - tasks.first().submittedNs >= 60000000000LL;
        if (!cancelled && !expired) {
            try {
                if (inserting) {
                    common::PerformanceTimer timing(QStringLiteral("crawler.index_ms"));
                    m.gauge(QStringLiteral("crawler.db_filter_version"), tasks.first().version);
                    QVector<domain::Torrent> torrents;
                    for (const auto& task : tasks) torrents.append(task.torrent);
                    if (backend_.insertBatch) result = backend_.insertBatch(std::move(torrents), tasks.first().settings);
                    else for (const auto& task : tasks) {
                        const auto single = backend_.insert(task.torrent, task.settings);
                        result.record({task.hash, single.success ? (single.alreadyExists ? IndexingService::Outcome::Merged : IndexingService::Outcome::Inserted)
                            : IndexingService::Outcome::Failed, {}, single.error, single.torrent});
                    }
                } else {
                    common::PerformanceTimer timing(QStringLiteral("crawler.known_lookup_ms"));
                    m.increment(QStringLiteral("crawler.known_lookup_calls"));
                    m.increment(QStringLiteral("crawler.known_lookup_hashes"), tasks.size());
                    QStringList hashes; for (const auto& task : tasks) hashes.append(task.hash);
                    if (backend_.knownBatch) known = backend_.knownBatch(hashes);
                    else for (const auto& task : tasks) known.insert(task.hash, backend_.known(task.hash) ? HashState::Known : HashState::Missing);
                }
            } catch (const std::exception& e) {
                m.increment(QStringLiteral("crawler.db_task_failed")); qWarning() << "[CrawlerPipeline] batch failed:" << e.what();
            } catch (...) { m.increment(QStringLiteral("crawler.db_task_failed")); }
        }
        m.increment(cancelled ? QStringLiteral("crawler.db_cancelled") : QStringLiteral("crawler.db_executed"), tasks.size());
        QMetaObject::invokeMethod(this, [this, tasks, inserting, generation, cancelled, known, result] {
            if (generation != generation_ || state_ != State::Running) return;
            QHash<QString, IndexingService::ItemResult> outcomes;
            QSet<QString> insertedHashes;
            for (const auto& item : result.items)
                if (!outcomes.contains(item.hash) || outcomes[item.hash].outcome != IndexingService::Outcome::Inserted) outcomes.insert(item.hash, item);
            for (const auto& task : tasks) {
                if (!ledger_.contains(task.id)) continue;
                if (!inserting) {
                    if (!cancelled && known.value(task.hash, HashState::Failed) == HashState::Failed && retry(task, false)) continue;
                    if (!cancelled) task.callback(known.value(task.hash, HashState::Failed));
                    release(task, cancelled);
                } else {
                    auto item = outcomes.value(task.hash, {task.hash, IndexingService::Outcome::Failed, "worker", "No confirmed batch result", {}});
                    if (item.outcome == IndexingService::Outcome::Inserted) {
                        if (insertedHashes.contains(task.hash)) item.outcome = IndexingService::Outcome::Duplicate;
                        else insertedHashes.insert(task.hash);
                    }
                    if (!cancelled && item.outcome == IndexingService::Outcome::Failed
                        && item.retryable
                        && retry(task, true)) continue;
                    auto& m = PerformanceMetrics::instance();
                    const auto key = item.outcome == IndexingService::Outcome::Inserted ? "crawler.index_inserted"
                        : item.outcome == IndexingService::Outcome::Merged ? "crawler.index_merged"
                        : item.outcome == IndexingService::Outcome::Rejected ? "crawler.index_rejected"
                        : item.outcome == IndexingService::Outcome::Collision ? "crawler.index_collided"
                        : item.outcome == IndexingService::Outcome::Duplicate ? "crawler.index_duplicate"
                        : item.outcome == IndexingService::Outcome::Invalid ? "crawler.index_invalid" : "crawler.index_failed";
                    m.increment(QLatin1String(key));
                    if (!cancelled && item.outcome == IndexingService::Outcome::Inserted) {
                        notifications_.append({task, item});
                        if (!notificationTimer_.isActive()) notificationTimer_.start();
                    } else { emit indexFinished(task.hash, item.outcome); release(task, cancelled); }
                }
            }
            refreshMetrics();
        }, Qt::QueuedConnection);
    }, Qt::QueuedConnection);
}

void CrawlerPipeline::release(const Pending& task, bool cancelled)
{
    if (!ledger_.contains(task.id)) return;
    bytes_ -= ledger_.take(task.id);
    PerformanceMetrics::instance().increment(cancelled ? QStringLiteral("crawler.capacity_cancelled") : QStringLiteral("crawler.capacity_completed"));
    PerformanceMetrics::instance().observe(QStringLiteral("crawler.task_latency_ms"), (backend_.timing.nowNs() - task.submittedNs) / 1000000.0);
    refreshMetrics(); emit settled(task.id, cancelled);
}

void CrawlerPipeline::notifyReady()
{
    const auto generation = generation_;
    QVector<domain::Torrent> batch;
    for (int i = 0; i < 5 && !notifications_.isEmpty(); ++i) {
        auto notification = notifications_.takeFirst();
        if (!ledger_.contains(notification.task.id)) continue;
        batch.append(notification.result.torrent);
        emit torrentIndexed(notification.result.torrent);
        if (state_ != State::Running || generation_ != generation) return;
        emit indexFinished(notification.task.hash, notification.result.outcome);
        release(notification.task);
    }
    if (!batch.isEmpty()) emit batchIndexed(batch);
    if (notifications_.isEmpty()) notificationTimer_.stop();
    refreshMetrics();
}

bool CrawlerPipeline::reserveResult(const QString& hash)
{
    if (reservations_.contains(hash)) return false;
    refreshMetrics();
    if (paused_) return false;
    Pending task; task.hash = hash;
    if (admit(task, ResultBytes) != Admission::Accepted) return false;
    reservations_.insert(hash, task); refreshMetrics(); return true;
}

void CrawlerPipeline::releaseResult(const QString& hash)
{
    auto it = reservations_.find(hash);
    if (it == reservations_.end()) return;
    const auto task = it.value(); reservations_.erase(it); release(task);
}

bool CrawlerPipeline::retry(Pending task, bool inserting)
{
    const auto now = backend_.timing.nowNs();
    if (task.retries >= 3 || now - task.submittedNs >= 60000000000LL) {
        PerformanceMetrics::instance().increment(QStringLiteral("crawler.db_retry_exhausted")); return false;
    }
    task.retryNs = now + ((250LL << task.retries) + backend_.timing.jitterMs(125)) * 1000000;
    ++task.retries; retries_.append({std::move(task), inserting});
    PerformanceMetrics::instance().increment(QStringLiteral("crawler.db_retries"));
    refreshMetrics(); return true;
}

void CrawlerPipeline::retryReady()
{
    if (state_ != State::Running) return;
    const auto now = backend_.timing.nowNs();
    for (int i = 0; i < retries_.size();) {
        if (retries_[i].task.retryNs > now) { ++i; continue; }
        auto retry = retries_.takeAt(i);
        if (now - retry.task.submittedNs >= 60000000000LL) {
            PerformanceMetrics::instance().increment(QStringLiteral("crawler.db_retry_exhausted"));
            if (retry.inserting) emit indexFinished(retry.task.hash, IndexingService::Outcome::Failed);
            else retry.task.callback(HashState::Failed);
            release(retry.task);
            if (state_ != State::Running) return;
            continue;
        }
        // Keep original admission time, filter snapshot and capacity token.
        dispatch({std::move(retry.task)}, retry.inserting);
    }
    refreshMetrics();
}

void CrawlerPipeline::refreshMetrics()
{
    auto& m = PerformanceMetrics::instance();
    const bool high = ledger_.size() >= (pendingLimit_ * 8 + 9) / 10 || bytes_ >= MaxBytes * 8 / 10;
    const bool low = ledger_.size() <= pendingLimit_ / 2 && bytes_ <= MaxBytes / 2;
    if (!paused_ && high) { paused_ = true; m.increment(QStringLiteral("crawler.backpressure_pauses")); }
    else if (paused_ && low) { paused_ = false; m.increment(QStringLiteral("crawler.backpressure_resumes")); }
    m.gauge(QStringLiteral("crawler.metadata_paused"), paused_);
    m.gauge(QStringLiteral("crawler.result_reservations"), reservations_.size());
    m.gauge(QStringLiteral("crawler.db_retry_pending"), retries_.size());
    m.gauge(QStringLiteral("crawler.db_pending_limit"), pendingLimit_);
    m.gauge(QStringLiteral("crawler.db_batch_limit"), batchSize_);
    m.gauge(QStringLiteral("crawler.db_pending"), ledger_.size());
    m.gauge(QStringLiteral("crawler.db_pending_bytes"), bytes_);
    m.gauge(QStringLiteral("crawler.notification_pending"), notifications_.size());
    m.gauge(QStringLiteral("crawler.db_state"), static_cast<int>(state_));
}
}
