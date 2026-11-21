#ifndef RATS_COMMON_CRAWLER_TIMING_H
#define RATS_COMMON_CRAWLER_TIMING_H
#include "common/performance_metrics.h"
#include <QRandomGenerator>
#include <functional>

namespace rats::common {
// A supplied clock must be monotonic and safe on the database worker thread.
// Production leaves both callbacks empty; tests advance an atomic virtual clock.
struct CrawlerTiming {
    std::function<qint64()> clock;
    std::function<int(int)> jitter;
    qint64 nowNs() const { return clock ? clock() : PerformanceMetrics::nowNs(); }
    int jitterMs(int upper) const {
        return jitter ? qBound(0, jitter(upper), upper - 1) : QRandomGenerator::global()->bounded(upper);
    }
};
}
#endif
