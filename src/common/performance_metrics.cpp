#include "common/performance_metrics.h"

#include <QJsonArray>
#include <QMutexLocker>
#include <algorithm>
#include <chrono>
#include <cmath>

namespace rats::common {

Q_LOGGING_CATEGORY(crawlerDetail, "rats.crawler.detail", QtInfoMsg)

PerformanceMetrics::PerformanceMetrics() : startedNs_(nowNs())
{
    enabled_ = qgetenv("RATS_METRICS") != "0";
}

PerformanceMetrics& PerformanceMetrics::instance()
{
    static PerformanceMetrics metrics;
    return metrics;
}

qint64 PerformanceMetrics::nowNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

void PerformanceMetrics::increment(const QString& key, qint64 delta)
{
    if (!enabled()) return;
    QMutexLocker lock(&mutex_);
    if (counters_.contains(key) || counters_.size() < kKeyLimit)
        counters_[key] += delta;
}

void PerformanceMetrics::gauge(const QString& key, qint64 value)
{
    if (!enabled()) return;
    QMutexLocker lock(&mutex_);
    if (!gauges_.contains(key) && gauges_.size() >= kKeyLimit) return;
    auto& g = gauges_[key];
    g.current = value;
    g.peak = qMax(g.peak, value);
}

void PerformanceMetrics::adjustGauge(const QString& key, qint64 delta)
{
    if (!enabled()) return;
    QMutexLocker lock(&mutex_);
    if (!gauges_.contains(key) && gauges_.size() >= kKeyLimit) return;
    auto& g = gauges_[key];
    g.current += delta;
    g.peak = qMax(g.peak, g.current);
}

void PerformanceMetrics::observe(const QString& key, double value)
{
    if (!enabled() || !std::isfinite(value) || value < 0) return;
    QMutexLocker lock(&mutex_);
    if (!samples_.contains(key) && samples_.size() >= kKeyLimit) return;
    auto& s = samples_[key];
    const auto slot = s.count % kSampleLimit;
    if (s.recent.size() < kSampleLimit) s.recent.append(value);
    else s.recent[slot] = value;
    ++s.count;
    s.sum += value;
    s.max = qMax(s.max, value);
}

QJsonObject PerformanceMetrics::snapshot() const
{
    QHash<QString, qint64> counters;
    QHash<QString, Gauge> gauges;
    QHash<QString, Samples> samples;
    qint64 started;
    {
        QMutexLocker lock(&mutex_);
        counters = counters_;
        gauges = gauges_;
        samples = samples_;
        started = startedNs_;
    }
    QJsonObject counterJson, gaugeJson, sampleJson;
    for (auto it = counters.cbegin(); it != counters.cend(); ++it)
        counterJson[it.key()] = it.value();
    for (auto it = gauges.cbegin(); it != gauges.cend(); ++it)
        gaugeJson[it.key()] = QJsonObject { { "current", it->current }, { "peak", it->peak } };
    for (auto it = samples.cbegin(); it != samples.cend(); ++it) {
        auto sorted = it->recent;
        std::sort(sorted.begin(), sorted.end());
        auto percentile = [&](double p) { return sorted[qMax(0, int(std::ceil(p * sorted.size())) - 1)]; };
        sampleJson[it.key()] = QJsonObject {
            { "count", qint64(it->count) }, { "mean", it->sum / it->count }, { "max", it->max },
            { "retained", int(sorted.size()) }, { "p50", percentile(0.50) }, { "p95", percentile(0.95) }
        };
    }
    return { { "schema", 1 }, { "enabled", enabled() }, { "elapsed_ms", (nowNs() - started) / 1000000.0 },
        { "counters", counterJson }, { "gauges", gaugeJson }, { "samples", sampleJson } };
}

void PerformanceMetrics::reset()
{
    QMutexLocker lock(&mutex_);
    counters_.clear();
    gauges_.clear();
    samples_.clear();
    startedNs_ = nowNs();
}

PerformanceTimer::PerformanceTimer(QString key)
    : key_(std::move(key)), startedNs_(PerformanceMetrics::instance().enabled() ? PerformanceMetrics::nowNs() : 0)
{
}

PerformanceTimer::~PerformanceTimer()
{
    if (startedNs_)
        PerformanceMetrics::instance().observe(key_, (PerformanceMetrics::nowNs() - startedNs_) / 1000000.0);
}

} // namespace rats::common
