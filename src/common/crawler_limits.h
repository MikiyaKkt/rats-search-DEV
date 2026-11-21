#ifndef RATS_COMMON_CRAWLER_LIMITS_H
#define RATS_COMMON_CRAWLER_LIMITS_H
#include <QtGlobal>

namespace rats::common {
// Estimated application payload budgets, not process RSS limits.
struct CrawlerLimits {
    static constexpr int Discovery = 5000;
    static constexpr int Pending = 1000;
    static constexpr qint64 PendingBytes = 64LL * 1024 * 1024;
    static constexpr qint64 MetadataBytes = 4LL * 1024 * 1024;
    static constexpr int DefaultConcurrency = 10;
    static constexpr int MaxConcurrency = 64;
};
}
#endif
