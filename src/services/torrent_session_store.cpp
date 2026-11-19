#include "services/torrent_session_store.h"
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <cmath>

namespace rats::service {
bool TorrentSessionStore::save(const QString& path, const QVector<Download>& downloads) const
{
    return saveDocument(path, {2, 0, downloads});
}
bool TorrentSessionStore::saveDocument(const QString& path, const SessionDocument& document, QString* error) const
{
    QJsonArray entries;
    for (const auto& d : document.downloads) {
        auto entry = d.toJson();
        entry["savePath"] = d.savePath;
        entry["taskPolicy"] = d.policy.toJson();
        entry["queuePosition"] = d.queuePosition;
        entry["userIntent"] = d.paused ? "paused" : "running";
        entries.append(entry);
    }
    QJsonObject object {{"schemaVersion", document.schemaVersion}, {"revision", document.revision}, {"downloads", entries}};
    auto message = validateDocument(object);
    if (!message.isEmpty()) { if (error) *error = message; return false; }
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        if (error) *error = "Cannot create session directory";
        return false;
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) { if (error) *error = file.errorString(); return false; }
    const auto bytes = QJsonDocument(object).toJson();
    if (file.write(bytes) != bytes.size() || !file.commit()) { if (error) *error = file.errorString(); return false; }
    if (error) error->clear();
    return true;
}
QVector<Download> TorrentSessionStore::load(const QString& path) const
{
    const auto result = loadDocument(path);
    if (!result.ok()) qWarning() << "TorrentSessionStore:" << result.error;
    return result.document.downloads;
}
SessionLoadResult TorrentSessionStore::loadDocument(const QString& path) const
{
    SessionLoadResult result;
    QFile file(path);
    if (!file.exists()) return result;
    if (!file.open(QIODevice::ReadOnly)) {
        result.status = SessionLoadResult::IoError;
        result.error = file.errorString(); return result;
    }
    QJsonParseError parse;
    const auto doc = QJsonDocument::fromJson(file.readAll(), &parse);
    if (parse.error != QJsonParseError::NoError) {
        result.status = SessionLoadResult::Invalid; result.error = parse.errorString(); return result;
    }
    const bool legacy = doc.isArray();
    const auto object = legacy ? migrateLegacyArray(doc.array()) : doc.object();
    result.error = validateDocument(object);
    if (!result.error.isEmpty()) { result.status = SessionLoadResult::Invalid; return result; }
    result.status = (legacy || object["schemaVersion"]==QJsonValue(1)) ? SessionLoadResult::Migrated : SessionLoadResult::Loaded;
    result.document.revision = object["revision"].toInteger();
    for (const auto& value : object["downloads"].toArray()) {
        const auto entry = value.toObject();
        Download d;
        d.hash = entry["hash"].toString().toLower();
        d.name = entry["name"].toString();
        d.savePath = entry["savePath"].toString();
        d.totalSize = entry["totalSize"].toInteger();
        d.paused = entry["userIntent"].toString() == "paused";
        d.selectionConfirmed=entry["selectionConfirmed"].toBool(false);
        d.selectionRevision=entry["selectionRevision"].toInteger();
        if(!entry.contains("selectionConfirmed")) d.paused=true;
        d.removeOnDone = entry["removeOnDone"].toBool();
        d.completed = entry["completed"].toBool(); // history; rechecked by the service
        d.downloadedBytes = entry["downloadedBytes"].toInteger();
        d.progress = entry["progress"].toDouble();
        d.verifiedBytes=entry["verifiedBytes"].toInteger();
        d.wantedBytes=entry["wantedBytes"].toInteger();
        d.wantedVerifiedBytes=entry["wantedVerifiedBytes"].toInteger();
        d.seededMilliseconds=entry["seededMilliseconds"].toInteger();d.seedingUploadedBytes=entry["seedingUploadedBytes"].toInteger();d.seedingStopped=entry["seedingStopped"].toBool();
        d.queuePosition = entry["queuePosition"].toInteger();
        QString ignored;
        TaskDownloadPolicy::fromJson(entry["taskPolicy"].toObject(), d.policy, ignored);
        for (const auto& fileValue : entry["files"].toArray()) {
            auto f = fileValue.toObject();
            DownloadFile fileEntry;
            fileEntry.path = f["path"].toString();
            fileEntry.size = f["size"].toInteger();
            fileEntry.index = f.contains("index") ? f["index"].toInt() : int(d.files.size());
            fileEntry.selected = f["selected"].toBool(true);
            fileEntry.priority=f["priority"].toInt(fileEntry.selected?1:0);
            fileEntry.progress = f["progress"].toDouble();
            fileEntry.verifiedBytes=f["verifiedBytes"].toInteger(qRound64(fileEntry.progress*fileEntry.size));
            d.files.append(fileEntry);
        }
        result.document.downloads.append(d);
    }
    return result;
}
QJsonObject TorrentSessionStore::migrateLegacyArray(const QJsonArray& array)
{
    QJsonArray migrated;
    for (const auto& value : array) {
        if (!value.isObject()) { migrated.append(value); continue; }
        auto entry = value.toObject();
        entry["taskPolicy"] = TaskDownloadPolicy{}.toJson();
        entry["queuePosition"] = migrated.size();
        // Legacy arrays do not establish reliable content-selection intent.
        // Preserve the task and selection history, but require manual resume.
        entry["paused"] = true;
        entry["userIntent"] = "paused";
        migrated.append(entry);
    }
    return {{"schemaVersion", 1}, {"revision", 0}, {"downloads", migrated}};
}
QString TorrentSessionStore::validateDocument(const QJsonObject& doc)
{
    auto exact = [](QJsonValue v) {
        const double n = v.toDouble(-1);
        return v.isDouble() && std::isfinite(n) && n >= 0 && n <= 9007199254740991.0 && std::floor(n) == n;
    };
    if (doc["schemaVersion"] != QJsonValue(1) && doc["schemaVersion"] != QJsonValue(2)) return "Unsupported session schemaVersion";
    if (!exact(doc["revision"]) || !doc["downloads"].isArray()) return "Invalid session document";
    QSet<QString> hashes;
    const QRegularExpression hashPattern("^[a-fA-F0-9]{40}$");
    for (const auto& value : doc["downloads"].toArray()) {
        if (!value.isObject()) return "Session entry must be an object";
        const auto entry = value.toObject();
        const auto hash = entry["hash"].toString().toLower();
        if (!hashPattern.match(hash).hasMatch() || hashes.contains(hash)) return "Invalid or duplicate session hash";
        hashes.insert(hash);
        if (!entry["savePath"].isString() || entry["savePath"].toString().isEmpty()) return "Missing session savePath";
        if (!exact(entry["queuePosition"])) return "Invalid queue position";
        if (entry["userIntent"] != QJsonValue("paused") && entry["userIntent"] != QJsonValue("running"))
            return "Invalid user intent";
        if (!entry["taskPolicy"].isObject()) return "Missing task policy";
        TaskDownloadPolicy policy;
        QString error;
        if (!TaskDownloadPolicy::fromJson(entry["taskPolicy"].toObject(), policy, error)) return error;
        for (const auto& name : {"totalSize", "downloadedBytes", "verifiedBytes", "wantedBytes", "wantedVerifiedBytes"}) {
            if (entry.contains(name) && !exact(entry[name])) return QString("Invalid session counter: ") + name;
        }
        for (const auto& name : {"paused", "removeOnDone", "completed"}) {
            if (entry.contains(name) && !entry[name].isBool()) return QString("Invalid session flag: ") + name;
        }
        if (entry.contains("progress") && (!entry["progress"].isDouble()
                || entry["progress"].toDouble() < 0 || entry["progress"].toDouble() > 1))
            return "Invalid session progress";
        if (entry.contains("files") && !entry["files"].isArray()) return "Invalid session files";
        if(entry.contains("selectionRevision") && !exact(entry["selectionRevision"])) return "Invalid selection revision";
        if(entry.contains("selectionConfirmed") && !entry["selectionConfirmed"].isBool()) return "Invalid selection confirmation";
        for(const auto& name:{"seededMilliseconds","seedingUploadedBytes"})if(entry.contains(name) && !exact(entry[name]))return "Invalid seeding counter";
        if(entry.contains("seedingStopped") && !entry["seedingStopped"].isBool())return "Invalid seeding stop state";
        QSet<qint64> indexes;
        qint64 ordinal = 0;
        for (const auto& file : entry["files"].toArray()) {
            if (!file.isObject()) return "Invalid session file entry";
            const auto f = file.toObject();
            if (!f["path"].isString() || !exact(f["size"])) return "Invalid session file path/size";
            if (f.contains("index") && (!exact(f["index"]) || f["index"].toDouble() > 2147483647.0))
                return "Invalid session file index";
            const qint64 index = f.contains("index") ? f["index"].toInteger() : ordinal;
            if (indexes.contains(index)) return "Duplicate session file index";
            indexes.insert(index);
            ++ordinal;
            if(f.contains("priority") && (!exact(f["priority"]) || f["priority"].toDouble()>2)) return "Invalid file priority";
            if(f.contains("verifiedBytes") && (!exact(f["verifiedBytes"]) || f["verifiedBytes"].toDouble()>f["size"].toDouble())) return "Invalid file verified bytes";
            if (f.contains("selected") && !f["selected"].isBool()) return "Invalid file selection";
            if (f.contains("progress") && (!f["progress"].isDouble()
                    || f["progress"].toDouble() < 0 || f["progress"].toDouble() > 1)) return "Invalid file progress";
        }
    }
    return {};
}
} // namespace rats::service
