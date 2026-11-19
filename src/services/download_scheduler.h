#pragma once
#include "common/download_status.h"
#include "services/download_policy.h"
#include <QHash>
#include <QSet>
#include <QVector>

namespace rats::service {
struct ScheduleTask {
    QString hash;
    DownloadPhase phase = DownloadPhase::Stopped;
    bool userPaused = false;
    bool hasMetadata = false;
    bool completed = false;
    bool failed = false;
    qint64 position = 0;
    int priority = 0;
    qint64 verifiedBytes = 0;
};
struct ScheduleDecision {
    QSet<QString> content;
    QSet<QString> metadata;
    QSet<QString> checking;
};

// Pure admission decisions. Caller executes all revocations before admissions.
// Aging promotes a low-priority waiter after at most four minutes of waiting;
// peers making no verified progress yield their download slot after 120s.
class DownloadScheduler {
public:
    ScheduleDecision reconcile(const QVector<ScheduleTask>&, const GlobalDownloadPolicy&, qint64 nowMs);
private:
    struct State {
        qint64 waitingSince = -1;
        qint64 lastProgress = -1;
        qint64 verified = 0;
        qint64 cooldownUntil = 0;
    };
    QHash<QString, State> states_;
    ScheduleDecision previous_;
    qint64 lastNow_ = -1;
};
} // namespace rats::service
