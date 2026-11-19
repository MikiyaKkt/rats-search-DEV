#pragma once

#include <QJsonObject>
#include <QString>
#include <optional>
#include "services/download_advanced_policy.h"

namespace rats::service {

// Wire rates are exact JSON integers in bytes/second. Zero removes only the
// current layer's cap; it never overrides a parent cap.
struct GlobalDownloadPolicy {
    int schemaVersion=2;
    qint64 cacheBytes=0; // opt-in shared read cache, bounded across all torrents
    BandwidthSchedule schedule;
    SeedingPolicy seeding;
    qint64 downloadRate = 0;
    qint64 uploadRate = 0;
    int maxConnections = 200;
    int taskConnections = 50;
    int downloadSlots = 3;
    int seedSlots = 2;
    int metadataSlots = 2;
    int checkingSlots = 1;
    qint64 revision = 0;

    QString validate() const;
    QJsonObject toJson() const;
    static bool fromJson(const QJsonObject&, GlobalDownloadPolicy&, QString& error);
};

struct TaskDownloadPolicy {
    QString strategy="rarestFirst";
    bool automaticWindow=true;
    int requestWindow=32,windowMin=2,windowMax=32;
    std::optional<SeedingPolicy> seeding;
    std::optional<qint64> downloadRate;
    std::optional<qint64> uploadRate;
    std::optional<int> maxConnections;
    int priority = 0; // -1 low, 0 normal, 1 high; queue admission only
    qint64 revision = 0;

    QString validate() const;
    QJsonObject toJson() const;
    static bool fromJson(const QJsonObject&, TaskDownloadPolicy&, QString& error);
};

struct EffectivePolicy {
    qint64 downloadRate;
    qint64 uploadRate;
    int maxConnections;
    QJsonObject toJson() const;
};
EffectivePolicy resolveEffectivePolicy(const GlobalDownloadPolicy&, const TaskDownloadPolicy&);
GlobalDownloadPolicy scheduledPolicy(const GlobalDownloadPolicy&,const QDateTime& utc);

} // namespace rats::service
