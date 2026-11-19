#include "net/torrent_engine.h"
#include <QJsonArray>

#include "net/p2p_transport.h"

// librats' EventBus exposes a method named emit(), and its BitTorrent headers use
// `emit`, `slots` and `signals` as ordinary identifiers — all of which collide
// with Qt's keyword macros. Neutralise the macros across the librats includes and
// restore them afterwards.
#pragma push_macro("emit")
#pragma push_macro("slots")
#pragma push_macro("signals")
#undef emit
#undef slots
#undef signals
#ifdef RATS_SEARCH_FEATURES
#include "librats/bittorrent/client.h"
#include "librats/bittorrent/torrent.h"
#include "librats/bittorrent/torrent_creator.h"
#include "librats/bittorrent/torrent_info.h"
#include "librats/bittorrent/types.h"
#include "librats/subsystems/bittorrent.h"
#endif
#pragma pop_macro("signals")
#pragma pop_macro("slots")
#pragma pop_macro("emit")

#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <optional>
#include <system_error>

namespace rats::net {

#ifdef RATS_SEARCH_FEATURES
namespace {
namespace bt = librats::bittorrent;

// Parse a 40-char hex info hash into librats' 20-byte form (nullopt if malformed).
std::optional<bt::InfoHash> toInfoHash(const QString& hex)
{
    return bt::info_hash_from_hex(hex.toStdString());
}

// Adapt our (int, int) creation-progress callback to librats' piece-hash
// callback, so the UI progress bar is driven while create_from_path runs.
bt::PieceHashProgress toPieceHashProgress(const TorrentEngine::CreationProgressCallback& cb)
{
    if (!cb) {
        return {};
    }
    return [cb](std::uint32_t done, std::uint32_t total) { cb(static_cast<int>(done), static_cast<int>(total)); };
}

// Write the .torrent bytes a creator already produced (from create_from_path) to
// disk. No hashing happens here — the pieces are reused.
bool writeTorrentFile(const bt::TorrentCreator& creator, const QString& outputFile)
{
    const auto& bytes = creator.torrent_file();
    QFile out(outputFile);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qWarning() << "TorrentEngine: Failed to open output file:" << outputFile;
        return false;
    }
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<qint64>(bytes.size()));
    out.close();
    return true;
}

// Wrap a raw info-dict (bytes preserved verbatim so the info hash stays valid)
// with announce/announce-list keys, producing a complete .torrent byte stream.
// Re-encoding via a bencode value would re-sort the info dict and change its bytes;
// splicing at the byte level keeps them exact. Keys are emitted in bencode-sorted
// order (announce < announce-list < info).
QByteArray buildTorrentBytes(const QByteArray& infoDict, const QStringList& trackers)
{
    QByteArray out;
    out.reserve(infoDict.size() + 256);
    out.append('d');

    if (!trackers.isEmpty()) {
        const QByteArray url = trackers.first().toUtf8();
        out.append("8:announce");
        out.append(QByteArray::number(url.size()));
        out.append(':');
        out.append(url);
    }
    if (trackers.size() > 1) {
        out.append("13:announce-list");
        out.append('l');
        for (const QString& t : trackers) {
            const QByteArray url = t.toUtf8();
            out.append('l');
            out.append(QByteArray::number(url.size()));
            out.append(':');
            out.append(url);
            out.append('e');
        }
        out.append('e');
    }

    out.append("4:info");
    out.append(infoDict);
    out.append('e');
    return out;
}

// Turn a librats TorrentInfo (with metadata) into a complete .torrent byte stream.
QByteArray assembleTorrentFile(const bt::TorrentInfo& info)
{
    const auto& raw = info.info_dict_bytes();
    const QByteArray infoDict(reinterpret_cast<const char*>(raw.data()), static_cast<int>(raw.size()));
    QStringList trackers;
    for (const std::string& url : info.all_trackers()) {
        trackers.append(QString::fromStdString(url));
    }
    return buildTorrentBytes(infoDict, trackers);
}
} // namespace
#endif

TorrentEngine::TorrentEngine(P2PTransport* transport) : transport_(transport) { }

librats::bittorrent::Client* TorrentEngine::client() const
{
#ifdef RATS_SEARCH_FEATURES
    if (!transport_) {
        return nullptr;
    }
    librats::Bittorrent* bt = transport_->bittorrent();
    return bt ? bt->client() : nullptr;
#else
    return nullptr;
#endif
}

bool TorrentEngine::isReady() const
{
#ifdef RATS_SEARCH_FEATURES
    if (!transport_) {
        return false;
    }
    return transport_->isBitTorrentEnabled() && client() != nullptr;
#else
    return false;
#endif
}

// ============================================================================
// Adding torrents
// ============================================================================

bool TorrentEngine::addMagnet(const QString& hash, const QString& savePath)
{
#ifdef RATS_SEARCH_FEATURES
    auto* c = client();
    if (!c) {
        return false;
    }
    // The torrent starts metadata-less and auto-starts; metadata is fetched
    // asynchronously via BEP 9 and surfaced by the caller's status poll.
    auto* download = c->add_magnet("magnet:?xt=urn:btih:" + hash.toStdString(), savePath.toStdString());
    return download != nullptr;
#else
    Q_UNUSED(hash);
    Q_UNUSED(savePath);
    return false;
#endif
}

OperationResult TorrentEngine::createDownload(const QString& source, const QString& savePath, CreateOptions options)
{
    OperationResult result;
#ifdef RATS_SEARCH_FEATURES
    auto* c = client();
    if (!c) { result.error.code = QStringLiteral("notReady"); return result; }
    bt::CreateOptions native;
    native.mode = options.mode == StartMode::Inactive ? bt::StartMode::Inactive
        : options.mode == StartMode::MetadataOnly ? bt::StartMode::MetadataOnly : bt::StartMode::Automatic;
    native.load_resume = options.loadResume;
    native.verify_existing = options.verifyExisting;
    bt::CreateResult created;
    if (options.torrentFile) {
        const auto info = bt::TorrentInfo::from_file(source.toStdString());
        if (!info) { result.error.code = QStringLiteral("invalidMetadata"); return result; }
        created = c->create_torrent(*info, savePath.toStdString(), native);
    } else {
        const auto hash = parseInfoHash(source);
        if (hash.isEmpty()) { result.error.code = QStringLiteral("invalidHash"); return result; }
        created = c->create_magnet("magnet:?xt=urn:btih:" + hash.toStdString(), savePath.toStdString(), native);
    }
    result.hash = QString::fromStdString(bt::to_hex(created.hash));
    result.outcome = created.applied ? OperationResult::Outcome::Applied : OperationResult::Outcome::Rejected;
    result.error.code = QString::fromStdString(created.error_code);
#else
    Q_UNUSED(source); Q_UNUSED(savePath); Q_UNUSED(options);
    result.error.code = QStringLiteral("notSupported");
#endif
    return result;
}

bool TorrentEngine::startMetadata(const QString& hash)
{
#ifdef RATS_SEARCH_FEATURES
    const auto ih = toInfoHash(hash);
    return client() && ih && client()->start_metadata(*ih);
#else
    Q_UNUSED(hash); return false;
#endif
}

bool TorrentEngine::startContent(const QString& hash)
{
#ifdef RATS_SEARCH_FEATURES
    const auto ih = toInfoHash(hash);
    return client() && ih && client()->start_content(*ih);
#else
    Q_UNUSED(hash); return false;
#endif
}

bool TorrentEngine::setGlobalPolicy(qint64 down, qint64 up, int peers, int taskPeers, int checking)
{
#ifdef RATS_SEARCH_FEATURES
    auto* c=client(); if(!c) return false;
    c->set_global_policy({std::uint64_t(down),std::uint64_t(up)},peers,taskPeers,checking); return true;
#else
    Q_UNUSED(down); Q_UNUSED(up); Q_UNUSED(peers); Q_UNUSED(taskPeers); Q_UNUSED(checking); return false;
#endif
}
bool TorrentEngine::setTaskPolicy(const QString& hash,qint64 down,qint64 up,int peers)
{
#ifdef RATS_SEARCH_FEATURES
    auto ih=toInfoHash(hash); return client() && ih && client()->set_task_policy(*ih,{std::uint64_t(down),std::uint64_t(up)},peers);
#else
    Q_UNUSED(hash); Q_UNUSED(down); Q_UNUSED(up); Q_UNUSED(peers); return false;
#endif
}
bool TorrentEngine::allowContent(const QString& hash,bool allowed)
{
#ifdef RATS_SEARCH_FEATURES
    auto ih=toInfoHash(hash); return client() && ih && client()->allow_content(*ih,allowed);
#else
    Q_UNUSED(hash); Q_UNUSED(allowed); return false;
#endif
}

bool TorrentEngine::addMagnetResumed(const QString& hash, const QString& savePath)
{
#ifdef RATS_SEARCH_FEATURES
    auto* c = client();
    if (!c) {
        return false;
    }
    // Loads any resume file saved next to the download: restores downloaded pieces
    // and, if the resume file embeds the info dict, the metadata without a re-fetch.
    auto* download = c->add_magnet_resumed("magnet:?xt=urn:btih:" + hash.toStdString(), savePath.toStdString());
    return download != nullptr;
#else
    Q_UNUSED(hash);
    Q_UNUSED(savePath);
    return false;
#endif
}

TorrentMetadata TorrentEngine::readTorrentFile(const QString& torrentFile) const
{
    TorrentMetadata meta;
#ifdef RATS_SEARCH_FEATURES
    // Pure parse — thread-safe and free of any reactor-owned Torrent object.
    auto info = bt::TorrentInfo::from_file(torrentFile.toStdString());
    if (!info || !info->is_valid()) {
        return meta;
    }

    meta.valid = true;
    meta.hash = QString::fromStdString(info->info_hash_hex()).toLower();
    meta.name = QString::fromStdString(info->name());
    meta.totalSize = static_cast<qint64>(info->total_size());
    for (size_t i = 0; i < info->files().files().size(); ++i) {
        const auto& file = info->files().files()[i];
        EngineFile f;
        f.path = QString::fromStdString(file.path);
        f.size = static_cast<qint64>(file.size);
        meta.files.append(f);
    }
#else
    Q_UNUSED(torrentFile);
#endif
    return meta;
}

bool TorrentEngine::addTorrentFile(const QString& torrentFile, const QString& savePath)
{
#ifdef RATS_SEARCH_FEATURES
    auto* c = client();
    if (!c) {
        return false;
    }
    auto info = bt::TorrentInfo::from_file(torrentFile.toStdString());
    if (!info || !info->is_valid()) {
        qWarning() << "TorrentEngine: Failed to parse torrent file:" << torrentFile;
        return false;
    }
    return c->add_torrent(*info, savePath.toStdString());
#else
    Q_UNUSED(torrentFile);
    Q_UNUSED(savePath);
    return false;
#endif
}

// ============================================================================
// Lifecycle
// ============================================================================

void TorrentEngine::pause(const QString& hash)
{
#ifdef RATS_SEARCH_FEATURES
    auto* c = client();
    auto ih = toInfoHash(hash);
    if (c && ih) {
        c->pause_torrent(*ih);
    }
#else
    Q_UNUSED(hash);
#endif
}

void TorrentEngine::resume(const QString& hash)
{
#ifdef RATS_SEARCH_FEATURES
    auto* c = client();
    auto ih = toInfoHash(hash);
    if (c && ih) {
        c->resume_torrent(*ih);
    }
#else
    Q_UNUSED(hash);
#endif
}

void TorrentEngine::remove(const QString& hash)
{
#ifdef RATS_SEARCH_FEATURES
    auto* c = client();
    auto ih = toInfoHash(hash);
    if (c && ih) {
        c->remove_torrent(*ih);
    }
#else
    Q_UNUSED(hash);
#endif
}

void TorrentEngine::saveResumeData(const QString& hash)
{
#ifdef RATS_SEARCH_FEATURES
    auto* c = client();
    auto ih = toInfoHash(hash);
    if (c && ih) {
        c->save_resume_data(*ih);
    }
#else
    Q_UNUSED(hash);
#endif
}

TorrentSnapshot TorrentEngine::status(const QString& hash) const
{
    TorrentSnapshot snap;
#ifdef RATS_SEARCH_FEATURES
    auto* c = client();
    auto ih = toInfoHash(hash);
    if (!c || !ih) {
        return snap;
    }

    // Thread-safe snapshot taken from the reactor thread.
    bt::TorrentStatus st = c->torrent_status(*ih);
    snap.exists = st.exists;
    snap.hasMetadata = st.has_metadata;
    snap.isComplete = st.is_complete;
    snap.paused = st.paused;
    snap.contentAllowed = st.content_allowed;
    snap.verifiedBytes = static_cast<qint64>(st.verified);
    snap.queuedUploadBytes = static_cast<qint64>(st.uploaded);
    snap.hashFailures = static_cast<qint64>(st.hash_failures);
    snap.wantedBytes=st.wanted_bytes;snap.wantedVerifiedBytes=st.wanted_verified;
    snap.selectionRevision=st.selection_revision;snap.selectedComplete=st.selected_complete;snap.fullTorrentComplete=st.full_complete;
    switch (st.state) {
    case bt::Torrent::State::Metadata: snap.phase = DownloadPhase::Metadata; break;
    case bt::Torrent::State::Checking: snap.phase = DownloadPhase::Checking; break;
    case bt::Torrent::State::Downloading: snap.phase = DownloadPhase::Downloading; break;
    case bt::Torrent::State::Seeding: snap.phase = DownloadPhase::Seeding; break;
    case bt::Torrent::State::PartialSeeding: snap.phase = DownloadPhase::PartialSeeding; break;
    case bt::Torrent::State::Waiting: snap.phase = DownloadPhase::Waiting; break;
    case bt::Torrent::State::Error: snap.phase = DownloadPhase::Error; break;
    default: break;
    }
    snap.error = {QString::fromStdString(st.error.code),
        st.error ? QString::fromStdString(std::error_code(st.error.system_code, std::generic_category()).message()) : QString(),
        QString::fromStdString(st.error.operation), QString::fromStdString(st.error.path), st.error.system_code, st.error.retryable};
    snap.name = QString::fromStdString(st.name);
    snap.totalSize = static_cast<qint64>(st.total_size);
    snap.downloaded = static_cast<qint64>(st.downloaded);
    snap.progress = st.progress;
    snap.numPeers = static_cast<int>(st.num_peers);
    for (size_t i = 0; i < st.files.size(); ++i) {
        EngineFile f;
        f.path = QString::fromStdString(st.files[i].path);
        f.verifiedBytes = static_cast<qint64>(st.files[i].verified);
        f.priority=st.files[i].priority;
        f.size = static_cast<qint64>(st.files[i].size);
        snap.files.append(f);
    }
#else
    Q_UNUSED(hash);
#endif
    return snap;
}

// ============================================================================
// Creation / seeding
// ============================================================================

SeedResult TorrentEngine::createAndSeed(const QString& path, const QStringList& trackers, const QString& comment,
    const QString& saveTorrentFilePath, const CreationProgressCallback& progress)
{
    SeedResult result;
#ifdef RATS_SEARCH_FEATURES
    auto* c = client();
    if (!c) {
        qWarning() << "TorrentEngine: BitTorrent client not available";
        return result;
    }

    // Build the torrent metadata (this hashes all file content).
    bt::TorrentCreator creator;
    for (const QString& tracker : trackers) {
        creator.add_tracker(tracker.toStdString());
    }
    if (!comment.isEmpty()) {
        creator.set_comment(comment.toStdString());
    }

    std::string createError;
    auto infoOpt = creator.create_from_path(path.toStdString(), &createError, toPieceHashProgress(progress));
    if (!infoOpt) {
        qWarning() << "TorrentEngine: Failed to create torrent from:" << path << QString::fromStdString(createError);
        return result;
    }
    const bt::TorrentInfo& info = *infoOpt;

    // The content lives in the parent directory of the source path (the path's own
    // name becomes the single file / top-level directory entry).
    const QString seedDir = QFileInfo(path).absolutePath();
    auto* download = c->add_torrent_for_seeding(info, seedDir.toStdString());
    if (!download) {
        qWarning() << "TorrentEngine: Failed to start seeding torrent from:" << path;
        return result;
    }

    // Reuse the just-hashed pieces to write the .torrent file (avoids re-hashing).
    if (!saveTorrentFilePath.isEmpty()) {
        writeTorrentFile(creator, saveTorrentFilePath);
    }

    result.ok = true;
    result.hash = QString::fromStdString(info.info_hash_hex()).toLower();
    result.name = QString::fromStdString(info.name());
    result.totalSize = static_cast<qint64>(info.total_size());
    result.savePath = seedDir;
    const auto& files = info.files().files();
    for (size_t i = 0; i < files.size(); ++i) {
        EngineFile f;
        f.path = QString::fromStdString(files[i].path);
        f.size = static_cast<qint64>(files[i].size);
        result.files.append(f);
    }
#else
    Q_UNUSED(path);
    Q_UNUSED(trackers);
    Q_UNUSED(comment);
    Q_UNUSED(saveTorrentFilePath);
    Q_UNUSED(progress);
    qWarning() << "TorrentEngine: RATS_SEARCH_FEATURES not enabled";
#endif
    return result;
}

bool TorrentEngine::createTorrentFile(const QString& path, const QString& outputFile, const QStringList& trackers,
    const QString& comment, const CreationProgressCallback& progress)
{
#ifdef RATS_SEARCH_FEATURES
    bt::TorrentCreator creator;
    for (const QString& tracker : trackers) {
        creator.add_tracker(tracker.toStdString());
    }
    if (!comment.isEmpty()) {
        creator.set_comment(comment.toStdString());
    }

    std::string createError;
    auto infoOpt = creator.create_from_path(path.toStdString(), &createError, toPieceHashProgress(progress));
    if (!infoOpt) {
        qWarning() << "TorrentEngine: create_from_path error:" << QString::fromStdString(createError);
        return false;
    }

    return writeTorrentFile(creator, outputFile);
#else
    Q_UNUSED(path);
    Q_UNUSED(outputFile);
    Q_UNUSED(trackers);
    Q_UNUSED(comment);
    Q_UNUSED(progress);
    qWarning() << "TorrentEngine: RATS_SEARCH_FEATURES not enabled";
    return false;
#endif
}

bool TorrentEngine::fetchTorrentFile(const QString& hash, TorrentFileCallback callback, int timeoutMs)
{
#ifdef RATS_SEARCH_FEATURES
    librats::Bittorrent* subsystem = transport_ ? transport_->bittorrent() : nullptr;
    if (!transport_ || !transport_->isBitTorrentEnabled() || !subsystem || !subsystem->is_running()) {
        return false;
    }

    // Fast path: if librats already has this torrent loaded WITH metadata (an active
    // download or a seed), assemble the .torrent from the in-memory info dict right
    // away — no DHT/BEP 9 round-trip. torrent_metadata() marshals onto the reactor,
    // so this is thread-safe and returns nullopt when the torrent isn't loaded yet.
    if (const auto ih = toInfoHash(hash)) {
        if (bt::Client* c = client()) {
            if (const auto meta = c->torrent_metadata(*ih)) {
                callback(assembleTorrentFile(*meta), QString::fromStdString(meta->name()), QString());
                return true;
            }
        }
    }

    // Slow path: fetch metadata over the DHT / BEP 9 from the swarm.
    subsystem->get_torrent_metadata(
        hash.toStdString(),
        [callback = std::move(callback)](const bt::TorrentInfo& info, bool success, const std::string& error) {
            if (!success || !info.is_valid() || !info.has_metadata()) {
                callback(QByteArray(), QString(),
                    error.empty() ? QStringLiteral("metadata download failed") : QString::fromStdString(error));
                return;
            }
            callback(assembleTorrentFile(info), QString::fromStdString(info.name()), QString());
        },
        timeoutMs);
    return true;
#else
    Q_UNUSED(hash);
    Q_UNUSED(callback);
    Q_UNUSED(timeoutMs);
    return false;
#endif
}

QString TorrentEngine::parseInfoHash(const QString& magnetLink)
{
    // If it's already a hash (40 hex chars), return it.
    static const QRegularExpression hashRegex("^[0-9a-fA-F]{40}$");
    if (hashRegex.match(magnetLink).hasMatch()) {
        return magnetLink.toLower();
    }

    // Parse magnet link: magnet:?xt=urn:btih:HASH...
    static const QRegularExpression magnetRegex(
        "(?:magnet:\\?.*?)?(?:xt=urn:btih:)?([0-9a-fA-F]{40})", QRegularExpression::CaseInsensitiveOption);
    auto match = magnetRegex.match(magnetLink);
    if (match.hasMatch()) {
        return match.captured(1).toLower();
    }

    // Base32-encoded hashes (32 chars) are recognised only so the caller gets a
    // diagnostic instead of a silent "no hash"; decoding them is not implemented.
    static const QRegularExpression base32Regex(
        "(?:magnet:\\?.*?)?(?:xt=urn:btih:)?([A-Z2-7]{32})", QRegularExpression::CaseInsensitiveOption);
    if (base32Regex.match(magnetLink).hasMatch()) {
        qWarning() << "TorrentEngine: base32-encoded info-hashes are not supported";
    }

    return QString();
}

void TorrentEngine::setFilePriorities(const QString& hash,const QVector<int>& priorities,qint64 expected,FileOperationCallback done) {
#ifdef RATS_SEARCH_FEATURES
    auto ih=toInfoHash(hash);auto* c=client();
    if(c && ih) {
        c->set_file_priorities(*ih,std::vector<int>(priorities.begin(),priorities.end()),expected,
            [hash,done=std::move(done)](bt::DiskError error,std::uint64_t revision) mutable {
                OperationResult r;r.hash=hash;r.revision=revision;r.outcome=error?OperationResult::Outcome::Rejected:OperationResult::Outcome::Applied;
                if(error) r.error={QString::fromStdString(error.code),QString::fromStdString(error.code),QString::fromStdString(error.operation),QString::fromStdString(error.path),error.system_code,error.retryable};
                done(r);
            });return;
    }
#else
    Q_UNUSED(priorities);Q_UNUSED(expected);
#endif
    OperationResult r;r.hash=hash;r.error={"engineUnavailable","Download engine is unavailable"};done(r);
}
void TorrentEngine::recheck(const QString& hash,bool retry,FileOperationCallback done) {
#ifdef RATS_SEARCH_FEATURES
    auto ih=toInfoHash(hash);auto* c=client();
    if(c && ih) {
        c->recheck_torrent(*ih,retry,[hash,done=std::move(done)](bt::DiskError error,std::uint64_t revision) mutable {
            OperationResult r;r.hash=hash;r.revision=revision;r.outcome=error?OperationResult::Outcome::Rejected:OperationResult::Outcome::Applied;
            if(error) r.error={QString::fromStdString(error.code),QString::fromStdString(error.code),QString::fromStdString(error.operation),QString::fromStdString(error.path),error.system_code,error.retryable};done(r);
        });return;
    }
#else
    Q_UNUSED(retry);
#endif
    OperationResult r;r.hash=hash;r.error={"engineUnavailable","Download engine is unavailable"};done(r);
}
bool TorrentEngine::setAdvancedPolicy(const QString& hash,const QString& strategy,bool automatic,int manual,int minimum,int maximum) {
#ifdef RATS_SEARCH_FEATURES
    auto ih=toInfoHash(hash);auto* c=client();if(!c || !ih)return false;
    bt::DownloadStrategy mode;
    if(strategy=="rarestFirst")mode=bt::DownloadStrategy::RarestFirst;else if(strategy=="sequential")mode=bt::DownloadStrategy::Sequential;else if(strategy=="firstLast")mode=bt::DownloadStrategy::FirstLast;else return false;
    return c->set_download_strategy(*ih,mode,{automatic,manual,minimum,maximum});
#else
    Q_UNUSED(hash);Q_UNUSED(strategy);Q_UNUSED(automatic);Q_UNUSED(manual);Q_UNUSED(minimum);Q_UNUSED(maximum);return false;
#endif
}
void TorrentEngine::setCacheBudget(qint64 bytes) {
#ifdef RATS_SEARCH_FEATURES
    if(auto* c=client())c->set_cache_budget(std::size_t(bytes));
#else
    Q_UNUSED(bytes);
#endif
}
QJsonObject TorrentEngine::pieceMap(const QString& hash,int maximum) const {
    QJsonArray ranges;
#ifdef RATS_SEARCH_FEATURES
    auto ih=toInfoHash(hash);auto* c=client();if(c && ih) {
        for(const auto& r:c->piece_ranges(*ih,std::size_t(qBound(1,maximum,2048))))ranges.append(QJsonObject{{"begin",qint64(r.begin)},{"end",qint64(r.end)},{"verified",qint64(r.verified)},{"wanted",qint64(r.wanted)},{"active",qint64(r.active)},{"high",qint64(r.high)}});
        return {{"hash",hash},{"ranges",ranges},{"cacheBytes",qint64(c->cache_bytes())}};
    }
#else
    Q_UNUSED(maximum);
#endif
    return {{"hash",hash},{"ranges",ranges}};
}
} // namespace rats::net
