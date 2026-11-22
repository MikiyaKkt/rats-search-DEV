/**
 * @file test_manticore_queries.cpp
 * @brief Integration test that starts a real Manticore instance and drives the
 *        new data layer (rats::data::Manticore + Database + TorrentRepository).
 *
 * Verifies the end-to-end CRUD / search / statistics path against a live
 * searchd, replacing the old raw-SphinxQL query test. Requires the bundled
 * searchd executable to be discoverable via the standard import paths.
 */

#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QTemporaryDir>
#include <QThread>
#include <QtTest/QtTest>

#include "data/database.h"
#include "common/performance_metrics.h"
#include "data/manticore.h"
#include "data/query.h"
#include "data/row_id.h"
#include "data/torrent_repository.h"
#include "domain/content.h"
#include "domain/torrent.h"
#include "domain/torrent_codec.h"
#include "services/database_dump.h"
#include "services/database_exporter.h"
#include "services/database_importer.h"
#include "services/filter_policy.h"
#include "services/indexing_service.h"
#include "services/migration_service.h"
#include "services/crawler_pipeline.h"
#include "services/search_service.h"
#include <thread>
#include <atomic>
#include <algorithm>

using rats::data::Database;
using rats::data::Manticore;
using rats::data::SelectQuery;
using rats::data::TorrentRepository;
using rats::domain::ContentCategory;
using rats::domain::ContentType;
using rats::domain::File;
using rats::domain::Torrent;

class TestManticoreQueries : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();

    void testConnected();
    void testAddAndGet();
    void testExists();
    void testGetWithFiles();
    void testSearchByName();
    void testSearchByHash();
    void testSearchQuerySyntax();
    void testFileSearchFiltersAndPages();
    void testSearchReindexResumes();
    void testFuzzySearchMode();
    void testFuzzySearchBudget();
    void testUpdateTrackerCounts();
    void testStatistics();
    void testRecent();
    void testTop();
    void testRemove();
    void testSelectQueryAgainstLiveIndex();
    void testPageAfterIdWalksPastMaxMatches();
    void testRowIdIsDerivedFromHash();
    void testFilesRowSharesTheTorrentId();
    void testReAddDoesNotDuplicateRow();
    void testRowIdCollisionIsRefused();
    void testBatchRowIdCollisionIsRefused();
    void testBatchDuplicateHashCollapsesToOneRow();
    void testMigrationReKeysLegacyRows();
    void testUpdateAppliesAndKeepsSearchable();
    void testFileSearchDoesNotExposeSnippetsAsFiles();
    void testBackfillMissingAndPartialFiles();
    void testDatabaseMetricOutcomes();
    void testIndexingMetricOutcomes();
    void testConcurrentIndexAndPartialWrites();
    void testConcurrentInsertAndDeleteStatistics();
    void testCrawlerWorkerConnectionsAndMainThreadSql();
    void testCrawlerReconnectsAfterDaemonRestart();

    void testExportImportRoundTrip();
    void testImportIsAMergeNotADuplicate();
    void testCancelledExportLeavesNoFile();
    void testInterruptedImportResumesWhereItStopped();

private:
    // Build a valid torrent with a distinct 40-hex hash derived from `n`.
    Torrent makeTorrent(
        int n, const QString& name, ContentType type = ContentType::Video, qint64 size = 1024 * 1024, int seeders = 10);
    // Poll get() until the RT row is visible (RT attributes settle asynchronously).
    bool waitForTorrent(const QString& hash, int maxRetries = 50, int delayMs = 20);
    // Same, for the files row. The two tables settle independently, so a
    // torrent being visible says nothing about its file list yet.
    bool waitForFiles(const QString& hash, int expected, int maxRetries = 50, int delayMs = 20);

    QTemporaryDir* tempDir_ = nullptr;
    Manticore* manticore_ = nullptr;
    Database* db_ = nullptr;
    TorrentRepository* repo_ = nullptr;
    rats::service::FilterPolicy* filter_ = nullptr;
    rats::service::IndexingService* indexing_ = nullptr;
};

void TestManticoreQueries::testConcurrentIndexAndPartialWrites()
{
    const auto before = repo_->statistics();
    Torrent t = makeTorrent(92001, "Concurrent insertion fixture");
    std::atomic<int> failures{0}, inserted{0};
    auto index = [&](bool batch) {
        rats::service::FilterPolicy policy;
        rats::service::IndexingService indexing(repo_, &policy);
        for (int i = 0; i < 20; ++i) {
            Torrent incoming = t;
            if (i % 2 == 0) incoming.hash = incoming.hash.toUpper();
            if (batch) inserted += indexing.insertBatch({incoming}).inserted;
            else {
                const auto result = indexing.insert(incoming);
                if (!result.success) ++failures;
                else if (!result.alreadyExists) ++inserted;
            }
        }
        manticore_->closeCurrentThreadConnection();
    };
    std::thread one(index, false), two(index, true), three(index, false);
    one.join(); two.join(); three.join();
    QCOMPARE(failures.load(), 0);
    QCOMPARE(inserted.load(), 1);
    QCOMPARE(repo_->statistics().torrents, before.torrents + 1);
    QCOMPARE(repo_->statistics().files, before.files + t.files);
    QCOMPARE(repo_->statistics().totalSize, before.totalSize + t.size);
    const QVector<File> files{{"one.mkv", 12}, {"two.mkv", 34}};
    auto run = [&](int lane) {
        for (int i = 0; i < 20; ++i) {
            bool ok = false;
            if (lane == 0) ok = repo_->incrementVote(t.hash, true).has_value();
            if (lane == 1) ok = repo_->updateTrackerCounts(t.hash, 77, 8, 9);
            if (lane == 2) ok = repo_->updateFiles(t.hash, files);
            if (lane == 3) ok = repo_->mergeInfo(t.hash, {{QString::number(i), i}});
            if (!ok) ++failures;
        }
        manticore_->closeCurrentThreadConnection();
    };
    std::thread vote(run, 0), tracker(run, 1), details(run, 2), info(run, 3);
    vote.join(); tracker.join(); details.join(); info.join();
    QCOMPARE(failures.load(), 0);
    const auto stored = repo_->get(t.hash, true);
    QVERIFY(stored);
    QCOMPARE(stored->good, 20);
    QCOMPARE(stored->seeders, 77);
    QCOMPARE(stored->files, 2);
    QCOMPARE(stored->fileList.size(), 2);
    QCOMPARE(stored->info.size(), 20);
    QCOMPARE(repo_->statistics().files, before.files + 2);
    QVERIFY(repo_->remove(t.hash));
}

void TestManticoreQueries::testConcurrentInsertAndDeleteStatistics()
{
    const auto before = repo_->statistics();
    Torrent t = makeTorrent(92002, "Insert delete fixture", ContentType::Video, 0);
    t.files = 0;
    std::atomic<int> failures{0};
    auto run = [&](bool deleting) {
        rats::service::IndexingService indexing(repo_, nullptr);
        for (int i = 0; i < 30; ++i) {
            if (deleting) { if (!repo_->remove(t.hash)) ++failures; }
            else if (!indexing.insert(t).success) ++failures;
        }
        manticore_->closeCurrentThreadConnection();
    };
    std::thread add(run, false), remove(run, true);
    add.join(); remove.join();
    QCOMPARE(failures.load(), 0);
    QVERIFY(repo_->remove(t.hash));
    QVERIFY(!repo_->exists(t.hash));
    QCOMPARE(repo_->statistics().torrents, before.torrents);
    QCOMPARE(repo_->statistics().files, before.files);
    QCOMPARE(repo_->statistics().totalSize, before.totalSize);
}

void TestManticoreQueries::testCrawlerWorkerConnectionsAndMainThreadSql()
{
    auto& metrics = rats::common::PerformanceMetrics::instance();
    metrics.setEnabled(true);
    const auto connections = QSqlDatabase::connectionNames();
    rats::service::CrawlerPipeline pipeline(repo_, manticore_);
    connect(&pipeline, &rats::service::CrawlerPipeline::torrentIndexed, indexing_,
        &rats::service::IndexingService::torrentIndexed);
    QSignalSpy indexed(indexing_, &rats::service::IndexingService::torrentIndexed);
    for (int run = 0; run < 100; ++run) {
        const auto before = metrics.snapshot()["counters"].toObject();
        Torrent t = makeTorrent(192010 + run, "Crawler worker fixture");
        pipeline.start();
        bool checked = false;
        QCOMPARE(pipeline.checkKnown(t.hash, [&](bool known) { checked = !known; }),
            rats::service::CrawlerPipeline::Admission::Accepted);
        QCOMPARE(pipeline.insert(t), rats::service::CrawlerPipeline::Admission::Accepted);
        QTRY_COMPARE_WITH_TIMEOUT(pipeline.pending(), 0, 10000);
        QVERIFY(checked);
        const auto after = metrics.snapshot()["counters"].toObject();
        QCOMPARE(after["db.main_query_calls"].toInteger(), before["db.main_query_calls"].toInteger());
        QCOMPARE(after["db.main_write_calls"].toInteger(), before["db.main_write_calls"].toInteger());
        QVERIFY(after["db.write_calls"].toInteger() > before["db.write_calls"].toInteger());
        pipeline.stop();
        QCOMPARE(QSqlDatabase::connectionNames().size(), connections.size());
        QVERIFY(repo_->remove(t.hash));
    }
    QCOMPARE(indexed.count(), 100);
}

void TestManticoreQueries::testCrawlerReconnectsAfterDaemonRestart()
{
    // Keep the worker's connection alive while the actual temporary daemon goes
    // away. A virtual retry clock holds retries until it has restarted, avoiding
    // a machine-speed dependency between daemon startup and the retry budget.
    std::atomic<qint64> now{1000000000LL};
    rats::service::CrawlerPipeline::Backend backend;
    backend.knownBatch=[&](const QStringList& hashes){return repo_->checkMany(hashes);};
    backend.close=[&]{manticore_->closeCurrentThreadConnection();};
    backend.timing.clock=[&]{return now.load();};backend.timing.jitter=[](int){return 0;};
    rats::service::CrawlerPipeline pipeline(backend);pipeline.start();pipeline.configure(1000,1);
    bool first=false;pipeline.checkState(makeTorrent(192251,"connection warmup").hash,[&](auto state){first=state==rats::domain::HashState::Missing;});
    QTRY_VERIFY(first);QTRY_COMPARE(pipeline.pending(),0);
    auto& metrics=rats::common::PerformanceMetrics::instance();
    const auto retryBefore=metrics.snapshot()["counters"].toObject()["crawler.db_retries"].toInteger();
    const int port=manticore_->port();manticore_->stop();
    bool replied=false;rats::domain::HashState answer=rats::domain::HashState::Failed;
    pipeline.checkState(makeTorrent(192252,"after disconnect").hash,[&](auto state){replied=true;answer=state;});
    QTRY_VERIFY_WITH_TIMEOUT(metrics.snapshot()["counters"].toObject()["crawler.db_retries"].toInteger()>retryBefore,10000);
    QVERIFY(!replied); // Disconnection must not masquerade as Missing.
    QVERIFY(manticore_->start());QVERIFY(manticore_->waitForReady());QCOMPARE(manticore_->port(),port);
    now+=2000000000LL;
    QTRY_VERIFY_WITH_TIMEOUT(replied,10000);QCOMPARE(answer,rats::domain::HashState::Missing);
    QTRY_COMPARE(pipeline.pending(),0);pipeline.stop();
}

void TestManticoreQueries::testDatabaseMetricOutcomes()
{
    auto& m = rats::common::PerformanceMetrics::instance();
    m.setEnabled(true);
    const auto before = m.snapshot()["counters"].toObject();
    bool ok = true;
    db_->query("SELECT missing_p0_column FROM torrents", {}, &ok);
    QVERIFY(!ok);
    db_->query("SELECT id FROM torrents LIMIT 1", {}, &ok);
    QVERIFY(ok);
    QVERIFY(!db_->execute("UPDATE missing_p0_table SET files = 1 WHERE id = 1"));
    const auto after = m.snapshot()["counters"].toObject();
    auto delta = [&](const QString& key) { return after[key].toInteger() - before[key].toInteger(); };
    QCOMPARE(delta("db.query_calls"), 2);
    QCOMPARE(delta("db.query_failed"), 1);
    QCOMPARE(delta("db.query_ok"), 1);
    QCOMPARE(delta("db.main_query_calls"), 2);
    QCOMPARE(delta("db.write_failed"), 1);
    QCOMPARE(delta("db.main_write_calls"), 1);
}

void TestManticoreQueries::testIndexingMetricOutcomes()
{
    auto& m = rats::common::PerformanceMetrics::instance();
    const auto before = m.snapshot()["counters"].toObject();
    rats::service::FilterSettings settings;
    settings.maxFiles = 1;
    rats::service::FilterPolicy filter(settings);
    rats::service::IndexingService indexing(repo_, &filter);
    Torrent t = makeTorrent(91004, "Metric outcome fixture");
    QVERIFY(indexing.insert(t).success);
    QVERIFY(indexing.insert(t).alreadyExists);
    QVERIFY(!indexing.insert(Torrent {}).success);
    Torrent rejected = makeTorrent(91005, "Rejected metric fixture");
    rejected.files = 2;
    QVERIFY(!indexing.insert(rejected).success);
    const auto after = m.snapshot()["counters"].toObject();
    auto delta = [&](const QString& key) { return after[key].toInteger() - before[key].toInteger(); };
    QCOMPARE(delta("index.single_attempts"), 4);
    QCOMPARE(delta("index.inserted"), 1);
    QCOMPARE(delta("index.merged"), 1);
    QCOMPARE(delta("index.invalid"), 1);
    QCOMPARE(delta("index.rejected"), 1);
    QVERIFY(repo_->remove(t.hash));
}

void TestManticoreQueries::testFileSearchDoesNotExposeSnippetsAsFiles()
{
    Torrent t = makeTorrent(91001, "Collection without matching title");
    t.files = 3;
    t.fileList = { { "folder/uniquefilematch.mkv", 100 }, { "folder/other.txt", 20 }, { "readme.txt", 5 } };
    QVERIFY(repo_->add(t));
    QVERIFY(waitForFiles(t.hash, 3));
    TorrentRepository::SearchQuery q;
    q.text = "uniquefilematch";
    const auto hits = repo_->searchFiles(q);
    QCOMPARE(hits.size(), 1);
    QCOMPARE(hits.first().torrent.files, 3);
    QVERIFY(!hits.first().matchingPaths.isEmpty());
    QVERIFY(hits.first().torrent.fileList.isEmpty());
    QCOMPARE(repo_->filesOf(t.hash).size(), 3);
    QVERIFY(repo_->remove(t.hash));
}

void TestManticoreQueries::testBackfillMissingAndPartialFiles()
{
    for (bool batch : { false, true }) {
        Torrent t = makeTorrent(batch ? 91003 : 91002, "File list repair");
        t.files = 3; // A known count does not prove details have been stored.
        QVERIFY(repo_->add(t));
        QVERIFY(waitForTorrent(t.hash));
        const auto before = repo_->statistics();
        t.fileList = { { "one.txt", 1 }, { "two.txt", 2 }, { "three.txt", 3 } };
        auto merge = [&]() {
            if (batch)
                QCOMPARE(indexing_->insertBatch({ t }).merged, 1);
            else
                QVERIFY(indexing_->insert(t).success);
        };
        const auto complete = t.fileList;
        t.fileList.resize(1);
        merge();
        QVERIFY(repo_->filesOf(t.hash).isEmpty());
        QCOMPARE(repo_->get(t.hash)->files, 3);
        t.fileList = complete;
        merge();
        QVERIFY(waitForFiles(t.hash, 3));
        // Simulate a truncated details row without changing the summary count.
        QVERIFY(db_->replace("files", { { "id", rats::data::rowIdFromHash(t.hash) },
            { "hash", t.hash }, { "path", "one.txt" }, { "size", "1" } }));
        QVERIFY(waitForFiles(t.hash, 1));
        t.good = 10; // The vote rewrite must preserve the repaired count.
        merge();
        QVERIFY(waitForFiles(t.hash, 3));
        QCOMPARE(repo_->get(t.hash)->files, 3);
        QCOMPARE(repo_->statistics().files, before.files);
        // A later partial copy must never replace the complete list.
        t.fileList.resize(1);
        merge();
        QCOMPARE(repo_->filesOf(t.hash).size(), 3);
        QVERIFY(repo_->remove(t.hash));
    }
}

Torrent TestManticoreQueries::makeTorrent(int n, const QString& name, ContentType type, qint64 size, int seeders)
{
    Torrent t;
    // 40-char hex hash: an 8-hex prefix from n, padded with 'a'.
    t.hash = QString("%1").arg(n, 8, 16, QChar('0')) + QString(32, 'a');
    t.name = name;
    t.size = size;
    t.files = 1;
    t.pieceLength = 262144;
    t.added = QDateTime::currentDateTime();
    t.ipv4 = "192.168.1.1";
    t.port = 6881;
    t.contentType = type;
    t.contentCategory = ContentCategory::Unknown;
    t.seeders = seeders;
    t.leechers = 5;
    t.completed = 100;
    return t;
}

bool TestManticoreQueries::waitForTorrent(const QString& hash, int maxRetries, int delayMs)
{
    for (int i = 0; i < maxRetries; ++i) {
        if (repo_->exists(hash))
            return true;
        QThread::msleep(delayMs);
    }
    return repo_->exists(hash);
}

bool TestManticoreQueries::waitForFiles(const QString& hash, int expected, int maxRetries, int delayMs)
{
    // exists() only asks the torrents table. A file list is a row in `files`,
    // written by a separate statement and made visible by the RT index on its own
    // schedule, so waiting for the torrent says nothing about its files yet.
    for (int i = 0; i < maxRetries; ++i) {
        if (repo_->filesOf(hash).size() == expected)
            return true;
        QThread::msleep(delayMs);
    }
    return repo_->filesOf(hash).size() == expected;
}

void TestManticoreQueries::initTestCase()
{
    tempDir_ = new QTemporaryDir();
    QVERIFY2(tempDir_->isValid(), "Failed to create temporary directory");
    qInfo() << "Test data directory:" << tempDir_->path();

    manticore_ = new Manticore(tempDir_->path());
    const bool started = manticore_->start();
    QVERIFY2(started, "Failed to start Manticore Search. Is searchd available?");
    QVERIFY(manticore_->isRunning());
    qInfo() << "Manticore started on port" << manticore_->port();

    db_ = new Database(manticore_);
    QVERIFY(db_->isConnected());

    repo_ = new TorrentRepository(db_);
    repo_->primeFromDatabase();

    filter_ = new rats::service::FilterPolicy();
    indexing_ = new rats::service::IndexingService(repo_, filter_);
}

void TestManticoreQueries::cleanupTestCase()
{
    delete indexing_;
    indexing_ = nullptr;
    delete filter_;
    filter_ = nullptr;
    delete repo_;
    repo_ = nullptr;
    delete db_;
    db_ = nullptr;
    if (manticore_) {
        manticore_->stop();
        delete manticore_;
        manticore_ = nullptr;
    }
    delete tempDir_;
    tempDir_ = nullptr;
}

void TestManticoreQueries::testConnected()
{
    QVERIFY(db_->isConnected());
    QVERIFY(manticore_->isRunning());
}

void TestManticoreQueries::testAddAndGet()
{
    const Torrent t = makeTorrent(1, "Ubuntu Linux ISO", ContentType::Archive, 4LL * 1024 * 1024 * 1024, 42);
    QVERIFY(repo_->add(t));
    QVERIFY(waitForTorrent(t.hash));

    const auto got = repo_->get(t.hash);
    QVERIFY(got.has_value());
    QCOMPARE(got->hash, t.hash);
    QCOMPARE(got->name, QString("Ubuntu Linux ISO"));
    QCOMPARE(got->size, (qint64)(4LL * 1024 * 1024 * 1024));
    QCOMPARE(got->seeders, 42);
    QCOMPARE(rats::domain::toId(got->contentType), rats::domain::toId(ContentType::Archive));
}

void TestManticoreQueries::testExists()
{
    QVERIFY(repo_->exists(makeTorrent(1, "x").hash)); // added in testAddAndGet
    QVERIFY(!repo_->exists(QString(40, 'f'))); // never inserted
}

void TestManticoreQueries::testGetWithFiles()
{
    Torrent t = makeTorrent(2, "Multi File Pack", ContentType::Video, 2000, 5);
    t.files = 2;
    t.fileList.append(File { "folder/movie.mkv", 1900 });
    t.fileList.append(File { "folder/readme.txt", 100 });
    QVERIFY(repo_->add(t));
    QVERIFY(waitForTorrent(t.hash));
    QVERIFY(waitForFiles(t.hash, 2));

    const auto got = repo_->get(t.hash, /*includeFiles=*/true);
    QVERIFY(got.has_value());
    QCOMPARE(got->fileList.size(), 2);
    QCOMPARE(got->fileList[0].path, QString("folder/movie.mkv"));
    QCOMPARE(got->fileList[0].size, (qint64)1900);
}

void TestManticoreQueries::testSearchByName()
{
    QVERIFY(repo_->add(makeTorrent(3, "Debian Server Edition", ContentType::Archive)));
    QVERIFY(waitForTorrent(makeTorrent(3, "x").hash));

    TorrentRepository::SearchQuery q;
    q.text = "Debian";
    q.limit = 10;
    const auto hits = repo_->searchTorrents(q);
    bool found = false;
    for (const auto& h : hits) {
        if (h.torrent.name.contains("Debian"))
            found = true;
    }
    QVERIFY2(found, "Full-text search should find the 'Debian' torrent");
}

void TestManticoreQueries::testSearchByHash()
{
    const Torrent t = makeTorrent(1, "x"); // Ubuntu torrent's hash
    TorrentRepository::SearchQuery q;
    q.text = t.hash; // a 40-hex string is treated as an exact hash lookup
    const auto hits = repo_->searchTorrents(q);
    QCOMPARE(hits.size(), 1);
    QCOMPARE(hits.first().torrent.hash, t.hash);
}

void TestManticoreQueries::testSearchQuerySyntax()
{
    using rats::service::SearchService;
    const QString movie = QStringLiteral("\u7535\u5f71");
    const QString separated = QStringLiteral("\u7535-\u5f71");
    const QString reversed = QStringLiteral("\u5f71\u7535");
    const QString plainTitle = QStringLiteral("StageZ Ubuntu Linux Archive");
    Torrent valid = makeTorrent(91011, movie + " 1080p " + plainTitle, ContentType::Video, 1234, 23);
    Torrent split = makeTorrent(91012, separated + " 1080p StageZ Ubuntux Linux Archive", ContentType::Video, 1234, 55);
    split.info.insert("name", QString(800, u'x') + movie); // beyond the name-index cutoff
    Torrent wrongOrder = makeTorrent(91013, reversed + " 1080p StageZ Linux Ubuntu Archive");
    Torrent fromInfo = makeTorrent(91014, "Metadata StageZ");
    fromInfo.info.insert("name", movie);
    valid.fileList = { { QStringLiteral("clip/") + movie + QStringLiteral(".1080p Ubuntu Linux.mkv"), 1234 } };
    split.fileList = { { QStringLiteral("clip/") + separated + QStringLiteral(".1080p Ubuntux Linux.mkv"), 1234 } };
    wrongOrder.fileList = { { QStringLiteral("clip/") + reversed + QStringLiteral(".1080p Linux Ubuntu.mkv"), wrongOrder.size } };
    for (const auto& torrent : { valid, split, wrongOrder, fromInfo }) {
        QVERIFY(repo_->add(torrent));
        QVERIFY(waitForTorrent(torrent.hash));
        if (!torrent.fileList.isEmpty())
            QVERIFY(waitForFiles(torrent.hash, 1));
    }

    SearchService search(repo_);
    SearchService::Request request;
    auto hashes = [](const auto& hits) {
        QStringList result;
        for (const auto& hit : hits)
            result.append(hit.torrent.hash);
        return result;
    };
    request.sort = "seeders";
    request.limit = 1;
    request.contentType = "video";
    request.sizeMin = 1234;
    request.sizeMax = 1234;
    request.query = movie + " 1080p";
    QCOMPARE(hashes(search.searchTorrents(request)), QStringList{ valid.hash });
    request.query = QStringLiteral("1080p\u3000") + movie;
    QCOMPARE(hashes(search.searchTorrents(request)), QStringList{ valid.hash });
    request.limit = 10;
    QCOMPARE(hashes(search.searchFiles(request)), QStringList{ valid.hash });

    request = SearchService::Request{};
    request.query = movie;
    QStringList names = hashes(search.searchTorrents(request));
    names.sort();
    QStringList expected{ valid.hash, fromInfo.hash };
    expected.sort();
    QCOMPARE(names, expected);
    const auto fileHits = search.searchFiles(request);
    QCOMPARE(hashes(fileHits), QStringList{ valid.hash });
    QVERIFY(fileHits.front().matchingPaths.front().contains(QStringLiteral("<b>") + movie + QStringLiteral("</b>")));
    QVERIFY(SearchService::queryError(movie, true).isEmpty());
    QVERIFY(!SearchService::queryError(QStringLiteral("ab"), true).isEmpty());
    const QString unclosed = QStringLiteral("\"Ubuntu");
    QVERIFY(!SearchService::queryError(unclosed).isEmpty());
    QCOMPARE(SearchService::queryError(unclosed), SearchService::queryError(unclosed, true));
    QVERIFY(!SearchService::queryError(QStringLiteral("\u201c \u201d")).isEmpty());

    request.query = QStringLiteral("\"Ubuntu Linux\" archiv StageZ");
    QCOMPARE(hashes(search.searchTorrents(request)), QStringList{ valid.hash });
    request.query = QStringLiteral("\u201cUbuntu Linux\u201d clip");
    QCOMPARE(hashes(search.searchFiles(request)), QStringList{ valid.hash });
    request.query = QStringLiteral("\"ubun* linux\" StageZ");
    QVERIFY(search.searchTorrents(request).isEmpty());
    request.query = QStringLiteral("ubun* occupancynotpresent");
    QVERIFY(search.searchTorrents(request).isEmpty());
    request.query = QStringLiteral("ubun* StageZ");
    QCOMPARE(search.searchTorrents(request).size(), 3);

    request.query = "  " + valid.hash.toUpper() + "  ";
    QCOMPARE(hashes(search.searchTorrents(request)), QStringList{ valid.hash });
    request.query = "magnet:?dn=Name&xt=urn:btih:" + valid.hash.toUpper();
    QCOMPARE(SearchService::extractInfoHash(request.query), valid.hash);
    QCOMPARE(hashes(search.searchTorrents(request)), QStringList{ valid.hash });
    const QString encodedMagnet = "MAGNET:?dn=" + split.hash + "&xt=urn:btmh:unused&XT=URN%3ABTIH%3A" + valid.hash;
    QCOMPARE(SearchService::extractInfoHash(encodedMagnet), valid.hash);
    request.query = QLatin1Char('"') + valid.hash + QLatin1Char('"');
    QVERIFY(SearchService::extractInfoHash(request.query).isEmpty());
    QVERIFY(search.searchTorrents(request).isEmpty());
    QVERIFY(SearchService::extractInfoHash(QStringLiteral("reference ") + valid.hash).isEmpty());

    for (const auto& torrent : { valid, split, wrongOrder, fromInfo })
        QVERIFY(repo_->remove(torrent.hash));
}

void TestManticoreQueries::testUpdateTrackerCounts()
{
    const Torrent t = makeTorrent(1, "x");
    QVERIFY(repo_->updateTrackerCounts(t.hash, 111, 22, 3));

    const auto got = repo_->get(t.hash);
    QVERIFY(got.has_value());
    QCOMPARE(got->seeders, 111);
    QCOMPARE(got->leechers, 22);
    QCOMPARE(got->completed, 3);
}

void TestManticoreQueries::testFileSearchFiltersAndPages()
{
    QVector<Torrent> entries;
    constexpr int count = 1134;
    for (int index = 0; index < count; ++index)
        entries.append(makeTorrent(936000 + index, "Stage3 path search", ContentType::Video, 4096));
    const auto id = [](const Torrent& t) { return rats::data::rowIdFromHash(t.hash); };
    std::sort(entries.begin(), entries.end(), [&](const Torrent& a, const Torrent& b) { return id(a) < id(b); });
    for (int index = 0; index < count; ++index) {
        Torrent& entry = entries[index];
        entry.files = 2;
        entry.fileList = { { "folder/phase3needle", 2000 }, { "edition.mkv", 2096 } };
        if (index < 1024)
            entry.contentCategory = ContentCategory::XXX;
    }
    const QString longPath = QStringLiteral("folder/") + QString(500, u'x') + "/phase3needle edition.mkv";
    for (int i : { 128, 1040, 1080, 1132, 1133 }) {
        entries[i].fileList = { { longPath, 2000 }, { "unmatched/<b>literal</b>.txt", 2096 } };
        entries[i].seeders = 21;
    }
    entries[1040].name = "Alpha";
    entries[1080].name = "Omega";
    entries[1080].seeders = 45;
    entries[1132].name = "Beta";
    entries[1133].name = "Delta";
    entries[1133].contentType = ContentType::Audio;
    QCOMPARE(repo_->addMany(entries), count);

    auto hashes = [](const auto& hits) {
        QStringList result;
        for (const auto& hit : hits)
            result.append(hit.torrent.hash);
        return result;
    };
    TorrentRepository::SearchQuery query;
    query.text = "phase3needle edition";
    query.safeSearch = true;
    query.contentType = "video";
    query.sizeMin = 4096;
    query.filesMax = 2;
    query.sort = "seeders";
    query.limit = 2;
    QElapsedTimer timer;
    timer.start();
    const auto first = repo_->searchFiles(query);
    qInfo() << "File search, first filtered sorted page:" << timer.elapsed() << "ms";
    const QStringList firstExpected{ entries[1080].hash, entries[1040].hash };
    QCOMPARE(hashes(first), firstExpected);
    for (const auto& hit : first) {
        QCOMPARE(hit.matchingPaths.size(), 1);
        const QString unmarked = QString(hit.matchingPaths.front()).remove("<b>").remove("</b>");
        QCOMPARE(unmarked, longPath);
        QVERIFY(hit.torrent.fileList.isEmpty());
    }
    query.offset = 2;
    QCOMPARE(hashes(repo_->searchFiles(query)), QStringList{ entries[1132].hash });
    query.offset = 0;
    query.limit = 10;
    const QStringList allExpected{ entries[1080].hash, entries[1040].hash, entries[1132].hash };
    QCOMPARE(hashes(repo_->searchFiles(query)), allExpected);
    query.sort = "name";
    query.descending = false;
    query.text = "\"phase3needle edition\"";
    const QStringList byName{ entries[1040].hash, entries[1132].hash, entries[1080].hash };
    QCOMPARE(hashes(repo_->searchFiles(query)), byName);

    query = TorrentRepository::SearchQuery{};
    query.text = "phase3needle";
    query.offset = 1025;
    query.limit = 4;
    QStringList deepPage;
    for (int i = query.offset; i < query.offset + query.limit; ++i)
        deepPage.append(entries[i].hash);
    timer.restart();
    const auto deep = repo_->searchFiles(query);
    qInfo() << "File search, unfiltered deep page:" << timer.elapsed() << "ms";
    QCOMPARE(hashes(deep), deepPage);

    for (const Torrent& entry : entries)
        QVERIFY(repo_->remove(entry.hash));
}

void TestManticoreQueries::testSearchReindexResumes()
{
    class MigrationDatabase : public Database {
    public:
        using Database::Database;
        int titleBatches = 0;
        int fileBatches = 0;
        bool failSecondFiles = false;
    protected:
        bool runWrite(const QString& sql, const char* operation, const QString& table) override
        {
            if (qstrcmp(operation, "REPLACE") == 0) {
                if (table == "torrents")
                    ++titleBatches;
                if (table == "files" && ++fileBatches == 2 && failSecondFiles) {
                    // Simulate a lost acknowledgment after the engine applied
                    // this page. The checkpoint must stay at the prior page.
                    Database::runWrite(sql, operation, table);
                    return false;
                }
            }
            return Database::runWrite(sql, operation, table);
        }
    };

    QTemporaryDir oldData;
    QVERIFY(oldData.isValid());
    Manticore engine(oldData.path());
    QVERIFY(engine.start());
    MigrationDatabase database(&engine);
    TorrentRepository repository(&database);
    QFile config(oldData.filePath("sphinx.conf"));
    QVERIFY(config.open(QIODevice::ReadOnly));
    QByteArray legacy = config.readAll();
    config.close();
    for (const QByteArray& line : { QByteArray("    charset_table = non_cont"), QByteArray("    ngram_len = 1"),
             QByteArray("    ngram_chars = cont") })
        legacy.replace(line, "");
    QVERIFY(config.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(config.write(legacy), legacy.size());
    config.close();
    QVERIFY(database.execute("ALTER TABLE torrents RECONFIGURE"));
    QVERIFY(database.execute("ALTER TABLE files RECONFIGURE"));

    constexpr int count = 505;
    const QString movie = QStringLiteral("\u7535\u5f71");
    QVector<Torrent> samples;
    for (int i = 0; i < count; ++i) {
        Torrent t = makeTorrent(949000 + i, movie + QString(" Migration fixture %1").arg(i), ContentType::Video, 1200, i);
        t.hash[0] = u'6'; // exercises 63-bit cursors above JSON's double precision
        t.info.insert("name", QStringLiteral("\u526a\u8f91\u7248"));
        t.info.insert("custom", QStringLiteral("kept 'exactly'"));
        t.good = 3;
        t.bad = 4;
        t.trackersChecked = QDateTime::fromSecsSinceEpoch(1700000000);
        t.fileList = { { QStringLiteral("root/") + movie + QStringLiteral(".mkv"), 1200 } };
        samples.append(t);
    }
    Torrent missingFiles = makeTorrent(950000, movie + " summary");
    const Torrent emptyFileRow = makeTorrent(950002, movie + " empty file row");
    samples.append(missingFiles);
    samples.append(emptyFileRow);
    QCOMPARE(repository.addMany(samples), count + 2);
    QVERIFY(database.replace("files", { { "id", rats::data::rowIdFromHash(emptyFileRow.hash) },
        { "hash", emptyFileRow.hash }, { "path", QStringLiteral("") }, { "size", QStringLiteral("") } }));
    QVERIFY(database.execute("FLUSH RAMCHUNK torrents"));
    QVERIFY(database.execute("FLUSH RAMCHUNK files"));
    TorrentRepository::SearchQuery query;
    query.text = movie;
    QVERIFY(repository.searchTorrents(query).isEmpty());
    QVERIFY(repository.searchFiles(query).isEmpty());
    const auto readTitles = [&] { return database.query("SELECT * FROM torrents ORDER BY id ASC LIMIT 600"); };
    const auto readFiles = [&] { return database.query("SELECT * FROM files ORDER BY id ASC LIMIT 600"); };
    const auto oldTitles = readTitles();
    const auto oldFiles = readFiles();
    const auto oldEmptyFileCount = repository.filesOf(emptyFileRow.hash).size();
    QCOMPARE(oldTitles.size(), count + 2);
    QCOMPARE(oldFiles.size(), count + 1);
    QVERIFY(oldTitles[499].value("id").toLongLong() > 9007199254740991LL);
    QVERIFY(oldFiles[499].value("id").toLongLong() > 9007199254740991LL);
    engine.stop();
    QVERIFY(engine.start()); // production startup writes the new configuration

    const auto state = [&] {
        QFile file(oldData.filePath("migrations.json"));
        if (!file.open(QIODevice::ReadOnly))
            return QJsonObject{};
        return QJsonDocument::fromJson(file.readAll()).object();
    };
    QElapsedTimer timer;
    timer.start();
    database.titleBatches = database.fileBatches = 0;
    {
        rats::service::MigrationService migration(oldData.path(), &database, &repository, nullptr);
        connect(&migration, &rats::service::MigrationService::migrationProgress, this,
            [&](const QString&, qint64 current, qint64) {
                if (current == 500)
                    migration.shutdown();
            });
        QVERIFY(!migration.runSyncMigrations());
        QVERIFY(!migration.searchIndexReady());
    }
    QCOMPARE(database.titleBatches, 1);
    QCOMPARE(database.fileBatches, 0);
    auto checkpoints = state().value("searchReindex").toObject();
    QCOMPARE(checkpoints.value("torrents").toObject().value("afterId").toString(),
        oldTitles[499].value("id").toString());
    QCOMPARE(checkpoints.value("torrents").toObject().value("rows").toInteger(), 500);

    database.titleBatches = database.fileBatches = 0;
    database.failSecondFiles = true;
    {
        rats::service::MigrationService migration(oldData.path(), &database, &repository, nullptr);
        QVERIFY(!migration.runSyncMigrations());
        QVERIFY(!migration.searchIndexReady());
    }
    QCOMPARE(database.titleBatches, 1);
    QCOMPARE(database.fileBatches, 2);
    checkpoints = state().value("searchReindex").toObject();
    QVERIFY(checkpoints.value("torrents").toObject().value("complete").toBool());
    QCOMPARE(checkpoints.value("files").toObject().value("rows").toInteger(), 500);
    QCOMPARE(checkpoints.value("files").toObject().value("afterId").toString(),
        oldFiles[499].value("id").toString());

    engine.stop();
    QVERIFY(engine.start());
    database.titleBatches = database.fileBatches = 0;
    database.failSecondFiles = false;
    {
        rats::service::MigrationService migration(oldData.path(), &database, &repository, nullptr);
        QVERIFY(migration.runSyncMigrations());
        QVERIFY(migration.searchIndexReady());
        QVERIFY(migration.runSyncMigrations());
    }
    qInfo() << "Search reindex with deliberate interruption, failed batch and daemon restart:" << timer.elapsed() << "ms";
    QCOMPARE(database.titleBatches, 0);
    QCOMPARE(database.fileBatches, 1);
    QCOMPARE(readTitles(), oldTitles);
    QCOMPARE(readFiles(), oldFiles);
    const auto completed = state();
    QVERIFY(completed.contains("completedMigrations"));
    QVERIFY(!completed.contains("searchReindex"));
    query.limit = 600;
    QCOMPARE(repository.searchTorrents(query).size(), count + 2);
    QCOMPARE(repository.searchFiles(query).size(), count);
    QVERIFY(repository.filesOf(missingFiles.hash).isEmpty());
    QCOMPARE(repository.filesOf(emptyFileRow.hash).size(), oldEmptyFileCount);
    Torrent later = makeTorrent(950001, movie + " post-migration");
    later.fileList = { { movie + ".mkv", later.size } };
    QVERIFY(repository.add(later));
    QCOMPARE(repository.searchFiles(query).size(), count + 1);
}

void TestManticoreQueries::testFuzzySearchMode()
{
    using rats::service::SearchService;
    const QString correctMovie = QStringLiteral("\u661f\u9645\u7a7f\u8d8a");
    const QString mistypedMovie = QStringLiteral("\u661f\u9645\u7a7f\u60a6");
    Torrent exact = makeTorrent(962001, "Ubntu Linux StageFour", ContentType::Video, 1200, 5);
    Torrent corrected = makeTorrent(962002, "Ubuntu Linux StageFour", ContentType::Video, 1200, 20);
    Torrent wrongPhrase = makeTorrent(962003, "Ubuntu Linux Classic StageFour", ContentType::Video, 1200, 35);
    Torrent split = makeTorrent(962004, "File search fixture", ContentType::Video, 1200, 40);
    Torrent excluded = makeTorrent(962005, "Ubuntu Linux StageFour", ContentType::Video, 1200, 100);
    excluded.contentCategory = ContentCategory::XXX;
    Torrent reordered = makeTorrent(962006, "Linux Ubuntu StageFour", ContentType::Video, 1200, 20);
    Torrent foreign = makeTorrent(962007, correctMovie + " StageFour", ContentType::Video, 1200, 7);
    Torrent foreignDecoy = makeTorrent(962008, QStringLiteral("\u592a\u7a7a\u7a7f\u60a6 StageFour"));
    Torrent overlap = makeTorrent(962009, "Ubntu Ubuntu StageOverlap");
    for (Torrent* torrent : { &exact, &corrected, &wrongPhrase, &excluded, &reordered, &foreign, &foreignDecoy, &overlap })
        torrent->fileList = { { "root/" + torrent->name + ".mkv", torrent->size } };
    split.files = 2;
    split.fileList = { { "Ubuntu", 600 }, { "Linux StageFour.mkv", 600 } };
    const QVector<Torrent> fixture{ exact, corrected, wrongPhrase, split, excluded, reordered, foreign, foreignDecoy, overlap };
    QCOMPARE(repo_->addMany(fixture), fixture.size());

    SearchService search(repo_);
    SearchService::Request request;
    request.sort = "seeders";
    request.safeSearch = true;
    request.query = "\"Linux StageFour\" ubntu";
    const QStringList ordered{ exact.hash, corrected.hash };
    const auto hashes = [](const auto& hits) {
        QStringList result;
        for (const auto& hit : hits)
            result.append(hit.torrent.hash);
        return result;
    };
    for (bool files : { false, true }) {
        const auto run = [&](const SearchService::Request& req) {
            return files ? search.searchFiles(req) : search.searchTorrents(req);
        };
        QCOMPARE(hashes(run(request)), QStringList{ exact.hash });
        auto fuzzy = request;
        fuzzy.fuzzy = true;
        auto hits = run(fuzzy);
        QCOMPARE(hashes(hits), ordered);
        QVERIFY(!hits.front().fuzzy);
        QVERIFY(hits.back().fuzzy);
        const auto json = rats::domain::codec::toJson(hits.back());
        QVERIFY(json.value("fuzzy").toBool());
        QVERIFY(rats::domain::codec::searchHitFromJson(json).fuzzy);
        QVERIFY(!rats::domain::codec::toJson(hits.front()).contains("fuzzy"));
        if (files) {
            QCOMPARE(hits.back().matchingPaths.size(), 1);
            QVERIFY(hits.back().matchingPaths.front().contains("Ubuntu"));
        }
        fuzzy.limit = 1;
        QCOMPARE(hashes(run(fuzzy)), QStringList{ exact.hash });
        fuzzy.offset = 1;
        QCOMPARE(hashes(run(fuzzy)), QStringList{ corrected.hash });
        fuzzy.offset = 2;
        QVERIFY(run(fuzzy).isEmpty());
        fuzzy = request;
        fuzzy.fuzzy = true;
        for (const QString& spelling : { QStringLiteral("xbuntu"), QStringLiteral("uubuntu") }) {
            fuzzy.query = "\"Linux StageFour\" " + spelling;
            QCOMPARE(hashes(run(fuzzy)), QStringList{ corrected.hash });
        }
        fuzzy.query = "\"StageFour\" " + mistypedMovie;
        QCOMPARE(hashes(run(fuzzy)), QStringList{ foreign.hash });
        fuzzy.query = "\"StageFour\" xbuntu linx";
        QVERIFY(run(fuzzy).isEmpty());
        fuzzy.query = "\"ubntu Linux\"";
        QCOMPARE(hashes(run(fuzzy)), QStringList{ exact.hash });
        fuzzy.query = "\"Linux StageFour\" xbunt*";
        QVERIFY(run(fuzzy).isEmpty());
        fuzzy.query = "\"StageOverlap\" ubntu";
        hits = run(fuzzy);
        QCOMPARE(hashes(hits), QStringList{ overlap.hash });
        QVERIFY(!hits.front().fuzzy);
        fuzzy.query = "\"Linux StageFour\" ub";
        auto off = fuzzy;
        off.fuzzy = false;
        QCOMPARE(hashes(run(fuzzy)), hashes(run(off)));
        if (!files) {
            fuzzy.query = corrected.magnetLink();
            hits = run(fuzzy);
            QCOMPARE(hashes(hits), QStringList{ corrected.hash });
            QVERIFY(!hits.front().fuzzy);
        }

        QVector<qint64> times;
        auto measured = request;
        measured.fuzzy = true;
        for (int i = 0; i < 26; ++i) {
            QElapsedTimer timer;
            timer.start();
            hits = run(measured);
            if (i > 0)
                times.append(timer.nsecsElapsed());
        }
        std::sort(times.begin(), times.end());
        qInfo() << "Fuzzy warm fixture p95" << (files ? "files" : "titles") << times[23] / 1000000.0 << "ms";
    }
    for (const auto& torrent : fixture)
        QVERIFY(repo_->remove(torrent.hash));
}

void TestManticoreQueries::testFuzzySearchBudget()
{
    constexpr int count = 90;
    QVector<Torrent> fixture;
    for (int i = 0; i < count; ++i)
        fixture.append(makeTorrent(964000 + i, "Ubuntu FuzzyBudgetFixture"));
    const auto exact = makeTorrent(965000, "Xbuntu FuzzyBudgetFixture");
    fixture.append(exact);
    QCOMPARE(repo_->addMany(fixture), fixture.size());
    TorrentRepository::SearchQuery query;
    query.text = "xbuntu \"FuzzyBudgetFixture\"";
    query.fuzzy = true;
    query.limit = 100;
    const auto hits = repo_->searchTorrents(query);
    QCOMPARE(hits.size(), 65);
    QCOMPARE(hits.front().torrent.hash, exact.hash);
    QVERIFY(!hits.front().fuzzy);
    for (qsizetype i = 1; i < hits.size(); ++i)
        QVERIFY(hits[i].fuzzy);
    query.offset = 63;
    query.limit = 5;
    const auto tail = repo_->searchTorrents(query);
    QCOMPARE(tail.size(), 2);
    QCOMPARE(tail[0].torrent.hash, hits[63].torrent.hash);
    QCOMPARE(tail[1].torrent.hash, hits[64].torrent.hash);

    class DelayedDictionary : public Database {
    public:
        using Database::Database;
        Rows query(const QString& sql, const QVariantList& params, bool* ok) override
        {
            if (sql.startsWith(QLatin1String("CALL KEYWORDS")))
                QThread::msleep(180);
            return Database::query(sql, params, ok);
        }
    } database(manticore_);
    TorrentRepository delayed(&database);
    query.offset = 0;
    query.limit = 100;
    const auto afterDeadline = delayed.searchTorrents(query);
    QCOMPARE(afterDeadline.size(), 1);
    QCOMPARE(afterDeadline.front().torrent.hash, exact.hash);
    QVERIFY(!afterDeadline.front().fuzzy);
    for (const auto& torrent : fixture)
        QVERIFY(repo_->remove(torrent.hash));
}

void TestManticoreQueries::testStatistics()
{
    const auto stats = repo_->statistics();
    // At least the three torrents added above.
    QVERIFY2(stats.torrents >= 3, qPrintable(QString("torrents=%1").arg(stats.torrents)));
    QVERIFY(stats.totalSize > 0);
}

void TestManticoreQueries::testRecent()
{
    const auto recent = repo_->recent(10);
    QVERIFY(!recent.isEmpty());
}

void TestManticoreQueries::testTop()
{
    // Ubuntu torrent now has 111 seeders; it should appear in the top list.
    const auto top = repo_->top(QString(), QString(), 0, 10);
    QVERIFY(!top.isEmpty());
    // Ordered by seeders descending.
    for (int i = 1; i < top.size(); ++i)
        QVERIFY(top[i - 1].seeders >= top[i].seeders);
}

void TestManticoreQueries::testRemove()
{
    const Torrent t = makeTorrent(3, "x"); // Debian torrent
    QVERIFY(repo_->exists(t.hash));
    QVERIFY(repo_->remove(t.hash));
    // Removal should eventually be visible.
    bool gone = false;
    for (int i = 0; i < 50; ++i) {
        if (!repo_->exists(t.hash)) {
            gone = true;
            break;
        }
        QThread::msleep(20);
    }
    QVERIFY2(gone, "Torrent should be removed");
}

void TestManticoreQueries::testSelectQueryAgainstLiveIndex()
{
    // Exercise the SelectQuery builder end-to-end against the live index.
    const QString sql = SelectQuery("torrents")
                            .columns("id, hash, name")
                            .whereRaw("seeders > 0")
                            .orderBy("seeders", true)
                            .limit(0, 5)
                            .build();
    bool ok = false;
    const auto rows = db_->query(sql, {}, &ok);
    QVERIFY(ok);
    // The Ubuntu torrent (111 seeders) guarantees at least one row.
    QVERIFY(!rows.isEmpty());
}

// A full-index sweep (the `torrent.cleanup` maintenance path) must be able to
// walk past Manticore's max_matches, which rejects any OFFSET >= 1000 with
// "offset out of bounds" — the reason the filter cleanup used to stop after the
// first 1000 torrents.
void TestManticoreQueries::testPageAfterIdWalksPastMaxMatches()
{
    constexpr int kExtra = 1200;
    const int base = 10000;
    for (int i = 0; i < kExtra; ++i)
        QVERIFY(repo_->add(makeTorrent(base + i, QString("Sweep Sample %1").arg(i))));
    QVERIFY(waitForTorrent(makeTorrent(base + kExtra - 1, "x").hash));

    // Plain OFFSET paging is what breaks: prove the limit is real, so this test
    // fails loudly if the sweep ever goes back to offsets.
    bool ok = false;
    db_->query(SelectQuery("torrents").orderBy("id", false).limit(1000, 500).build(), {}, &ok);
    QVERIFY2(!ok, "Manticore should reject an offset of 1000 (max_matches)");

    int seen = 0;
    qint64 afterId = 0;
    qint64 previousId = 0;
    for (;;) {
        const QVector<Torrent> batch = repo_->pageAfterId(afterId, 500);
        if (batch.isEmpty())
            break;
        for (const Torrent& t : batch) {
            QVERIFY(t.id > previousId); // strictly ascending, no repeats
            previousId = t.id;
            ++seen;
        }
        afterId = batch.last().id;
    }
    QVERIFY2(seen >= kExtra, qPrintable(QString("swept %1 of >= %2 torrents").arg(seen).arg(kExtra)));
    QCOMPARE(static_cast<qint64>(seen), repo_->statistics().torrents);
}

// ---------------------------------------------------------------------------
// Row ids derived from the infohash
// ---------------------------------------------------------------------------

void TestManticoreQueries::testRowIdIsDerivedFromHash()
{
    const Torrent t = makeTorrent(770001, "Derived Id Sample");
    QVERIFY(repo_->add(t));
    QVERIFY(waitForTorrent(t.hash));

    const auto stored = repo_->get(t.hash);
    QVERIFY(stored.has_value());
    // The row id is not a counter value: it is the hash, and a peer computing it
    // from the same hash must arrive at the same number.
    QCOMPARE(stored->id, rats::data::rowIdFromHash(t.hash));
}

void TestManticoreQueries::testFilesRowSharesTheTorrentId()
{
    Torrent t = makeTorrent(770002, "Shared Id Sample");
    t.fileList = { File { "shared/one.mkv", 111 }, File { "shared/two.mkv", 222 } };
    t.files = t.fileList.size();
    QVERIFY(repo_->add(t));
    QVERIFY(waitForTorrent(t.hash));
    QVERIFY(waitForFiles(t.hash, 2));

    // The files row carrying the torrent's own id is what turns the export join
    // into a docid lookup, so assert the id itself rather than just the contents.
    const qint64 expected = rats::data::rowIdFromHash(t.hash);
    const auto rows = db_->query(QStringLiteral("SELECT id FROM files WHERE id = %1").arg(expected));
    QCOMPARE(rows.size(), 1);

    const QVector<File> files = repo_->filesOf(t.hash);
    QCOMPARE(files.size(), 2);
    QCOMPARE(files.at(0).path, QString("shared/one.mkv"));
}

void TestManticoreQueries::testReAddDoesNotDuplicateRow()
{
    // With a derived id, adding the same torrent twice targets one row, and the
    // second add leaves the stored copy alone. Under the old counter the same
    // sequence produced two rows carrying one hash.
    Torrent t = makeTorrent(770003, "Duplicate Guard");
    QVERIFY(repo_->add(t));
    QVERIFY(waitForTorrent(t.hash));

    t.name = "Duplicate Guard Reindexed";
    QVERIFY(repo_->add(t));

    const auto rows
        = db_->query(QStringLiteral("SELECT id FROM torrents WHERE id = %1").arg(rats::data::rowIdFromHash(t.hash)));
    QCOMPARE(rows.size(), 1);
    QCOMPARE(repo_->get(t.hash)->name, QString("Duplicate Guard"));
}

void TestManticoreQueries::testRowIdCollisionIsRefused()
{
    // Two hashes that agree on their first 64 bits and differ afterwards map to
    // one row. The second one must be refused, never stored over the first.
    const QString shared = QStringLiteral("beefbeefbeefbeef");
    Torrent first = makeTorrent(1, "Collision First");
    first.hash = shared + QString(24, QLatin1Char('1'));
    Torrent second = makeTorrent(2, "Collision Second");
    second.hash = shared + QString(24, QLatin1Char('2'));
    QCOMPARE(rats::data::rowIdFromHash(first.hash), rats::data::rowIdFromHash(second.hash));
    const auto staleSlot = repo_->resolve(second.hash);
    QVERIFY(repo_->add(first));
    QVERIFY(waitForTorrent(first.hash));

    QVERIFY2(!repo_->add(second), "a torrent whose row id belongs to another must be refused");
    QVERIFY2(!repo_->add(second, staleSlot), "a stale empty slot must not bypass collision checks");

    // The original survives untouched, and the impostor is not findable.
    const auto stored = repo_->get(first.hash);
    QVERIFY(stored.has_value());
    QCOMPARE(stored->name, QString("Collision First"));
    QVERIFY(!repo_->get(second.hash).has_value());
    QVERIFY(!repo_->exists(second.hash));

    // Batch lookups must reach the same verdict, and report which hash lost.
    QSet<QString> collided;
    const auto found = repo_->getMany({ first.hash, second.hash }, &collided);
    QVERIFY(found.contains(first.hash));
    QVERIFY(!found.contains(second.hash));
    QVERIFY(collided.contains(second.hash));
}

void TestManticoreQueries::testBatchRowIdCollisionIsRefused()
{
    // The collision add() catches needs a stored row to compare against. Two
    // torrents that are both *new* to the index have none, so a batch write is the
    // one path where nothing upstream can see the clash: getMany() finds neither,
    // reports neither as collided, and both arrive here as "known absent".
    const QString shared = QStringLiteral("cafecafecafecafe");
    Torrent first = makeTorrent(1, "Batch Collision First");
    first.hash = shared + QString(24, QLatin1Char('a'));
    Torrent second = makeTorrent(2, "Batch Collision Second");
    second.hash = shared + QString(24, QLatin1Char('b'));
    QCOMPARE(rats::data::rowIdFromHash(first.hash), rats::data::rowIdFromHash(second.hash));
    QVERIFY(!repo_->exists(first.hash));
    QVERIFY(!repo_->exists(second.hash));

    const auto before = repo_->statistics();
    // One row written, not two: the count is the contract the statistics ride on.
    QCOMPARE(repo_->addMany({ first, second }), 1);
    QVERIFY(waitForTorrent(first.hash));

    // The first one owns the row, whole and unmixed, and the loser stayed out.
    const auto stored = repo_->get(first.hash);
    QVERIFY(stored.has_value());
    QCOMPARE(stored->name, QString("Batch Collision First"));
    QVERIFY(!repo_->exists(second.hash));

    // The statistics must describe the row that exists, not the rows handed in --
    // an over-count here never heals, since nothing recomputes it until a restart.
    QCOMPARE(repo_->statistics().torrents, before.torrents + 1);
    QCOMPARE(repo_->statistics().files, before.files + first.files);
    QCOMPARE(repo_->statistics().totalSize, before.totalSize + first.size);

    // Re-priming from the index must agree with the counters kept in memory.
    const auto inMemory = repo_->statistics();
    repo_->primeFromDatabase();
    QCOMPARE(repo_->statistics().torrents, inMemory.torrents);
    QCOMPARE(repo_->statistics().totalSize, inMemory.totalSize);
}

// The same hash twice is not a collision: it is one torrent, and collapsing it is
// ordinary dedupe rather than a refusal. Dump batches repeat hashes routinely.
void TestManticoreQueries::testBatchDuplicateHashCollapsesToOneRow()
{
    Torrent t = makeTorrent(92030, "Batch Duplicate");
    const auto before = repo_->statistics();

    QCOMPARE(repo_->addMany({ t, t }), 1);
    QVERIFY(waitForTorrent(t.hash));

    QCOMPARE(repo_->statistics().torrents, before.torrents + 1);
    const auto stored = repo_->get(t.hash);
    QVERIFY(stored.has_value());
    QCOMPARE(stored->name, QString("Batch Duplicate"));
    // Replaying a batch must not overwrite an existing row or increment stats.
    t.name = "Stale replay";
    QCOMPARE(repo_->addMany({t, t}), 0);
    QCOMPARE(repo_->statistics().torrents, before.torrents + 1);
    QCOMPARE(repo_->get(t.hash)->name, QString("Batch Duplicate"));
}

void TestManticoreQueries::testMigrationReKeysLegacyRows()
{
    // Write rows the way the pre-migration code did: a small sequential id that
    // has nothing to do with the hash. The repository can no longer produce these,
    // so they go in through the raw database layer.
    struct Legacy {
        qint64 id;
        QString hash;
        QString name;
    };
    const QVector<Legacy> legacy {
        { 41, QStringLiteral("11110000") + QString(32, QLatin1Char('d')), QStringLiteral("Legacy One") },
        { 42, QStringLiteral("22220000") + QString(32, QLatin1Char('d')), QStringLiteral("Legacy Two") },
    };

    for (const Legacy& row : legacy) {
        QVERIFY(db_->insert("torrents",
            QVariantMap { { "id", row.id }, { "hash", row.hash }, { "name", row.name }, { "nameIndex", row.name },
                { "size", 4096 }, { "files", 1 }, { "piecelength", 262144 },
                { "added", QDateTime::currentSecsSinceEpoch() }, { "ipv4", "10.0.0.1" }, { "port", 6881 },
                { "contentType", 1 }, { "contentCategory", 0 }, { "seeders", 1 }, { "leechers", 1 }, { "completed", 1 },
                { "trackersChecked", 0 }, { "good", 0 }, { "bad", 0 } }));
        QVERIFY(db_->insert("files",
            QVariantMap { { "id", row.id }, { "hash", row.hash }, { "path", "legacy/file.bin" }, { "size", "4096" } }));
    }

    // A legacy row is invisible to a lookup precisely because its id is wrong —
    // that is what the migration exists to fix.
    QVERIFY(!repo_->exists(legacy.first().hash));

    for (const QString& table : { QStringLiteral("torrents"), QStringLiteral("files") }) {
        qint64 cursor = 0;
        for (;;) {
            const auto page = repo_->migrateRowIdPage(table, cursor, 500);
            // A failed page leaves the cursor where it was, so looping on
            // `finished` alone would spin forever.
            QVERIFY2(!page.failed, qPrintable("migration page failed on " + table));
            if (page.finished)
                break;
        }
    }

    for (const Legacy& row : legacy) {
        QVERIFY2(waitForTorrent(row.hash), qPrintable("not reachable after migration: " + row.name));
        QVERIFY2(waitForFiles(row.hash, 1), qPrintable("files not reachable after migration: " + row.name));
        const auto stored = repo_->get(row.hash, /*includeFiles*/ true);
        QVERIFY(stored.has_value());
        QCOMPARE(stored->name, row.name);
        // Re-keyed, and the full-text column survived the rewrite.
        QCOMPARE(stored->id, rats::data::rowIdFromHash(row.hash));
        QCOMPARE(stored->fileList.size(), 1);
        QCOMPARE(stored->fileList.at(0).path, QString("legacy/file.bin"));
        // The old row is gone rather than left behind as a duplicate.
        QVERIFY(db_->query(QStringLiteral("SELECT id FROM torrents WHERE id = %1").arg(row.id)).isEmpty());
    }

    TorrentRepository::SearchQuery q;
    q.text = "Legacy One";
    q.limit = 10;
    QVERIFY2(!repo_->searchTorrents(q).isEmpty(), "re-keyed row must still be findable by name");

    // Running it again must be a no-op, not a second rewrite.
    qint64 cursor = 0;
    int rewritten = 0;
    for (;;) {
        const auto page = repo_->migrateRowIdPage(QStringLiteral("torrents"), cursor, 500);
        QVERIFY(!page.failed);
        if (page.finished)
            break;
        rewritten += page.rewritten;
    }
    QCOMPARE(rewritten, 0);
}

void TestManticoreQueries::testUpdateAppliesAndKeepsSearchable()
{
    // update() used to be a partial UPDATE naming nameIndex, which Manticore
    // rejects outright as a full-text field -- so nothing was written and vote
    // merges vanished. Pin that it now actually applies.
    Torrent t = makeTorrent(770004, "Updatable Sample zephyr");
    t.good = 1;
    QVERIFY(repo_->add(t));
    QVERIFY(waitForTorrent(t.hash));

    Torrent changed = *repo_->get(t.hash);
    changed.good = 42;
    changed.bad = 7;
    changed.seeders = 999;
    QVERIFY(repo_->update(changed));

    const auto stored = repo_->get(t.hash);
    QVERIFY(stored.has_value());
    QCOMPARE(stored->good, 42);
    QCOMPARE(stored->bad, 7);
    QCOMPARE(stored->seeders, 999);
    // Rewriting the row must not cost it its place in the full-text index, and
    // must not leave a second row behind.
    QCOMPARE(stored->id, rats::data::rowIdFromHash(t.hash));
    TorrentRepository::SearchQuery q;
    q.text = "zephyr";
    q.limit = 10;
    QCOMPARE(repo_->searchTorrents(q).size(), 1);
}

// ============================================================================
// Whole-database export / import
// ============================================================================
//
// These drive the real DatabaseExporter and DatabaseImporter against a live
// index, which is the only place the two halves meet: the exporter's keyset
// sweep, its file-list join and its background serialiser on one side, the
// importer's batched merge through IndexingService on the other.

void TestManticoreQueries::testExportImportRoundTrip()
{
    // A distinct hash space, so the rows the other tests left behind cannot make
    // this one pass by accident.
    const int kCount = 1200; // several frames, and past max_matches
    QSet<QString> written;
    QVector<Torrent> batch;
    for (int i = 0; i < kCount; ++i) {
        Torrent t = makeTorrent(500000 + i, QString("roundtrip subject %1").arg(i));
        t.fileList = { File { QString("dir/file-%1.mkv").arg(i), 4096 + i } };
        t.files = 1;
        written.insert(t.hash);
        batch.append(t);
    }
    QCOMPARE(repo_->addMany(batch), kCount);
    QVERIFY(waitForTorrent(batch.last().hash));
    QVERIFY(waitForFiles(batch.first().hash, 1));

    const QString path = QDir(tempDir_->path()).absoluteFilePath("roundtrip.ratsdb");
    rats::service::DatabaseExporter exporter(repo_);
    rats::service::CancelToken noCancel;
    qint64 lastReported = 0;
    const auto result = exporter.run(
        path, rats::service::dump::Header {}, noCancel, [&](qint64 torrents, qint64) { lastReported = torrents; });

    QVERIFY2(result.ok, qPrintable(result.error));
    QVERIFY(result.torrents >= kCount); // plus whatever earlier tests left in the index
    QCOMPARE(lastReported, result.torrents);
    QVERIFY(result.bytes > 0);
    QCOMPARE(QFileInfo(path).size(), result.bytes);

    // Read it straight back and confirm every hash we wrote came out again, with
    // its file list intact — the join is a separate query from the page sweep, and
    // losing it would be silent.
    rats::service::DumpReader reader;
    QString error;
    QVERIFY2(reader.open(path, &error), qPrintable(error));
    QSet<QString> seen;
    int withFiles = 0;
    QVector<Torrent> frame;
    while (reader.readBatch(frame, &error)) {
        for (const Torrent& t : frame) {
            if (!written.contains(t.hash))
                continue;
            seen.insert(t.hash);
            if (t.fileList.size() == 1)
                ++withFiles;
        }
    }
    QVERIFY(reader.atEnd());
    QVERIFY2(reader.complete(), "a finished export must carry its end marker and footer");
    QCOMPARE(seen.size(), kCount);
    QCOMPARE(withFiles, kCount);
}

void TestManticoreQueries::testImportIsAMergeNotADuplicate()
{
    const int kCount = 40;
    QVector<Torrent> batch;
    for (int i = 0; i < kCount; ++i) {
        Torrent t = makeTorrent(600000 + i, QString("merge subject %1").arg(i));
        t.fileList = { File { QString("m/%1.bin").arg(i), 100 + i } };
        t.files = 1;
        batch.append(t);
    }
    QCOMPARE(repo_->addMany(batch), kCount);
    QVERIFY(waitForTorrent(batch.last().hash));

    const QString path = QDir(tempDir_->path()).absoluteFilePath("merge.ratsdb");
    rats::service::DatabaseExporter exporter(repo_);
    rats::service::CancelToken noCancel;
    QVERIFY(exporter.run(path, rats::service::dump::Header {}, noCancel, {}).ok);

    const qint64 before = repo_->statistics().torrents;

    // Importing our own dump must add nothing: every row is already ours. This is
    // the invariant that makes a dump merge idempotent, and it only holds because
    // a row id is derived from the infohash rather than handed out by a counter.
    rats::service::DatabaseImporter importer(indexing_);
    rats::service::DatabaseImporter::Options options;
    options.applyFilters = false;
    const auto result = importer.run(path, options, noCancel, {});

    QVERIFY2(result.ok, qPrintable(result.error));
    QVERIFY(!result.truncated);
    QCOMPARE(result.inserted, 0LL);
    QVERIFY(result.merged >= kCount);
    QCOMPARE(repo_->statistics().torrents, before);
}

void TestManticoreQueries::testCancelledExportLeavesNoFile()
{
    const QString path = QDir(tempDir_->path()).absoluteFilePath("cancelled.ratsdb");
    QFile::remove(path);

    rats::service::CancelToken cancel;
    cancel.cancel(); // already raised: the sweep must stop before writing anything

    rats::service::DatabaseExporter exporter(repo_);
    const auto result = exporter.run(path, rats::service::dump::Header {}, cancel, {});

    QVERIFY(!result.ok);
    // Cancellation is not a failure, so it reports no error — and it must not
    // leave a file that looks like a dump but has no footer.
    QVERIFY(result.error.isEmpty());
    QVERIFY(!QFile::exists(path));
}

void TestManticoreQueries::testInterruptedImportResumesWhereItStopped()
{
    const int kCount = 1500; // at least three frames
    QVector<Torrent> batch;
    for (int i = 0; i < kCount; ++i)
        batch.append(makeTorrent(700000 + i, QString("resume subject %1").arg(i)));
    QCOMPARE(repo_->addMany(batch), kCount);
    QVERIFY(waitForTorrent(batch.last().hash));

    const QString path = QDir(tempDir_->path()).absoluteFilePath("resume.ratsdb");
    rats::service::DatabaseExporter exporter(repo_);
    rats::service::CancelToken noCancel;
    const auto exported = exporter.run(path, rats::service::dump::Header {}, noCancel, {});
    QVERIFY(exported.ok);

    // Stop after the first committed frame and keep the offset it reported.
    rats::service::CancelToken cancel;
    qint64 resumeOffset = 0;
    qint64 firstPass = 0;
    rats::service::DatabaseImporter importer(indexing_);
    rats::service::DatabaseImporter::Options options;
    options.applyFilters = false;
    const auto partial
        = importer.run(path, options, cancel, [&](const rats::service::DatabaseImporter::Result& progress) {
              resumeOffset = progress.offset;
              firstPass = progress.processed;
              cancel.cancel();
          });

    QVERIFY(partial.cancelled);
    QVERIFY(!partial.ok);
    QVERIFY(firstPass > 0);
    QVERIFY(resumeOffset > 0);
    QVERIFY(resumeOffset < QFileInfo(path).size());

    // Resuming reads only what is left. The offset is recorded after the batch it
    // follows is committed, so a resume may repeat work but must never skip it —
    // the two passes together have to cover the whole dump.
    options.startOffset = resumeOffset;
    const auto rest = importer.run(path, options, noCancel, {});
    QVERIFY2(rest.ok, qPrintable(rest.error));
    QVERIFY(rest.processed > 0);
    QVERIFY(rest.processed < exported.torrents);
    QVERIFY(firstPass + rest.processed >= exported.torrents);
}

QTEST_MAIN(TestManticoreQueries)
#include "test_manticore_queries.moc"
