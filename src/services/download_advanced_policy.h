#pragma once
#include <QDateTime>
#include <QJsonObject>
#include <QString>
#include <QVector>

namespace rats::service {
struct SeedingPolicy {
    qint64 ratioPermille=0, durationSeconds=0;
    bool all=false;
    QString validate() const;
    bool shouldStop(qint64 uploaded,qint64 wanted,qint64 seconds) const;
    QJsonObject toJson() const;
    static bool fromJson(const QJsonObject&,SeedingPolicy&,QString&);
};
struct ScheduleRule {
    int days=127,startMinute=0,endMinute=1440; // Monday bit 0; end exclusive; overnight allowed
    qint64 downloadRate=0,uploadRate=0;
    bool pause=false;
    QJsonObject toJson() const;
};
struct ScheduleResolution {
    qint64 downloadRate=0,uploadRate=0;
    bool pause=false;
    bool operator==(const ScheduleResolution& b) const {return downloadRate==b.downloadRate && uploadRate==b.uploadRate && pause==b.pause;}
};
struct BandwidthSchedule {
    bool enabled=false;
    QString timeZone="UTC";
    QVector<ScheduleRule> rules;
    QString validate() const;
    ScheduleResolution resolve(const QDateTime& utc) const;
    QDateTime nextChange(const QDateTime& utc) const;
    QJsonObject toJson() const;
    static bool fromJson(const QJsonObject&,BandwidthSchedule&,QString&);
};
}
