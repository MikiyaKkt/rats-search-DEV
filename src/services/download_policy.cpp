#include "services/download_policy.h"

#include <QSet>
#include <cmath>
#include <limits>

namespace rats::service {
namespace {
constexpr qint64 maxExact = 9007199254740991LL;
bool integer(const QJsonValue& v, qint64 max, qint64& result)
{
    if (!v.isDouble()) return false;
    const double n = v.toDouble();
    if (!std::isfinite(n) || n < 0 || n > double(max) || std::floor(n) != n) return false;
    result = qint64(n);
    return true;
}
bool keys(const QJsonObject& o, const QSet<QString>& allowed, QString& error)
{
    for (auto i = o.begin(); i != o.end(); ++i) {
        if (!allowed.contains(i.key())) { error = "Unknown policy field: " + i.key(); return false; }
    }
    return true;
}
bool read(const QJsonObject& o, const QString& key, qint64 max, qint64& value, QString& error)
{
    if (!o.contains(key)) return true;
    if (integer(o[key], max, value)) return true;
    error = key + " must be a nonnegative exact integer <= " + QString::number(max);
    return false;
}
qint64 cap(qint64 global, qint64 task)
{
    return global == 0 ? task : task == 0 ? global : qMin(global, task);
}
}

QString GlobalDownloadPolicy::validate() const
{
    if(schemaVersion!=2 || cacheBytes<0 || cacheBytes>268435456)return "Invalid policy schema or cache budget";
    if(const auto error=schedule.validate();!error.isEmpty())return error;
    if(const auto error=seeding.validate();!error.isEmpty())return error;
    if (downloadRate < 0 || downloadRate > maxExact || uploadRate < 0 || uploadRate > maxExact
        || revision < 0 || revision > maxExact) return "Invalid rate or revision";
    if (maxConnections < 1 || maxConnections > 10000 || taskConnections < 1 || taskConnections > 10000)
        return "Connection limits must be between 1 and 10000";
    for (int count : { downloadSlots, seedSlots, metadataSlots, checkingSlots })
        if (count < 1 || count > 1000) return "Slot limits must be between 1 and 1000";
    return {};
}
QJsonObject GlobalDownloadPolicy::toJson() const
{
    return {{"schemaVersion",schemaVersion},{"cacheBytes",cacheBytes},{"schedule",schedule.toJson()},{"seeding",seeding.toJson()},
        {"downloadLimitBps", downloadRate}, {"uploadLimitBps", uploadRate}, {"maxConnections", maxConnections},
        {"taskConnections", taskConnections}, {"downloadSlots", downloadSlots}, {"seedSlots", seedSlots},
        {"metadataSlots", metadataSlots}, {"checkingSlots", checkingSlots}, {"revision", revision}};
}
bool GlobalDownloadPolicy::fromJson(const QJsonObject& o, GlobalDownloadPolicy& out, QString& error)
{
    error.clear();
    if (!keys(o, {"downloadLimitBps", "uploadLimitBps", "maxConnections", "taskConnections", "downloadSlots",
            "seedSlots", "metadataSlots", "checkingSlots", "revision", "schemaVersion", "cacheBytes", "schedule", "seeding"}, error)) return false;
    GlobalDownloadPolicy p = out;
    if(o.contains("schemaVersion") && o["schemaVersion"]!=QJsonValue(1) && o["schemaVersion"]!=QJsonValue(2)){error="Unsupported policy schema";return false;}
    p.schemaVersion=2;
    if(!read(o,"cacheBytes",268435456,p.cacheBytes,error))return false;
    if(o.contains("schedule") && (!o["schedule"].isObject() || !BandwidthSchedule::fromJson(o["schedule"].toObject(),p.schedule,error)))return false;
    if(o.contains("seeding") && (!o["seeding"].isObject() || !SeedingPolicy::fromJson(o["seeding"].toObject(),p.seeding,error)))return false;
    if (!read(o, "downloadLimitBps", maxExact, p.downloadRate, error)
        || !read(o, "uploadLimitBps", maxExact, p.uploadRate, error)
        || !read(o, "revision", maxExact, p.revision, error)) return false;
    for (auto entry : { std::pair<const char*, int*>{"maxConnections", &p.maxConnections},
             {"taskConnections", &p.taskConnections}, {"downloadSlots", &p.downloadSlots},
             {"seedSlots", &p.seedSlots}, {"metadataSlots", &p.metadataSlots}, {"checkingSlots", &p.checkingSlots} }) {
        qint64 n = *entry.second;
        if (!read(o, entry.first, 10000, n, error)) return false;
        *entry.second = int(n);
    }
    error = p.validate();
    if (!error.isEmpty()) return false;
    out = p;
    return true;
}
QString TaskDownloadPolicy::validate() const
{
    if(strategy!="rarestFirst" && strategy!="sequential" && strategy!="firstLast")return "Unknown download strategy";
    if(requestWindow<1 || requestWindow>128 || windowMin<1 || windowMin>windowMax || windowMax>128)return "Request window must be 1..128 with min <= max";
    if(seeding)if(const auto error=seeding->validate();!error.isEmpty())return error;
    for (auto rate : {downloadRate, uploadRate})
        if (rate && (*rate < 0 || *rate > maxExact)) return "Invalid task rate";
    if (maxConnections && (*maxConnections < 1 || *maxConnections > 10000)) return "Invalid task connection limit";
    if (priority < -1 || priority > 1) return "Priority must be -1, 0 or 1";
    if (revision < 0 || revision > maxExact) return "Invalid revision";
    return {};
}
QJsonObject TaskDownloadPolicy::toJson() const
{
    return {{"strategy",strategy},{"automaticWindow",automaticWindow},{"requestWindow",requestWindow},{"windowMin",windowMin},{"windowMax",windowMax},
        {"seeding",seeding?QJsonValue(seeding->toJson()):QJsonValue(QJsonValue::Null)},
        {"downloadLimitBps", downloadRate ? QJsonValue(*downloadRate) : QJsonValue(QJsonValue::Null)},
        {"uploadLimitBps", uploadRate ? QJsonValue(*uploadRate) : QJsonValue(QJsonValue::Null)},
        {"maxConnections", maxConnections ? QJsonValue(*maxConnections) : QJsonValue(QJsonValue::Null)},
        {"queuePriority", priority < 0 ? "low" : priority > 0 ? "high" : "normal"}, {"revision", revision}};
}
bool TaskDownloadPolicy::fromJson(const QJsonObject& o, TaskDownloadPolicy& out, QString& error)
{
    error.clear();
    if (!keys(o, {"downloadLimitBps", "uploadLimitBps", "maxConnections", "queuePriority", "revision", "strategy", "automaticWindow", "requestWindow", "windowMin", "windowMax", "seeding"}, error)) return false;
    TaskDownloadPolicy p = out;
    if(o.contains("strategy")) {if(!o["strategy"].isString()){error="Invalid strategy";return false;}p.strategy=o["strategy"].toString();}
    if(o.contains("automaticWindow")){if(!o["automaticWindow"].isBool()){error="Invalid window mode";return false;}p.automaticWindow=o["automaticWindow"].toBool();}
    for(auto entry:{std::pair<const char*,int*>{"requestWindow",&p.requestWindow},{"windowMin",&p.windowMin},{"windowMax",&p.windowMax}}) {
        qint64 n=*entry.second;if(!read(o,entry.first,128,n,error))return false;*entry.second=int(n);
    }
    if(o.contains("seeding")) {if(o["seeding"].isNull())p.seeding.reset();else {SeedingPolicy seed;if(!o["seeding"].isObject() || !SeedingPolicy::fromJson(o["seeding"].toObject(),seed,error))return false;p.seeding=seed;}}
    for (auto entry : {std::pair<const char*, std::optional<qint64>*>{"downloadLimitBps", &p.downloadRate},
             {"uploadLimitBps", &p.uploadRate}}) {
        if (!o.contains(entry.first)) continue;
        if (o[entry.first].isNull()) { entry.second->reset(); continue; }
        qint64 n = 0;
        if (!read(o, entry.first, maxExact, n, error)) return false;
        *entry.second = n;
    }
    if (o.contains("maxConnections")) {
        if (o["maxConnections"].isNull()) p.maxConnections.reset();
        else {
            qint64 n = 0;
            if (!read(o, "maxConnections", 10000, n, error)) return false;
            p.maxConnections = int(n);
        }
    }
    if (o.contains("queuePriority")) {
        const auto v = o["queuePriority"];
        if (!v.isString() || (v.toString() != "low" && v.toString() != "normal" && v.toString() != "high")) {
            error = "queuePriority must be low, normal or high"; return false;
        }
        p.priority = v.toString() == "low" ? -1 : v.toString() == "high" ? 1 : 0;
    }
    if (!read(o, "revision", maxExact, p.revision, error)) return false;
    error = p.validate();
    if (!error.isEmpty()) return false;
    out = p;
    return true;
}
EffectivePolicy resolveEffectivePolicy(const GlobalDownloadPolicy& g, const TaskDownloadPolicy& t)
{
    return {cap(g.downloadRate, t.downloadRate.value_or(0)), cap(g.uploadRate, t.uploadRate.value_or(0)),
        qMin(g.maxConnections, t.maxConnections.value_or(g.taskConnections))};
}
GlobalDownloadPolicy scheduledPolicy(const GlobalDownloadPolicy& base,const QDateTime& utc) {
    auto p=base;const auto schedule=p.schedule.resolve(utc);
    p.downloadRate=cap(p.downloadRate,schedule.downloadRate);p.uploadRate=cap(p.uploadRate,schedule.uploadRate);return p;
}
QJsonObject EffectivePolicy::toJson() const
{
    return {{"downloadLimitBps", downloadRate}, {"uploadLimitBps", uploadRate}, {"maxConnections", maxConnections}};
}
} // namespace rats::service
