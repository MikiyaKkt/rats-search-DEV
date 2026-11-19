#include "services/download_scheduler.h"
#include <algorithm>

namespace rats::service {
ScheduleDecision DownloadScheduler::reconcile(const QVector<ScheduleTask>& tasks, const GlobalDownloadPolicy& policy, qint64 now)
{
    if (now < 0) now = 0;
    if (now < lastNow_) { states_.clear(); previous_ = {}; }
    lastNow_ = now;
    ScheduleDecision decision;
    QSet<QString> present;
    QVector<ScheduleTask> candidates;
    for (const auto& task : tasks) {
        present.insert(task.hash);
        auto& state = states_[task.hash];
        if (state.lastProgress < 0 || task.verifiedBytes != state.verified) {
            state.lastProgress = now; state.verified = task.verifiedBytes;
        }
        if (task.userPaused || task.failed) { state.waitingSince = -1; continue; }
        const bool active = previous_.content.contains(task.hash) || previous_.metadata.contains(task.hash)
            || previous_.checking.contains(task.hash);
        if (active) state.waitingSince = -1;
        else if (state.waitingSince < 0) state.waitingSince = now;
        candidates.append(task);
    }
    for (auto i = states_.begin(); i != states_.end();) {
        if (!present.contains(i.key())) i = states_.erase(i); else ++i;
    }
    const bool waitingDownload = std::any_of(candidates.begin(), candidates.end(), [&](const auto& t) {
        return t.hasMetadata && !t.completed && t.phase != DownloadPhase::Checking
            && !previous_.content.contains(t.hash) && states_[t.hash].cooldownUntil <= now;
    });
    if (waitingDownload) for (const auto& t : candidates) {
        auto& s = states_[t.hash];
        if (previous_.content.contains(t.hash) && !t.completed && t.hasMetadata
            && t.phase == DownloadPhase::Downloading && now - s.lastProgress >= 120000) {
            s.cooldownUntil = now + 60000;
            s.waitingSince = now;
        }
    }
    std::sort(candidates.begin(), candidates.end(), [&](const auto& a, const auto& b) {
        auto score = [&](const auto& t) {
            const auto& s = states_[t.hash];
            const auto age = s.waitingSince < 0 ? 0 : (now - s.waitingSince) / 60000;
            return qint64(t.priority) + age;
        };
        if (score(a) != score(b)) return score(a) > score(b);
        if (a.position != b.position) return a.position < b.position;
        return a.hash < b.hash;
    });
    int downloads = 0, seeds = 0, metadata = 0, checking = 0;
    for (const auto& t : candidates) {
        auto& s = states_[t.hash];
        if (s.cooldownUntil > now) continue;
        if (t.phase == DownloadPhase::Checking) {
            if (checking++ < policy.checkingSlots) decision.checking.insert(t.hash);
        } else if (!t.hasMetadata) {
            if (metadata++ < policy.metadataSlots) decision.metadata.insert(t.hash);
        } else if (t.completed) {
            if (seeds++ < policy.seedSlots) decision.content.insert(t.hash);
        } else if (downloads++ < policy.downloadSlots) decision.content.insert(t.hash);
        const bool admitted = decision.content.contains(t.hash) || decision.metadata.contains(t.hash)
            || decision.checking.contains(t.hash);
        if (admitted && !previous_.content.contains(t.hash)) s.lastProgress = now;
    }
    previous_ = decision;
    return decision;
}
} // namespace rats::service
