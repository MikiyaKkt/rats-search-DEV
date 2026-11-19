#include "download_test_support.h"
#include <QJsonDocument>
#include <QSaveFile>
#include "services/torrent_session_store.h"
#include "app/application.h"
#include "rest/api_router.h"
using namespace download_test;

class QueueEngine : public FakeEngine {
public:
    QHash<QString,rats::net::TorrentSnapshot> states;
    QSet<QString> running;
    int maximum=0;
    rats::OperationResult createDownload(const QString& h,const QString&,CreateOptions) override {
        states[h].exists=true; rats::OperationResult r; r.hash=h; r.outcome=rats::OperationResult::Outcome::Applied; return r;
    }
    bool startMetadata(const QString& h) override { states[h].files={{"payload.bin",100,0}};states[h].hasMetadata=true; states[h].phase=rats::DownloadPhase::Waiting; states[h].paused=false; return true; }
    void setFilePriorities(const QString& h,const QVector<int>& p,qint64 expected,FileOperationCallback done) override {states[h].files[0].priority=p[0];states[h].selectionRevision=expected+1;rats::OperationResult r;r.hash=h;r.revision=expected+1;r.outcome=rats::OperationResult::Outcome::Applied;done(r);}
    bool startContent(const QString& h) override { running.insert(h); maximum=qMax(maximum,int(running.size())); states[h].phase=rats::DownloadPhase::Downloading; states[h].paused=false; return true; }
    void pause(const QString& h) override { running.remove(h); states[h].paused=true; }
    rats::net::TorrentSnapshot status(const QString& h) const override { return states.value(h); }
};
class TestDownloadS1 : public QObject {
    Q_OBJECT
private slots:
    void bucketBoundsAndClockReset() {
        bt::TokenBucket bucket; bucket.configure(1000,0);
        QCOMPARE(bucket.available(0),std::uint64_t(0));
        QCOMPARE(bucket.available(500),std::uint64_t(500));
        QVERIFY(bucket.consume(500,500)); QVERIFY(!bucket.consume(1,500));
        QCOMPARE(bucket.nextReadyAt(1,500),qint64(501));
        QCOMPARE(bucket.available(400),std::uint64_t(0));
        QCOMPARE(bucket.available(1000000),bucket.capacity());
        bucket.configure(1,1000000); QCOMPARE(bucket.available(1000000),std::uint64_t(0));
        bucket.configure(0,1000000); QVERIFY(bucket.consume(999999999,1000000));
        QVERIFY(!bucket.configure(-1,1000000));
        QVERIFY(bucket.configure(0.1L,0)); QCOMPARE(bucket.available(10000),std::uint64_t(1));
    }
    void connectionAndInflightBudgets() {
        bt::BandwidthController bandwidth; bt::InfoHash hash{};
        for(int i=0;i<32;++i) QVERIFY(bandwidth.reserve_download(hash,16384));
        QVERIFY(!bandwidth.reserve_download(hash,1));
        bandwidth.release_reservation(hash,16384); QVERIFY(bandwidth.reserve_download(hash,16384));
        bandwidth.remove_task(hash); QVERIFY(bandwidth.reserve_download(hash,16384));
        QTemporaryDir directory;
        bt::Client client(localConfig()), listener(localConfig()); client.open(); listener.open();
        const auto a=metadata(librats::Bytes(16384,'a')),b=metadata(librats::Bytes(16384,'b'));
        auto* ta=client.add_torrent(a,directory.path().toStdString());
        auto* tb=client.add_torrent(b,directory.path().toStdString());
        client.set_global_policy({0,0},2,1,1);
        bt::PeerList::Endpoint peer; peer.ip="127.0.0.1"; peer.port=listener.listen_port();
        client.connect_peer(*ta,peer); client.connect_peer(*ta,peer); client.connect_peer(*tb,peer);
        QCOMPARE(client.num_pending_connects(),std::size_t(2));
        client.set_global_policy({0,0},1,1,1); QCOMPARE(client.num_pending_connects(),std::size_t(1));
        client.remove_torrent(a.info_hash());client.remove_torrent(b.info_hash());
        QCOMPARE(client.num_pending_connects(),std::size_t(0));
    }
    void queueAdmissionAndRevisionConflicts() {
        QueueEngine engine; QTemporaryDir directory;
        rats::service::DownloadService service(&engine);
        QStringList hashes;
        for(int n=1;n<=10;++n) { auto h=QString::number(n,16).rightJustified(40,'0'); hashes<<h; QVERIFY(service.add(h,directory.path())); }
        for(int n=0;n<12;++n) {poll(service);for(const auto& h:hashes) {const auto d=service.getDownload(h);if(d.ready && !d.selectionConfirmed && !d.selectionApplying)service.setFilePriorities(h,{1},d.selectionRevision);}QCoreApplication::processEvents();}
        QCOMPARE(engine.running.size(),3); QVERIFY(engine.maximum<=3);
        auto policy=service.globalPolicy(); policy.downloadSlots=1;
        QVERIFY(service.setGlobalPolicy(policy,policy.revision).applied());
        QCOMPARE(engine.running.size(),1);
        const auto revision=service.globalPolicy().revision;
        auto stale=service.setGlobalPolicy(policy,revision-1);
        QCOMPARE(stale.error.code,QString("revisionConflict")); QCOMPARE(service.globalPolicy().downloadSlots,1);
        const auto first=*engine.running.begin();
        QVERIFY(service.pause(first)); for(int i=0;i<3;++i) poll(service);
        QVERIFY(!engine.running.contains(first)); QCOMPARE(engine.running.size(),1);
        QVERIFY(service.getDownload(first).paused);
        QVERIFY(service.resume(first)); QVERIFY(engine.running.size()<=1);
        auto task=service.getDownload(hashes.last()).policy; task.priority=1;
        QVERIFY(service.setTaskPolicy(hashes.last(),task,task.revision).applied());
        QCOMPARE(engine.running.size(),1); QVERIFY(engine.running.contains(hashes.last()));
        auto failed=service.setTaskPolicy(hashes.last(),task,0);
        QCOMPARE(failed.error.code,QString("revisionConflict"));
        service.setPolicyPersistence([](const auto&){return false;});
        auto r=service.setGlobalPolicy(service.globalPolicy(),service.globalPolicy().revision);
        QVERIFY(r.outcome==rats::OperationResult::Outcome::AppliedNotPersisted);
        QCOMPARE(service.operationStatus(r.operationId).operationId,r.operationId);
    }
    void apiValidationRespondsOnce() {
        QTemporaryDir directory;
        rats::app::Application::Options options; options.dataDirectory=directory.path(); options.headless=true;
        rats::app::Application app(options);
        for(const auto& method: {"download.policy.get","download.policy.set","download.taskPolicy.set","download.queue.move","download.operation.get","download.files.set","download.recheck","download.retry","download.pieces.get","download.schedule.get","download.schedule.set","download.seeding.set"}) {
            int replies=0;
            app.api()->call(method,{},[&](const rats::Result&){++replies;});
            QCOMPARE(replies,1);
        }
        const auto before=app.downloads()->globalPolicy().toJson();
        int replies=0;
        app.api()->call("download.policy.set",{{"policy",QJsonObject{{"downloadLimitBps",-1}}},{"expectedRevision",0}},
            [&](const rats::Result& result){++replies; QVERIFY(!result.ok());});
        QCOMPARE(replies,1); QCOMPARE(app.downloads()->globalPolicy().toJson(),before);
    }
    void actualUploadCountsPartialWrites() {
        class Link : public bt::PeerLink {
        public:
            Observer* observer=nullptr; std::size_t credit=0;
            bt::PeerTransport transport() const noexcept override {return bt::PeerTransport::Tcp;}
            void start(Observer* o) override {observer=o;}
            IoResult read(librats::ByteSpan) override {return {0,Status::WouldBlock};}
            IoResult write(const librats::ByteView* slices,std::size_t count) override {
                if(!credit || !count) return {0,Status::WouldBlock};
                const auto n=(std::min)({credit,slices[0].size(),std::size_t(3)});credit-=n;return {n,Status::Ok};
            }
            void want_write(bool) override {}
            void close() override {observer=nullptr;}
        };
        class Observer : public bt::PeerConnection::Observer {public: std::size_t sent=0;
            void on_payload_sent(bt::PeerConnection&,std::size_t n) override {sent+=n;}} observer;
        bt::Reactor reactor; auto link=std::make_unique<Link>();auto* raw=link.get();
        bt::InfoHash hash{}; bt::PeerId id{};
        bt::PeerConnection peer(reactor,std::move(link),true,hash,id,1,&observer);
        peer.start();peer.send_piece(0,0,librats::Bytes(10,'a')); QCOMPARE(observer.sent,std::size_t(0));
        raw->credit=85; raw->observer->on_link_writable(); QCOMPARE(observer.sent,std::size_t(4));
        raw->credit=100;raw->observer->on_link_writable();QCOMPARE(observer.sent,std::size_t(10));
        peer.close("test");
    }
    void realTransferCaps_data() {
        QTest::addColumn<bool>("utp"); QTest::addColumn<bool>("upload");
        QTest::newRow("tcp-download")<<false<<false;
        QTest::newRow("utp-mse-upload")<<true<<true;
    }
    void realTransferCaps() {
        QFETCH(bool,utp); QFETCH(bool,upload);
        QTemporaryDir directory;
        const auto seed=directory.filePath("seed"), target=directory.filePath("target");
        QVERIFY(QDir().mkpath(seed));
        librats::Bytes payload(4*1024*1024);
        for(std::size_t i=0;i<payload.size();++i) payload[i]=std::uint8_t((i*31+i/173)%251);
        const auto info=metadata(payload);
        QFile original(seed+"/payload.bin"); QVERIFY(original.open(QIODevice::WriteOnly));
        QCOMPARE(original.write(reinterpret_cast<const char*>(payload.data()),payload.size()),qint64(payload.size())); original.close();
        auto config=localConfig();
        if(utp) { config.enable_incoming_utp=true; config.enable_outgoing_utp=true; config.enable_outgoing_tcp=false;
            config.in_enc_policy=bt::EncPolicy::Forced; config.out_enc_policy=bt::EncPolicy::Forced; }
        bt::Client seeder(config),leecher(config); seeder.open();leecher.open();
        auto* source=seeder.add_torrent(info,seed.toStdString());
        auto* download=leecher.add_torrent(info,target.toStdString());
        QVERIFY(pump(seeder,leecher,[&]{return source->is_complete();}));
        constexpr std::uint64_t rate=32768;
        if(upload) seeder.set_global_policy({0,rate},200,50,1);
        else leecher.set_global_policy({rate,0},200,50,1);
        download->add_peer("127.0.0.1",seeder.listen_port());
        QVERIFY(pump(seeder,leecher,[&]{return leecher.torrent_status(info.info_hash()).downloaded>=32768;},12000));
        const auto before=upload?seeder.torrent_status(info.info_hash()).uploaded:leecher.torrent_status(info.info_hash()).downloaded;
        QElapsedTimer timer;timer.start();
        while(timer.elapsed()<60000) { seeder.reactor().run_one(1);leecher.reactor().run_one(1); }
        const auto bytes=(upload?seeder.torrent_status(info.info_hash()).uploaded:leecher.torrent_status(info.info_hash()).downloaded)-before;
        const double seconds=timer.elapsed()/1000.0, average=bytes/seconds;
        qInfo()<<"S1 saturated 60s"<<utp<<upload<<bytes<<seconds<<average;
        QVERIFY2(average<=rate*1.05,"Content exceeded configured cap");
        QVERIFY2(average>=rate*0.80,"Saturated local transfer failed to use its budget");
        // Removing a cap is live and must not reset the task or its verified data.
        if(upload) seeder.set_global_policy({0,0},200,50,1); else leecher.set_global_policy({0,0},200,50,1);
        QVERIFY(pump(seeder,leecher,[&]{return download->is_complete();},12000));
        QFile actual(target+"/payload.bin"); QVERIFY(actual.open(QIODevice::ReadOnly));
        QCOMPARE(actual.readAll(),QByteArray(reinterpret_cast<const char*>(payload.data()),payload.size()));
        QJsonArray measurements;
        const QString report=QDir::current().filePath("download-s1-transport.json");
        QFile old(report);if(old.open(QIODevice::ReadOnly)) measurements=QJsonDocument::fromJson(old.readAll()).array();
        old.close(); // Windows cannot atomically replace a file still open here.
        measurements.append(QJsonObject{{"transport",utp?"utp-mse":"tcp"},{"direction",upload?"upload":"download"},
            {"limitBps",qint64(rate)},{"bytes",qint64(bytes)},{"seconds",seconds},{"averageBps",average}});
        QSaveFile output(report);QVERIFY(output.open(QIODevice::WriteOnly));output.write(QJsonDocument(measurements).toJson());QVERIFY(output.commit());
    }
};
QTEST_GUILESS_MAIN(TestDownloadS1)
#include "test_download_s1.moc"
