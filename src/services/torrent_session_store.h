#ifndef RATS_SERVICE_TORRENT_SESSION_STORE_H
#define RATS_SERVICE_TORRENT_SESSION_STORE_H

#include "services/download_service.h" // for Download / DownloadFile

#include <QString>
#include <QVector>

namespace rats::service {

struct SessionDocument {
    int schemaVersion = 2;
    qint64 revision = 0;
    QVector<Download> downloads;
};
struct SessionLoadResult {
    enum Status { Loaded, Missing, Migrated, Invalid, IoError } status = Missing;
    SessionDocument document;
    QString error;
    bool ok() const { return status == Loaded || status == Missing || status == Migrated; }
};

// Reads and writes the download session file (torrents_session.json): the set
// of active downloads to restore on the next launch. This is pure JSON file I/O
// — it holds no state, touches no librats and saves no resume data (the service
// asks the engine to do that separately).
//
// Serialisation reuses DownloadFile::toJson(), the very same helper the live
// progress path uses, so the persisted per-file shape cannot drift from the
// live one. Never hand-build the file object here.
class TorrentSessionStore {
public:
    SessionLoadResult loadDocument(const QString& filePath) const;
    bool saveDocument(const QString& filePath, const SessionDocument&, QString* error = nullptr) const;
    static QJsonObject migrateLegacyArray(const QJsonArray&);
    static QString validateDocument(const QJsonObject&);
    // Compatibility wrapper. Empty lists remain a versioned document. Writes
    // validate first and atomically replace the file using QSaveFile.
    bool save(const QString& filePath, const QVector<Download>& downloads) const;

    // Compatibility wrapper; logs errors. New callers should use loadDocument
    // to distinguish a missing file from corruption or an unsupported version.
    QVector<Download> load(const QString& filePath) const;
};

} // namespace rats::service

#endif // RATS_SERVICE_TORRENT_SESSION_STORE_H
