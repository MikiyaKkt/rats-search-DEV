#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#pragma push_macro("emit")
#pragma push_macro("slots")
#pragma push_macro("signals")
#undef emit
#undef slots
#undef signals
#include "librats/bittorrent/client.h"
#include "librats/bittorrent/bencode.h"
#include "librats/crypto/sha1.h"
#include "librats/util/logger.h"
#pragma pop_macro("signals")
#pragma pop_macro("slots")
#pragma pop_macro("emit")

#include <psapi.h>
#include <atomic>
#include <iostream>

namespace bt = librats::bittorrent;
std::atomic<std::uint64_t> logicalWrites{0};
extern "C" bool __real__ZN7librats16write_file_chunkEPKcyPKvy(const char*, std::uint64_t, const void*, size_t);
extern "C" bool __wrap__ZN7librats16write_file_chunkEPKcyPKvy(const char* path, std::uint64_t offset, const void* data, size_t size) {
    const bool ok = __real__ZN7librats16write_file_chunkEPKcyPKvy(path, offset, data, size);
    if (ok) logicalWrites.fetch_add(size, std::memory_order_relaxed);
    return ok;
}
extern "C" bool __real__ZN7librats21create_file_with_sizeEPKcy(const char*, std::uint64_t);
extern "C" bool __wrap__ZN7librats21create_file_with_sizeEPKcy(const char* path, std::uint64_t size) {
    const bool ok = __real__ZN7librats21create_file_with_sizeEPKcy(path, size);
    if (ok && size) logicalWrites.fetch_add(1, std::memory_order_relaxed);
    return ok;
}

double cpuSeconds() {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return -1;
    auto ticks = [](FILETIME time) { return (std::uint64_t(time.dwHighDateTime) << 32) | time.dwLowDateTime; };
    return double(ticks(kernel) + ticks(user)) / 1e7;
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    librats::Logger::getInstance().set_log_level(librats::LogLevel::ERROR);
    const QByteArray payload(32 * 1024 * 1024, 's');
    auto info = librats::BencodeValue::create_dict();
    info["name"] = librats::BencodeValue(std::string("payload.bin"));
    info["length"] = librats::BencodeValue(std::int64_t(payload.size()));
    info["piece length"] = librats::BencodeValue(std::int64_t(262144));
    std::string hashes;
    for (qsizetype offset = 0; offset < payload.size(); offset += 262144) {
        const auto hash = librats::SHA1::hash_raw(reinterpret_cast<const std::uint8_t*>(payload.data() + offset), 262144);
        hashes.append(reinterpret_cast<const char*>(hash.data()), hash.size());
    }
    info["pieces"] = librats::BencodeValue(hashes);
    const auto metadata = bt::TorrentInfo::from_info_dict(info.encode(), bt::InfoHash{}).value();
    QJsonArray runs;
    for (int iteration = 0; iteration < 3; ++iteration) {
        QTemporaryDir temp;
        if (!temp.isValid() || !QDir().mkpath(temp.filePath("seed"))) return 2;
        QFile original(temp.filePath("seed/payload.bin"));
        if (!original.open(QIODevice::WriteOnly) || original.write(payload) != payload.size()) return 3;
        original.close();
        bt::Client::Config config;
        config.listen_port = 0;
        config.enable_incoming_utp = config.enable_outgoing_utp = false;
        config.in_enc_policy = config.out_enc_policy = bt::EncPolicy::Disabled;
        bt::Client seed(config), download(config);
        seed.open(); download.open();
        auto* source = seed.add_torrent(metadata, temp.filePath("seed").toStdString());
        auto* target = download.add_torrent(metadata, temp.filePath("download").toStdString());
#ifdef BENCH_ADVANCED
        const auto cache=qEnvironmentVariableIntValue("RATS_BENCH_CACHE_BYTES");
        seed.set_cache_budget(qMax(0,cache));download.set_cache_budget(qMax(0,cache));
        bt::RequestWindowPolicy window;
        const int manual=qEnvironmentVariableIntValue("RATS_BENCH_WINDOW");if(manual>0){window.automatic=false;window.manual=manual;}
        const auto strategy=qEnvironmentVariable("RATS_BENCH_STRATEGY");
        if(!download.set_download_strategy(metadata.info_hash(),strategy=="sequential"?bt::DownloadStrategy::Sequential:strategy=="firstLast"?bt::DownloadStrategy::FirstLast:bt::DownloadStrategy::RarestFirst,window))return 9;
#endif
        QElapsedTimer timer;
        timer.start();
        while (!source->is_complete() || target->state() == bt::Torrent::State::Checking) {
            if (timer.elapsed() > 15000) return 4;
            seed.reactor().run_one(1); download.reactor().run_one(1);
        }
        logicalWrites = 0;
        const double cpuStart = cpuSeconds();
        timer.restart();
        target->add_peer("127.0.0.1", seed.listen_port());
        while (!target->is_complete()) {
            if (timer.elapsed() > 60000) return 5;
            seed.reactor().run_one(0); download.reactor().run_one(1);
        }
        const auto milliseconds = timer.elapsed();
        const double cpu = cpuSeconds() - cpuStart;
        const auto written = logicalWrites.load();
        QFile result(temp.filePath("download/payload.bin"));
        if (!result.open(QIODevice::ReadOnly) || result.readAll() != payload) return 6;
        result.close();
        PROCESS_MEMORY_COUNTERS memory{};
        GetProcessMemoryInfo(GetCurrentProcess(), &memory, sizeof(memory));
        runs.append(QJsonObject{{"elapsedMs", milliseconds}, {"MiBps", 32000.0 / milliseconds},
            {"cpuSeconds", cpu}, {"peakWorkingSetMiB", double(memory.PeakWorkingSetSize) / (1024 * 1024)},
            {"logicalWrittenBytes", qint64(written)}, {"writeAmplification", double(written) / payload.size()}});
        seed.stop(); download.stop();
    }
    std::cout << QJsonDocument(QJsonObject{{"payloadBytes", payload.size()}, {"runs", runs}}).toJson().constData();
    return 0;
}
