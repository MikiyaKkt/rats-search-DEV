#include "app/data_relocation.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStorageInfo>
#include <QTemporaryDir>
#include <QUuid>
#include <QVector>

namespace rats::app::relocation {
namespace {

constexpr auto requestName = ".rats-relocation.json";
constexpr auto markerName = ".rats-relocation-ready.json";

QString message(const char* text) { return QCoreApplication::translate("DataRelocation", text); }

QString normalizedPath(const QString& path)
{
    const QFileInfo info(path);
    const auto canonical = info.canonicalFilePath();
    if (!canonical.isEmpty())
        return canonical;
    const auto parent = QFileInfo(info.absolutePath()).canonicalFilePath();
    return parent.isEmpty() ? info.absoluteFilePath() : QDir(parent).filePath(info.fileName());
}

QString comparable(QString path)
{
    path = QDir::cleanPath(path);
#ifdef Q_OS_WIN
    path = path.toCaseFolded();
#endif
    return path;
}

bool inside(const QString& child, QString parent)
{
    if (!parent.endsWith(u'/'))
        parent += u'/';
    return child.startsWith(parent);
}

QJsonObject readJson(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.read(32768)).object() : QJsonObject();
}

bool saveJson(const QString& path, const QJsonObject& value)
{
    const auto bytes = QJsonDocument(value).toJson(QJsonDocument::Compact);
    QSaveFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
}

bool isEmptyDirectory(const QString& path)
{
    return QDir(path).entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot).isEmpty();
}

QString validatePaths(const QString& source, const QString& destination)
{
    if (destination.trimmed().isEmpty() || !QFileInfo(destination).isAbsolute())
        return message("Choose an absolute destination path.");
    const auto from = comparable(normalizedPath(source));
    const auto to = comparable(normalizedPath(destination));
    if (from == to || inside(to, from) || inside(from, to))
        return message("Choose a destination outside the current data directory.");
    const QFileInfo target(destination);
    if (target.isSymLink() || target.isJunction() || (target.exists() && !target.isDir())
        || !QFileInfo(target.absolutePath()).isDir())
        return message("Choose an empty folder or a new folder inside an existing directory.");
    return {};
}

// These are the persistent assets needed by the index and its profile.
// Generated paths, process locks and caches are rebuilt on the next startup.
bool persistent(const QString& name)
{
    static const QStringList entries { "database", "rats.json", "favorites.json", "search-history.json",
        "migrations.json", "torrents_session.json", "peers.json", "identity.key", "storage", "translations",
        "dht_routing.json", "dht_routing_v6.json" };
    return entries.contains(name) || (name.startsWith(QLatin1String("binlog.")) && name != QLatin1String("binlog.lock"));
}

} // namespace

Request pending(const QString& source)
{
    const auto json = readJson(QDir(source).filePath(QLatin1String(requestName)));
    const auto token = json["token"].toString();
    return QUuid::fromString(token).isNull() ? Request{} : Request{ json["destination"].toString(), token };
}

QString validate(const QString& source, const QString& destination)
{
    if (const auto error = validatePaths(source, destination); !error.isEmpty())
        return error;
    if (QFileInfo::exists(destination) && !isEmptyDirectory(destination))
        return message("The destination folder already contains files.");
    return {};
}

QString schedule(const QString& source, const QString& destination)
{
    if (const auto error = validate(source, destination); !error.isEmpty())
        return error;
    const auto token = QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (!saveJson(QDir(source).filePath(QLatin1String(requestName)),
            { { "destination", normalizedPath(destination) }, { "token", token } }))
        return message("Could not save the migration request.");
    return {};
}

QString clear(const QString& source)
{
    QFile file(QDir(source).filePath(QLatin1String(requestName)));
    return !file.exists() || file.remove() ? QString() : message("Could not clear the migration request.");
}

QString prepare(const QString& source, const Request& request, const Progress& progress)
{
    if (const auto error = validatePaths(source, request.destination); !error.isEmpty())
        return error;
    const QDir from(source);
    const QFileInfo target(request.destination);
    const QDir to(target.absoluteFilePath());
    const auto marker = readJson(to.filePath(QLatin1String(markerName)));
    if (!request.token.isEmpty() && marker["token"].toString() == request.token
        && marker["source"].toString() == normalizedPath(source))
        return {};
    if (target.exists() && !isEmptyDirectory(to.absolutePath()))
        return message("The destination folder already contains files.");

    struct Item { QString relative; bool directory; qint64 size; };
    QVector<Item> items;
    qint64 total = 0;
    const auto collect = [&](const QFileInfo& info) {
        if (info.isSymLink() || info.isJunction() || (!info.isFile() && !info.isDir()) || !info.isReadable())
            return false;
        const qint64 size = info.isFile() ? info.size() : 0;
        items.append({ from.relativeFilePath(info.absoluteFilePath()), info.isDir(), size });
        total += size;
        return true;
    };
    constexpr auto flags = QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot;
    for (const auto& entry : from.entryInfoList(flags)) {
        if (!persistent(entry.fileName()))
            continue;
        if (!collect(entry))
            return message("Cannot copy profile entry: %1").arg(entry.fileName());
        if (entry.isDir()) {
            QDirIterator list(entry.absoluteFilePath(), flags, QDirIterator::Subdirectories);
            while (list.hasNext()) {
                list.next();
                if (!collect(list.fileInfo()))
                    return message("Cannot copy profile entry: %1").arg(from.relativeFilePath(list.filePath()));
            }
        }
    }
    if (items.isEmpty())
        return message("No database or profile files were found.");
    const QStorageInfo volume(target.absolutePath());
    if (volume.isReady() && volume.bytesAvailable() >= 0 && volume.bytesAvailable() < total)
        return message("The destination does not have enough free space.");

    QTemporaryDir staging(QDir(target.absolutePath()).filePath(QStringLiteral(".rats-migrate-XXXXXX")));
    if (!staging.isValid())
        return message("Could not create a staging directory at the destination.");
    const QDir copied(staging.path());
    QByteArray buffer(2 * 1024 * 1024, Qt::Uninitialized);
    qint64 done = 0;
    for (const auto& item : items) {
        if (!progress(done, total))
            return message("Data migration was cancelled.");
        if (item.directory) {
            if (!copied.mkpath(item.relative))
                return message("Cannot copy profile entry: %1").arg(item.relative);
            continue;
        }
        const auto outPath = copied.filePath(item.relative);
        QFile in(from.filePath(item.relative));
        QFile out(outPath);
        if (!copied.mkpath(QFileInfo(item.relative).path()) || !in.open(QIODevice::ReadOnly)
            || !out.open(QIODevice::WriteOnly | QIODevice::NewOnly))
            return message("Cannot copy profile entry: %1").arg(item.relative);
        qint64 remaining = item.size;
        while (remaining > 0) {
            const qint64 n = in.read(buffer.data(), qMin<qint64>(remaining, buffer.size()));
            if (n <= 0 || out.write(buffer.constData(), n) != n)
                return message("Cannot copy profile entry: %1").arg(item.relative);
            remaining -= n;
            done += n;
            if (!progress(done, total))
                return message("Data migration was cancelled.");
        }
        if (!in.atEnd() || !out.flush() || out.size() != item.size)
            return message("Cannot copy profile entry: %1").arg(item.relative);
        out.setPermissions(in.permissions());
    }
    if (!saveJson(copied.filePath(QLatin1String(markerName)),
            { { "source", normalizedPath(source) }, { "token", request.token } }))
        return message("Could not finalize the copied data directory.");
    if ((QFileInfo::exists(to.absolutePath()) && !QDir().rmdir(to.absolutePath()))
        || !QDir().rename(staging.path(), to.absolutePath()))
        return message("Could not publish the copied data directory.");
    staging.setAutoRemove(false);
    return {};
}

void finish(const QString& source, const Request& request)
{
    if (clear(source).isEmpty())
        QFile::remove(QDir(request.destination).filePath(QLatin1String(markerName)));
}

} // namespace rats::app::relocation
