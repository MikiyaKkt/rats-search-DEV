#pragma once
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>
#include "services/download_service.h"

#pragma push_macro("emit")
#pragma push_macro("slots")
#pragma push_macro("signals")
#undef emit
#undef slots
#undef signals
#include "librats/bittorrent/client.h"
#include "librats/bittorrent/bencode.h"
#include "librats/crypto/sha1.h"
#include "librats/subsystems/bittorrent.h"
#pragma pop_macro("signals")
#pragma pop_macro("slots")
#pragma pop_macro("emit")

#include <future>

namespace download_test {
namespace bt = librats::bittorrent;

inline bt::TorrentInfo metadata(const librats::Bytes& payload, const std::string& name = "payload.bin") {
    auto info = librats::BencodeValue::create_dict();
    info["name"] = librats::BencodeValue(name);
    info["length"] = librats::BencodeValue(std::int64_t(payload.size()));
    info["piece length"] = librats::BencodeValue(std::int64_t(16384));
    std::string hashes;
    for (size_t offset = 0; offset < payload.size(); offset += 16384) {
        const auto hash = librats::SHA1::hash_raw(payload.data() + offset, std::min<size_t>(16384, payload.size() - offset));
        hashes.append(reinterpret_cast<const char*>(hash.data()), hash.size());
    }
    info["pieces"] = librats::BencodeValue(hashes);
    return bt::TorrentInfo::from_info_dict(info.encode(), bt::InfoHash{}).value();
}

inline bool pump(bt::Client& a, bt::Client& b, const std::function<bool()>& done, int timeout = 8000) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeout && !done()) { a.reactor().run_one(1); b.reactor().run_one(1); }
    return done();
}

inline bt::Client::Config localConfig() {
    bt::Client::Config config;
    config.listen_port = 0;
    config.enable_incoming_utp = false;
    config.enable_outgoing_utp = false;
    config.in_enc_policy = bt::EncPolicy::Disabled;
    config.out_enc_policy = bt::EncPolicy::Disabled;
    return config;
}

class FakeEngine : public rats::net::TorrentEngine {
public:
    FakeEngine() : TorrentEngine(nullptr) {}
    mutable rats::net::TorrentSnapshot snapshot;
    QStringList calls;
    CreateOptions creation;
    std::function<void()> onStart;
    bool isReady() const override { return true; }
    bool setGlobalPolicy(qint64,qint64,int,int,int) override { return true; }
    bool setTaskPolicy(const QString&,qint64,qint64,int) override { return true; }
    bool setAdvancedPolicy(const QString&,const QString&,bool,int,int,int) override {return true;}
    bool allowContent(const QString&,bool) override { return true; }
    void setFilePriorities(const QString& hash,const QVector<int>& priorities,qint64 expected,FileOperationCallback done) override {
        for(int i=0;i<priorities.size();++i)snapshot.files[i].priority=priorities[i];
        snapshot.selectionRevision=expected+1;rats::OperationResult result;result.hash=hash;result.revision=expected+1;
        result.outcome=rats::OperationResult::Outcome::Applied;done(result);
    }
    rats::OperationResult createDownload(const QString& source, const QString&, CreateOptions options) override {
        creation = options;
        calls.append("create");
        snapshot.exists = true;
        rats::OperationResult result;
        result.hash = source;
        result.outcome = rats::OperationResult::Outcome::Applied;
        return result;
    }
    bool startContent(const QString&) override {
        calls.append("content");
        if (onStart) onStart();
        snapshot.phase = rats::DownloadPhase::Downloading;
        snapshot.paused = false;
        return true;
    }
    bool startMetadata(const QString&) override { calls.append("metadata"); snapshot.phase = rats::DownloadPhase::Checking; return true; }
    rats::net::TorrentSnapshot status(const QString&) const override { return snapshot; }
    void pause(const QString&) override { calls.append("pause"); snapshot.paused = true; }
    void remove(const QString&) override { calls.append("remove"); }
    void saveResumeData(const QString&) override {}
};

inline void poll(rats::service::DownloadService& service) {
    if (!QMetaObject::invokeMethod(&service, "onUpdateTimer", Qt::DirectConnection)) qFatal("Cannot poll download service");
}
} // namespace download_test
