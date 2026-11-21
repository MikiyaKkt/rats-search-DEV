#include "common/performance_metrics.h"
#include "common/performance_reporter.h"

#include <QtTest>
#include <thread>
#include <vector>

using rats::common::PerformanceMetrics;
using rats::common::PerformanceTimer;
using rats::common::PerformanceReporter;

class TestPerformanceMetrics : public QObject {
    Q_OBJECT
private slots:
    void init() {
        PerformanceMetrics::instance().setEnabled(true);
        PerformanceMetrics::instance().reset();
    }
    void cleanup() { PerformanceMetrics::instance().setEnabled(true); }
    void boundedSamplesAndPercentiles() {
        auto& m = PerformanceMetrics::instance();
        for (int i = 1; i <= 2048; ++i) m.observe("latency_ms", i);
        const auto sample = m.snapshot()["samples"].toObject()["latency_ms"].toObject();
        QCOMPARE(sample["count"].toInt(), 2048);
        QCOMPARE(sample["retained"].toInt(), PerformanceMetrics::kSampleLimit);
        QCOMPARE(sample["p50"].toDouble(), 1536.0);
        QCOMPARE(sample["p95"].toDouble(), 1997.0);
        QCOMPARE(sample["mean"].toDouble(), 1024.5);
    }
    void concurrentCountersAndGauges() {
        auto& m = PerformanceMetrics::instance();
        std::vector<std::thread> threads;
        for (int t = 0; t < 4; ++t) threads.emplace_back([&] {
            for (int i = 0; i < 1000; ++i) {
                m.increment("completed");
                m.adjustGauge("inflight", 1);
                m.observe("latency_ms", i);
                m.adjustGauge("inflight", -1);
            }
        });
        for (auto& t : threads) t.join();
        const auto snapshot = m.snapshot();
        QCOMPARE(snapshot["counters"].toObject()["completed"].toInt(), 4000);
        const auto g = snapshot["gauges"].toObject()["inflight"].toObject();
        QCOMPARE(g["current"].toInt(), 0);
        QVERIFY(g["peak"].toInt() >= 1);
        QCOMPARE(snapshot["samples"].toObject()["latency_ms"].toObject()["count"].toInt(), 4000);
    }
    void disabledAndKeyLimits() {
        auto& m = PerformanceMetrics::instance();
        m.setEnabled(false);
        { PerformanceTimer timer("disabled_ms"); m.increment("disabled"); m.gauge("disabled", 1); }
        QVERIFY(m.snapshot()["counters"].toObject().isEmpty());
        QVERIFY(m.snapshot()["samples"].toObject().isEmpty());
        m.setEnabled(true);
        for (int i = 0; i < 1000; ++i) {
            const auto key = QString::number(i);
            m.increment(key); m.gauge(key, i); m.observe(key, i);
        }
        const auto s = m.snapshot();
        QCOMPARE(s["counters"].toObject().size(), PerformanceMetrics::kKeyLimit);
        QCOMPARE(s["gauges"].toObject().size(), PerformanceMetrics::kKeyLimit);
        QCOMPARE(s["samples"].toObject().size(), PerformanceMetrics::kKeyLimit);
    }
    void reporterRatesAndMainThreadDelay() {
        auto& m = PerformanceMetrics::instance();
        PerformanceReporter reporter;
        reporter.start();
        QSignalSpy before(&reporter, &PerformanceReporter::beforeSnapshot);
        m.increment("crawler.metadata_succeeded", 3);
        m.increment("crawler.metadata_failed", 1);
        m.increment("crawler.metadata_timeouts", 1);
        m.increment("crawler.index_inserted", 2);
        QTest::qWait(20);
        auto report = reporter.report();
        QCOMPARE(before.size(), 1);
        QCOMPARE(report["metadata_success_ratio"].toDouble(), 0.75);
        QCOMPARE(report["metadata_timeout_ratio"].toDouble(), 0.25);
        QVERIFY(report["inserted_per_minute"].toDouble() > 0);
        QThread::msleep(250); // Deliberately stall this thread beyond the 200ms probe.
        QCoreApplication::processEvents();
        report = reporter.report();
        QVERIFY(report["samples"].toObject()["main.event_loop_drift_ms"].toObject()["max"].toDouble() >= 40);
        QCOMPARE(report["inserted_per_minute"].toDouble(), 0.0);
        reporter.stop();
    }
};
QTEST_GUILESS_MAIN(TestPerformanceMetrics)
#include "test_performance_metrics.moc"
