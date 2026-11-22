#include "peer/peer_api.h"

#include "app/application.h"
#include "app/config_store.h"
#include "common/infohash.h"
#include "data/torrent_repository.h"
#include "domain/torrent_codec.h"
#include "net/p2p_transport.h"
#include "services/database_sync_service.h"
#include "services/feed_service.h"
#include "services/indexing_service.h"
#include "services/peer_registry.h"
#include "services/replication_service.h"
#include "services/search_service.h"

#include <QDebug>
#include <QUuid>

namespace rats::peer {

namespace {

const QString searchV2 = QStringLiteral("search_v2");
const QString searchV2Response = QStringLiteral("search_v2_response");

// Build a SearchService request from a peer message, tolerating both the legacy
// "navigation" wrapper and the flat top-level form the newer peers send.
service::SearchService::Request parseSearchRequest(const QJsonObject& data)
{
    service::SearchService::Request req;
    req.query = data["text"].toString();
    if (req.query.isEmpty())
        req.query = data["query"].toString();

    // v1 clients wrap the paging/filter options in a "navigation" object; newer
    // ones put them at the top level. Both spellings stay understood — the wire
    // names are frozen.
    const QJsonObject opts = data.contains("navigation") ? data["navigation"].toObject() : data;
    req.limit = opts["limit"].toInt(10);
    req.offset = opts["index"].toInt(0);
    req.sort = opts["orderBy"].toString();
    req.descending = opts["orderDesc"].toBool(true);
    req.safeSearch = opts["safeSearch"].toBool(false);
    req.contentType = opts["type"].toString();

    // Size / file-count ranges, in the same {min,max} shape the REST router
    // takes. A peer too old to send them simply leaves the range unset, which is
    // exactly what a missing object means.
    const QJsonObject size = opts["size"].toObject();
    req.sizeMin = size["min"].toVariant().toLongLong();
    req.sizeMax = size["max"].toVariant().toLongLong();
    const QJsonObject files = opts["files"].toObject();
    req.filesMin = files["min"].toInt();
    req.filesMax = files["max"].toInt();
    return req;
}

QString shortId(const QString& peerId)
{
    return peerId.left(8);
}

QJsonObject searchOptions(const service::SearchService::Request& request)
{
    QJsonObject data{ { "query", request.query }, { "text", request.query }, { "index", request.offset },
        { "limit", request.limit }, { "orderBy", request.sort }, { "orderDesc", request.descending },
        { "safeSearch", request.safeSearch } };
    if (!request.contentType.isEmpty())
        data["type"] = request.contentType;
    QJsonObject size;
    if (request.sizeMin > 0)
        size["min"] = request.sizeMin;
    if (request.sizeMax > 0)
        size["max"] = request.sizeMax;
    if (!size.isEmpty())
        data["size"] = size;
    QJsonObject files;
    if (request.filesMin > 0)
        files["min"] = request.filesMin;
    if (request.filesMax > 0)
        files["max"] = request.filesMax;
    if (!files.isEmpty())
        data["files"] = files;
    return data;
}

} // namespace

PeerApi::PeerApi(app::Application* app, QObject* parent) : QObject(parent), app_(app)
{
    install();
}

void PeerApi::install()
{
    net::P2PTransport* transport = app_->transport();
    if (!transport) {
        qWarning() << "[PeerApi] no transport available; handlers not installed";
        return;
    }

    // --- Requests we answer -------------------------------------------------
    transport->registerHandler(
        "searchTorrent", [this](const QString& peerId, const QJsonObject& data) { handleSearchRequest(peerId, data, false); });
    transport->registerHandler("searchFiles",
        [this](const QString& peerId, const QJsonObject& data) { handleSearchRequest(peerId, data, true); });
    transport->registerHandler(searchV2,
        [this](const QString& peerId, const QJsonObject& data) { handleVersionedSearch(peerId, data); });
    transport->registerHandler("topTorrents",
        [this](const QString& peerId, const QJsonObject& data) { handleTopTorrentsRequest(peerId, data); });
    transport->registerHandler(
        "torrent", [this](const QString& peerId, const QJsonObject& data) { handleTorrentRequest(peerId, data); });
    transport->registerHandler(
        "feed", [this](const QString& peerId, const QJsonObject& data) { handleFeedRequest(peerId, data); });
    transport->registerHandler("randomTorrents",
        [this](const QString& peerId, const QJsonObject& data) { handleRandomTorrentsRequest(peerId, data); });
    transport->registerHandler("databaseRequest",
        [this](const QString& peerId, const QJsonObject& data) { handleDatabaseRequest(peerId, data); });
    transport->registerHandler("databaseCancel",
        [this](const QString& peerId, const QJsonObject& data) { handleDatabaseCancel(peerId, data); });

    // --- Responses we consume ----------------------------------------------
    transport->registerHandler("searchTorrent_response",
        [this](const QString& peerId, const QJsonObject& data) { handleLegacySearchResult(peerId, data, false); });
    transport->registerHandler("searchFiles_response",
        [this](const QString& peerId, const QJsonObject& data) { handleLegacySearchResult(peerId, data, true); });
    transport->registerHandler(searchV2Response,
        [this](const QString& peerId, const QJsonObject& data) { handleVersionedSearchResult(peerId, data); });
    transport->registerHandler("torrent_response",
        [this](const QString& peerId, const QJsonObject& data) { handleTorrentResponse(peerId, data); });
    transport->registerHandler(
        "feed_response", [this](const QString& peerId, const QJsonObject& data) { handleFeedResponse(peerId, data); });
    transport->registerHandler("randomTorrents_response",
        [this](const QString& peerId, const QJsonObject& data) { handleRandomTorrentsResponse(peerId, data); });
    transport->registerHandler("databaseRequest_response",
        [this](const QString& peerId, const QJsonObject& data) { handleDatabaseResponse(peerId, data); });
    // Unsolicited, like torrentAnnounce: the serving peer heartbeats while it
    // builds its snapshot so our deadline measures silence rather than its size.
    transport->registerHandler("databaseProgress",
        [this](const QString& peerId, const QJsonObject& data) { handleDatabaseProgress(peerId, data); });
    transport->registerHandler("torrentAnnounce",
        [this](const QString& peerId, const QJsonObject& data) { handleTorrentAnnounce(peerId, data); });

    // Follow-up work on connect (the client_info handshake itself is
    // PeerRegistry's).
    connect(transport, &net::P2PTransport::peerConnected, this, &PeerApi::onPeerConnected);
    connect(transport, &net::P2PTransport::peerDisconnected, this, [this](const QString& peerId) {
        versionedSearchPeers_.remove(peerId);
        legacySearchPeers_.remove(peerId);
    });
    connect(transport, &net::P2PTransport::stopped, this, [this] {
        activeSearchId_.clear();
        activeSearchText_.clear();
        versionedSearchPeers_.clear();
        legacySearchPeers_.clear();
    });

    qInfo() << "[PeerApi] P2P handlers installed";
}

// ============================================================================
// Requests we answer
// ============================================================================

void PeerApi::requestSearch(const QString& searchId, const service::SearchService::Request& request)
{
    activeSearchId_ = searchId;
    activeSearchText_ = request.query;
    versionedSearchPeers_.clear();
    legacySearchPeers_.clear();

    auto* transport = app_->transport();
    if (!transport || !transport->isRunning() || QUuid::fromString(searchId).isNull()
        || !service::SearchService::queryError(request.query, true).isEmpty())
        return;
    const auto stats = app_->peers()->connectedPeers();
    const bool allowLegacy = !request.fuzzy && service::SearchService::canSendLegacyQuery(request.query);
    const QJsonObject legacy = searchOptions(request);
    QJsonObject modern = legacy;
    modern["searchId"] = searchId;
    modern["fuzzy"] = request.fuzzy;
    for (const QString& peerId : transport->connectedPeerIds()) {
        if (stats.value(peerId).searchProtocol >= domain::PeerStats::kSearchProtocol) {
            versionedSearchPeers_.insert(peerId);
            modern["kind"] = QStringLiteral("torrents");
            transport->sendMessage(peerId, searchV2, modern);
            modern["kind"] = QStringLiteral("files");
            transport->sendMessage(peerId, searchV2, modern);
        } else if (allowLegacy) {
            legacySearchPeers_.insert(peerId);
            transport->sendMessage(peerId, QStringLiteral("searchTorrent"), legacy);
            transport->sendMessage(peerId, QStringLiteral("searchFiles"), legacy);
        }
    }
}

void PeerApi::handleVersionedSearch(const QString& peerId, const QJsonObject& data)
{
    const QString searchId = data["searchId"].toString();
    const QString kind = data["kind"].toString();
    if (QUuid::fromString(searchId).isNull() || (kind != QLatin1String("torrents") && kind != QLatin1String("files")))
        return;
    handleSearchRequest(peerId, data, kind == QLatin1String("files"), searchId);
}

void PeerApi::handleSearchRequest(const QString& peerId, const QJsonObject& data, bool files, const QString& searchId)
{
    service::SearchService::Request req = parseSearchRequest(data);
    if (!searchId.isEmpty()) {
        if (!service::SearchService::queryError(req.query, true).isEmpty())
            return;
        req.limit = qBound(1, req.limit, 50);
        req.offset = qMax(0, req.offset);
        req.fuzzy = data["fuzzy"].toBool();
    } else if (!service::SearchService::canSendLegacyQuery(req.query)) {
        return;
    }

    const auto hits = files ? app_->search()->searchFiles(req) : app_->search()->searchTorrents(req);
    qInfo() << "[PeerApi] search" << (files ? "files" : "torrents") << "returned" << hits.size()
            << "results for" << shortId(peerId);
    for (const domain::SearchHit& hit : hits) {
        if (!searchId.isEmpty()) {
            app_->transport()->sendMessage(peerId, searchV2Response, { { "searchId", searchId },
                { "kind", files ? QStringLiteral("files") : QStringLiteral("torrents") },
                { "hit", domain::codec::toJson(hit) } });
        } else if (files) {
            QJsonObject result = domain::codec::toJson(hit);
            // Older peers read paths from this key.
            if (!hit.matchingPaths.isEmpty())
                result["path"] = QJsonArray::fromStringList(hit.matchingPaths);
            app_->transport()->sendMessage(peerId, QStringLiteral("searchFiles_response"), result);
        } else {
            app_->transport()->sendMessage(peerId, QStringLiteral("searchTorrent_response"), domain::codec::toJson(hit.torrent));
        }
    }
}

void PeerApi::handleTopTorrentsRequest(const QString& peerId, const QJsonObject& data)
{
    QString type = data["type"].toString();
    QString time;
    int index = 0;
    int limit = 20;
    if (data.contains("navigation")) {
        const QJsonObject nav = data["navigation"].toObject();
        time = nav["time"].toString();
        index = nav["index"].toInt(0);
        limit = nav["limit"].toInt(20);
    } else {
        time = data["time"].toString();
        index = data["index"].toInt(0);
        limit = data["limit"].toInt(20);
    }

    const QVector<domain::Torrent> results = app_->search()->top(type, time, index, limit);
    qInfo() << "[PeerApi] topTorrents ->" << results.size() << "results for" << shortId(peerId);

    QJsonArray torrents;
    for (const domain::Torrent& t : results)
        torrents.append(domain::codec::toJson(t));

    app_->transport()->sendMessage(
        peerId, "topTorrents_response", QJsonObject { { "torrents", torrents }, { "type", type }, { "time", time } });
}

void PeerApi::handleTorrentRequest(const QString& peerId, const QJsonObject& data)
{
    const QString hash = data["hash"].toString();
    if (hash.length() != 40)
        return;

    bool includeFiles = false;
    if (data.contains("options"))
        includeFiles = data["options"].toObject()["files"].toBool(false);
    else
        includeFiles = data["files"].toBool(false);

    const std::optional<domain::Torrent> torrent = app_->search()->get(hash, includeFiles);
    if (!torrent || !torrent->isValid())
        return; // not found: the wire protocol has no "miss" reply, so stay silent

    qInfo() << "[PeerApi] torrent" << hash.left(8) << "for" << shortId(peerId);
    app_->transport()->sendMessage(peerId, "torrent_response",
        domain::codec::toJson(*torrent, { /*includeFiles*/ includeFiles, /*includeInfo*/ true }));
}

void PeerApi::handleFeedRequest(const QString& peerId, const QJsonObject& data)
{
    Q_UNUSED(data);
    service::FeedService* feed = app_->feed();
    if (!feed)
        return;

    qInfo() << "[PeerApi] feed request from" << shortId(peerId);

    // FeedService serialises each item with its file list already, so no per-item
    // enrichment query is needed here.
    QJsonObject response;
    response["feed"] = feed->toJsonArray(0, feed->size());
    response["feedDate"] = feed->feedDate();
    response["size"] = feed->size();
    app_->transport()->sendMessage(peerId, "feed_response", response);
}

void PeerApi::handleRandomTorrentsRequest(const QString& peerId, const QJsonObject& data)
{
    if (!app_->config() || !app_->config()->p2pReplicationServer()) {
        qInfo() << "[PeerApi] replication server disabled; ignoring randomTorrents from" << shortId(peerId);
        return;
    }

    int limit = qBound(1, data["limit"].toInt(5), 10);
    const QVector<domain::Torrent> torrents = app_->torrents()->random(limit, /*includeFiles*/ true);
    qInfo() << "[PeerApi] randomTorrents ->" << torrents.size() << "for" << shortId(peerId);

    QJsonArray array;
    for (const domain::Torrent& t : torrents)
        array.append(domain::codec::toJson(t, { /*includeFiles*/ true, /*includeInfo*/ true }));

    app_->transport()->sendMessage(peerId, "randomTorrents_response", QJsonObject { { "torrents", array } });
}

void PeerApi::handleDatabaseRequest(const QString& peerId, const QJsonObject& data)
{
    qInfo() << "[PeerApi] databaseRequest from" << shortId(peerId);
    service::DatabaseSyncService* sync = app_->databaseSync();
    if (!sync) {
        app_->transport()->sendMessage(peerId, "databaseRequest_response",
            QJsonObject { { "accepted", false }, { "reason", QStringLiteral("unsupported") } });
        return;
    }
    sync->handlePeerRequest(peerId, data);
}

void PeerApi::handleDatabaseCancel(const QString& peerId, const QJsonObject& data)
{
    qInfo() << "[PeerApi] databaseCancel from" << shortId(peerId);
    if (service::DatabaseSyncService* sync = app_->databaseSync())
        sync->handlePeerCancel(peerId, data);
}

// ============================================================================
// Responses we consume
// ============================================================================

void PeerApi::handleLegacySearchResult(const QString& peerId, const QJsonObject& data, bool files)
{
    if (!legacySearchPeers_.contains(peerId))
        return;
    const QString text = data["text"].toString();
    if (!text.isEmpty() && text != activeSearchText_)
        return;
    emitSearchHit(peerId, {}, data, files);
}

void PeerApi::handleVersionedSearchResult(const QString& peerId, const QJsonObject& data)
{
    const QString searchId = data["searchId"].toString();
    if (searchId.isEmpty() || searchId != activeSearchId_ || !versionedSearchPeers_.contains(peerId))
        return;
    const QString kind = data["kind"].toString();
    if (kind != QLatin1String("torrents") && kind != QLatin1String("files"))
        return;
    emitSearchHit(peerId, searchId, data["hit"].toObject(), kind == QLatin1String("files"));
}

void PeerApi::emitSearchHit(const QString& peerId, const QString& searchId, QJsonObject hit, bool files)
{
    QString hash = hit["hash"].toString();
    if (hash.isEmpty())
        hash = hit["info_hash"].toString();
    if (!infohash::isValid(hash))
        return;
    hit["hash"] = infohash::normalize(hash);
    // Search hits carry summary data. Opening the torrent fetches its full file
    // list via requestTorrent() before it can be indexed locally.
    hit["remote"] = true;
    hit["peer"] = peerId;
    if (files) {
        hit["fileMatch"] = true;
        if (!hit.contains("matchingPaths") && hit.contains("path"))
            hit["matchingPaths"] = hit["path"];
        emit remoteFileSearchResults(searchId, QJsonArray{ hit });
    } else {
        emit remoteSearchResults(searchId, QJsonArray{ hit });
    }
}

void PeerApi::handleTorrentResponse(const QString& peerId, const QJsonObject& data)
{
    QString hash = data["hash"].toString();
    if (hash.isEmpty())
        hash = data["info_hash"].toString();
    if (hash.length() != 40) {
        qWarning() << "[PeerApi] invalid torrent_response from" << shortId(peerId);
        return;
    }

    // Emit before insertion so callers awaiting a fetch get the payload even when
    // the torrent already exists locally.
    QJsonObject payload = data;
    payload["peer"] = peerId;
    payload["remote"] = true;
    emit remoteTorrentReceived(hash, payload);

    insertFromPeer(data, /*trackReplication*/ true);
}

void PeerApi::handleFeedResponse(const QString& peerId, const QJsonObject& data)
{
    service::FeedService* feed = app_->feed();
    if (!feed)
        return;

    const QJsonArray remoteFeed = data["feed"].toArray();
    const int remoteSize = data["size"].toInt(remoteFeed.size());
    const qint64 remoteFeedDate = data["feedDate"].toVariant().toLongLong();

    const int localSize = feed->size();
    const qint64 localFeedDate = feed->feedDate();

    // Replace when the remote feed is strictly larger, or equal-sized but newer.
    const bool replace = remoteSize > localSize || (remoteSize == localSize && remoteFeedDate > localFeedDate);
    if (!replace)
        return;

    qInfo() << "[PeerApi] replacing feed with" << remoteFeed.size() << "items from" << shortId(peerId);
    feed->fromJsonArray(remoteFeed, remoteFeedDate);

    // Replicate the torrents behind the feed into the index. These deliberately
    // do not count toward replication accounting.
    for (const QJsonValue& v : remoteFeed) {
        if (v.isObject())
            insertFromPeer(v.toObject(), /*trackReplication*/ false);
    }
}

void PeerApi::handleRandomTorrentsResponse(const QString& peerId, const QJsonObject& data)
{
    const QJsonArray torrents = data["torrents"].toArray();
    int inserted = 0;
    for (const QJsonValue& v : torrents) {
        if (v.isObject() && insertFromPeer(v.toObject(), /*trackReplication*/ true))
            ++inserted;
    }
    if (inserted > 0)
        qInfo() << "[PeerApi] replicated" << inserted << "torrents from" << shortId(peerId);
}

void PeerApi::handleDatabaseResponse(const QString& peerId, const QJsonObject& data)
{
    qInfo() << "[PeerApi] databaseRequest_response from" << shortId(peerId)
            << (data["accepted"].toBool(false) ? "accepted" : "refused");
    if (service::DatabaseSyncService* sync = app_->databaseSync())
        sync->handlePeerResponse(peerId, data);
}

void PeerApi::handleDatabaseProgress(const QString& peerId, const QJsonObject& data)
{
    if (service::DatabaseSyncService* sync = app_->databaseSync())
        sync->handlePeerProgress(peerId, data);
}

void PeerApi::handleTorrentAnnounce(const QString& peerId, const QJsonObject& data)
{
    const QString hash = data["info_hash"].toString();
    const QString name = data["name"].toString();
    if (hash.isEmpty() || name.isEmpty())
        return;

    qDebug() << "[PeerApi] torrent announce from" << shortId(peerId) << ":" << name;
    insertFromPeer(data, /*trackReplication*/ false);
}

// ============================================================================
// Outgoing requests
// ============================================================================

void PeerApi::requestTorrent(const QString& peerId, const QString& hash, bool includeFiles)
{
    net::P2PTransport* transport = app_->transport();
    if (!transport || peerId.isEmpty() || hash.length() != 40)
        return;

    qInfo() << "[PeerApi] requesting torrent" << hash.left(8) << "from" << shortId(peerId);
    transport->sendMessage(
        peerId, "torrent", QJsonObject { { "hash", hash }, { "options", QJsonObject { { "files", includeFiles } } } });
}

// ============================================================================
// Peer lifecycle
// ============================================================================

void PeerApi::onPeerConnected(const QString& peerId)
{
    net::P2PTransport* transport = app_->transport();
    if (!transport)
        return;

    qInfo() << "[PeerApi] peer connected:" << peerId.left(16);

    // Pull the peer's feed for P2P feed sync.
    if (service::FeedService* feed = app_->feed()) {
        transport->sendMessage(
            peerId, "feed", QJsonObject { { "localSize", feed->size() }, { "localFeedDate", feed->feedDate() } });
    }

    // Ask for an initial replication batch when replication is enabled.
    if (app_->config() && app_->config()->p2pReplication())
        transport->sendMessage(peerId, "randomTorrents", QJsonObject { { "limit", 5 } });
}

// ============================================================================
// Insertion (single write path)
// ============================================================================

bool PeerApi::insertFromPeer(const QJsonObject& data, bool trackReplication)
{
    domain::Torrent torrent = domain::codec::torrentFromJson(data);
    if (torrent.hash.length() != 40)
        return false;

    const service::IndexingService::Result result = app_->indexing()->insert(std::move(torrent));
    if (!result.success || result.alreadyExists)
        return false;

    if (trackReplication && app_->replication())
        app_->replication()->notifyReceived();

    return true;
}

} // namespace rats::peer
