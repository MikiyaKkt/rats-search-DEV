#include "services/download_advanced_policy.h"
#include <QJsonArray>
#include <QTimeZone>
#include <QSet>
#include <cmath>
#include <array>

namespace rats::service {
namespace {
bool exact(const QJsonValue& v,qint64 max,qint64& out) {
    const double n=v.toDouble(-1);if(!v.isDouble() || !std::isfinite(n) || n<0 || n>max || std::floor(n)!=n)return false;
    out=qint64(n);return true;
}
bool fields(const QJsonObject& o,const QSet<QString>& names) {for(auto i=o.begin();i!=o.end();++i)if(!names.contains(i.key()))return false;return true;}
bool matches(const ScheduleRule& r,int day,int minute) {
    if(r.startMinute<r.endMinute)return (r.days&(1<<day)) && minute>=r.startMinute && minute<r.endMinute;
    return ((r.days&(1<<day)) && minute>=r.startMinute) || ((r.days&(1<<((day+6)%7))) && minute<r.endMinute);
}
}
QString SeedingPolicy::validate() const {
    if(ratioPermille<0 || ratioPermille>1000000 || durationSeconds<0 || durationSeconds>315360000)return "Invalid seeding ratio or duration";
    return {};
}
bool SeedingPolicy::shouldStop(qint64 uploaded,qint64 wanted,qint64 seconds) const {
    // All counters are JSON-safe integers. Split the multiplication so the
    // exact threshold cannot overflow or round prematurely at large sizes.
    const bool ratio=ratioPermille>0 && wanted>0 && wanted<=9007199254740991LL
        && uploaded>=wanted*(ratioPermille/1000)+(wanted*(ratioPermille%1000)+999)/1000;
    const bool duration=durationSeconds>0 && seconds>=durationSeconds;
    if(all)return (ratioPermille>0 || durationSeconds>0) && (ratioPermille==0 || ratio) && (durationSeconds==0 || duration);
    return ratio || duration;
}
QJsonObject SeedingPolicy::toJson() const {return {{"ratioPermille",ratioPermille},{"durationSeconds",durationSeconds},{"mode",all?"all":"any"}};}
bool SeedingPolicy::fromJson(const QJsonObject& o,SeedingPolicy& out,QString& error) {
    SeedingPolicy p;
    if(!fields(o,{"ratioPermille","durationSeconds","mode"}) || !exact(o["ratioPermille"],1000000,p.ratioPermille)
        || !exact(o["durationSeconds"],315360000,p.durationSeconds) || (o["mode"]!="any" && o["mode"]!="all")) {error="Invalid seeding policy";return false;}
    p.all=o["mode"]=="all";out=p;error.clear();return true;
}
QJsonObject ScheduleRule::toJson() const {return {{"days",days},{"startMinute",startMinute},{"endMinute",endMinute},{"downloadLimitBps",downloadRate},{"uploadLimitBps",uploadRate},{"pause",pause}};}
QString BandwidthSchedule::validate() const {
    if(!QTimeZone(timeZone.toUtf8()).isValid())return "Unknown IANA time zone";
    if(rules.size()>64)return "At most 64 schedule rules are supported";
    std::array<bool,10080> occupied{};
    for(const auto& r:rules) {
        if(r.days<1 || r.days>127 || r.startMinute<0 || r.startMinute>1439 || r.endMinute<0 || r.endMinute>1440 || r.startMinute==r.endMinute
            || r.downloadRate<0 || r.uploadRate<0 || r.downloadRate>9007199254740991LL || r.uploadRate>9007199254740991LL)return "Invalid schedule interval or rate";
        for(int day=0;day<7;++day)for(int minute=0;minute<1440;++minute)if(matches(r,day,minute)) {
            auto& used=occupied[day*1440+minute];if(used)return "Schedule intervals must not overlap";used=true;
        }
    }return {};
}
ScheduleResolution BandwidthSchedule::resolve(const QDateTime& utc) const {
    if(!enabled || !utc.isValid())return {};
    const auto local=utc.toTimeZone(QTimeZone(timeZone.toUtf8()));const int minute=local.time().hour()*60+local.time().minute();
    for(const auto& r:rules)if(matches(r,local.date().dayOfWeek()-1,minute))return {r.downloadRate,r.uploadRate,r.pause};
    return {};
}
QDateTime BandwidthSchedule::nextChange(const QDateTime& utc) const {
    if(!enabled || !utc.isValid())return {};
    const auto current=resolve(utc);auto candidate=QDateTime::fromMSecsSinceEpoch((utc.toMSecsSinceEpoch()/60000+1)*60000, QTimeZone::UTC);
    // Scan absolute minutes: skipped/repeated local DST minutes require no special-case arithmetic.
    for(int minute=0;minute<8*1440;++minute,candidate=candidate.addSecs(60))if(!(resolve(candidate)==current))return candidate;
    return {};
}
QJsonObject BandwidthSchedule::toJson() const {QJsonArray a;for(const auto& r:rules)a.append(r.toJson());return {{"enabled",enabled},{"timeZone",timeZone},{"rules",a}};}
bool BandwidthSchedule::fromJson(const QJsonObject& o,BandwidthSchedule& out,QString& error) {
    if(!fields(o,{"enabled","timeZone","rules"}) || !o["enabled"].isBool() || !o["timeZone"].isString() || !o["rules"].isArray() || o["rules"].toArray().size()>64){error="Invalid bandwidth schedule";return false;}
    BandwidthSchedule p;p.enabled=o["enabled"].toBool();p.timeZone=o["timeZone"].toString();
    for(const auto& value:o["rules"].toArray()) {
        if(!value.isObject()){error="Invalid schedule rule";return false;}const auto r=value.toObject();ScheduleRule rule;qint64 days,start,end;
        if(!fields(r,{"days","startMinute","endMinute","downloadLimitBps","uploadLimitBps","pause"}) || !exact(r["days"],127,days) || !exact(r["startMinute"],1439,start) || !exact(r["endMinute"],1440,end)
            || !exact(r["downloadLimitBps"],9007199254740991LL,rule.downloadRate) || !exact(r["uploadLimitBps"],9007199254740991LL,rule.uploadRate) || !r["pause"].isBool()){error="Invalid schedule rule";return false;}
        rule.days=int(days);rule.startMinute=int(start);rule.endMinute=int(end);rule.pause=r["pause"].toBool();p.rules.append(rule);
    }error=p.validate();if(!error.isEmpty())return false;out=p;return true;
}
}
