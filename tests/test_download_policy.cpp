#include <QtTest>
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>
#include "services/download_policy.h"
#include "services/download_scheduler.h"
#include "services/torrent_session_store.h"
#include "app/config_store.h"

using namespace rats;
using namespace rats::service;
class DownloadPolicyTest : public QObject {
    Q_OBJECT
private slots:
    void validationIsAtomic() {
        GlobalDownloadPolicy p;
        QString error;
        for (auto value : {QJsonValue(-1), QJsonValue(0.5), QJsonValue("100"),
                 QJsonValue(QJsonValue::Null), QJsonValue(9007199254740992.0), QJsonValue(true)}) {
            QVERIFY(!GlobalDownloadPolicy::fromJson({{"uploadLimitBps", 10}, {"downloadLimitBps", value}}, p, error));
            QCOMPARE(p.uploadRate, 0);
        }
        QVERIFY(!GlobalDownloadPolicy::fromJson({{"typo", 1}}, p, error));
        QVERIFY(GlobalDownloadPolicy::fromJson({{"downloadLimitBps", 9007199254740991LL}}, p, error));
        QCOMPARE(p.downloadRate, 9007199254740991LL);
    }
    void inheritance() {
        GlobalDownloadPolicy g; g.downloadRate = 1000;
        TaskDownloadPolicy t;
        QCOMPARE(resolveEffectivePolicy(g, t).downloadRate, 1000);
        t.downloadRate = 0;
        QCOMPARE(resolveEffectivePolicy(g, t).downloadRate, 1000);
        t.downloadRate = 2000;
        QCOMPARE(resolveEffectivePolicy(g, t).downloadRate, 1000);
        t.downloadRate = 100;
        QCOMPARE(resolveEffectivePolicy(g, t).downloadRate, 100);
        QString error;
        QVERIFY(TaskDownloadPolicy::fromJson({{"downloadLimitBps", QJsonValue::Null}}, t, error));
        QVERIFY(!t.downloadRate);
        g.downloadRate = 0;
        QCOMPARE(resolveEffectivePolicy(g, t).downloadRate, 0);
    }
    void configRejectsAndRoundTrips() {
        QTemporaryDir dir;
        app::ConfigStore c(dir.filePath("config.json"));
        QVERIFY(!c.setValue("downloadPolicy.downloadLimitBps", -1));
        QVERIFY(c.setValue("downloadPolicy.downloadLimitBps", 4096));
        QCOMPARE(c.downloadPolicy().downloadRate, 4096);
        QVERIFY(c.save());
        app::ConfigStore loaded(dir.filePath("config.json"));
        QVERIFY(loaded.load());
        QCOMPARE(loaded.downloadPolicy().downloadRate, 4096);
        const auto before = c.toJson();
        QVERIFY(c.fromJson({{"downloadPath", "changed"}, {"downloadPolicy.downloadLimitBps", -1},
            {"downloadPolicy.uploadLimitBps", 8192}}).isEmpty());
        QCOMPARE(c.toJson(), before);
    }
    void sessionMigrationAndDiagnostics() {
        QTemporaryDir dir;
        const auto path = dir.filePath("session.json");
        TorrentSessionStore store;
        QCOMPARE(store.loadDocument(path).status, SessionLoadResult::Missing);
        Download d; d.hash = QString(40, 'a'); d.savePath = dir.path(); d.paused = true;
        d.policy.downloadRate = 1234; d.queuePosition = 7;
        QVERIFY(store.saveDocument(path, {2, 12, {d}}));
        auto result = store.loadDocument(path);
        QCOMPARE(result.status, SessionLoadResult::Loaded);
        QCOMPARE(result.document.revision, 12);
        QCOMPARE(result.document.downloads[0].queuePosition, 7);
        QVERIFY(result.document.downloads[0].paused);
        QCOMPARE(*result.document.downloads[0].policy.downloadRate, 1234);
        QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QJsonDocument(QJsonArray{QJsonObject{{"hash", d.hash}, {"savePath", dir.path()}, {"paused", true}}}).toJson());
        file.close();
        result = store.loadDocument(path);
        QCOMPARE(result.status, SessionLoadResult::Migrated);
        QVERIFY(result.document.downloads[0].paused);
        QVERIFY(file.open(QIODevice::WriteOnly)); file.write("{broken"); file.close();
        result = store.loadDocument(path);
        QCOMPARE(result.status, SessionLoadResult::Invalid);
        QVERIFY(!result.error.isEmpty());
    }
    void failedSavePreservesPreviousDocument() {
        QTemporaryDir dir;
        const auto path = dir.filePath("session.json");
        TorrentSessionStore store;
        QVERIFY(store.saveDocument(path, {}));
        QFile file(path); QVERIFY(file.open(QIODevice::ReadOnly)); const auto before = file.readAll(); file.close();
        SessionDocument invalid; invalid.schemaVersion = 999;
        QString error;
        QVERIFY(!store.saveDocument(path, invalid, &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), before);
    }
    void corruptSessionCannotBeOverwrittenOnShutdown() {
        QTemporaryDir dir;
        const auto path = dir.filePath("session.json");
        QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly));
        const QByteArray original("{broken-but-recoverable");
        file.write(original); file.close();
        DownloadService service(nullptr);
        QSignalSpy failure(&service, &DownloadService::downloadFailed);
        QCOMPARE(service.loadSession(path), 0);
        QCOMPARE(failure.size(), 1);
        QVERIFY(!service.saveSession(path));
        QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), original);
    }
    void sessionRejectsDuplicatesAndFutureSchemas() {
        auto entry = QJsonObject{{"hash", QString(40, 'b')}, {"savePath", "C:/downloads"},
            {"paused", false}};
        auto doc = TorrentSessionStore::migrateLegacyArray({entry, entry});
        QVERIFY(!TorrentSessionStore::validateDocument(doc).isEmpty());
        doc = TorrentSessionStore::migrateLegacyArray({entry});
        QVERIFY(TorrentSessionStore::validateDocument(doc).isEmpty());
        doc["schemaVersion"] = 999;
        QVERIFY(!TorrentSessionStore::validateDocument(doc).isEmpty());
    }
    void schedulerSlotsAndIntent() {
        DownloadScheduler scheduler;
        GlobalDownloadPolicy policy;
        QVector<ScheduleTask> tasks;
        for (int i = 0; i < 10; ++i) {
            ScheduleTask t; t.hash = QString::number(i); t.hasMetadata = true; t.position = i;
            tasks.append(t);
        }
        auto d = scheduler.reconcile(tasks, policy, 0);
        QCOMPARE(d.content.size(), 3);
        QVERIFY(d.content.contains("0"));
        tasks[0].userPaused = true;
        d = scheduler.reconcile(tasks, policy, 1000);
        QCOMPARE(d.content.size(), 3);
        QVERIFY(!d.content.contains("0")); QVERIFY(d.content.contains("3"));
        policy.downloadSlots = 1;
        QCOMPARE(scheduler.reconcile(tasks, policy, 2000).content.size(), 1);
    }
    void schedulerAgingAndCooldown() {
        DownloadScheduler scheduler;
        GlobalDownloadPolicy policy; policy.downloadSlots = 1;
        ScheduleTask low; low.hash = "low"; low.hasMetadata = true; low.priority = -1; low.position = 1;
        ScheduleTask high; high.hash = "high"; high.hasMetadata = true; high.priority = 1;
        QVERIFY(scheduler.reconcile({low, high}, policy, 0).content.contains("high"));
        QVERIFY(scheduler.reconcile({low, high}, policy, 240000).content.contains("low"));
        DownloadScheduler stalled; high.phase = DownloadPhase::Downloading;
        QVERIFY(stalled.reconcile({low, high}, policy, 0).content.contains("high"));
        QVERIFY(stalled.reconcile({low, high}, policy, 120000).content.contains("low"));
        QVERIFY(!stalled.reconcile({low, high}, policy, 121000).content.contains("high"));
    }
    void schedulerSeparatesWorkClasses() {
        DownloadScheduler scheduler;
        GlobalDownloadPolicy p;
        QVector<ScheduleTask> tasks;
        for (int i = 0; i < 20; ++i) {
            ScheduleTask t; t.hash = QString::number(i); t.position = i;
            if (i < 5) { t.hasMetadata = true; t.phase = DownloadPhase::Checking; }
            else if (i < 10) { t.hasMetadata = false; t.phase = DownloadPhase::Metadata; }
            else if (i < 15) { t.hasMetadata = true; t.completed = true; }
            else { t.hasMetadata = true; }
            tasks.append(t);
        }
        auto d = scheduler.reconcile(tasks, p, 0);
        QCOMPARE(d.checking.size(), 1);
        QCOMPARE(d.metadata.size(), 2);
        QCOMPARE(d.content.size(), 5); // 3 downloads plus 2 seeds
        for (auto& t : tasks) t.userPaused = true;
        d = scheduler.reconcile(tasks, p, 1);
        QVERIFY(d.checking.isEmpty()); QVERIFY(d.metadata.isEmpty()); QVERIFY(d.content.isEmpty());
    }
};
QTEST_GUILESS_MAIN(DownloadPolicyTest)
#include "test_download_policy.moc"
