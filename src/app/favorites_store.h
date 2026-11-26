#ifndef RATS_APP_FAVORITES_STORE_H
#define RATS_APP_FAVORITES_STORE_H

#include "domain/torrent.h"

#include <QDateTime>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QVector>
#include <functional>

namespace rats::app {

// Personal favourites, persisted to favorites.json in the data directory.
class FavoritesStore : public QObject {
    Q_OBJECT

public:
    struct Entry {
        domain::Torrent torrent;
        QDateTime favoritedAt;
        QString folder;
    };
    struct ExportOptions {
        bool includeFiles = false;
        bool includeDate = false;
    };

    explicit FavoritesStore(const QString& dataDirectory, QObject* parent = nullptr);

    bool add(const domain::Torrent& torrent); // false if already present
    bool remove(const QString& hash);
    qsizetype addMany(const QVector<domain::Torrent>& torrents, const QString& folder = {});
    qsizetype removeMany(const QSet<QString>& hashes);
    qsizetype moveMany(const QSet<QString>& hashes, const QString& folder);
    bool createFolder(const QString& name);
    bool renameFolder(const QString& folder, const QString& name);
    bool removeFolder(const QString& folder); // keeps its entries in the unfiled group
    bool isFavorite(const QString& hash) const;
    QVector<Entry> favorites() const { return favorites_; }
    QStringList folders() const { return folders_; }

    using FileLookup = std::function<QVector<domain::File>(const QString& hash)>;
    static QString exportToFile(const QString& path, const QVector<Entry>& entries, ExportOptions options,
        const FileLookup& lookup, const std::function<bool()>& cancelled);

signals:
    void favoritesChanged();
    void saveFailed(const QString& error);

private:
    void load();
    bool commit(QVector<Entry> entries, QStringList folders);

    QString filePath_;
    QString loadError_;
    QVector<Entry> favorites_;
    QStringList folders_;
    QSet<QString> hashes_;
};

} // namespace rats::app

#endif // RATS_APP_FAVORITES_STORE_H
