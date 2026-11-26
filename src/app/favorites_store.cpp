#include "app/favorites_store.h"

#include "common/infohash.h"
#include "domain/torrent_codec.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

namespace rats::app {

namespace {

QSet<QString> normalizedHashes(const QSet<QString>& hashes)
{
    QSet<QString> normalized;
    for (const auto& hash : hashes)
        normalized.insert(infohash::normalize(hash));
    return normalized;
}

bool completeFiles(const domain::Torrent& torrent)
{
    return !torrent.fileList.isEmpty() && torrent.fileList.size() >= torrent.files;
}

} // namespace

FavoritesStore::FavoritesStore(const QString& dataDirectory, QObject* parent)
    : QObject(parent), filePath_(dataDirectory + QStringLiteral("/favorites.json"))
{
    load();
}

void FavoritesStore::load()
{
    QFile file(filePath_);
    if (!file.exists())
        return;
    if (!file.open(QIODevice::ReadOnly)) {
        loadError_ = file.errorString();
        return;
    }
    const auto document = QJsonDocument::fromJson(file.readAll());
    const auto object = document.object();
    const bool supported = object["version"].toInt() == 1 && object["favorites"].isArray();
    if (!document.isArray() && !supported) {
        loadError_ = tr("Favorites file could not be read or has an unsupported format.");
        return;
    }

    for (const auto value : object["folders"].toArray()) {
        const auto name = value.toString();
        if (!name.isEmpty() && !folders_.contains(name))
            folders_.append(name);
    }
    const auto entries = document.isArray() ? document.array() : object["favorites"].toArray();
    for (const auto value : entries) {
        const auto json = value.toObject();
        auto torrent = domain::codec::torrentFromJson(json);
        if (!torrent.isValid() || hashes_.contains(torrent.hash))
            continue;
        const qint64 timestamp = json["favoritedAt"].toInteger();
        const auto added = timestamp > 0 ? QDateTime::fromMSecsSinceEpoch(timestamp) : QDateTime::currentDateTime();
        const auto folder = json["folder"].toString();
        if (!folder.isEmpty() && !folders_.contains(folder))
            folders_.append(folder);
        hashes_.insert(torrent.hash);
        favorites_.append({ std::move(torrent), added, folder });
    }
}

bool FavoritesStore::commit(QVector<Entry> entries, QStringList folders)
{
    if (!loadError_.isEmpty()) {
        emit saveFailed(loadError_);
        return false;
    }
    QJsonArray array;
    QSet<QString> hashes;
    for (const auto& entry : entries) {
        auto json = domain::codec::toJson(entry.torrent, { completeFiles(entry.torrent), false });
        json["favoritedAt"] = entry.favoritedAt.toMSecsSinceEpoch();
        if (!entry.folder.isEmpty())
            json["folder"] = entry.folder;
        array.append(json);
        hashes.insert(entry.torrent.hash);
    }
    const QJsonDocument document(QJsonObject { { "version", 1 },
        { "folders", QJsonArray::fromStringList(folders) }, { "favorites", array } });
    const auto bytes = document.toJson(QJsonDocument::Compact);
    QSaveFile file(filePath_);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        emit saveFailed(file.errorString());
        return false;
    }
    favorites_ = std::move(entries);
    folders_ = std::move(folders);
    hashes_ = std::move(hashes);
    emit favoritesChanged();
    return true;
}

bool FavoritesStore::add(const domain::Torrent& torrent)
{
    return addMany({ torrent }) != 0;
}

bool FavoritesStore::remove(const QString& hash)
{
    return removeMany({ hash }) != 0;
}

qsizetype FavoritesStore::addMany(const QVector<domain::Torrent>& torrents, const QString& folder)
{
    if (!folder.isEmpty() && !folders_.contains(folder))
        return 0;
    auto entries = favorites_;
    auto seen = hashes_;
    const auto added = QDateTime::currentDateTime();
    for (auto torrent : torrents) {
        if (!torrent.isValid())
            continue;
        torrent.hash = infohash::normalize(torrent.hash);
        if (seen.contains(torrent.hash))
            continue;
        seen.insert(torrent.hash);
        entries.append({ std::move(torrent), added, folder });
    }
    const auto count = entries.size() - favorites_.size();
    return count && commit(std::move(entries), folders_) ? count : 0;
}

qsizetype FavoritesStore::removeMany(const QSet<QString>& hashes)
{
    auto entries = favorites_;
    const auto normalized = normalizedHashes(hashes);
    const auto count = entries.removeIf([&](const Entry& entry) { return normalized.contains(entry.torrent.hash); });
    return count && commit(std::move(entries), folders_) ? count : 0;
}

qsizetype FavoritesStore::moveMany(const QSet<QString>& hashes, const QString& folder)
{
    if (!folder.isEmpty() && !folders_.contains(folder))
        return 0;
    auto entries = favorites_;
    const auto normalized = normalizedHashes(hashes);
    qsizetype count = 0;
    for (auto& entry : entries) {
        if (entry.folder != folder && normalized.contains(entry.torrent.hash)) {
            entry.folder = folder;
            ++count;
        }
    }
    return count && commit(std::move(entries), folders_) ? count : 0;
}

bool FavoritesStore::createFolder(const QString& name)
{
    const QString folder = name.trimmed();
    if (folder.isEmpty() || folders_.contains(folder, Qt::CaseInsensitive))
        return false;
    auto folders = folders_;
    folders.append(folder);
    return commit(favorites_, std::move(folders));
}

bool FavoritesStore::renameFolder(const QString& folder, const QString& name)
{
    const QString renamed = name.trimmed();
    auto folders = folders_;
    const auto index = folders.indexOf(folder);
    if (index < 0 || renamed.isEmpty() || folder == renamed)
        return false;
    folders.removeAt(index);
    if (folders.contains(renamed, Qt::CaseInsensitive))
        return false;
    folders.insert(index, renamed);
    auto entries = favorites_;
    for (auto& entry : entries) {
        if (entry.folder == folder)
            entry.folder = renamed;
    }
    return commit(std::move(entries), std::move(folders));
}

bool FavoritesStore::removeFolder(const QString& folder)
{
    auto folders = folders_;
    if (!folders.removeOne(folder))
        return false;
    auto entries = favorites_;
    for (auto& entry : entries) {
        if (entry.folder == folder)
            entry.folder.clear();
    }
    return commit(std::move(entries), std::move(folders));
}

bool FavoritesStore::isFavorite(const QString& hash) const
{
    return hashes_.contains(infohash::normalize(hash));
}

QString FavoritesStore::exportToFile(const QString& path, const QVector<Entry>& entries, ExportOptions options,
    const FileLookup& lookup, const std::function<bool()>& cancelled)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return file.errorString();
    const auto write = [&](const QByteArray& bytes) { return file.write(bytes) == bytes.size(); };
    if (!write("{\"version\":1,\"favorites\":[\n"))
        return file.errorString();

    bool first = true;
    for (const auto& entry : entries) {
        if (cancelled())
            return tr("Export cancelled.");
        const auto& torrent = entry.torrent;
        QJsonObject json { { "title", torrent.name }, { "magnet", torrent.magnetLink() },
            { "hash", torrent.hash }, { "folder", entry.folder } };
        if (options.includeDate)
            json["favoritedAt"] = entry.favoritedAt.toUTC().toString(Qt::ISODateWithMs);
        if (options.includeFiles) {
            const auto files = completeFiles(torrent) ? torrent.fileList : lookup(torrent.hash);
            json["files"] = files.isEmpty() ? QJsonValue(QJsonValue::Null) : domain::codec::filesToJson(files);
        }
        if ((!first && !write(",\n")) || !write(QJsonDocument(json).toJson(QJsonDocument::Compact)))
            return file.errorString();
        first = false;
    }
    if (cancelled())
        return tr("Export cancelled.");
    if (!write("\n]}\n") || !file.commit())
        return file.errorString();
    return {};
}

} // namespace rats::app
