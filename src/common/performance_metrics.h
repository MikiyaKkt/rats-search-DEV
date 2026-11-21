#ifndef RATS_COMMON_PERFORMANCE_METRICS_H
#define RATS_COMMON_PERFORMANCE_METRICS_H

#include <QHash>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QMutex>
#include <QString>
#include <QVector>
#include <atomic>

namespace rats::common {

Q_DECLARE_LOGGING_CATEGORY(crawlerDetail)

// Process-lifetime counters; samples retain only the latest 1024 observations.
// Keys are fixed operation names, never hashes, queries, paths or peer addresses.
class PerformanceMetrics {
public:
    static PerformanceMetrics& instance();
    static qint64 nowNs();
    static constexpr int kSampleLimit = 1024;
    static constexpr int kKeyLimit = 128;

    bool enabled() const { return enabled_.load(std::memory_order_relaxed); }
    void setEnabled(bool enabled) { enabled_.store(enabled, std::memory_order_relaxed); }
    void increment(const QString& key, qint64 delta = 1);
    void gauge(const QString& key, qint64 value);
    void adjustGauge(const QString& key, qint64 delta);
    void observe(const QString& key, double value);
    QJsonObject snapshot() const;
    // Only at an idle measurement boundary; never while requests are active.
    void reset();

private:
    PerformanceMetrics();
    struct Gauge { qint64 current = 0; qint64 peak = 0; };
    struct Samples {
        quint64 count = 0;
        double sum = 0;
        double max = 0;
        QVector<double> recent;
    };
    mutable QMutex mutex_;
    std::atomic<bool> enabled_ { true };
    qint64 startedNs_ = 0;
    QHash<QString, qint64> counters_;
    QHash<QString, Gauge> gauges_;
    QHash<QString, Samples> samples_;
};

class PerformanceTimer {
public:
    explicit PerformanceTimer(QString key);
    ~PerformanceTimer();
    PerformanceTimer(const PerformanceTimer&) = delete;
    PerformanceTimer& operator=(const PerformanceTimer&) = delete;
private:
    QString key_;
    qint64 startedNs_;
};

} // namespace rats::common
#endif
