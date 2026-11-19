#include "download_test_support.h"

using namespace download_test;

class TestDownloadErrors : public QObject {
    Q_OBJECT
private slots:
    void readOnlyFileReportsPermissionDenied()
    {
#ifdef Q_OS_WIN
        QTemporaryDir directory;
        const auto path = directory.filePath("payload.bin");
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(QByteArray(4096, 's')), qint64(4096)); file.close();
        QVERIFY(QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::ReadUser));
        const auto info = metadata(librats::Bytes(4096, 's'));
        bt::ThreadedDiskIo disk(info, directory.path().toStdString());
        std::promise<bt::DiskError> promise;
        auto result = promise.get_future();
        disk.async_write_detailed(0, 0, librats::Bytes(4096, 's'), [&](bt::DiskError error) { promise.set_value(error); });
        const auto error = result.get();
        QVERIFY(QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ReadUser | QFileDevice::WriteUser));
        QCOMPARE(error.code, std::string("permissionDenied"));
        QVERIFY(!error.retryable);
#else
        QSKIP("Windows read-only CRT behavior; errno classification is tested separately");
#endif
    }

    void hashMismatchDoesNotAdvanceVerifiedProgressOrBecomeIoError()
    {
        QTemporaryDir directory;
        QVERIFY(QDir().mkpath(directory.filePath("seed")));
        QFile file(directory.filePath("seed/payload.bin"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(QByteArray(4096, 'x')), qint64(4096)); file.close();
        const auto info = metadata(librats::Bytes(4096, 's'));
        bt::Client seeder(localConfig()), leecher(localConfig());
        seeder.open(); leecher.open();
        auto* source = seeder.add_torrent_for_seeding(info, directory.filePath("seed").toStdString());
        auto* target = leecher.add_torrent(info, directory.filePath("download").toStdString());
        QVERIFY(pump(seeder, leecher, [&] { return source->is_complete(); }));
        target->add_peer("127.0.0.1", seeder.listen_port());
        QVERIFY(pump(seeder, leecher, [&] { return target->bytes_downloaded() >= 3 * 4096; }));
        QCOMPARE(target->verified_bytes(), std::uint64_t(0));
        QCOMPARE(target->progress(), 0.0);
        QVERIFY(!target->last_error());
        QVERIFY(target->hash_failures() > 0);
        QVERIFY(!target->is_complete());
    }

    void classificationAndWorkerCapture()
    {
        QVERIFY(!bt::classify_fs_error(ENOSPC, "write", "p").retryable);
        QCOMPARE(bt::classify_fs_error(ENOSPC, "write", "p").code, std::string("diskFull"));
        QCOMPARE(bt::classify_fs_error(EACCES, "open", "p").code, std::string("permissionDenied"));
        QVERIFY(bt::classify_fs_error(EINTR, "write", "p").retryable);
        QTemporaryDir directory;
        const auto blocker = directory.filePath("blocker");
        QFile file(blocker);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.close();
        const auto info = metadata(librats::Bytes(4096, 's'));
        bt::ThreadedDiskIo disk(info, (blocker + "/child").toStdString());
        std::promise<bt::DiskError> promise;
        auto result = promise.get_future();
        disk.async_write_detailed(0, 0, librats::Bytes(4096, 's'), [&](bt::DiskError error) { promise.set_value(std::move(error)); });
        const auto error = result.get();
        QVERIFY(bool(error));
        QVERIFY(error.system_code != 0);
        QVERIFY(!error.operation.empty());
        QVERIFY(QString::fromStdString(error.path).startsWith(blocker));
        QCOMPARE(disk.queued_write_bytes(), size_t(0));
        QVERIFY(!QFile::exists(blocker + "/child/payload.bin"));
    }

    void diskFailureStopsRequestsAndCannotBeClearedByResume()
    {
        QTemporaryDir directory;
        const auto seed = directory.filePath("seed");
        QVERIFY(QDir().mkpath(seed));
        const auto blocker = directory.filePath("blocker");
        QFile block(blocker);
        QVERIFY(block.open(QIODevice::WriteOnly)); block.close();
        const librats::Bytes payload(35000, 's');
        const auto info = metadata(payload);
        QFile file(seed + "/payload.bin");
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(QByteArray(35000, 's')), qint64(35000)); file.close();
        bt::Client seeder(localConfig()), leecher(localConfig());
        seeder.open(); leecher.open();
        auto* source = seeder.add_torrent(info, seed.toStdString());
        QVERIFY(pump(seeder, leecher, [&] { return source->is_complete(); }));
        bt::CreateOptions options;
        options.mode = bt::StartMode::Automatic;
        QVERIFY(leecher.create_torrent(info, blocker.toStdString(), options).applied);
        auto* download = leecher.get_torrent(info.info_hash());
        download->add_peer("127.0.0.1", seeder.listen_port());
        QVERIFY(pump(seeder, leecher, [&] { return bool(download->last_error()); }));
        const auto status = leecher.torrent_status(info.info_hash());
        QVERIFY(status.state == bt::Torrent::State::Error);
        QVERIFY(status.paused);
        QVERIFY(!status.is_complete);
        QCOMPARE(status.verified, std::uint64_t(0));
        QCOMPARE(download->num_outstanding_requests(), size_t(0));
        QVERIFY(!leecher.start_content(info.info_hash()));
        leecher.resume_torrent(info.info_hash());
        QVERIFY(download->is_paused());
        const auto received = download->bytes_downloaded();
        QElapsedTimer timer; timer.start();
        pump(seeder, leecher, [&] { return timer.elapsed() > 1200; });
        QCOMPARE(download->bytes_downloaded(), received);
        leecher.remove_torrent(info.info_hash());
        for (int i = 0; i < 10; ++i) leecher.reactor().run_one(1); // queued completions must not dereference removed torrent
        QVERIFY(!leecher.torrent_status(info.info_hash()).exists);
    }
};

QTEST_GUILESS_MAIN(TestDownloadErrors)
#include "test_download_errors.moc"
