#include "app/application.h"
#include "data/database.h"
#include "data/manticore.h"
#include "data/torrent_repository.h"
#include "domain/torrent_codec.h"
#include "net/p2p_transport.h"
#include "peer/peer_api.h"
#include "services/feed_service.h"
#include "services/indexing_service.h"
#include "services/peer_registry.h"
#include "ui/searchresultmodel.h"

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QProcess>
#include <QScopeGuard>
#include <QSemaphore>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QTimer>
#include <QUdpSocket>
#include <QUuid>
#include <QtTest/QtTest>

using rats::app::Application;
using rats::domain::ContentCategory;
using rats::domain::ContentType;
using rats::domain::SearchHit;
using rats::domain::Torrent;
using rats::service::SearchService;

namespace {

class SlowSearchDatabase : public rats::data::Database {
public:
    explicit SlowSearchDatabase(rats::data::Manticore* manticore) : Database(manticore) {}

    Rows query(const QString&, const QVariantList&, bool* ok) override
    {
        if (++calls == 1)
            gate.tryAcquire(1, 6000);
        if (ok)
            *ok = true;
        return {};
    }

    std::atomic_int calls = 0;
    QSemaphore gate;
};

quint16 tcpPort()
{
    QTcpServer server;
    return server.listen(QHostAddress::LocalHost) ? server.serverPort() : 0;
}

bool writeFile(const QString& path, const QByteArray& bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(bytes) == bytes.size();
}

bool prepareNode(const QTemporaryDir& directory, int port, int knownPeer = 0, int httpPort = 0)
{
    QUdpSocket udp;
    if (!directory.isValid() || port == 0 || !udp.bind(QHostAddress(QHostAddress::LocalHost), 0))
        return false;
    const QJsonObject config{ { "agreementAccepted", true }, { "indexer", false }, { "trackers", false },
        { "upnp", false }, { "p2pReplication", false }, { "databaseSharing", false }, { "holePunch", false },
        { "relay", false }, { "checkUpdatesOnStartup", false }, { "p2pPort", port }, { "dhtPort", udp.localPort() },
        { "restApi", httpPort != 0 }, { "httpPort", httpPort } };
    if (!writeFile(directory.filePath("rats.json"), QJsonDocument(config).toJson()))
        return false;
    return knownPeer == 0
        || writeFile(directory.filePath("peers.json"),
            QJsonDocument(QJsonArray{ QJsonObject{ { "ip", "127.0.0.1" }, { "port", knownPeer } } }).toJson());
}

Application::Options options(const QTemporaryDir& directory)
{
    Application::Options result;
    result.dataDirectory = directory.path();
    result.clientVersion = QStringLiteral("peer-search-test");
    result.headless = true;
    return result;
}

QString searchId()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

Torrent fixture(int id, const QString& name, const QStringList& paths)
{
    Torrent torrent;
    torrent.hash = QStringLiteral("%1").arg(id, 8, 16, u'0') + QString(32, u'a');
    torrent.name = name;
    torrent.size = 1000 * paths.size();
    torrent.files = paths.size();
    torrent.pieceLength = 16384;
    torrent.contentType = ContentType::Video;
    for (const QString& path : paths)
        torrent.fileList.append({ path, 1000 });
    return torrent;
}

struct Replies {
    QSignalSpy torrents;
    QSignalSpy files;

    explicit Replies(rats::peer::PeerApi* peer)
        : torrents(peer, &rats::peer::PeerApi::remoteSearchResults)
        , files(peer, &rats::peer::PeerApi::remoteFileSearchResults)
    {
    }

    void clear()
    {
        torrents.clear();
        files.clear();
    }

    static SearchHit hit(const QList<QVariant>& reply)
    {
        return rats::domain::codec::searchHitFromJson(reply.at(1).toJsonArray().first().toObject());
    }
};

QJsonObject callApi(QNetworkAccessManager& network, int port, const QString& method, const QJsonObject& params)
{
    const QUrl url(QStringLiteral("http://127.0.0.1:%1/api/%2").arg(port).arg(method));
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    QNetworkReply* reply = network.post(request, QJsonDocument(params).toJson(QJsonDocument::Compact));
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    timer.start(1500);
    if (!reply->isFinished())
        loop.exec();
    const auto json = QJsonDocument::fromJson(reply->readAll()).object();
    reply->deleteLater();
    return json;
}

} // namespace

class TestPeerSearch : public QObject {
    Q_OBJECT

private slots:
    void blockedSearchKeepsMainLoopResponsive();
    void twoNodes();
    void packagedPeer();
};

void TestPeerSearch::blockedSearchKeepsMainLoopResponsive()
{
    QTemporaryDir dir;
    rats::data::Manticore manticore(dir.path());
    SlowSearchDatabase database(&manticore);
    rats::data::TorrentRepository repository(&database);
    SearchService service(&repository);
    service.startAsync();
    const auto release = qScopeGuard([&] { database.gate.release(); });

    SearchService::Request request;
    request.query = "blocked worker";
    int oldCallbacks = 0;
    int currentCallbacks = 0;
    QVERIFY(service.searchAsync(this, request, SearchService::Mode::Both, SearchService::Lane::Desktop,
        [&](SearchService::Results) { ++oldCallbacks; }));
    QTRY_COMPARE(database.calls.load(), 1);

    request.query = "replacement search";
    QVERIFY(service.searchAsync(this, request, SearchService::Mode::Torrents, SearchService::Lane::Desktop,
        [&](SearchService::Results) { ++currentCallbacks; }));
    // Prove an event runs while the query is blocked, without relying on how
    // many ticks the platform coalesces under parallel CI load.
    bool responsiveWhileBlocked = false;
    QTimer probe;
    probe.setSingleShot(true);
    connect(&probe, &QTimer::timeout, this, [&] {
        responsiveWhileBlocked = database.calls.load() == 1 && oldCallbacks == 0 && currentCallbacks == 0;
    });
    probe.start(100);
    QTest::qWait(3000);
    QVERIFY(responsiveWhileBlocked);
    QCOMPARE(database.calls.load(), 1);
    database.gate.release();
    QTRY_COMPARE_WITH_TIMEOUT(currentCallbacks, 1, 4500);
    QCOMPARE(oldCallbacks, 0);
    QCOMPARE(database.calls.load(), 2); // The old job skipped its file search.
}

void TestPeerSearch::twoNodes()
{
    QTemporaryDir sendingData;
    QTemporaryDir servingData;
    const int servingPort = tcpPort();
    const int sendingPort = tcpPort();
    QVERIFY(sendingPort != servingPort);
    QVERIFY(prepareNode(servingData, servingPort));
    QVERIFY(prepareNode(sendingData, sendingPort, servingPort));
    const QString testProtocol = QStringLiteral("rats-search-test/") + searchId();
    auto servingOptions = options(servingData);
    auto sendingOptions = options(sendingData);
    servingOptions.p2pProtocolOverride = testProtocol;
    sendingOptions.p2pProtocolOverride = testProtocol;
    Application serving(servingOptions);
    Application sending(sendingOptions);
    QVERIFY(serving.start());
    QVERIFY(sending.start());
    const QString serverId = serving.transport()->ourPeerId();
    const QString clientId = sending.transport()->ourPeerId();
    QVERIFY(!serverId.isEmpty());
    QVERIFY(!clientId.isEmpty());
    QTRY_COMPARE_WITH_TIMEOUT(sending.peers()->connectedPeers().value(serverId).searchProtocol, 2, 10000);

    const QString movie = QStringLiteral("\u7535\u5f71");
    const Torrent exact = fixture(1, movie + " Ubuntu Linux 1080p",
        { "root/" + movie + " Ubuntu Linux 1080p.mkv" });
    const Torrent split = fixture(2, "Split paths", { movie + ".mkv", "1080p.txt" });
    Torrent excluded = fixture(3, movie + " Ubuntu Linux 1080p unsafe", { movie + " Ubuntu Linux 1080p.mkv" });
    excluded.contentCategory = ContentCategory::XXX;
    const Torrent prefix = fixture(4, movie + " Ubuntux Linux 1080p", { movie + " Ubuntux Linux 1080p.mkv" });
    QCOMPARE(serving.torrents()->addMany({ exact, split, excluded, prefix }), 4);
    Replies replies(sending.peerApi());
    auto send = [&](const SearchService::Request& request) {
        replies.clear();
        const QString id = searchId();
        sending.peerApi()->requestSearch(id, request);
        return id;
    };

    SearchService::Request request;
    request.query = movie;
    request.safeSearch = true;
    request.sizeMin = request.sizeMax = 1000;
    request.filesMin = request.filesMax = 1;
    request.contentType = QStringLiteral("video");
    const auto shortId = send(request);
    QTRY_COMPARE(replies.torrents.size(), 2);
    QTRY_COMPARE(replies.files.size(), 2);
    QCOMPARE(replies.torrents.front().at(0).toString(), shortId);
    const auto shortHit = Replies::hit(replies.files.front());
    QCOMPARE(shortHit.sourcePeerId, serverId);
    QVERIFY(shortHit.remote);
    QVERIFY(shortHit.matchingPaths.front().contains(QStringLiteral("<b>") + movie + QStringLiteral("</b>")));

    request.query = movie + " \"Ubuntu Linux\" 1080p";
    const auto phraseId = send(request);
    QTRY_COMPARE(replies.torrents.size(), 1);
    QTRY_COMPARE(replies.files.size(), 1);
    QCOMPARE(replies.files.front().at(0).toString(), phraseId);
    QCOMPARE(Replies::hit(replies.files.front()).torrent.hash, exact.hash);

    request.query = movie + " 1080p";
    request.sizeMax = 0;
    request.filesMax = 0;
    send(request);
    QTRY_COMPARE(replies.files.size(), 2);
    for (const auto& reply : replies.files)
        QVERIFY(Replies::hit(reply).torrent.hash != split.hash);

    request.query = "\"Linux 1080p\" ubntu";
    request.fuzzy = true;
    const auto fuzzyId = send(request);
    QTRY_COMPARE(replies.torrents.size(), 1);
    QTRY_COMPARE(replies.files.size(), 1);
    QCOMPARE(replies.files.front().at(0).toString(), fuzzyId);
    QVERIFY(Replies::hit(replies.files.front()).fuzzy);

    // Delay both real request lanes on the serving node. Reply to the old
    // search after the same text was submitted again with different options.
    QList<QJsonObject> received;
    serving.transport()->registerHandler(QStringLiteral("search_v2"),
        [&](const QString& from, const QJsonObject& payload) {
            if (from == clientId)
                received.append(payload);
        });
    const auto oldId = send(request);
    QTRY_COMPARE(received.size(), 2);
    request.descending = false;
    const auto currentId = send(request);
    QTRY_COMPARE(received.size(), 4);
    QCOMPARE(received[2]["searchId"].toString(), currentId);
    QVERIFY(!received[2]["orderDesc"].toBool(true));

    SearchResultModel model;
    connect(sending.peerApi(), &rats::peer::PeerApi::remoteFileSearchResults, &model,
        [&](const QString&, const QJsonArray& hits) {
            for (const auto& item : hits)
                model.addFileResult(rats::domain::codec::searchHitFromJson(item.toObject()));
        });
    const auto reply = [&](const QString& id, const QString& kind, const SearchHit& hit) {
        return serving.transport()->sendMessage(clientId, QStringLiteral("search_v2_response"),
            { { "searchId", id }, { "kind", kind }, { "hit", rats::domain::codec::toJson(hit) } });
    };
    SearchHit hit;
    hit.torrent = exact;
    hit.fuzzy = true;
    hit.fromFileMatch = true;
    hit.matchingPaths = { "root/<b>Ubuntu</b>.mkv" };
    QVERIFY(reply(oldId, "files", hit));
    QVERIFY(reply(currentId, "files", hit));
    QVERIFY(reply(oldId, "torrents", hit));
    QVERIFY(reply(currentId, "files", hit));
    hit.fuzzy = false;
    hit.matchingPaths = { "root/<b>Ubuntu Linux</b>.mkv" };
    QVERIFY(reply(currentId, "files", hit));
    QTRY_COMPARE(replies.files.size(), 3);
    QTest::qWait(200);
    QCOMPARE(replies.files.size(), 3);
    QVERIFY(replies.torrents.isEmpty());
    QCOMPARE(model.rowCount(), 1);
    QVERIFY(!model.getHit(0).fuzzy);
    QCOMPARE(model.getHit(0).matchingPaths, hit.matchingPaths);

    // A late legacy response from the queried peer must be ignored even when
    // it claims to match the current text.
    QJsonObject legacyHit = rats::domain::codec::toJson(exact);
    legacyHit["text"] = request.query;
    QVERIFY(serving.transport()->sendMessage(clientId, "searchTorrent_response", legacyHit));
    QVERIFY(serving.transport()->sendMessage(clientId, "searchFiles_response", legacyHit));
    QVERIFY(reply(QString(), "files", hit));
    QTest::qWait(100);
    QCOMPARE(replies.files.size(), 3);
    QVERIFY(replies.torrents.isEmpty());

    // Legacy-only peers aren't queried by the new client. Upgraded peers can
    // still serve ordinary queries received from an older client.
    QVERIFY(serving.transport()->sendMessage(clientId, "client_info", {}));
    QTRY_COMPARE(sending.peers()->connectedPeers().value(serverId).searchProtocol, 0);
    request = SearchService::Request{};
    request.query = "Ubuntu";
    request.safeSearch = true;
    received.clear();
    send(request);

    const auto legacyReplies = std::make_shared<QVector<QJsonObject>>();
    sending.transport()->registerHandler("searchTorrent_response",
        [legacyReplies](const QString&, const QJsonObject& result) { legacyReplies->append(result); });
    QTest::qWait(200);
    QVERIFY(legacyReplies->isEmpty());
    QVERIFY(sending.transport()->sendMessage(serverId, "searchTorrent", { { "text", "Ubuntu" } }));
    QTRY_VERIFY(!legacyReplies->isEmpty());
    QVERIFY(replies.torrents.isEmpty());
    QVERIFY(replies.files.isEmpty());
    QVERIFY(received.isEmpty());

    // Replication/feed traffic can arrive with no search in progress. Verify
    // that bulk feed ingestion leaves the application's main thread.
    std::atomic_int mainInserts = 0;
    std::atomic_int workerInserts = 0;
    const auto indexed = connect(sending.indexing(), &rats::service::IndexingService::torrentIndexed, this,
        [&](const Torrent&) {
            if (QThread::currentThread() == thread())
                ++mainInserts;
            else
                ++workerInserts;
        }, Qt::DirectConnection);
    const auto disconnectIndexed = qScopeGuard([&] { disconnect(indexed); });
    QJsonArray feed;
    constexpr int feedSize = 64;
    for (int i = 0; i < feedSize; ++i)
        feed.append(rats::domain::codec::toJson(fixture(100 + i, "Synthetic feed item", { "feed.mkv" })));
    QVERIFY(serving.transport()->sendMessage(clientId, "feed_response",
        { { "feed", feed }, { "size", feedSize }, { "feedDate", QDateTime::currentSecsSinceEpoch() } }));
    QTRY_COMPARE(sending.feed()->size(), feedSize);
    QCOMPARE(mainInserts.load(), 0);
    QCOMPARE(workerInserts.load(), feedSize);

    serving.stop();
    QTRY_VERIFY(!sending.transport()->connectedPeerIds().contains(serverId));
    QTRY_VERIFY(!sending.peers()->connectedPeers().contains(serverId));
}

void TestPeerSearch::packagedPeer()
{
    const QString exe = qEnvironmentVariable("RATS_PEER_SEARCH_PACKAGE");
    if (exe.isEmpty())
        QSKIP("Run explicitly against an extracted package.");
    const bool modern = qEnvironmentVariableIntValue("RATS_PEER_SEARCH_LEGACY") == 0;
    QTemporaryDir servingData;
    QTemporaryDir sendingData;
    const int serverPort = tcpPort();
    const int clientPort = tcpPort();
    const int apiPort = tcpPort();
    QVERIFY(serverPort != clientPort && clientPort != apiPort && serverPort != apiPort);
    QVERIFY(prepareNode(servingData, serverPort, 0, apiPort));
    QVERIFY(prepareNode(sendingData, clientPort, serverPort));

    QProcess process;
    const QString package = QFileInfo(exe).absolutePath();
    process.setWorkingDirectory(package);
    process.setStandardOutputFile(servingData.filePath("package.log"));
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(exe, { "--console", "--data-dir", servingData.path(), "--no-remember-data-dir" });
    QVERIFY(process.waitForStarted(10000));
    const auto stop = qScopeGuard([&] {
        process.kill();
        process.waitForFinished(5000);
#ifdef Q_OS_WIN
        QProcess daemon;
        daemon.start(package + "/searchd.exe", { "--config", servingData.filePath("sphinx.conf"), "--stopwait" });
        if (!daemon.waitForFinished(5000)) {
            daemon.kill();
            daemon.waitForFinished(1000);
        }
#endif
    });

    QNetworkAccessManager network;
    QTRY_VERIFY_WITH_TIMEOUT(callApi(network, apiPort, "search.torrents", { { "text", "ready" } })["success"].toBool(), 10000);
    const QString movie = QStringLiteral("\u7535\u5f71");
    const auto name = (movie + " Ubuntu StageFivePackage.mkv").toUtf8();
    const auto piece = QCryptographicHash::hash(QByteArray(1048576, 'k'), QCryptographicHash::Sha1);
    const auto data = QByteArray("d4:infod6:lengthi1048576e4:name") + QByteArray::number(name.size()) + ':' + name
        + QByteArray("12:piece lengthi1048576e6:pieces20:") + piece + QByteArray("ee");
    const QString file = servingData.filePath("sample.torrent");
    QVERIFY(writeFile(file, data));
    const auto imported = callApi(network, apiPort, "torrent.import", { { "path", file } });
    QVERIFY(imported["success"].toBool());
    const QString hash = imported["data"].toObject()["hash"].toString();
    QVERIFY(!hash.isEmpty());

    Application sending(options(sendingData));
    QVERIFY(sending.start());
    QTRY_VERIFY_WITH_TIMEOUT(!sending.peers()->connectedPeers().isEmpty(), 10000);
    const auto advertised = sending.peers()->connectedPeers();
    QCOMPARE(advertised.constBegin().value().searchProtocol, modern ? 2 : 0);
    Replies replies(sending.peerApi());
    auto id = searchId();
    SearchService::Request request;
    request.query = modern ? movie : QStringLiteral("StageFivePackage");
    sending.peerApi()->requestSearch(id, request);
    if (!modern) {
        const auto wireReplies = std::make_shared<QVector<QJsonObject>>();
        sending.transport()->registerHandler("searchFiles_response",
            [wireReplies](const QString&, const QJsonObject& reply) { wireReplies->append(reply); });
        QTest::qWait(200);
        QVERIFY(wireReplies->isEmpty());
        QVERIFY(sending.transport()->sendMessage(advertised.constBegin().key(), "searchFiles",
            { { "text", request.query } }));
        QTRY_VERIFY(!wireReplies->isEmpty());
        QCOMPARE(wireReplies->front()["hash"].toString(), hash);
        QVERIFY(replies.torrents.isEmpty());
        QVERIFY(replies.files.isEmpty());
        return;
    }
    QTRY_VERIFY_WITH_TIMEOUT(!replies.torrents.isEmpty(), 10000);
    QTRY_VERIFY(!replies.files.isEmpty());
    QCOMPARE(replies.files.front().at(0).toString(), id);
    QCOMPARE(Replies::hit(replies.files.front()).torrent.hash, hash);
    QVERIFY(!Replies::hit(replies.files.front()).matchingPaths.isEmpty());
    replies.clear();
    request.query = "\"StageFivePackage\" ubntu";
    request.fuzzy = true;
    id = searchId();
    sending.peerApi()->requestSearch(id, request);
    QTRY_VERIFY(!replies.files.isEmpty());
    QCOMPARE(replies.files.front().at(0).toString(), id);
    QVERIFY(Replies::hit(replies.files.front()).fuzzy);
}

QTEST_GUILESS_MAIN(TestPeerSearch)
#include "test_peer_search.moc"
