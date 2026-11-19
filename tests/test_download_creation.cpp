#include "download_test_support.h"

using namespace download_test;

class TestDownloadCreation : public QObject {
    Q_OBJECT
private slots:
    void largeSelectionRetainsProgressWithin500ms()
    {
        // 128 GiB logical torrent, 32,768 pieces. Trusted resume fixtures avoid
        // allocating payload files; any full recheck would destroy this state.
        QTemporaryDir directory;auto dict=librats::BencodeValue::create_dict();
        constexpr std::uint64_t total=128ULL*1024*1024*1024;constexpr int pieces=32768;
        dict["name"]=librats::BencodeValue(std::string("logical"));
        auto files=librats::BencodeValue::create_list();
        for(int i=0;i<2;++i){auto file=librats::BencodeValue::create_dict();file["length"]=librats::BencodeValue(std::int64_t(total/2));
            auto path=librats::BencodeValue::create_list();path.as_list().push_back(librats::BencodeValue(std::to_string(i)+".bin"));file["path"]=path;files.as_list().push_back(file);}
        dict["files"]=files;
        dict["piece length"]=librats::BencodeValue(std::int64_t(4*1024*1024));dict["pieces"]=librats::BencodeValue(std::string(pieces*20,'h'));
        const auto info=bt::TorrentInfo::from_info_dict(dict.encode(),{}).value();
        bt::ResumeData resume;resume.info_hash=info.info_hash();resume.have=bt::Bitfield(pieces,true);
        bt::Client client(localConfig()),idle(localConfig());client.open();idle.open();
        auto* torrent=client.add_torrent_with_resume(info,resume,directory.path().toStdString());
        QVERIFY(pump(client,idle,[&]{return torrent->state()!=bt::Torrent::State::Checking;}));
        QCOMPARE(client.torrent_status(info.info_hash()).verified,total);qint64 worst=0;
        for(int revision=0;revision<10;++revision) {
            bool done=false;bt::DiskError failure;QElapsedTimer timer;timer.start();
            client.set_file_priorities(info.info_hash(),{revision%2?1:0,2},revision,[&](auto error,auto){failure=error;done=true;});
            QVERIFY(pump(client,idle,[&]{return done;},1000));worst=qMax(worst,timer.elapsed());QVERIFY(!failure);
            const auto status=client.torrent_status(info.info_hash());QCOMPARE(status.verified,total);QCOMPARE(status.wanted_verified,revision%2?total:total/2);
            QCOMPARE(status.progress,1.0);QVERIFY(status.state!=bt::Torrent::State::Checking);QVERIFY(timer.elapsed()<500);
        }
        qInfo()<<"Selection 128 GiB / 32768 pieces / 10 changes; worst milliseconds:"<<worst;
        QVERIFY(!QFile::exists(directory.filePath("logical/0.bin")));QVERIFY(!QFile::exists(directory.filePath("logical/1.bin")));
    }

    void selectiveNetworkDownloadAndRecheck()
    {
        QTemporaryDir directory;
        auto info=librats::BencodeValue::create_dict();info["name"]=librats::BencodeValue(std::string("bundle"));
        info["piece length"]=librats::BencodeValue(std::int64_t(16384));auto files=librats::BencodeValue::create_list();
        const librats::Bytes payload(98304,'s');std::string hashes;
        for(int i=0;i<3;++i) {auto f=librats::BencodeValue::create_dict();f["length"]=librats::BencodeValue(std::int64_t(32768));auto path=librats::BencodeValue::create_list();path.as_list().push_back(librats::BencodeValue(std::to_string(i)+".bin"));f["path"]=path;files.as_list().push_back(f);}
        info["files"]=files;for(size_t offset=0;offset<payload.size();offset+=16384){const auto h=librats::SHA1::hash_raw(payload.data()+offset,16384);hashes.append(reinterpret_cast<const char*>(h.data()),20);}info["pieces"]=librats::BencodeValue(hashes);
        const auto meta=bt::TorrentInfo::from_info_dict(info.encode(),{}).value();
        const auto seed=directory.filePath("seed"),target=directory.filePath("target");QVERIFY(QDir().mkpath(seed+"/bundle"));
        for(int i=0;i<3;++i){QFile file(seed+"/bundle/"+QString::number(i)+".bin");QVERIFY(file.open(QIODevice::WriteOnly));QCOMPARE(file.write(QByteArray(32768,'s')),qint64(32768));}
        bt::Client source(localConfig()),client(localConfig());source.open();client.open();auto* seeder=source.add_torrent(meta,seed.toStdString());
        bt::CreateOptions options;options.mode=bt::StartMode::MetadataOnly;QVERIFY(client.create_torrent(meta,target.toStdString(),options).applied);
        QVERIFY(pump(source,client,[&]{return seeder->is_complete() && client.torrent_status(meta.info_hash()).state!=bt::Torrent::State::Checking;}));
        bool applied=false;client.set_file_priorities(meta.info_hash(),{0,2,0},0,[&](auto error,auto revision){applied=!error && revision==1;});
        QVERIFY(pump(source,client,[&]{return applied && client.torrent_status(meta.info_hash()).state!=bt::Torrent::State::Checking;}));
        QVERIFY(client.start_content(meta.info_hash()));client.get_torrent(meta.info_hash())->add_peer("127.0.0.1",source.listen_port());
        QVERIFY(pump(source,client,[&]{return client.torrent_status(meta.info_hash()).selected_complete;}));
        auto status=client.torrent_status(meta.info_hash());QCOMPARE(status.wanted_bytes,std::uint64_t(32768));QCOMPARE(status.wanted_verified,std::uint64_t(32768));
        QVERIFY(!status.full_complete);QVERIFY(!status.is_complete);QVERIFY(status.state==bt::Torrent::State::PartialSeeding);
        QVERIFY(!QFile::exists(target+"/bundle/0.bin"));QVERIFY(!QFile::exists(target+"/bundle/2.bin"));QCOMPARE(status.verified,std::uint64_t(32768));
        QElapsedTimer selectionTimer;selectionTimer.start();bool changed=false;
        client.set_file_priorities(meta.info_hash(),{1,2,0},1,[&](auto error,auto){changed=!error;});
        QVERIFY(pump(source,client,[&]{return changed;}));QVERIFY(selectionTimer.elapsed()<500);
        status=client.torrent_status(meta.info_hash());QCOMPARE(status.progress,0.5);QCOMPARE(status.verified,std::uint64_t(32768));
        QVERIFY(status.state!=bt::Torrent::State::Checking);QVERIFY(!QFile::exists(target+"/bundle/0.bin"));
        changed=false;client.set_file_priorities(meta.info_hash(),{0,2,0},2,[&](auto error,auto){changed=!error;});
        QVERIFY(pump(source,client,[&]{return changed;}));QCOMPARE(client.torrent_status(meta.info_hash()).progress,1.0);
        QVERIFY(client.start_content(meta.info_hash())); // service normally resumes after the file-operation barrier
        bt::Client peer(localConfig());peer.open();
        QVERIFY(peer.create_torrent(meta,directory.filePath("peer").toStdString(),options).applied);
        QVERIFY(pump(client,peer,[&]{return peer.torrent_status(meta.info_hash()).state!=bt::Torrent::State::Checking;}));
        bool selected=false;peer.set_file_priorities(meta.info_hash(),{0,1,0},0,[&](auto error,auto){selected=!error;});
        QVERIFY(pump(client,peer,[&]{return selected && peer.torrent_status(meta.info_hash()).state!=bt::Torrent::State::Checking;}));
        QVERIFY(peer.start_content(meta.info_hash()));peer.get_torrent(meta.info_hash())->add_peer("127.0.0.1",client.listen_port());
        QVERIFY(pump(client,peer,[&]{return peer.torrent_status(meta.info_hash()).selected_complete;}));
        QVERIFY(!peer.torrent_status(meta.info_hash()).full_complete);
        QVERIFY(QFile::remove(target+"/bundle/1.bin"));bool checked=false;
        client.recheck_torrent(meta.info_hash(),false,[&](auto error,auto){checked=!error;});
        QVERIFY(pump(source,client,[&]{return checked;}));QCOMPARE(client.torrent_status(meta.info_hash()).verified,std::uint64_t(0));
        bool conflict=false;client.set_file_priorities(meta.info_hash(),{1,1,1},0,[&](auto error,auto){conflict=error.code=="revisionConflict";});QVERIFY(conflict);
    }

    void progressIsWeightedByVerifiedBytesNotPieceCount()
    {
        QTemporaryDir directory;
        const auto info = metadata(librats::Bytes(20000, 's'));
        QFile file(directory.filePath("payload.bin"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(QByteArray(16384, 's') + QByteArray(3616, 'x')), qint64(20000)); file.close();
        bt::Client client(localConfig()); client.open();
        bt::CreateOptions options; options.mode = bt::StartMode::MetadataOnly;
        QVERIFY(client.create_torrent(info, directory.path().toStdString(), options).applied);
        QElapsedTimer timer; timer.start();
        while (timer.elapsed() < 3000 && client.torrent_status(info.info_hash()).state == bt::Torrent::State::Checking)
            client.reactor().run_one(1);
        const auto status = client.torrent_status(info.info_hash());
        QCOMPARE(status.verified, std::uint64_t(16384));
        QCOMPARE(status.progress, 16384.0 / 20000);
        QCOMPARE(status.files[0].verified, std::uint64_t(16384));
        QVERIFY(!status.is_complete);
    }

    void oldCheckCompletionCannotReviveRestartedTorrent()
    {
        QTemporaryDir directory;
        QFile file(directory.filePath("payload.bin"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(QByteArray(4096, 's')), qint64(4096)); file.close();
        const auto info = metadata(librats::Bytes(4096, 's'));
        bt::Client client(localConfig()); client.open();
        auto* torrent = client.add_torrent(info, directory.path().toStdString());
        torrent->stop(); // join worker, leaving its old completion queued
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(file.write(QByteArray(4096, 'x')), qint64(4096)); file.close();
        torrent->start();
        QElapsedTimer timer; timer.start();
        while (timer.elapsed() < 3000 && torrent->state() == bt::Torrent::State::Checking)
            client.reactor().run_one(1);
        QVERIFY(torrent->state() == bt::Torrent::State::Downloading);
        QCOMPARE(torrent->verified_bytes(), std::uint64_t(0));
        QVERIFY(!torrent->is_complete());
    }

    void backgroundMetadataFetchDoesNotOwnUserTasks()
    {
        QTemporaryDir directory;
        librats::Bittorrent::Config config;
        config.client = localConfig();
        config.client.download_path = directory.path().toStdString();
        librats::Bittorrent subsystem(config);
        subsystem.start();
        const auto info = metadata(librats::Bytes(4096, 's'));
        QVERIFY(subsystem.client()->create_torrent(info, directory.path().toStdString()).applied);
        std::promise<bool> promise;
        auto done = promise.get_future();
        subsystem.get_torrent_metadata(info.info_hash_hex(), [&](const bt::TorrentInfo& result, bool ok, const std::string&) {
            promise.set_value(ok && result.info_hash() == info.info_hash());
        }, 1000);
        QVERIFY(done.wait_for(std::chrono::seconds(3)) == std::future_status::ready);
        QVERIFY(done.get());
        // This synchronous status call is also a reactor barrier after cleanup.
        const auto status = subsystem.client()->torrent_status(info.info_hash());
        QVERIFY(status.exists);
        QVERIFY(!status.content_allowed);
        QVERIFY(status.state == bt::Torrent::State::Stopped);
        QVERIFY(QDir(directory.path()).isEmpty());
    }

    void inactiveCreationHasNoSideEffects()
    {
        QTemporaryDir directory;
        const auto destination = directory.filePath("not-created");
        const auto info = metadata(librats::Bytes(4096, 's'));
        bt::Client client(localConfig());
        client.open();
        QVERIFY(!client.create_magnet("invalid", destination.toStdString()).applied);
        const auto result = client.create_torrent(info, destination.toStdString());
        QVERIFY(result.applied);
        for (int i = 0; i < 10; ++i) client.reactor().run_one(1);
        const auto status = client.torrent_status(info.info_hash());
        QVERIFY(status.exists);
        QVERIFY(status.has_metadata);
        QVERIFY(status.state == bt::Torrent::State::Stopped);
        QVERIFY(!status.content_allowed);
        QVERIFY(!QFile::exists(destination));
        QVERIFY(!client.create_torrent(info, destination.toStdString()).applied);
        QVERIFY(client.start_metadata(info.info_hash()));
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < 3000 && client.torrent_status(info.info_hash()).state == bt::Torrent::State::Checking)
            client.reactor().run_one(1);
        QVERIFY(client.torrent_status(info.info_hash()).state == bt::Torrent::State::Waiting);
        QVERIFY(!QFile::exists(destination));
        QVERIFY(client.start_content(info.info_hash()));
        QVERIFY(client.torrent_status(info.info_hash()).state == bt::Torrent::State::Downloading);
    }

    void magnetMetadataCannotStartContentUntilAllowed()
    {
        QTemporaryDir directory;
        const auto seed = directory.filePath("seed");
        const auto target = directory.filePath("download");
        QVERIFY(QDir().mkpath(seed));
        const librats::Bytes payload(35000, 's');
        const auto info = metadata(payload);
        QFile original(seed + "/payload.bin");
        QVERIFY(original.open(QIODevice::WriteOnly));
        QCOMPARE(original.write(reinterpret_cast<const char*>(payload.data()), payload.size()), qint64(payload.size()));
        original.close();
        bt::Client seeder(localConfig()), leecher(localConfig());
        seeder.open(); leecher.open();
        auto* source = seeder.add_torrent(info, seed.toStdString());
        QVERIFY(pump(seeder, leecher, [&] { return source->is_complete(); }));
        bt::CreateOptions options;
        options.mode = bt::StartMode::MetadataOnly;
        const auto created = leecher.create_magnet("magnet:?xt=urn:btih:" + info.info_hash_hex(), target.toStdString(), options);
        QVERIFY(created.applied);
        auto* download = leecher.get_torrent(info.info_hash());
        download->add_peer("127.0.0.1", seeder.listen_port());
        QVERIFY(pump(seeder, leecher, [&] {
            return leecher.torrent_status(info.info_hash()).state == bt::Torrent::State::Waiting;
        }));
        QCOMPARE(download->bytes_downloaded(), std::uint64_t(0));
        QCOMPARE(download->num_outstanding_requests(), size_t(0));
        QVERIFY(!QFile::exists(target + "/payload.bin"));
        QVERIFY(leecher.start_content(info.info_hash()));
        QVERIFY(pump(seeder, leecher, [&] { return download->is_complete(); }));
        QCOMPARE(download->verified_bytes(), std::uint64_t(payload.size()));
        QCOMPARE(leecher.torrent_status(info.info_hash()).files[0].verified, std::uint64_t(payload.size()));
        QFile actual(target + "/payload.bin");
        QVERIFY(actual.open(QIODevice::ReadOnly));
        QCOMPARE(actual.readAll(), QByteArray(35000, 's'));
        actual.close();
        download->stop(); download->start();
        QVERIFY(pump(seeder, leecher, [&] { return download->is_complete(); }));
        QCOMPARE(download->verified_bytes(), std::uint64_t(payload.size())); // not counted twice on restart

        QVERIFY(download->save_resume_data());
        leecher.remove_torrent(info.info_hash());
        QVERIFY(actual.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(actual.write(QByteArray(35000, 'x')), qint64(35000));
        actual.close();
        options.load_resume = true;
        options.verify_existing = true;
        QVERIFY(leecher.create_magnet("magnet:?xt=urn:btih:" + info.info_hash_hex(), target.toStdString(), options).applied);
        QVERIFY(pump(seeder, leecher, [&] { return leecher.torrent_status(info.info_hash()).state == bt::Torrent::State::Waiting; }));
        const auto checked = leecher.torrent_status(info.info_hash());
        QCOMPARE(checked.verified, std::uint64_t(0));
        QVERIFY(!checked.is_complete);
        QVERIFY(!checked.error);
    }
};

QTEST_GUILESS_MAIN(TestDownloadCreation)
#include "test_download_creation.moc"
