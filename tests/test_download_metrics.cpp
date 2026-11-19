#include "download_test_support.h"
#include "services/torrent_session_store.h"

using namespace download_test;

class TestDownloadMetrics : public QObject {
    Q_OBJECT
private slots:
    void newerNativeSelectionWinsAndMismatchedHistoryStaysPaused()
    {
        for(bool mismatch:{false,true}) {
            QTemporaryDir directory;rats::service::Download entry;entry.hash=QString(40,mismatch?'5':'6');entry.savePath=directory.path();
            entry.selectionConfirmed=true;entry.selectionRevision=3;rats::service::DownloadFile file;file.path="file";file.size=100;file.priority=0;file.selected=false;entry.files={file};
            const auto path=directory.filePath("session.json");rats::service::TorrentSessionStore store;QVERIFY(store.save(path,{entry}));
            FakeEngine engine;rats::service::DownloadService service(&engine);QSignalSpy prompts(&service,&rats::service::DownloadService::selectionRequired);
            QCOMPARE(service.loadSession(path),1);engine.snapshot.hasMetadata=true;engine.snapshot.phase=rats::DownloadPhase::Waiting;
            engine.snapshot.selectionRevision=mismatch?2:4;engine.snapshot.files={{mismatch?"different":"file",100,50,2}};
            poll(service);QCoreApplication::processEvents();const auto current=service.getDownload(entry.hash);
            QCOMPARE(prompts.size(),0);QVERIFY(!current.selectionRestorePending);
            if(mismatch) {QVERIFY(current.paused);QVERIFY(!current.selectionConfirmed);QVERIFY(!engine.calls.contains("content"));}
            else {QVERIFY(current.selectionConfirmed);QCOMPARE(current.selectionRevision,qint64(4));QCOMPARE(current.files[0].priority,2);}
        }
    }
    void progressIsCheckpointedBeforeRestart()
    {
        QTemporaryDir directory;FakeEngine engine;qint64 now=0;
        rats::service::DownloadService service(&engine,nullptr,[&]{return now;});
        const auto path=directory.filePath("session.json");QCOMPARE(service.loadSession(path),0);
        const QString hash(40,'7');QVERIFY(service.add(hash,directory.path()));
        engine.snapshot.hasMetadata=true;engine.snapshot.phase=rats::DownloadPhase::Waiting;engine.snapshot.files={{"file",100,50,1}};
        engine.snapshot.progress=0.5;engine.snapshot.verifiedBytes=50;engine.snapshot.wantedBytes=100;engine.snapshot.wantedVerifiedBytes=50;
        poll(service);service.setFilePriorities(hash,{1},0);QCoreApplication::processEvents();
        now=10001;poll(service);rats::service::TorrentSessionStore store;auto saved=store.load(path);
        QCOMPARE(saved.size(),1);QCOMPARE(saved[0].progress,0.5);QCOMPARE(saved[0].files[0].verifiedBytes,qint64(50));
        engine.snapshot.progress=1;engine.snapshot.verifiedBytes=100;engine.snapshot.files[0].verifiedBytes=100;
        engine.snapshot.wantedVerifiedBytes=100;engine.snapshot.fullTorrentComplete=true;engine.snapshot.selectedComplete=true;
        poll(service);saved=store.load(path);QCOMPARE(saved[0].progress,1.0);QVERIFY(saved[0].completed);
    }
    void restoredSettingsAndProgressSurviveInitialization()
    {
        FakeEngine engine;QTemporaryDir directory;rats::service::Download entry;
        entry.hash=QString(40,'f');entry.savePath=directory.path();entry.selectionConfirmed=true;entry.selectionRevision=7;
        entry.totalSize=200;entry.downloadedBytes=180;entry.verifiedBytes=100;entry.wantedBytes=100;entry.wantedVerifiedBytes=50;entry.progress=0.5;
        rats::service::DownloadFile first;first.path="first";first.size=100;first.priority=0;first.selected=false;first.verifiedBytes=50;first.progress=0.5;
        auto second=first;second.path="second";second.index=1;second.priority=2;second.selected=true;entry.files={first,second};
        entry.policy.downloadRate=2048;entry.policy.strategy="sequential";
        const auto path=directory.filePath("session.json");rats::service::TorrentSessionStore store;QVERIFY(store.save(path,{entry}));
        rats::service::DownloadService service(&engine);QSignalSpy prompts(&service,&rats::service::DownloadService::selectionRequired);
        QSignalSpy completion(&service,&rats::service::DownloadService::downloadCompleted);
        QCOMPARE(service.loadSession(path),1);poll(service);
        auto current=service.getDownload(entry.hash);QCOMPARE(current.progress,0.5);QCOMPARE(current.verifiedBytes,qint64(100));
        QCOMPARE(current.files[1].verifiedBytes,qint64(50));QVERIFY(current.selectionConfirmed);QVERIFY(current.progressPending);
        engine.snapshot.hasMetadata=true;engine.snapshot.files={{"first",100,0,1},{"second",100,0,1}};
        poll(service);QCOMPARE(service.getDownload(entry.hash).selectionRevision,qint64(7));QCOMPARE(prompts.size(),0);
        // Initial disk check has finished with a missing/stale native selection.
        engine.snapshot.phase=rats::DownloadPhase::Waiting;poll(service);QCoreApplication::processEvents();QCoreApplication::processEvents();
        current=service.getDownload(entry.hash);QVERIFY(current.selectionConfirmed);QCOMPARE(current.files[0].priority,0);QCOMPARE(current.files[1].priority,2);
        QCOMPARE(current.policy.downloadRate.value(),qint64(2048));QCOMPARE(current.policy.strategy,QString("sequential"));
        QCOMPARE(prompts.size(),0);QCOMPARE(completion.size(),0);QVERIFY(!current.selectionRestorePending);
    }

    void selectionPublishesProgressWithoutWaitingForPoll()
    {
        FakeEngine engine;QTemporaryDir directory;rats::service::DownloadService service(&engine);const QString hash(40,'9');
        QVERIFY(service.add(hash,directory.path()));engine.snapshot.hasMetadata=true;engine.snapshot.phase=rats::DownloadPhase::Waiting;
        engine.snapshot.files={{"first",100,50,1}};engine.snapshot.totalSize=100;engine.snapshot.progress=0.5;engine.snapshot.verifiedBytes=50;
        poll(service);QSignalSpy progress(&service,&rats::service::DownloadService::progressUpdated);QElapsedTimer elapsed;elapsed.start();
        const auto result=service.setFilePriorities(hash,{2},0);QCoreApplication::processEvents();
        QVERIFY(service.operationStatus(result.operationId).applied());QCOMPARE(progress.size(),1);QVERIFY(elapsed.elapsed()<500);
        QCOMPARE(progress.last()[1].toJsonObject()["progress"].toDouble(),0.5);
    }

    void selectionRequiresConfirmationAndSeparatesCompletion()
    {
        FakeEngine engine;QTemporaryDir directory;rats::service::DownloadService service(&engine);
        const QString hash(40,'e');QVERIFY(service.add(hash,directory.path()));
        engine.snapshot.hasMetadata=true;engine.snapshot.phase=rats::DownloadPhase::Waiting;
        engine.snapshot.files={{"first",100,0},{"second",100,0}};poll(service);
        QVERIFY(!engine.calls.contains("content"));
        QCOMPARE(service.setFilePriorities(hash,{1},0).error.code,QString("invalidSelection"));
        QCOMPARE(service.setFilePriorities(hash,{0,2},1).error.code,QString("revisionConflict"));
        auto result=service.setFilePriorities(hash,{0,2},0);QVERIFY(result.outcome==rats::OperationResult::Outcome::Accepted);
        QCOMPARE(service.setFilePriorities(hash,{1,1},0).error.code,QString("operationBusy"));
        QSignalSpy operations(&service,&rats::service::DownloadService::operationFinished);QCoreApplication::processEvents();
        QCOMPARE(operations.size(),1);QVERIFY(service.operationStatus(result.operationId).applied());
        const auto d=service.getDownload(hash);QVERIFY(d.selectionConfirmed);QCOMPARE(d.selectionRevision,qint64(1));QCOMPARE(d.files[1].priority,2);
        QSignalSpy full(&service,&rats::service::DownloadService::downloadCompleted),selected(&service,&rats::service::DownloadService::selectedCompleted);
        engine.snapshot.selectedComplete=true;engine.snapshot.phase=rats::DownloadPhase::PartialSeeding;poll(service);poll(service);
        QCOMPARE(selected.size(),1);QCOMPARE(full.size(),0);QVERIFY(!service.getDownload(hash).completed);
    }

    void metadataLessSnapshotsPreservePendingSelection()
    {
        FakeEngine engine;
        QTemporaryDir directory;
        rats::service::Download entry;
        entry.hash = QString(40, 'd');
        entry.savePath = directory.path();
        entry.totalSize = 100;
        entry.paused = true;
        rats::service::DownloadFile file;
        file.path = "payload.bin"; file.size = 100; file.selected = false;
        entry.files = {file};
        const auto session = directory.filePath("session.json");
        rats::service::TorrentSessionStore store;
        QVERIFY(store.save(session, {entry}));
        rats::service::DownloadService service(&engine);
        QCOMPARE(service.loadSession(session), 1);
        poll(service);
        QCOMPARE(service.getDownload(entry.hash).files.size(), 1);
        QVERIFY(!service.getDownload(entry.hash).files[0].selected);
        QCOMPARE(service.getDownload(entry.hash).totalSize, 100);
    }

    void networkBytesAreNotVerifiedBytes()
    {
        FakeEngine engine;
        qint64 now = 0;
        rats::service::DownloadService service(&engine, nullptr, [&] { return now; });
        QTemporaryDir directory;
        const QString hash(40, 'a');
        engine.onStart = [&] { QVERIFY(service.isDownloading(hash)); };
        QVERIFY(service.add(hash, directory.path()));
        QVERIFY(engine.creation.mode == rats::net::TorrentEngine::StartMode::Inactive);
        QCOMPARE(engine.calls, QStringList({"create", "metadata"}));
        engine.snapshot.phase = rats::DownloadPhase::Downloading;
        engine.snapshot.hasMetadata = true;
        engine.snapshot.totalSize = 100;
        engine.snapshot.downloaded = 200;
        engine.snapshot.verifiedBytes = 40;
          engine.snapshot.wantedBytes=100;engine.snapshot.wantedVerifiedBytes=40;
        engine.snapshot.progress = 0.4;
        engine.snapshot.files = {{"payload.bin", 100, 40}};
        QSignalSpy progress(&service, &rats::service::DownloadService::progressUpdated);
        poll(service);
        auto json = progress.last()[1].toJsonObject();
        QCOMPARE(json["downloaded"].toInteger(), 200); // compatibility
        QCOMPARE(json["verifiedBytes"].toInteger(), 40);
        QCOMPARE(service.getDownload(hash).files[0].progress, 0.4);
          QVERIFY(service.selectFiles(hash, {true}));
          QCoreApplication::processEvents();
        now = 1000;
        engine.snapshot.downloaded = 300;
        engine.snapshot.verifiedBytes = 60;
          engine.snapshot.wantedVerifiedBytes=60;
        engine.snapshot.files[0].verifiedBytes = 60;
        poll(service);
        const auto current = service.getDownload(hash);
        QCOMPARE(current.downloadSpeed, 100.0);
        QCOMPARE(current.verifiedSpeed, 20.0);
          QVERIFY(current.files[0].selected);
        json = progress.last()[1].toJsonObject();
        QCOMPARE(json["timeRemaining"].toInteger(), 2);

        now = 2000;
        engine.snapshot.phase = rats::DownloadPhase::Checking;
        engine.snapshot.verifiedBytes = 0;
        poll(service);
        QCOMPARE(service.getDownload(hash).verifiedSpeed, 0.0);
        QCOMPARE(service.getDownload(hash).verifiedBytes, 0);
        now = 1000; // an injected clock reset must never produce negative rates
        poll(service);
        QCOMPARE(service.getDownload(hash).downloadSpeed, 0.0);
    }

    void errorsAreObservableAndEmittedOnce()
    {
        FakeEngine engine;
        rats::service::DownloadService service(&engine);
        QTemporaryDir directory;
        const QString hash(40, 'b');
        QVERIFY(service.add(hash, directory.path()));
        QSignalSpy failures(&service, &rats::service::DownloadService::downloadFailed);
        engine.snapshot.phase = rats::DownloadPhase::Error;
        engine.snapshot.error = {"diskFull", "No space", "write", "payload.bin", ENOSPC, false};
        poll(service);
        poll(service);
        QCOMPARE(failures.count(), 1);
        QVERIFY(!service.getDownload(hash).completed);
        const auto error = rats::DownloadError::fromJson(failures[0][1].toJsonObject());
        QCOMPARE(error.code, QString("diskFull"));
        QVERIFY(!error.isRetryable());
    }

    void persistedCompletionDoesNotOverrideRecheck()
    {
        QTemporaryDir directory;
        rats::service::Download entry;
        entry.hash = QString(40, 'c');
        entry.savePath = directory.path();
        entry.completed = true;
        entry.progress = 1.0;
        entry.totalSize = entry.downloadedBytes = 100;
        entry.paused = true;
        const auto session = directory.filePath("session.json");
        rats::service::TorrentSessionStore store;
        QVERIFY(store.save(session, {entry}));
        FakeEngine engine;
        rats::service::DownloadService service(&engine);
        QSignalSpy completed(&service, &rats::service::DownloadService::downloadCompleted);
        QCOMPARE(service.loadSession(session), 1);
        QVERIFY(engine.creation.loadResume);
        QVERIFY(engine.creation.verifyExisting);
        QVERIFY(!engine.calls.contains("content"));
        QVERIFY(!service.getDownload(entry.hash).completed);
        QCOMPARE(service.getDownload(entry.hash).progress, 1.0);
        QVERIFY(service.getDownload(entry.hash).progressPending);
        poll(service);
        QCOMPARE(completed.count(), 0);
        QCOMPARE(service.getDownload(entry.hash).progress, 1.0);
    }
};

QTEST_GUILESS_MAIN(TestDownloadMetrics)
#include "test_download_metrics.moc"
