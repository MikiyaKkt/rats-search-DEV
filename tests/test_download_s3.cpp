#include "download_test_support.h"
#include "services/torrent_session_store.h"
#include "app/config_store.h"
#include <QTimeZone>
#pragma push_macro("emit")
#pragma push_macro("slots")
#pragma push_macro("signals")
#undef emit
#undef slots
#undef signals
#include "librats/bittorrent/disk_cache_budget.h"
#include "librats/bittorrent/download_strategy.h"
#pragma pop_macro("signals")
#pragma pop_macro("slots")
#pragma pop_macro("emit")
using namespace download_test;
using namespace rats::service;

class TestDownloadS3:public QObject {
    Q_OBJECT
private slots:
    void advancedPolicyValidationAndMigration() {
        GlobalDownloadPolicy p;QString error;
        QVERIFY(GlobalDownloadPolicy::fromJson({{"schemaVersion",1},{"downloadLimitBps",123}},p,error));QCOMPARE(p.schemaVersion,2);QCOMPARE(p.cacheBytes,0);
        const auto before=p.toJson();QVERIFY(!GlobalDownloadPolicy::fromJson({{"cacheBytes",268435457}},p,error));QCOMPARE(p.toJson(),before);
        QVERIFY(!GlobalDownloadPolicy::fromJson({{"schemaVersion",3}},p,error));
        TaskDownloadPolicy t;t.strategy="firstLast";t.windowMax=64;t.seeding=SeedingPolicy{1500,3600,true};
        TaskDownloadPolicy decoded;QVERIFY(TaskDownloadPolicy::fromJson(t.toJson(),decoded,error));QCOMPARE(decoded.toJson(),t.toJson());
        QVERIFY(!TaskDownloadPolicy::fromJson({{"strategy","typo"}},decoded,error));QCOMPARE(decoded.toJson(),t.toJson());
        QVERIFY(!TaskDownloadPolicy::fromJson({{"windowMin",65}},decoded,error));
        BandwidthSchedule schedule;schedule.enabled=true;schedule.timeZone="Asia/Tokyo";schedule.rules={{1,1380,60,1000,2000,false}};
        p.schedule=schedule;p.cacheBytes=1048576;p.seeding={1000,60,false};GlobalDownloadPolicy copy;QVERIFY(GlobalDownloadPolicy::fromJson(p.toJson(),copy,error));QCOMPARE(copy.toJson(),p.toJson());
        QTemporaryDir dir;rats::service::Download d;d.hash=QString(40,'a');d.savePath=dir.path();d.policy=t;d.seededMilliseconds=12345;d.seedingUploadedBytes=99;d.seedingStopped=true;
        TorrentSessionStore store;QVERIFY(store.saveDocument(dir.filePath("session"),{2,4,{d}}));auto loaded=store.loadDocument(dir.filePath("session"));QVERIFY(loaded.ok());
        QCOMPARE(loaded.document.schemaVersion,2);QCOMPARE(loaded.document.downloads[0].policy.toJson(),t.toJson());QCOMPARE(loaded.document.downloads[0].seededMilliseconds,12345);QVERIFY(loaded.document.downloads[0].seedingStopped);
        QVERIFY(store.saveDocument(dir.filePath("old"),{1,4,{d}}));QCOMPARE(store.loadDocument(dir.filePath("old")).status,SessionLoadResult::Migrated);
    }
    void unknownPolicyCannotOverwriteConfiguration() {
        QTemporaryDir dir;const auto path=dir.filePath("rats.json");QFile file(path);QVERIFY(file.open(QIODevice::WriteOnly));
        const QByteArray original="{\"downloadPolicy\":{\"schemaVersion\":99}}";file.write(original);file.close();
        rats::app::ConfigStore config(path);QVERIFY(!config.load());config.setValue("httpPort",9000);QVERIFY(!config.save());
        QVERIFY(file.open(QIODevice::ReadOnly));QCOMPARE(file.readAll(),original);file.close();
        QVERIFY(file.open(QIODevice::WriteOnly));file.write("{}");file.close();QVERIFY(config.load());QVERIFY(config.save());
    }
    void weeklyScheduleOvernightAndDst() {
        BandwidthSchedule p;p.enabled=true;p.timeZone="Asia/Tokyo";p.rules={{1,1380,60,1000,2000,true}};
        QVERIFY(p.validate().isEmpty());
        QVERIFY(p.resolve(QDateTime::fromString("2024-01-01T15:30:00Z",Qt::ISODate)).pause); // Tue 00:30, Monday's overnight rule
        QVERIFY(!p.resolve(QDateTime::fromString("2024-01-02T15:30:00Z",Qt::ISODate)).pause);
        p.rules.append({2,0,120,0,0,false});QVERIFY(!p.validate().isEmpty());p.rules.removeLast();
        p.timeZone="America/New_York";p.rules={{64,150,210,1000,0,true}};QVERIFY(p.validate().isEmpty());
        auto before=QDateTime::fromString("2024-03-10T06:59:00Z",Qt::ISODate);QVERIFY(!p.resolve(before).pause);
        QCOMPARE(p.nextChange(before).toUTC(),QDateTime::fromString("2024-03-10T07:00:00Z",Qt::ISODate));
        p.rules={{64,90,120,1000,0,true}};
        QVERIFY(p.resolve(QDateTime::fromString("2024-11-03T05:45:00Z",Qt::ISODate)).pause);
        QVERIFY(p.resolve(QDateTime::fromString("2024-11-03T06:45:00Z",Qt::ISODate)).pause);
        QVERIFY(!p.resolve(QDateTime::fromString("2024-11-03T06:15:00Z",Qt::ISODate)).pause);
        p.timeZone="not-a-zone";QVERIFY(!p.validate().isEmpty());
    }
    void seedingThresholdsAreExact() {
        SeedingPolicy p{1500,60,false};QVERIFY(!p.shouldStop(149,100,59));QVERIFY(p.shouldStop(150,100,0));QVERIFY(p.shouldStop(0,100,60));
        p.all=true;QVERIFY(!p.shouldStop(150,100,59));QVERIFY(p.shouldStop(150,100,60));
        p={1001,0,false};QVERIFY(!p.shouldStop(1000,1000,999));QVERIFY(p.shouldStop(1001,1000,0));
        p={1000000,0,false};QVERIFY(!p.shouldStop(9007199254740991LL,9007199254740991LL,0));
        p={0,0,true};QVERIFY(!p.shouldStop(1000,100,10000));p={1000,0,false};QVERIFY(!p.shouldStop(1000,0,0));
    }
    void windowsAreBounded() {
        bt::RequestWindowPolicy p;QCOMPARE(p.computeWindow(0,0),32);QCOMPARE(p.computeWindow(1,100),2);
        QCOMPARE(p.computeWindow(1e12,1e12),32);QCOMPARE(p.computeWindow(1e6,100,5*1024*1024),0);
        p.automatic=false;p.manual=7;QCOMPARE(p.computeWindow(1,1),7);p.minimum=40;QVERIFY(!p.valid());
    }
    void sharedCacheEvictionAndGeneration() {
        bt::DiskCacheBudget cache(12);librats::Bytes out;std::uint64_t generation=0;
        QVERIFY(!cache.acquire("a",out,generation));cache.release("a",librats::Bytes(6,'a'),generation);cache.release("b",librats::Bytes(6,'b'),generation);QCOMPARE(cache.used(),size_t(12));
        QVERIFY(cache.acquire("a",out,generation));cache.release("c",librats::Bytes(6,'c'),generation);QVERIFY(!cache.acquire("b",out,generation));QVERIFY(cache.acquire("a",out,generation));
        cache.invalidate();cache.release("late",librats::Bytes(6,'x'),generation);QCOMPARE(cache.used(),size_t(0));
        cache.acquire("missing",out,generation);cache.release("big",librats::Bytes(20,'x'),generation);QCOMPARE(cache.used(),size_t(0));
        cache.release("valid",librats::Bytes(6,'a'),generation);cache.set_limit(2);QCOMPARE(cache.used(),size_t(0));cache.set_limit(0);QVERIFY(!cache.enabled());
    }
    void diskCacheRecheckInvalidates() {
        QTemporaryDir dir;auto info=metadata(librats::Bytes(16384,'a'));QFile file(dir.filePath("payload.bin"));QVERIFY(file.open(QIODevice::WriteOnly));file.write(QByteArray(16384,'a'));file.close();
        auto cache=std::make_shared<bt::DiskCacheBudget>(32768);bt::ThreadedDiskIo disk(info,dir.path().toStdString(),{},{2,cache});
        auto read=[&]{std::promise<librats::Bytes> done;auto f=done.get_future();disk.async_read(0,0,16384,[&](bool ok,librats::Bytes bytes){done.set_value(ok?std::move(bytes):librats::Bytes{});});return f.get();};
        auto bytes=read();QCOMPARE(bytes.size(),size_t(16384));QCOMPARE(cache->used(),size_t(16384));
        QVERIFY(file.open(QIODevice::WriteOnly));file.write(QByteArray(16384,'b'));file.close();
        std::promise<bt::Bitfield> done;auto f=done.get_future();disk.async_check_files({},nullptr,[&](auto bits){done.set_value(bits);});QCOMPARE(f.get().count(),0u);QCOMPARE(cache->used(),size_t(0));
        bytes=read();QCOMPARE(bytes[0],std::uint8_t('b'));
    }
    void scheduleAdmissionAndSeedingGoals() {
        class Engine:public FakeEngine {public:qint64 down=0;bool setGlobalPolicy(qint64 d,qint64,int,int,int)override{down=d;return true;}} engine;
        qint64 now=0;auto utc=QDateTime::fromString("2024-01-01T00:00:00Z",Qt::ISODate);
        rats::service::DownloadService service(&engine,nullptr,[&]{return now;},[&]{return utc;});QTemporaryDir dir;const QString hash(40,'b');QVERIFY(service.add(hash,dir.path()));
        engine.snapshot.hasMetadata=true;engine.snapshot.phase=rats::DownloadPhase::Waiting;engine.snapshot.files={{"payload.bin",100,0}};engine.snapshot.wantedBytes=100;poll(service);
        auto result=service.setFilePriorities(hash,{1},0);QCoreApplication::processEvents();QVERIFY(service.operationStatus(result.operationId).applied());
        auto policy=service.globalPolicy();policy.downloadRate=100;policy.schedule.enabled=true;policy.schedule.rules={{127,0,1,40,0,true}};QVERIFY(service.setGlobalPolicy(policy,policy.revision).applied());
        QCOMPARE(engine.down,40);QCOMPARE(service.getDownload(hash).queueReason,QString("timeSchedule"));QVERIFY(!service.getDownload(hash).paused);
        utc=utc.addSecs(60);poll(service);QCOMPARE(engine.down,100);QVERIFY(service.getDownload(hash).contentStarted);
        auto task=service.getDownload(hash).policy;task.seeding=SeedingPolicy{0,2,false};QVERIFY(service.setTaskPolicy(hash,task,task.revision).applied());
        engine.snapshot.selectedComplete=true;engine.snapshot.phase=rats::DownloadPhase::PartialSeeding;
        now=1000;poll(service);now=2000;poll(service);QVERIFY(service.getDownload(hash).seedingStopped);QVERIFY(!service.getDownload(hash).paused);
        QVERIFY(service.resume(hash));QVERIFY(!service.getDownload(hash).seedingStopped);QCOMPARE(service.getDownload(hash).seededMilliseconds,0);
        now=3000;poll(service);QVERIFY(!service.getDownload(hash).seedingStopped);
    }
    void uploadedGoalSurvivesTransportCounterReset() {
        QTemporaryDir dir;Download d;d.hash=QString(40,'c');d.savePath=dir.path();d.selectionConfirmed=true;d.selectionRevision=1;
        d.files={{"payload.bin",100,0,true,1.0,100,1}};d.seedingUploadedBytes=70;d.policy.seeding=SeedingPolicy{1000,0,false};
        TorrentSessionStore store;QVERIFY(store.saveDocument(dir.filePath("session"),{2,1,{d}}));
        FakeEngine engine;engine.snapshot.hasMetadata=true;engine.snapshot.files={{"payload.bin",100,100,1}};engine.snapshot.selectionRevision=1;engine.snapshot.wantedBytes=100;engine.snapshot.selectedComplete=true;
        qint64 now=0;rats::service::DownloadService service(&engine,nullptr,[&]{return now;});QCOMPARE(service.loadSession(dir.filePath("session")),1);
        engine.snapshot.phase=rats::DownloadPhase::PartialSeeding;engine.snapshot.queuedUploadBytes=0;poll(service);
        QCOMPARE(service.getDownload(d.hash).seedingUploadedBytes,70);QVERIFY(!service.getDownload(d.hash).seedingStopped);
        now=1000;engine.snapshot.queuedUploadBytes=40;poll(service);QCOMPARE(service.getDownload(d.hash).seedingUploadedBytes,110);QVERIFY(service.getDownload(d.hash).seedingStopped);
        const auto stopped=service.getDownload(d.hash);QVERIFY(store.saveDocument(dir.filePath("stopped"),{2,2,{stopped}}));
        FakeEngine restarted;rats::service::DownloadService restored(&restarted);QCOMPARE(restored.loadSession(dir.filePath("stopped")),1);
        QVERIFY(restarted.calls.contains("metadata"));QVERIFY(!restarted.calls.contains("content"));QVERIFY(restored.getDownload(d.hash).seedingStopped);
    }
    void realSequentialTransferAndPieceMap_data() {
        QTest::addColumn<bool>("utp");QTest::newRow("tcp")<<false;QTest::newRow("utp-mse")<<true;
    }
    void realSequentialTransferAndPieceMap() {
        QFETCH(bool,utp);
        QTemporaryDir dir;const auto seed=dir.filePath("seed"),target=dir.filePath("target");QVERIFY(QDir().mkpath(seed));
        const librats::Bytes payload(128*1024,'s');auto info=metadata(payload);QFile file(seed+"/payload.bin");QVERIFY(file.open(QIODevice::WriteOnly));file.write(QByteArray(payload.size(),'s'));file.close();
        auto config=localConfig();if(utp){config.enable_incoming_utp=true;config.enable_outgoing_utp=true;config.enable_outgoing_tcp=false;config.in_enc_policy=bt::EncPolicy::Forced;config.out_enc_policy=bt::EncPolicy::Forced;}
        bt::Client source(config),client(config);source.open();client.open();client.set_cache_budget(32768);source.set_cache_budget(32768);
        auto* original=source.add_torrent(info,seed.toStdString());bt::CreateOptions options;options.mode=bt::StartMode::MetadataOnly;QVERIFY(client.create_torrent(info,target.toStdString(),options).applied);
        bt::RequestWindowPolicy window;window.automatic=false;window.manual=2;
        QVERIFY(pump(source,client,[&]{return original->is_complete() && client.torrent_status(info.info_hash()).state!=bt::Torrent::State::Checking;}));
        QVERIFY(client.set_download_strategy(info.info_hash(),bt::DownloadStrategy::FirstLast,window));
        auto priorities=client.piece_ranges(info.info_hash(),8);QCOMPARE(priorities.size(),size_t(8));QCOMPARE(priorities.front().high,1u);QCOMPARE(priorities.back().high,1u);QCOMPARE(priorities[3].high,0u);
        QVERIFY(client.set_download_strategy(info.info_hash(),bt::DownloadStrategy::Sequential,window));
        QVERIFY(client.start_content(info.info_hash()));client.get_torrent(info.info_hash())->add_peer("127.0.0.1",source.listen_port());
        QVERIFY(pump(source,client,[&]{return client.torrent_status(info.info_hash()).is_complete;}));
        auto ranges=client.piece_ranges(info.info_hash(),3);QVERIFY(ranges.size()<=3);unsigned count=0;for(const auto& r:ranges){QCOMPARE(r.verified,r.end-r.begin);count+=r.verified;}QCOMPARE(count,8u);
        QFile actual(target+"/payload.bin");QVERIFY(actual.open(QIODevice::ReadOnly));QCOMPARE(actual.readAll(),QByteArray(payload.size(),'s'));QVERIFY(source.cache_bytes()<=32768);
    }
};
QTEST_GUILESS_MAIN(TestDownloadS3)
#include "test_download_s3.moc"
