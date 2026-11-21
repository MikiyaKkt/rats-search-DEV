#include "data/database.h"
#include "data/manticore.h"
#include "data/torrent_repository.h"
#include "services/indexing_service.h"
#include "services/filter_policy.h"
#include "services/crawler_pipeline.h"
#include "services/database_dump.h"
#include "services/database_importer.h"
#include "common/performance_metrics.h"
#include <QtTest>
#include <QTemporaryDir>

using namespace rats;
using service::IndexingService;

// Faults affect this temporary test connection only. Can model either a rejected
// statement or a committed statement whose acknowledgement was lost.
class FaultDatabase : public data::Database {
public:
    using Database::Database;
    int summaryStatements = 0, failSummary = 0;
    int fileStatements = 0, failFiles = 0;
    bool loseAck = false, failVerification = false, failReads = false, failAllVerification = false;
    bool failVerificationOnce = false;
    Rows query(const QString& sql, const QVariantList& params = {}, bool* ok = nullptr) override {
        if (failVerificationOnce && failSummary>0 && summaryStatements>=failSummary) {
            failVerificationOnce=false; if(ok)*ok=false; return {};
        }
        if (failReads || (summaryStatements >= failSummary && failSummary > 0
            && (failAllVerification || (failVerification && !sql.contains("COUNT"))))) {
            if (ok) *ok = false;
            return {};
        }
        return Database::query(sql, params, ok);
    }
protected:
    bool runWrite(const QString& sql, const char* op, const QString& table) override {
        bool fault = false;
        if (QByteArray(op) == "REPLACE") {
            if (table == "torrents") fault = ++summaryStatements == failSummary;
            if (table == "files") fault = ++fileStatements == failFiles;
        }
        if (fault) {
            if (loseAck) Database::runWrite(sql, op, table);
            return false;
        }
        return Database::runWrite(sql, op, table);
    }
};

class CrawlerBatchIndexingTest : public QObject {
    Q_OBJECT
    QTemporaryDir dir_;
    data::Manticore* manticore_ = nullptr;
    int serial_ = 150000;
    domain::Torrent torrent(int nameSize = 20) {
        domain::Torrent t;
        t.hash = QString("%1").arg(++serial_, 8, 16, QChar('0')) + QString(32, 'a');
        t.name = QString(nameSize, QChar('x'));
        t.size = 120; t.files = 2;
        t.fileList = {{"one.mkv", 70}, {"two.mkv", 50}};
        t.contentType = domain::ContentType::Video;
        return t;
    }
private slots:
    void pipelineRetriesPartialCommitAndRecoversLostAcknowledgement() {
        for(bool lostAck : {false,true}) {
            FaultDatabase db(manticore_); data::TorrentRepository repo(&db);
            repo.primeFromDatabase(); const auto before=repo.statistics();
            service::CrawlerPipeline pipeline(&repo,manticore_);
            QSet<QString> notified; int notifications=0;
            connect(&pipeline,&service::CrawlerPipeline::torrentIndexed,this,[&](const auto& t){++notifications;notified.insert(t.hash);});
            db.failSummary=lostAck?1:2; db.loseAck=lostAck; db.failVerificationOnce=lostAck;
            pipeline.start();
            QVector<domain::Torrent> input;
            for(int i=0;i<12;++i){input.append(torrent(90000));QCOMPARE(pipeline.insert(input.last()),service::CrawlerPipeline::Admission::Accepted);}
            QTRY_COMPARE_WITH_TIMEOUT(pipeline.pending(),0,15000);
            QCOMPARE(notifications,12); QCOMPARE(notified.size(),12);
            QCOMPARE(repo.statistics().torrents,before.torrents+12);
            for(const auto& t:input) { QVERIFY(repo.exists(t.hash)); QCOMPARE(repo.filesOf(t.hash).size(),2); }
            pipeline.stop(); const auto saved=repo.statistics(); repo.primeFromDatabase();
            QCOMPARE(repo.statistics().torrents,saved.torrents); QCOMPARE(repo.statistics().files,saved.files);
        }
    }

    void initTestCase() {
        QVERIFY(dir_.isValid());
        manticore_ = new data::Manticore(dir_.path(), this);
        QVERIFY(manticore_->start());
        QVERIFY(manticore_->waitForReady());
    }
    void cleanupTestCase() { manticore_->stop(); }

    void reportsEveryOutcomeAndRepairsDetails() {
        FaultDatabase db(manticore_); data::TorrentRepository repo(&db);
        service::FilterSettings settings; settings.maxFiles = 2;
        service::FilterPolicy policy(settings); IndexingService indexing(&repo, &policy);
        repo.primeFromDatabase();
        auto stored = torrent(); stored.fileList.clear();
        QVERIFY(repo.add(stored));
        auto repair = stored; repair.fileList = {{"one.mkv",70},{"two.mkv",50}}; repair.good = 3;
        auto fresh = torrent(); auto duplicate = fresh; duplicate.hash = duplicate.hash.toUpper();
        auto rejected = torrent(); rejected.files = 3;
        auto collision = fresh; collision.hash = fresh.hash.left(16) + QString(24, 'b');
        const auto result = indexing.insertBatch({repair, fresh, duplicate, rejected, collision, {}});
        QCOMPARE(result.items.size(), 6);
        QCOMPARE(result.inserted, 1); QCOMPARE(result.merged, 1);
        QCOMPARE(result.duplicates, 1); QCOMPARE(result.rejected, 1);
        QCOMPARE(result.collided, 1); QCOMPARE(result.invalid, 1); QCOMPARE(result.failed, 0);
        QCOMPARE(repo.get(stored.hash, true)->fileList.size(), 2);
        QCOMPARE(repo.get(stored.hash)->good, 3);
        QCOMPARE(repo.checkMany({stored.hash, fresh.hash}).value(stored.hash), domain::HashState::Known);
    }

    void lookupDistinguishesIncompleteCollisionAndFailure() {
        FaultDatabase db(manticore_); data::TorrentRepository repo(&db);
        auto complete = torrent(), incomplete = torrent(), missing = torrent();
        incomplete.fileList.clear();
        QVERIFY(repo.add(complete)); QVERIFY(repo.add(incomplete));
        const auto collision = complete.hash.left(16) + QString(24, 'b');
        const QStringList hashes{complete.hash, incomplete.hash, missing.hash, collision};
        const auto result = repo.checkMany(hashes);
        QCOMPARE(result.value(complete.hash), domain::HashState::Known);
        QCOMPARE(result.value(incomplete.hash), domain::HashState::Incomplete);
        QCOMPARE(result.value(missing.hash), domain::HashState::Missing);
        QCOMPARE(result.value(collision), domain::HashState::Collision);
        db.failReads = true;
        for (auto state : repo.checkMany(hashes)) QCOMPARE(state, domain::HashState::Failed);
    }

    void partialSummaryFailureReplaysWithoutDoubleCounting() {
        FaultDatabase db(manticore_); data::TorrentRepository repo(&db);
        repo.primeFromDatabase(); const auto before = repo.statistics();
        IndexingService indexing(&repo, nullptr);
        QVector<domain::Torrent> input;
        for (int i = 0; i < 12; ++i) input.append(torrent(90000));
        db.failSummary = 2;
        const auto first = indexing.insertBatch(input);
        QVERIFY(db.summaryStatements >= 3);
        QVERIFY(first.inserted > 0); QVERIFY(first.failed > 0);
        QCOMPARE(first.inserted + first.failed, 12);
        QCOMPARE(repo.statistics().torrents, before.torrents + first.inserted);
        for (const auto& item : first.items) if (item.outcome == IndexingService::Outcome::Failed) QCOMPARE(item.stage, "summary");
        db.failSummary = 0;
        const auto retry = indexing.insertBatch(input);
        QCOMPARE(retry.failed, 0); QCOMPARE(retry.inserted, first.failed);
        QCOMPARE(repo.statistics().torrents, before.torrents + 12);
        QCOMPARE(indexing.insertBatch(input).inserted, 0);
        const auto saved = repo.statistics(); repo.primeFromDatabase();
        QCOMPARE(repo.statistics().torrents, saved.torrents);
        QCOMPARE(repo.statistics().files, saved.files);
    }

    void lostAcknowledgementIsVerifiedAndNotInsertedTwice() {
        FaultDatabase db(manticore_); data::TorrentRepository repo(&db);
        repo.primeFromDatabase(); const auto before = repo.statistics();
        IndexingService indexing(&repo, nullptr);
        QVector<domain::Torrent> input; for (int i = 0; i < 12; ++i) input.append(torrent(90000));
        db.failSummary = 2; db.loseAck = true;
        const auto result = indexing.insertBatch(input);
        QCOMPARE(result.failed, 0); QCOMPARE(result.inserted, 12);
        QCOMPARE(repo.statistics().torrents, before.torrents + 12);
        QCOMPARE(indexing.insertBatch(input).inserted, 0);
        QCOMPARE(repo.statistics().torrents, before.torrents + 12);
    }

    void failedVerificationReconcilesAndRecoversNewNotificationReceipt() {
        FaultDatabase db(manticore_); data::TorrentRepository repo(&db);
        repo.primeFromDatabase(); const auto before = repo.statistics();
        IndexingService indexing(&repo, nullptr);
        const auto input = torrent();
        db.failSummary = 1; db.loseAck = true; db.failVerification = true;
        const auto first = indexing.insertBatch({input});
        QCOMPARE(first.failed, 1); QCOMPARE(first.inserted, 0);
        QCOMPARE(repo.statistics().torrents, before.torrents + 1);
        db.failSummary = 0; db.failVerification = false;
        const auto retry = indexing.insertBatch({input});
        QCOMPARE(retry.inserted, 1); // newly confirmed receipt, already counted by recount
        QCOMPARE(repo.statistics().torrents, before.torrents + 1);
        QCOMPARE(indexing.insertBatch({input}).inserted, 0);
    }

    void filesFailureDoesNotPublishSummaryAndOversizeIsExplicit() {
        FaultDatabase db(manticore_); data::TorrentRepository repo(&db);
        IndexingService indexing(&repo, nullptr);
        auto one = torrent(); db.failFiles = 1;
        const auto failed = indexing.insertBatch({one});
        QCOMPARE(failed.failed, 1); QCOMPARE(failed.items.first().stage, "files");
        QVERIFY(!repo.exists(one.hash));
        db.failFiles = 0;
        QCOMPARE(indexing.insertBatch({one}).inserted, 1);
        auto acknowledgedLate = torrent();
        db.failFiles = db.fileStatements + 1; db.loseAck = true;
        QCOMPARE(indexing.insertBatch({acknowledgedLate}).inserted, 1);
        QCOMPARE(repo.filesOf(acknowledgedLate.hash).size(), 2);
        auto large = torrent(); large.fileList[0].path = QString(data::Database::MaxStatementBytes, QChar(0x4e2d));
        const auto rejected = indexing.insertBatch({large});
        QCOMPARE(rejected.failed, 1); QCOMPARE(rejected.items.first().stage, "files");
        QVERIFY(rejected.items.first().error.contains("limit")); QVERIFY(!repo.exists(large.hash));
          QVERIFY(!rejected.items.first().retryable);
    }

    void unavailableVerificationMarksStatisticsInexactUntilRecovery() {
        FaultDatabase db(manticore_); data::TorrentRepository repo(&db);
        repo.primeFromDatabase(); const auto before = repo.statistics();
        IndexingService indexing(&repo, nullptr); const auto input = torrent();
        db.failSummary = 1; db.loseAck = true; db.failAllVerification = true;
        QCOMPARE(indexing.insertBatch({input}).failed, 1);
        QVERIFY(!repo.statistics().exact);
        const auto blocked = indexing.insertBatch({torrent()});
        QCOMPARE(blocked.failed, 1); // no new write while reconciliation is unavailable
        db.failAllVerification = false; db.failSummary = 0;
        QCOMPARE(indexing.insertBatch({input}).inserted, 1);
        QVERIFY(repo.statistics().exact);
        QCOMPARE(repo.statistics().torrents, before.torrents + 1);
        QCOMPARE(indexing.insertBatch({input}).inserted, 0);
    }

    void failedImportRetainsFrameOffsetAndCanResume() {
        FaultDatabase db(manticore_); data::TorrentRepository repo(&db);
        repo.primeFromDatabase(); const auto before = repo.statistics();
        IndexingService indexing(&repo, nullptr);
        const auto path = dir_.filePath("partial.ratsdb");
        service::DumpWriter writer;
        QVERIFY(writer.open(path, {}));
        for (int i = 0; i < 12; ++i) QVERIFY(writer.write(torrent(90000)));
        QVERIFY(writer.finish());
        db.failSummary = 2;
        service::DatabaseImporter importer(&indexing);
        const auto first = importer.run(path, {}, service::CancelToken{}, {});
        QVERIFY(!first.ok); QVERIFY(!first.error.isEmpty());
        QVERIFY(first.inserted > 0);
        db.failSummary = 0;
        service::DatabaseImporter::Options options; options.startOffset = first.offset;
        const auto resumed = importer.run(path, options, service::CancelToken{}, {});
        QVERIFY(resumed.ok);
        QCOMPARE(first.inserted + resumed.inserted, 12);
        QCOMPARE(repo.statistics().torrents, before.torrents + 12);
    }
};
QTEST_GUILESS_MAIN(CrawlerBatchIndexingTest)
#include "test_crawler_batch_indexing.moc"
