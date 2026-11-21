#include "net/crawler.h"
#include "common/performance_metrics.h"
#include <QtTest>
#include <stdexcept>

namespace rats::net {
class CrawlerMetricsTest : public QObject {
    Q_OBJECT
private slots:
    void lateAnnouncementAfterStopIsCancelledWhenTransportJoins() {
        auto& metrics=common::PerformanceMetrics::instance();metrics.setEnabled(true);metrics.reset();
        Crawler crawler(nullptr);crawler.running_=true;crawler.generation_=9;crawler.stop();
        {std::lock_guard<std::mutex> lock(crawler.gate_->mutex);crawler.enqueueAnnouncement(9,{},"127.0.0.1",6881);}
        QCOMPARE(crawler.gate_->discoverySlots.load(),1);
        crawler.releaseStoppedFetches();
        QCOMPARE(crawler.gate_->discoverySlots.load(),0);QCOMPARE(crawler.gate_->dispatches.load(),0);
        QVERIFY(crawler.gate_->mailbox.empty());QVERIFY(!crawler.gate_->wakePending);
        QCoreApplication::sendPostedEvents(&crawler,QEvent::MetaCall);
        const auto counters=metrics.snapshot()["counters"].toObject();
        QCOMPARE(counters["crawler.announce_accepted"].toInt(),1);QCOMPARE(counters["crawler.announce_cancelled"].toInt(),1);
    }

    void synchronousDuplicateSuccessThenThrowCompletesExactlyOnce() {
        auto& metrics=common::PerformanceMetrics::instance(); metrics.setEnabled(true); metrics.reset();
        Crawler crawler(nullptr); crawler.running_=true;
        crawler.setMetadataFetcher([](const QString& hash,const QString&,uint16_t,auto done){
            domain::Torrent t; t.hash=hash; t.name="valid";
            done(true,t,false); done(false,{},true);
            throw std::runtime_error("after callback");
        });
        QSignalSpy discovered(&crawler,&Crawler::discovered);
        Crawler::MetadataRequest r; r.infoHash=QString(40,'a'); crawler.queueMetadata(r); crawler.processMetadataQueue();
        QTRY_COMPARE(discovered.count(),1); QCOMPARE(crawler.activeMetadataFetches(),0);
        const auto counters=metrics.snapshot()["counters"].toObject();
        QCOMPARE(counters["crawler.metadata_attempts"].toInt(),1);
        QCOMPARE(counters["crawler.metadata_succeeded"].toInt(),1);
        QCOMPARE(counters["crawler.metadata_failed"].toInt(),0);
        crawler.stop();
    }

    void virtualClockControlsNetworkBackoffAndTotalDeadline() {
        qint64 now=1000000000LL; Crawler crawler(nullptr);
        common::CrawlerTiming timing; timing.clock=[&]{return now;}; timing.jitter=[](int){return 0;};
        crawler.setTiming(timing); crawler.running_=true;
        std::array<uint8_t,20> hash{}; hash.back()=1; crawler.onAnnounce(hash,"127.0.0.1",6881);
        const auto hex=QString(39,'0')+"1";
        crawler.metadataQueue_={}; crawler.queuedBytes_=0;
        crawler.indexResult(hex,true); QCOMPARE(crawler.candidates_[hex].expiresNs,now+500000000LL);
        now+=499000000; crawler.retryCandidates(); QVERIFY(crawler.metadataQueue_.empty());
        now+=1000000; crawler.retryCandidates(); QCOMPARE(crawler.metadataQueue_.size(),size_t(1));
        crawler.metadataQueue_={}; crawler.queuedBytes_=0;
        now+=121000000000LL; crawler.indexResult(hex,true);
        QCOMPARE(crawler.candidates_[hex].phase,Crawler::CandidatePhase::Cached);
        QCOMPARE(crawler.gate_->discoverySlots.load(),0); crawler.stop();
    }

    void concurrencyHotUpdateAndOutOfOrderDuplicateCompletion() {
        Crawler crawler(nullptr); crawler.running_ = true; crawler.generation_ = 11;
        QVector<Crawler::MetadataCompletion> completions;
        crawler.setMetadataFetcher([&](const QString&, const QString&, uint16_t, auto done) { completions.append(done); });
        for (int i = 0; i < 12; ++i) {
            Crawler::MetadataRequest request; request.infoHash = QString::number(i); request.estimatedBytes = 100;
            crawler.queueMetadata(request);
        }
        crawler.setMetadataConcurrency(3); crawler.processMetadataQueue();
        QCOMPARE(crawler.activeMetadataFetches(), 3); QCOMPARE(completions.size(), 3);
        crawler.setMetadataConcurrency(6);
        QTRY_COMPARE(crawler.activeMetadataFetches(), 6);
        crawler.setMetadataConcurrency(2); crawler.processMetadataQueue();
        QCOMPARE(crawler.activeMetadataFetches(), 6); QCOMPARE(completions.size(), 6);
        for (int i : {5, 3, 1, 4}) completions[i](false, {}, false);
        QTRY_COMPARE(crawler.activeMetadataFetches(), 2); crawler.processMetadataQueue();
        QCOMPARE(completions.size(), 6);
        completions[0](false, {}, false); completions[0](false, {}, false);
        QTRY_COMPARE(crawler.activeMetadataFetches(), 1); crawler.processMetadataQueue();
        QCOMPARE(completions.size(), 7); QCOMPARE(crawler.activeMetadataFetches(), 2);
        crawler.setMetadataConcurrency(1000); QCOMPARE(crawler.metadataConcurrency(), 64);
        crawler.setMetadataConcurrency(0); QCOMPARE(crawler.metadataConcurrency(), 1);
        crawler.stop();
        for (const auto& done : completions) done(false, {}, true);
        QTRY_COMPARE(crawler.activeMetadataFetches(), 0);
        crawler.releaseStoppedFetches();
    }

    void concurrencyStillObeysResultCapacityAndRecoversWithoutAnnouncement() {
        Crawler crawler(nullptr); crawler.running_ = true;
        QVector<Crawler::MetadataCompletion> completions;
        int reserved = 0;
        crawler.setResultBudget([&](const QString&) { if (reserved >= 2) return false; ++reserved; return true; },
            [&](const QString&) { --reserved; });
        crawler.setMetadataFetcher([&](const QString&, const QString&, uint16_t, auto done) { completions.append(done); });
        crawler.setMetadataConcurrency(40);
        for (int i = 0; i < 5; ++i) { Crawler::MetadataRequest r; r.infoHash=QString::number(i); crawler.queueMetadata(r); }
        crawler.processMetadataQueue(); QCOMPARE(completions.size(), 2); QVERIFY(crawler.metadataBackpressured());
        completions[0](false, {}, false); QTRY_COMPARE(crawler.activeMetadataFetches(), 1);
        crawler.processMetadataQueue(); QCOMPARE(completions.size(), 3); QCOMPARE(reserved, 2);
        crawler.stop(); for (const auto& done : completions) done(false, {}, false);
        QTRY_COMPARE(crawler.activeMetadataFetches(), 0); QCOMPARE(reserved, 0);
    }

    void announcementBurstUsesOneBoundedMailboxAndStaleWakeIsSafe() {
        auto& m = common::PerformanceMetrics::instance(); m.setEnabled(true); m.reset();
        Crawler crawler(nullptr); crawler.running_ = true; crawler.generation_ = 4;
        crawler.setHashLookup([](const QString&, auto) { return true; });
        {
            std::lock_guard<std::mutex> lock(crawler.gate_->mutex);
            for (int i = 0; i < 6000; ++i) {
                std::array<uint8_t,20> hash{}; hash[0] = i >> 8; hash[1] = i & 255;
                crawler.enqueueAnnouncement(4, hash, "127.0.0.1", 6881);
            }
        }
        QCOMPARE(crawler.gate_->mailbox.size(), size_t(5000));
        QVERIFY(crawler.gate_->wakePending);
        QTRY_COMPARE(crawler.gate_->dispatches.load(), 0);
        QCOMPARE(crawler.gate_->activeCandidates.load(), 5000);
        QCOMPARE(crawler.candidates_.size(), 5000);
        QCOMPARE(m.snapshot()["counters"].toObject()["crawler.announce_rejected_full"].toInt(), 1000);
        const auto oldHashes = crawler.candidates_.keys();
        for (int i = 0; i < 100; ++i) crawler.indexResult(oldHashes[i], false);
        {
            std::lock_guard<std::mutex> lock(crawler.gate_->mutex);
            for (int i = 0; i < 100; ++i) {
                std::array<uint8_t,20> hash{}; hash[0] = 255; hash[1] = i;
                crawler.enqueueAnnouncement(4, hash, "127.0.0.2", 6881);
            }
        }
        QCOMPARE(crawler.gate_->discoverySlots.load(), 5000);
        QTRY_COMPARE(crawler.gate_->dispatches.load(), 0);
        QCOMPARE(crawler.gate_->activeCandidates.load(), 5000);
        QCOMPARE(crawler.candidates_.size(), 5000);
        crawler.stop(); QCOMPARE(crawler.gate_->activeCandidates.load(), 0);
        crawler.running_ = true; ++crawler.generation_;
        {
            std::lock_guard<std::mutex> lock(crawler.gate_->mutex);
            crawler.enqueueAnnouncement(4, {}, "127.0.0.1", 6881);
        }
        QTRY_COMPARE(crawler.gate_->dispatches.load(), 0);
        QVERIFY(crawler.candidates_.isEmpty()); crawler.stop();
    }

    void retriesKeepPeersAndStopAfterThreeWithoutNewAnnouncements() {
        Crawler crawler(nullptr); crawler.running_ = true;
        std::array<uint8_t,20> hash{}; hash.back() = 9;
        for (int i = 1; i <= 3; ++i) crawler.onAnnounce(hash, "127.0.0." + std::to_string(i), 6881);
        const QString hex = QString(39, '0') + "9";
        crawler.metadataQueue_ = {}; crawler.queuedBytes_ = 0;
        for (int attempt = 0; attempt < 3; ++attempt) {
            crawler.indexResult(hex, true);
            auto& candidate = crawler.candidates_[hex];
            QCOMPARE(candidate.retries, attempt + 1);
            candidate.expiresNs = common::PerformanceMetrics::nowNs() - 1;
            crawler.retryCandidates();
            QCOMPARE(crawler.metadataQueue_.size(), size_t(1));
            QCOMPARE(crawler.metadataQueue_.front().peerIp, QString("127.0.0.%1").arg((attempt + 1) % 3 + 1));
            crawler.metadataQueue_ = {}; crawler.queuedBytes_ = 0;
        }
        crawler.indexResult(hex, true);
        QCOMPARE(crawler.candidates_[hex].phase, Crawler::CandidatePhase::Cached);
        QCOMPARE(crawler.gate_->activeCandidates.load(), 0);
        crawler.retryCandidates(); QVERIFY(crawler.metadataQueue_.empty()); crawler.stop();
    }

    void invalidMetadataAndElapsedDeadlineDoNotRetry() {
        Crawler crawler(nullptr); crawler.running_ = true; crawler.generation_ = 5;
        std::array<uint8_t,20> hash{}; hash.back() = 7;
        crawler.onAnnounce(hash, "127.0.0.1", 6881);
        const QString hex = QString(39, '0') + "7";
        crawler.metadataQueue_ = {}; crawler.queuedBytes_ = 0;
        crawler.fetchTasks_.insert(1); crawler.fetchHashes_[1] = hex; crawler.activeFetches_ = 1;
        crawler.finishMetadata(1, 5, false, {}, false);
        QCOMPARE(crawler.candidates_[hex].phase, Crawler::CandidatePhase::Cached);
        QCOMPARE(crawler.candidates_[hex].retries, 0);
        crawler.candidates_[hex].expiresNs = 0;
        crawler.onAnnounce(hash, "127.0.0.2", 6881);
        crawler.candidates_[hex].createdNs -= 121000000000LL;
        crawler.indexResult(hex, true);
        QCOMPARE(crawler.candidates_[hex].phase, Crawler::CandidatePhase::Cached);
        QCOMPARE(crawler.candidates_[hex].retries, 0);
        crawler.stop();
    }

    void reservationRefusalLeavesQueueAndOldCompletionCannotReleaseNewToken() {
        Crawler crawler(nullptr); crawler.running_ = true; crawler.generation_ = 8;
        int reserves = 0, releases = 0;
        crawler.setResultBudget([&](const QString&) { ++reserves; return false; }, [&](const QString&) { ++releases; });
        Crawler::MetadataRequest request; request.infoHash = "budget"; request.estimatedBytes = 100;
        crawler.queueMetadata(request); crawler.processMetadataQueue();
        QCOMPARE(reserves, 1); QCOMPARE(releases, 0); QCOMPARE(crawler.metadataQueue_.size(), size_t(1));
        crawler.fetchTasks_.insert(1); crawler.fetchHashes_[1] = "same-hash";
        crawler.reservedFetches_.insert(1); crawler.activeFetches_ = 1;
        crawler.stop(); QCOMPARE(releases, 0); // In-flight reservation survives stop.
        crawler.running_ = true; ++crawler.generation_;
        crawler.finishMetadata(1, 8, false, {});
        QCOMPARE(releases, 1); QCOMPARE(crawler.activeFetches_.load(), 0);
        crawler.finishMetadata(1, 8, false, {}); QCOMPARE(releases, 1);
        crawler.stop();
    }

    void candidatesKeepLimitedPeersAndFailedHashesCanBeRediscovered() {
        Crawler crawler(nullptr); crawler.running_ = true;
        QVector<std::function<void(domain::HashState)>> replies;
        crawler.setHashLookup([&](const QString&, auto reply) { replies.append(reply); return true; });
        std::array<uint8_t, 20> hash {}; hash.back() = 8;
        for (int i = 0; i < 8; ++i) crawler.onAnnounce(hash, "127.0.0." + std::to_string(i+1), 6881);
        QCOMPARE(replies.size(), 1);
        const auto hex = QString(39, '0') + "8";
        QCOMPARE(crawler.candidates_.value(hex).peers.size(), 3);
        replies.first()(domain::HashState::Failed);
        QVERIFY(crawler.metadataQueue_.empty());
        QCOMPARE(crawler.candidates_.value(hex).phase, Crawler::CandidatePhase::RetryWaiting);
        crawler.candidates_[hex].expiresNs = common::PerformanceMetrics::nowNs() - 1;
        crawler.retryCandidates();
        QCOMPARE(replies.size(), 2);
        replies.first()(domain::HashState::Missing); // expired check must not change the new candidate
        QVERIFY(crawler.metadataQueue_.empty());
        replies.last()(domain::HashState::Incomplete);
        QCOMPARE(crawler.metadataQueue_.size(), size_t(1));
        QCOMPARE(crawler.candidates_.value(hex).phase, Crawler::CandidatePhase::Queued);
        crawler.stop();
    }

    void staleLookupCannotRepopulateStoppedQueue() {
        Crawler crawler(nullptr);
        crawler.running_ = true;
        std::function<void(bool)> reply;
        crawler.setKnownHashLookup([&](const QString&, std::function<void(bool)> callback) {
            reply = std::move(callback); return true;
        });
        std::array<uint8_t, 20> hash {}; hash.back() = 3;
        crawler.onAnnounce(hash, "127.0.0.1", 6881);
        QVERIFY(reply);
        crawler.stop();
        crawler.running_ = true; // next run (no real transport is needed)
        ++crawler.generation_;
        reply(false);
        QVERIFY(crawler.metadataQueue_.empty());
        crawler.stop();
        reply(false);
        QVERIFY(crawler.metadataQueue_.empty());
    }

    void oldMetadataSettlesExactlyOnceAcrossRestart() {
        Crawler crawler(nullptr);
        crawler.running_ = true;
        crawler.generation_ = 10;
        crawler.activeFetches_ = 1;
        crawler.fetchTasks_.insert(7);
        common::PerformanceMetrics::instance().gauge(QStringLiteral("crawler.metadata_inflight"), 1);
        QSignalSpy discovered(&crawler, &Crawler::discovered);
        crawler.stop();
        QCOMPARE(crawler.activeFetches_.load(), 1);
        crawler.running_ = true;
        ++crawler.generation_;
        crawler.finishMetadata(7, 10, true, domain::Torrent{});
        QCOMPARE(crawler.activeFetches_.load(), 0);
        QCOMPARE(discovered.count(), 0);
        crawler.finishMetadata(7, 10, true, domain::Torrent{});
        QCOMPARE(crawler.activeFetches_.load(), 0);
        crawler.activeFetches_ = 1;
        crawler.fetchTasks_.insert(8);
        crawler.stop();
        crawler.releaseStoppedFetches();
        crawler.finishMetadata(8, crawler.generation_, true, domain::Torrent{});
        QCOMPARE(crawler.activeFetches_.load(), 0);
        QCOMPARE(discovered.count(), 0);
    }

    void pendingLookupIsSafeAfterCrawlerDestruction() {
        auto* crawler = new Crawler(nullptr);
        crawler->running_ = true;
        auto gate = crawler->gate_;
        std::function<void(bool)> reply;
        crawler->setKnownHashLookup([&](const QString&, std::function<void(bool)> callback) {
            reply = std::move(callback); return true;
        });
        std::array<uint8_t, 20> hash {}; hash.back() = 4;
        crawler->onAnnounce(hash, "127.0.0.1", 6881);
        delete crawler;
        QVERIFY(gate->owner == nullptr);
        reply(false);
    }

    void queueHasCapacityAndStopDiscardsUnstartedRequests() {
        Crawler crawler(nullptr);
        crawler.running_ = true;
        for (int i = 0; i < 1200; ++i) {
            Crawler::MetadataRequest request;
            request.infoHash = QString::number(i);
            request.estimatedBytes = 100;
            crawler.queueMetadata(request);
        }
        QCOMPARE(crawler.metadataQueue_.size(), size_t(1024));
        crawler.stop();
        QVERIFY(crawler.metadataQueue_.empty());
        QCOMPARE(crawler.queuedBytes_, 0);
    }

    void announcementsAccountForDedupAndKnownHashes() {
        auto& m = common::PerformanceMetrics::instance();
        m.setEnabled(true);
        m.reset();
        Crawler crawler(nullptr); // No timers or network are started.
        int checks = 0;
        crawler.setKnownHashFilter([&](const QString&) { return ++checks == 2; });
        std::array<uint8_t, 20> first {}, second {};
        first.back() = 1;
        second.back() = 2;
        crawler.onAnnounce(first, "127.0.0.1", 6881);
        crawler.onAnnounce(first, "127.0.0.2", 6881);
        crawler.onAnnounce(second, "127.0.0.3", 6881);
        // Simulate an old queued request without a blocking sleep.
        crawler.metadataQueue_.front().queuedNs -= 500000000;
        crawler.refreshMetrics();
        const auto s = m.snapshot();
        const auto c = s["counters"].toObject();
        QCOMPARE(c["crawler.candidates"].toInt(), 2);
        QCOMPARE(c["crawler.recent_duplicates"].toInt(), 1);
        QCOMPARE(c["crawler.already_indexed"].toInt(), 1);
        QCOMPARE(c["crawler.metadata_queued"].toInt(), 1);
        QCOMPARE(c["crawler.known_lookup_calls"].toInt(), 2);
        const auto g = s["gauges"].toObject();
        QCOMPARE(g["crawler.metadata_queue_items"].toObject()["current"].toInt(), 1);
        QCOMPARE(g["crawler.metadata_queue_items"].toObject()["peak"].toInt(), 1);
        QVERIFY(g["crawler.metadata_queue_estimated_bytes"].toObject()["current"].toInteger() > 0);
        QVERIFY(g["crawler.metadata_queue_oldest_ms"].toObject()["current"].toInteger() >= 500);
        QCOMPARE(s["samples"].toObject()["crawler.known_lookup_ms"].toObject()["count"].toInt(), 2);
    }
};
}
QTEST_GUILESS_MAIN(rats::net::CrawlerMetricsTest)
#include "test_crawler_metrics.moc"
