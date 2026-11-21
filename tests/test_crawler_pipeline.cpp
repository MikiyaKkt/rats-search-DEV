#include "services/crawler_pipeline.h"
#include "common/performance_metrics.h"
#include <QtTest>
#include <QSemaphore>
#include <QTimer>
#include <QScopeGuard>
#include <stdexcept>

using rats::service::CrawlerPipeline;
using rats::service::IndexingService;
using rats::service::FilterSettings;
using rats::domain::Torrent;

class CrawlerPipelineTest : public QObject {
    Q_OBJECT
private slots:
    void virtualClockControlsSparseBatchRetryAndDeadline() {
        std::atomic<qint64> now{1000000000LL}; std::atomic<int> calls{0};
        CrawlerPipeline::Backend backend;
        backend.timing.clock=[&]{return now.load();}; backend.timing.jitter=[](int){return 0;};
        backend.knownBatch=[&](const QStringList&){++calls;return QHash<QString,rats::domain::HashState>{};};
        CrawlerPipeline pipeline(backend); pipeline.start();
        pipeline.retryTimer_.stop();
        bool replied=false;
        pipeline.checkState("virtual",[&](auto state){QCOMPARE(state,rats::domain::HashState::Failed);replied=true;});
        now+=99000000; pipeline.flushReady(true); QCOMPARE(calls.load(),0);
        now+=1000000; pipeline.flushReady(true);
        QTRY_COMPARE(pipeline.retries_.size(),1);
        QCOMPARE(pipeline.retries_.first().task.retryNs,now.load()+250000000);
        now+=249000000; pipeline.retryReady(); QCOMPARE(calls.load(),1);
        now+=1000000; pipeline.retryReady();
        QTRY_COMPARE(calls.load(),2); QTRY_COMPARE(pipeline.retries_.size(),1);
        QCOMPARE(pipeline.retries_.first().task.retryNs,now.load()+500000000);
        now+=61000000000LL; pipeline.retryReady();
        QVERIFY(replied); QCOMPARE(calls.load(),2); QCOMPARE(pipeline.pending(),0);
    }

    void stopRemovesUndeliveredCapturesAcrossOneHundredRestarts() {
        std::atomic<int> executed{0},closed{0};
        CrawlerPipeline::Backend backend;
        backend.knownBatch=[&](const QStringList& hashes){
            ++executed; QHash<QString,rats::domain::HashState> result;
            for(const auto& h:hashes)result[h]=rats::domain::HashState::Missing; return result;
        };
        backend.close=[&]{++closed;};
        CrawlerPipeline pipeline(backend); int callbacks=0;
        for(int cycle=0;cycle<100;++cycle){
            pipeline.start(); pipeline.configure(10,1);
            auto marker=std::make_shared<int>(cycle);std::weak_ptr<int> captured=marker;
            pipeline.checkState(QString::number(cycle),[&,marker](auto){++callbacks;});marker.reset();
            pipeline.stop();
            QVERIFY(captured.expired()); // Proves old captures were destroyed, not merely ignored later.
            QCOMPARE(pipeline.pending(),0); QVERIFY(!pipeline.thread_.isRunning());
            QVERIFY(pipeline.retries_.isEmpty()); QVERIFY(pipeline.notifications_.isEmpty());
        }
        QCoreApplication::sendPostedEvents(&pipeline,QEvent::MetaCall);
        QCOMPARE(callbacks,0); QCOMPARE(closed.load(),100);
        QVERIFY(executed.load()<=100);
    }

    void retryExpiresWhileWaitingBehindBusyWorker() {
        std::atomic<qint64> now{1000000000LL};std::atomic<int> retryExecutions{0};
        QSemaphore entered,release;
        CrawlerPipeline::Backend backend;backend.timing.clock=[&]{return now.load();};backend.timing.jitter=[](int){return 0;};
        backend.knownBatch=[&](const QStringList& hashes){
            if(hashes.first()=="barrier"){entered.release();release.acquire();}
            else ++retryExecutions;
            QHash<QString,rats::domain::HashState> result;
            if(hashes.first()=="barrier")result["barrier"]=rats::domain::HashState::Known;
            return result;
        };
        CrawlerPipeline pipeline(backend);pipeline.start();pipeline.configure(10,1);pipeline.retryTimer_.stop();
        const auto unblock=qScopeGuard([&]{release.release(10);});
        bool terminal=false;pipeline.checkState("retry",[&](auto s){terminal=s==rats::domain::HashState::Failed;});
        QTRY_COMPARE(pipeline.retries_.size(),1);
        pipeline.checkState("barrier",[](auto){});QTRY_VERIFY(entered.available()>0);entered.acquire();
        now+=250000000;pipeline.retryReady();now+=61000000000LL;release.release();
        QTRY_COMPARE(pipeline.pending(),0);QVERIFY(terminal);QCOMPARE(retryExecutions.load(),1);
    }

    void runtimeQueueLimitNeverEvictsAdmittedTasksAndBatchSizeTakesEffect() {
        CrawlerPipeline::Backend backend;
        std::atomic<int> largest{0};
        backend.knownBatch = [&](const QStringList& hashes) {
            largest = qMax(largest.load(), int(hashes.size()));
            QHash<QString, rats::domain::HashState> result;
            for (const auto& h : hashes) result.insert(h, rats::domain::HashState::Missing);
            return result;
        };
        CrawlerPipeline pipeline(backend); pipeline.start();
        for (int i=0;i<10;++i) pipeline.checkState(QString::number(i), [](auto){});
        pipeline.configure(3,2);
        QCOMPARE(pipeline.pending(),10); QCOMPARE(pipeline.pendingLimit(),3); QCOMPARE(pipeline.batchSize(),2);
        QCOMPARE(pipeline.checkState("full",[](auto){}), CrawlerPipeline::Admission::Full);
        QTRY_COMPARE(pipeline.pending(),0); QCOMPARE(largest.load(),2);
        pipeline.configure(1,1); QVERIFY(!pipeline.metadataPaused());
        QVERIFY(pipeline.reserveResult("one")); QVERIFY(pipeline.metadataPaused());
        QVERIFY(!pipeline.reserveResult("two")); pipeline.releaseResult("one"); QVERIFY(!pipeline.metadataPaused());
        QVERIFY(pipeline.reserveResult("two")); pipeline.releaseResult("two");
    }

    void simultaneousLargeResultsConvertReservationsWithinBudget() {
        auto& metrics = rats::common::PerformanceMetrics::instance(); metrics.setEnabled(true); metrics.reset();
        CrawlerPipeline::Backend backend;
        backend.insertBatch = [](QVector<Torrent> input, FilterSettings) {
            IndexingService::BatchResult result;
            for (const auto& t : input) result.record({t.hash, IndexingService::Outcome::Inserted, "summary", {}, t});
            return result;
        };
        CrawlerPipeline pipeline(backend); pipeline.start();
        QSignalSpy indexed(&pipeline, &CrawlerPipeline::torrentIndexed);
        for (int i = 0; i < 10; ++i) QVERIFY(pipeline.reserveResult(QString::number(i)));
        for (int i = 0; i < 10; ++i) {
            Torrent t; t.hash = QString::number(i); t.name = QString(1536 * 1024, QChar('a' + i));
            QCOMPARE(pipeline.insert(t), CrawlerPipeline::Admission::Accepted);
            QCOMPARE(pipeline.pending(), 10);
        }
        const auto gauge = metrics.snapshot()["gauges"].toObject()["crawler.db_pending_bytes"].toObject();
        QVERIFY(gauge["peak"].toInteger() <= CrawlerPipeline::MaxBytes);
        QCOMPARE(gauge["peak"].toInteger(), 10 * CrawlerPipeline::ResultBytes);
        QTRY_COMPARE_WITH_TIMEOUT(pipeline.pending(), 0, 5000);
        QCOMPARE(indexed.count(), 10);
        const auto counters = metrics.snapshot()["counters"].toObject();
        QCOMPARE(counters["crawler.capacity_accepted"].toInt(), 10);
        QCOMPARE(counters["crawler.capacity_completed"].toInt(), 10);
    }

    void reservationsShareCapacityConvertAndApplyHysteresis() {
        CrawlerPipeline::Backend backend;
        backend.insertBatch = [](QVector<Torrent> input, FilterSettings) {
            IndexingService::BatchResult result;
            for (const auto& t : input) result.record({t.hash, IndexingService::Outcome::Rejected, "filter", {}, {}});
            return result;
        };
        CrawlerPipeline pipeline(backend); pipeline.start();
        for (int i = 0; i < 13; ++i) QVERIFY(pipeline.reserveResult(QString::number(i)));
        QVERIFY(pipeline.metadataPaused()); // 52/64 MiB crosses 80%.
        QVERIFY(!pipeline.reserveResult("blocked"));
        pipeline.releaseResult("0");
        QVERIFY(pipeline.metadataPaused()); // Below high water, still above low.
        for (int i = 1; i < 5; ++i) pipeline.releaseResult(QString::number(i));
        QVERIFY(!pipeline.metadataPaused()); // 32/64 MiB.
        QCOMPARE(pipeline.pending(), 8);
        Torrent t; t.hash = "5"; t.name = "reserved";
        QCOMPARE(pipeline.insert(t), CrawlerPipeline::Admission::Accepted);
        QCOMPARE(pipeline.pending(), 8); // Convert, do not double-charge.
        pipeline.releaseResult("5"); // Converted token is now owned by SQL.
        QCOMPARE(pipeline.pending(), 8);
        QTRY_COMPARE(pipeline.pending(), 7);
        QVERIFY(pipeline.reserveResult("oversize"));
        t.hash = "oversize"; t.name = QString(CrawlerPipeline::ResultBytes, 'a');
        QCOMPARE(pipeline.insert(t), CrawlerPipeline::Admission::TooLarge);
        QCOMPARE(pipeline.pending(), 7);
        pipeline.stop(); QCOMPARE(pipeline.pending(), 7);
        t.hash = "6"; QCOMPARE(pipeline.insert(t), CrawlerPipeline::Admission::Stopped);
        QCOMPARE(pipeline.pending(), 7);
        pipeline.start(); QVERIFY(!pipeline.reserveResult("6")); // Old network token remains charged.
        for (int i = 6; i < 13; ++i) pipeline.releaseResult(QString::number(i));
        pipeline.stop(); QCOMPARE(pipeline.pending(), 0); QVERIFY(!pipeline.metadataPaused());
    }

    void temporaryDatabaseFailureRetriesMetadataWithoutReadmission() {
        auto& metrics = rats::common::PerformanceMetrics::instance(); metrics.setEnabled(true); metrics.reset();
        std::atomic<int> attempts{0};
        CrawlerPipeline::Backend backend;
        backend.insertBatch = [&](QVector<Torrent> input, FilterSettings settings) {
            IndexingService::BatchResult result;
            const bool recovered = ++attempts >= 3;
            if (settings.maxFiles != 7) return result;
            for (const auto& t : input) result.record({t.hash, recovered ? IndexingService::Outcome::Inserted
                : IndexingService::Outcome::Failed, "summary", recovered ? "" : "Summary write unconfirmed", t});
            return result;
        };
        CrawlerPipeline pipeline(backend); pipeline.start();
        FilterSettings settings; settings.maxFiles = 7; pipeline.setFilterSettings(settings);
        QSignalSpy indexed(&pipeline, &CrawlerPipeline::torrentIndexed), settled(&pipeline, &CrawlerPipeline::settled);
        QVERIFY(pipeline.reserveResult("retry"));
        Torrent t; t.hash = "retry"; pipeline.insert(t);
        settings.maxFiles = 99; pipeline.setFilterSettings(settings);
        QTRY_COMPARE_WITH_TIMEOUT(pipeline.pending(), 0, 6000);
        QCOMPARE(attempts.load(), 3); QCOMPARE(indexed.count(), 1); QCOMPARE(settled.count(), 1);
        const auto counters = metrics.snapshot()["counters"].toObject();
        QCOMPARE(counters["crawler.capacity_accepted"].toInt(), 1);
        QCOMPARE(counters["crawler.capacity_completed"].toInt(), 1);
        QCOMPARE(counters["crawler.db_retries"].toInt(), 2);
    }

    void retryExhaustionAndPermanentFailureSettleOnce() {
        std::atomic<int> temporary{0}, permanent{0};
        CrawlerPipeline::Backend backend;
        backend.insertBatch = [&](QVector<Torrent> input, FilterSettings) {
            IndexingService::BatchResult result;
            for (const auto& t : input) {
                const bool large = t.hash == "large";
                if (large) ++permanent; else ++temporary;
                result.record({t.hash, IndexingService::Outcome::Failed, "files",
                    large ? "File row exceeds SQL byte limit" : "File write unconfirmed", {}, !large});
            }
            return result;
        };
        CrawlerPipeline pipeline(backend); pipeline.start();
        QSignalSpy settled(&pipeline, &CrawlerPipeline::settled), finished(&pipeline, &CrawlerPipeline::indexFinished);
        Torrent t; t.hash = "temporary"; pipeline.insert(t); t.hash = "large"; pipeline.insert(t);
        QTRY_COMPARE_WITH_TIMEOUT(pipeline.pending(), 0, 6000);
        QCOMPARE(temporary.load(), 4); QCOMPARE(permanent.load(), 1);
        QCOMPARE(settled.count(), 2); QCOMPARE(finished.count(), 2);
    }

    void stoppingRetryWaitCancelsAndOldGenerationCannotRetry() {
        std::atomic<int> attempts{0};
        CrawlerPipeline::Backend backend;
        backend.knownBatch = [&](const QStringList&) { ++attempts; return QHash<QString, rats::domain::HashState>{}; };
        CrawlerPipeline pipeline(backend); pipeline.start();
        bool oldReply = false;
        pipeline.checkState("old", [&](auto) { oldReply = true; });
        QTRY_COMPARE(attempts.load(), 1);
        // Process the failure receipt, then stop while the delayed retry is pending.
        QTest::qWait(50); pipeline.stop(); QCOMPARE(pipeline.pending(), 0);
        pipeline.start(); QTest::qWait(600);
        QCOMPARE(attempts.load(), 1); QVERIFY(!oldReply);
    }

    void countThresholdAndSparseDeadlineBatchQueries() {
        QSemaphore entered;
        std::atomic<int> calls{0}, size{0};
        CrawlerPipeline::Backend backend;
        backend.knownBatch = [&](const QStringList& hashes) {
            ++calls; size = hashes.size(); entered.release();
            QHash<QString, rats::domain::HashState> result;
            for (const auto& hash : hashes) result.insert(hash, rats::domain::HashState::Missing);
            return result;
        };
        CrawlerPipeline pipeline(backend); pipeline.start();
        int callbacks = 0;
        for (int i = 0; i < 100; ++i) pipeline.checkState(QString::number(i), [&](auto state) {
            QCOMPARE(state, rats::domain::HashState::Missing); ++callbacks;
        });
        QVERIFY(entered.tryAcquire(1, 2000)); // count trigger does not need a Qt timer tick
        QCOMPARE(calls.load(), 1); QCOMPARE(size.load(), 100);
        QTRY_COMPARE(callbacks, 100);
        QElapsedTimer elapsed; elapsed.start();
        pipeline.checkState("sparse", [&](auto) { ++callbacks; });
        QTRY_COMPARE(callbacks, 101);
        QVERIFY(elapsed.elapsed() < 1000); // generous scheduler tolerance, not a performance claim
        QCOMPARE(calls.load(), 2); QCOMPARE(size.load(), 1);
    }

    void notificationsAreLimitedAndRetainCapacityUntilConsumed() {
        CrawlerPipeline::Backend backend;
        backend.insertBatch = [](QVector<Torrent> input, FilterSettings) {
            IndexingService::BatchResult result;
            for (const auto& t : input) result.record({t.hash, IndexingService::Outcome::Inserted, "summary", {}, t});
            return result;
        };
        CrawlerPipeline pipeline(backend);
        int batches = 0, delivered = 0;
        connect(&pipeline, &CrawlerPipeline::batchIndexed, this, [&](const auto& batch) {
            QVERIFY(batch.size() <= 5);
            delivered += batch.size(); ++batches;
            QCOMPARE(pipeline.pending(), 100 - delivered);
        });
        pipeline.start();
        for (int i = 0; i < 100; ++i) {
            Torrent t; t.hash = QString("%1").arg(i + 1, 40, 16, QChar('0'));
            QCOMPARE(pipeline.insert(t), CrawlerPipeline::Admission::Accepted);
        }
        QTRY_COMPARE_WITH_TIMEOUT(pipeline.pending(), 0, 5000);
        QCOMPARE(delivered, 100); QCOMPARE(batches, 20);
    }

    void repeatedHashHasOneNewNotificationAndFailuresStayFailures() {
        CrawlerPipeline::Backend backend;
        backend.knownBatch = [](const QStringList&) { return QHash<QString, rats::domain::HashState>{}; };
        backend.insertBatch = [](QVector<Torrent> input, FilterSettings) {
            IndexingService::BatchResult result;
            result.record({input.first().hash, IndexingService::Outcome::Inserted, "summary", {}, input.first()});
            result.record({input.first().hash, IndexingService::Outcome::Duplicate, "validation", {}, {}});
            return result;
        };
        CrawlerPipeline pipeline(backend); QSignalSpy indexed(&pipeline, &CrawlerPipeline::torrentIndexed);
        pipeline.start();
        Torrent t; t.hash = QString(40, 'a');
        pipeline.insert(t); pipeline.insert(t);
        bool failed = false;
        pipeline.checkState("unknown", [&](auto state) { failed = state == rats::domain::HashState::Failed; });
        QTRY_COMPARE(pipeline.pending(), 0);
        QVERIFY(failed); QCOMPARE(indexed.count(), 1);
    }

    void slowDatabaseDoesNotBlockEventLoop() {
        QSemaphore entered, release;
        std::atomic<bool> onWorker{false};
        auto* mainThread = QThread::currentThread();
        CrawlerPipeline pipeline({[&](const QString&) {
            onWorker = QThread::currentThread() != mainThread;
            entered.release(); release.acquire(); return false;
        }, {}, {}});
        pipeline.start();
        bool returned = false, heartbeat = false;
        QCOMPARE(pipeline.checkKnown("hash", [&](bool known) { returned = !known; }), CrawlerPipeline::Admission::Accepted);
        const auto unblock = qScopeGuard([&] { release.release(512); });
        QTRY_VERIFY(entered.available() > 0); entered.acquire();
        QTimer::singleShot(0, &pipeline, [&] { heartbeat = true; });
        QTRY_VERIFY(heartbeat);
        QVERIFY(!returned);
        release.release();
        QTRY_VERIFY(returned);
        QVERIFY(onWorker.load());
        QTRY_COMPARE(pipeline.pending(), 0);
    }

    void boundedMailboxCancelsAndRestartsWithoutOldResults() {
        QSemaphore entered, release;
        std::atomic<int> executed{0}, closed{0};
        CrawlerPipeline pipeline({[&](const QString&) {
            if (++executed == 1) { entered.release(); release.acquire(); }
            return false;
        }, {}, [&] { ++closed; }});
        pipeline.start();
        int callbacks = 0;
        for (int i = 0; i < CrawlerPipeline::MaxJobs; ++i)
            QCOMPARE(pipeline.checkKnown("hash", [&](bool) { ++callbacks; }), CrawlerPipeline::Admission::Accepted);
        const auto unblock = qScopeGuard([&] { release.release(512); });
        QTRY_VERIFY(entered.available() > 0); entered.acquire();
        QCOMPARE(pipeline.checkKnown("overflow", [](bool) {}), CrawlerPipeline::Admission::Full);
        release.release();
        pipeline.stop(); // worker must never depend on callbacks running here
        QCOMPARE(pipeline.state(), CrawlerPipeline::State::Stopped);
        QCOMPARE(pipeline.pending(), 0);
        QCOMPARE(closed.load(), 1);
        QCOMPARE(pipeline.checkKnown("stopped", [](bool) {}), CrawlerPipeline::Admission::Stopped);
        pipeline.start();
        bool newCallback = false;
        QCOMPARE(pipeline.checkKnown("new", [&](bool) { newCallback = true; }), CrawlerPipeline::Admission::Accepted);
        QTRY_VERIFY(newCallback);
        QCOMPARE(callbacks, 0);
        pipeline.stop();
        QCOMPARE(closed.load(), 2);
    }

    void settingsAreCapturedAtAdmissionAndOnlyNewRowsNotify() {
        QSemaphore entered, release;
        QVector<int> versions;
        CrawlerPipeline pipeline({[&](const QString&) { entered.release(); release.acquire(); return true; },
            [&](Torrent torrent, FilterSettings settings) {
                versions.append(settings.maxFiles);
                IndexingService::Result result;
                result.success = true;
                result.alreadyExists = settings.maxFiles == 2;
                result.torrent = torrent;
                return result;
            }, {}});
        QSignalSpy indexed(&pipeline, &CrawlerPipeline::torrentIndexed);
        pipeline.start();
        pipeline.checkKnown("barrier", [](bool) {});
        const auto unblock = qScopeGuard([&] { release.release(512); });
        QTRY_VERIFY(entered.available() > 0); entered.acquire();
        Torrent torrent; torrent.hash = QString(40, 'a'); torrent.name = "fixture";
        FilterSettings settings; settings.maxFiles = 1;
        pipeline.setFilterSettings(settings);
        QCOMPARE(pipeline.insert(torrent), CrawlerPipeline::Admission::Accepted);
        settings.maxFiles = 2;
        pipeline.setFilterSettings(settings);
        QCOMPARE(pipeline.insert(torrent), CrawlerPipeline::Admission::Accepted);
        release.release();
        QTRY_COMPARE(pipeline.pending(), 0);
        QCOMPARE(indexed.count(), 1);
        QCOMPARE(versions, QVector<int>({1, 2}));
    }

    void rejectsOversizeAndContainsBackendException() {
        CrawlerPipeline pipeline({[](const QString&) -> bool { throw std::runtime_error("fixture"); }, {}, {}});
        QSignalSpy settled(&pipeline, &CrawlerPipeline::settled);
        pipeline.start();
        Torrent torrent; torrent.name = QString(CrawlerPipeline::MaxJobBytes, 'a');
        QCOMPARE(pipeline.insert(torrent), CrawlerPipeline::Admission::TooLarge);
        bool completed = false;
        pipeline.checkKnown("throws", [&](bool) { completed = true; });
        QTRY_COMPARE(settled.count(), 1);
        QVERIFY(!completed);
        QCOMPARE(pipeline.pending(), 0);
    }

    void aggregateByteCapacityIncludesQueuedResults() {
        QSemaphore entered, release;
        CrawlerPipeline pipeline({[&](const QString&) { entered.release(); release.acquire(); return false; },
            [](Torrent, FilterSettings) { return IndexingService::Result{}; }, {}});
        pipeline.start();
        pipeline.checkKnown("barrier", [](bool) {});
        const auto unblock = qScopeGuard([&] { release.release(512); });
        QTRY_VERIFY(entered.available() > 0); entered.acquire();
        Torrent torrent; torrent.name = QString(7 * 1024 * 1024, 'a');
        QCOMPARE(pipeline.insert(torrent), CrawlerPipeline::Admission::Accepted);
        QCOMPARE(pipeline.insert(torrent), CrawlerPipeline::Admission::Accepted);
        QCOMPARE(pipeline.insert(torrent), CrawlerPipeline::Admission::Accepted);
        QCOMPARE(pipeline.insert(torrent), CrawlerPipeline::Admission::Accepted);
        QCOMPARE(pipeline.insert(torrent), CrawlerPipeline::Admission::Full);
        QCOMPARE(pipeline.pending(), 5);
        release.release();
        QTRY_COMPARE(pipeline.pending(), 0);
        QCOMPARE(pipeline.insert(torrent), CrawlerPipeline::Admission::Accepted);
        QTRY_COMPARE(pipeline.pending(), 0);
    }
};
QTEST_GUILESS_MAIN(CrawlerPipelineTest)
#include "test_crawler_pipeline.moc"
