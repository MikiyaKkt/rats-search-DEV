#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

#pragma push_macro("emit")
#pragma push_macro("slots")
#pragma push_macro("signals")
#undef emit
#undef slots
#undef signals
#include "librats/bittorrent/bencode.h"
#include "librats/bittorrent/disk_io.h"
#include "librats/crypto/sha1.h"
#include "librats/util/fs.h"
#pragma pop_macro("signals")
#pragma pop_macro("slots")
#pragma pop_macro("emit")

#include <future>

class TestUtf8DownloadPaths : public QObject {
    Q_OBJECT
private slots:
    void selectiveCrossFileStorage()
    {
        namespace bt=librats::bittorrent;
        QTemporaryDir temp;QVERIFY(temp.isValid());
        auto info=librats::BencodeValue::create_dict();info["name"]=librats::BencodeValue(std::string(u8"中文"));
        info["piece length"]=librats::BencodeValue(std::int64_t(16384));
        auto list=librats::BencodeValue::create_list();
        for(const auto& entry:std::vector<std::pair<std::string,int>>{{"left",8192},{"middle",16384},{"right",8192},{"empty",0}}) {
            auto f=librats::BencodeValue::create_dict();f["length"]=librats::BencodeValue(std::int64_t(entry.second));
            auto path=librats::BencodeValue::create_list();path.as_list().push_back(librats::BencodeValue(entry.first));f["path"]=path;list.as_list().push_back(f);
        }
        info["files"]=list;librats::Bytes bytes(32768);for(size_t i=0;i<bytes.size();++i)bytes[i]=i%251;
        std::string hashes;for(size_t offset:{size_t(0),size_t(16384)}) {auto hash=librats::SHA1::hash_raw(bytes.data()+offset,16384);hashes.append(reinterpret_cast<const char*>(hash.data()),20);}
        info["pieces"]=librats::BencodeValue(hashes);
        auto meta=bt::TorrentInfo::from_info_dict(info.encode(),bt::InfoHash{});QVERIFY(meta);
        auto apply=[](bt::ThreadedDiskIo& disk,std::vector<int> priorities,int revision){std::promise<bt::DiskError> result;auto future=result.get_future();disk.apply_file_selection(priorities,revision,[&](auto error){result.set_value(error);});return !future.get();};
        auto check=[](bt::ThreadedDiskIo& disk){std::promise<bt::Bitfield> result;auto future=result.get_future();disk.async_check_files({},nullptr,[&](auto bits){result.set_value(bits);});return future.get().count();};
        const auto root=temp.path()+QString::fromUtf8(u8"/中文/");
        {
            bt::ThreadedDiskIo disk(*meta,temp.path().toStdString(),{},{4});
            QVERIFY(apply(disk,{0,2,0,0},1));
            for(unsigned piece=0;piece<2;++piece) {std::promise<bool> result;auto future=result.get_future();disk.async_write(piece,0,librats::Bytes(bytes.begin()+piece*16384,bytes.begin()+(piece+1)*16384),[&](bool ok){result.set_value(ok);});QVERIFY(future.get());}
            QCOMPARE(check(disk),2u);QVERIFY(!QFile::exists(root+"left"));QVERIFY(!QFile::exists(root+"right"));
            QFile middle(root+"middle");QVERIFY(middle.open(QIODevice::ReadOnly));QCOMPARE(middle.readAll(),QByteArray(reinterpret_cast<const char*>(bytes.data()+8192),16384));middle.close();
        }
        {
            bt::ThreadedDiskIo disk(*meta,temp.path().toStdString());QCOMPARE(disk.selection_revision(),std::uint64_t(1));QCOMPARE(check(disk),2u);
            QVERIFY(apply(disk,{1,2,0,1},2));QVERIFY(QFile::exists(root+"left"));QCOMPARE(QFileInfo(root+"empty").size(),qint64(0));QCOMPARE(check(disk),2u);
            QVERIFY(QFile::remove(root+"middle"));QCOMPARE(check(disk),0u);
            QVERIFY(apply(disk,{0,0,0,0},3));
        }
        const auto manifest=temp.path()+"/.parts/"+QString::fromStdString(meta->info_hash_hex())+"/manifest";
        QFile corrupt(manifest);QVERIFY(corrupt.open(QIODevice::WriteOnly));corrupt.write("corrupt");corrupt.close();
        bt::ThreadedDiskIo broken(*meta,temp.path().toStdString());std::promise<bt::DiskError> result;auto future=result.get_future();
        broken.async_check_files_detailed({},nullptr,[&](auto error,auto){result.set_value(error);});QVERIFY(bool(future.get()));
    }

    void filesystemRoundTrip()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString directory = temp.path() + QString::fromUtf8(u8"/\u4e0b\u8f7d/\u6d4b\u8bd5\U0001f4e6");
        const QString file = directory + QString::fromUtf8(u8"/\u65e5\u672c\u8a9e.bin");
        QVERIFY(librats::create_directories(directory.toStdString().c_str()));
        QVERIFY(QDir(directory).exists());
        QVERIFY(librats::create_file_with_size(file.toStdString().c_str(), 8));
        QVERIFY(QFile::exists(file));
        QVERIFY(librats::write_file_chunk(file.toStdString(), 0, "payload!", 8));
        QCOMPARE(librats::get_file_size(file.toStdString().c_str()), qint64(8));
        {
            librats::FileStream stream;
            QVERIFY(stream.open_read(file.toStdString().c_str()));
            char bytes[8]{};
            QCOMPARE(stream.read(bytes, sizeof(bytes)), sizeof(bytes));
            QCOMPARE(QByteArray(bytes, sizeof(bytes)), QByteArray("payload!"));
        }
        std::vector<librats::DirectoryEntry> entries;
        QVERIFY(librats::list_directory(directory.toStdString().c_str(), entries));
        QCOMPARE(entries.size(), size_t(1));
        QCOMPARE(QString::fromStdString(entries.front().name), QFileInfo(file).fileName());
        const QString renamed = file + QString::fromUtf8(u8"-\u526f\u672c");
        QVERIFY(librats::rename_file(file.toStdString().c_str(), renamed.toStdString().c_str()));
        QVERIFY(QFile::exists(renamed));
        QFile native(renamed);
        QVERIFY(native.open(QIODevice::ReadOnly));
        QCOMPARE(native.readAll(), QByteArray("payload!"));
        native.close();
        QVERIFY(librats::delete_file(renamed.toStdString().c_str()));
        QVERIFY(librats::delete_directory(directory.toStdString().c_str()));
    }

    void torrentDiskWriteAndRecheck()
    {
        namespace bt = librats::bittorrent;
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString save = temp.path() + QString::fromUtf8(u8"/\u4e0b\u8f7d\U0001f4e6");
        const std::string name = u8"\u4e2d\u6587\u65e5\u672c\u8a9e.bin";
        const librats::Bytes payload(4096, 0x73);
        const auto hash = librats::SHA1::hash_raw(payload.data(), payload.size());
        auto info = librats::BencodeValue::create_dict();
        info["name"] = librats::BencodeValue(name);
        info["length"] = librats::BencodeValue(std::int64_t(payload.size()));
        info["piece length"] = librats::BencodeValue(std::int64_t(16384));
        info["pieces"] = librats::BencodeValue(std::string(reinterpret_cast<const char*>(hash.data()), hash.size()));
        const auto metadata = bt::TorrentInfo::from_info_dict(info.encode(), bt::InfoHash{});
        QVERIFY(metadata.has_value());
        {
            bt::ThreadedDiskIo disk(*metadata, save.toStdString(), {}, {2});
            std::promise<bool> result;
            auto done = result.get_future();
            disk.async_write(0, 0, payload, [&](bool ok) { result.set_value(ok); });
            QVERIFY(done.get());
        }
        QFile actual(save + '/' + QString::fromStdString(name));
        QVERIFY(actual.open(QIODevice::ReadOnly));
        QCOMPARE(actual.readAll(), QByteArray(4096, 's'));
        actual.close();
        bt::ThreadedDiskIo reopened(*metadata, save.toStdString(), {}, {2});
        std::promise<bt::Bitfield> result;
        auto done = result.get_future();
        reopened.async_check_files(bt::Bitfield{}, nullptr, [&](bt::Bitfield have) { result.set_value(std::move(have)); });
        QCOMPARE(done.get().count(), std::uint32_t(1));
    }
};

QTEST_GUILESS_MAIN(TestUtf8DownloadPaths)
#include "test_utf8_download_paths.moc"
