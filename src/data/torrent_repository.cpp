#include "data/torrent_repository.h"

#include "data/database.h"
#include "data/query.h"
#include "data/row_id.h"
#include "common/infohash.h"

#include <QDateTime>
#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutexLocker>
#include <QThread>
#include <QUuid>
#include <algorithm>
#include <limits>

namespace rats::data {

using domain::ContentCategory;
using domain::ContentType;
using domain::File;
using domain::SearchHit;
using domain::Torrent;

namespace {
const QString kTorrents = QStringLiteral("torrents");
const QString kFiles = QStringLiteral("files");
constexpr int kInfoNameMaxLength = 800;

// Use the same full-text expression for each original path. Highlighting the
// aggregate files document lets a phrase or AND clause span multiple paths.
bool highlightMatchingPaths(Database& db, const QString& pathBlob, const FullTextQuery& query,
    const QString& expression, QStringList& matches, const FullTextQuery::FuzzyTerm* fuzzy = nullptr,
    const QDeadlineTimer* deadline = nullptr, const TorrentRepository::SearchQuery* request = nullptr)
{
    const auto expired = [&] {
        return (deadline && deadline->hasExpired()) || (request && request->cancelled());
    };
    const auto paths = pathBlob.split(u'\n', Qt::SkipEmptyParts);
    if (paths.isEmpty())
        return true;
    const QString marker = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString before = marker + u'[';
    const QString after = u']' + marker;
    const QString suffix = QStringLiteral("), 'files', %1, 0 AS limit, 1 AS force_all_words, 1 AS allow_empty, "
                                          "%2 AS before_match, %3 AS after_match)")
        .arg(sql::quote(expression), sql::quote(before), sql::quote(after));
    QStringList batch;
    QSet<QString> seen;
    qsizetype bytes = 0;
    const auto flush = [&] {
        if (batch.isEmpty())
            return true;
        if (expired())
            return false;
        bool ok = false;
        const auto rows = db.query(QStringLiteral("CALL SNIPPETS((") + batch.join(u',') + suffix, {}, &ok);
        if (!ok || rows.size() != batch.size())
            return false;
        for (const auto& row : rows) {
            QString snippet = row.value(QStringLiteral("snippet")).toString();
            if (snippet.contains(before) && snippet.contains(after))
                matches.append(snippet.replace(before, QStringLiteral("<b>")).replace(after, QStringLiteral("</b>")));
        }
        batch.clear();
        bytes = 0;
        return true;
    };

    for (const QString& path : paths) {
        if (expired())
            return false;
        if (seen.contains(path))
            continue;
        if (std::any_of(query.continuousTerms().cbegin(), query.continuousTerms().cend(),
                [&path](const QString& term) { return !path.contains(term); }))
            continue;
        if (fuzzy && !fuzzy->matches(path))
            continue;
        seen.insert(path);
        const QString quoted = sql::quote(path);
        const auto pathBytes = quoted.toUtf8().size() + 1;
        if ((batch.size() >= 128 || bytes + pathBytes > Database::MaxStatementBytes / 2) && !flush())
            return false;
        batch.append(quoted);
        bytes += pathBytes;
    }
    return flush();
}

struct RankedHit {
    SearchHit hit;
    QVariant sortKey;
};

bool betterHit(const RankedHit& a, const RankedHit& b, const QString& sortColumn, bool descending)
{
    if (sortColumn == QLatin1String("name")) {
        const int order = QString::compare(a.sortKey.toString(), b.sortKey.toString(), Qt::CaseInsensitive);
        if (order != 0)
            return descending ? order > 0 : order < 0;
    } else if (!sortColumn.isEmpty() && a.sortKey.toLongLong() != b.sortKey.toLongLong()) {
        return descending ? a.sortKey.toLongLong() > b.sortKey.toLongLong()
                          : a.sortKey.toLongLong() < b.sortKey.toLongLong();
    }
    return a.hit.torrent.id < b.hit.torrent.id;
}

// Split the "\n"-joined path/size blobs stored in the files table back into a
// file list.
QVector<File> parseFileBlob(const QString& pathBlob, const QString& sizeBlob)
{
    const QStringList paths = pathBlob.split(QLatin1Char('\n'));
    const QStringList sizes = sizeBlob.split(QLatin1Char('\n'));
    QVector<File> files;
    files.reserve(paths.size());
    for (int i = 0; i < paths.size(); ++i)
        files.append(File { paths.at(i), i < sizes.size() ? sizes.at(i).toLongLong() : 0 });
    return files;
}

// Hashes reach us from the DHT, from peers and from dumps, and not all of those
// agree on case. Row ids already ignore it (rowIdFromHash parses hex either
// way), so the verification that guards against id collisions must too.
bool sameHash(const QString& a, const QString& b)
{
    return !a.isEmpty() && a.compare(b, Qt::CaseInsensitive) == 0;
}

// Whether a row that came back under some id belongs to one of the hashes that
// asked for it. Distinct hashes can share an id, so this is a search over the
// requesters rather than a comparison against a single one.
bool wantedMatches(const QStringList& wanted, const QString& hash)
{
    for (const QString& w : wanted) {
        if (sameHash(w, hash))
            return true;
    }
    return false;
}

// Resolve a batch of hashes to row ids, keeping the reverse map so each returned
// row can be checked against the hashes that asked for it.
//
// The map holds a list per id, not a single hash: two different torrents whose
// hashes agree on their first 63 bits ask for the same row, and keeping only the
// first left the other one invisible — neither returned as found nor reported as
// collided, which let a batch write REPLACE a stranger's row.
QVector<qint64> idsFor(const QStringList& hashes, QHash<qint64, QStringList>& wantedById)
{
    QVector<qint64> ids;
    ids.reserve(hashes.size());
    for (const QString& hash : hashes) {
        const qint64 id = rowIdFromHash(hash);
        if (id == 0)
            continue;
        QStringList& wanted = wantedById[id];
        // Ask the database for the id once, however many hashes want it.
        if (wanted.isEmpty())
            ids.append(id);
        // Two spellings of one hash are one request; two hashes that merely
        // collide are two, and both need an answer.
        if (!wantedMatches(wanted, hash))
            wanted.append(hash);
    }
    return ids;
}

// Size / file-count range filters, shared by the torrent search and the parent
// lookup of the file search so both lanes answer one request the same way.
//
// Bounds are inclusive: "up to 1 GB" has to keep a torrent of exactly 1 GB, and
// a "from 1 file" filter must not drop every single-file torrent. 0 means the
// bound is unset — a torrent of size 0 is nothing anyone filters *for*.
void applyRangeFilters(SelectQuery& builder, const TorrentRepository::SearchQuery& q)
{
    if (q.sizeMin > 0)
        builder.whereRaw(QStringLiteral("size >= %1").arg(q.sizeMin));
    if (q.sizeMax > 0)
        builder.whereRaw(QStringLiteral("size <= %1").arg(q.sizeMax));
    if (q.filesMin > 0)
        builder.whereRaw(QStringLiteral("files >= %1").arg(q.filesMin));
    if (q.filesMax > 0)
        builder.whereRaw(QStringLiteral("files <= %1").arg(q.filesMax));
}
} // namespace

TorrentRepository::TorrentRepository(Database* db, QObject* parent) : QObject(parent), db_(db) { }

void TorrentRepository::closeCurrentThreadConnection() { db_->closeCurrentThreadConnection(); }

TorrentRepository::WriteGuard::WriteGuard(TorrentRepository* repository)
    : repository_(repository), lock_(repository->writeMutex_, std::defer_lock)
{
    QElapsedTimer wait;
    const auto* app = QCoreApplication::instance();
    if (app && QThread::currentThread() == app->thread())
        wait.start();
    lock_.lock();
    if (wait.isValid() && wait.elapsed() >= 250)
        qWarning() << "[MainThread] index write lock waited" << wait.elapsed() << "ms";
    ++repository_->writeDepth_;
}

TorrentRepository::WriteGuard::~WriteGuard() { unlock(); }

void TorrentRepository::WriteGuard::unlock()
{
    if (!lock_.owns_lock()) return;
    QVector<std::function<void()>> pending;
    if (--repository_->writeDepth_ == 0) pending.swap(repository_->notifications_);
    // Posting under the lock preserves commit order; receivers execute after unlock.
    for (auto& notification : pending)
        QMetaObject::invokeMethod(repository_, std::move(notification), Qt::QueuedConnection);
    lock_.unlock();
}

void TorrentRepository::notify(std::function<void()> notification)
{
    notifications_.append(std::move(notification));
}

TorrentRepository::Statistics TorrentRepository::statistics() const
{
    QMutexLocker lock(&statsMutex_);
    auto copy = stats_;
    copy.exact = !statisticsDirty_.load();
    return copy;
}

bool TorrentRepository::reconcileStatistics()
{
    WriteGuard write(this);
    if (statisticsDirty_) primeFromDatabase();
    return !statisticsDirty_;
}

bool TorrentRepository::takeConfirmedInsert(const QString& hash)
{
    WriteGuard write(this);
    if (!unconfirmedInserts_.contains(hash)) return false;
    const auto slot = resolve(hash);
    return !slot.failed && slot.ours && unconfirmedInserts_.remove(hash);
}

void TorrentRepository::bumpStatistics(qint64 torrents, qint64 files, qint64 totalSize)
{
    if (statisticsDirty_) {
        primeFromDatabase();
        return; // recount includes this write; applying its delta would double count
    }
    Statistics now;
    {
        QMutexLocker lock(&statsMutex_);
        // Clamped, not asserted: a delta is derived from what a write reported, and
        // a row that vanished underneath us would otherwise drive a counter
        // negative and keep it there for the life of the process.
        stats_.torrents = qMax(0LL, stats_.torrents + torrents);
        stats_.files = qMax(0LL, stats_.files + files);
        stats_.totalSize = qMax(0LL, stats_.totalSize + totalSize);
        now = stats_;
    }
    notify([this, now] { emit statisticsChanged(now.torrents, now.files, now.totalSize); });
}

void TorrentRepository::primeFromDatabase()
{
    WriteGuard write(this);
    bool ok = false;
    const auto rows = db_->query(QStringLiteral("SELECT COUNT(*) AS cnt, SUM(files) AS numfiles, "
                                                "SUM(size) AS totalsize FROM torrents"), {}, &ok);
    statisticsDirty_ = !ok;
    if (!ok) return;
    if (!rows.isEmpty()) {
        QMutexLocker lock(&statsMutex_);
        stats_.torrents = rows.first().value(QStringLiteral("cnt")).toLongLong();
        stats_.files = rows.first().value(QStringLiteral("numfiles")).toLongLong();
        stats_.totalSize = rows.first().value(QStringLiteral("totalsize")).toLongLong();
    }
    const Statistics primed = statistics();
    qInfo() << "[TorrentRepository] primed:" << primed.torrents << "torrents," << primed.files << "files";
    notify([this, primed] { emit statisticsChanged(primed.torrents, primed.files, primed.totalSize); });
}

// ---------------------------------------------------------------------------
// CRUD
// ---------------------------------------------------------------------------

TorrentRepository::RowSlot TorrentRepository::resolve(const QString& hash, bool withRow)
{
    RowSlot slot;
    slot.id = rowIdFromHash(hash);
    if (slot.id == 0)
        return slot;

    // One docid lookup answers both questions a write has: is this torrent
    // already stored, and is its slot claimed by a different one? `withRow`
    // widens it to the whole row for callers that would otherwise follow up with
    // a get() on the same id — the crawler's insert path asks every torrent this
    // question, so the columns are only paid for when someone needs them.
    const QString columns = withRow ? QStringLiteral("*") : QStringLiteral("hash");
    bool ok = false;
    const auto rows
        = db_->query(QStringLiteral("SELECT %1 FROM torrents WHERE id = ? LIMIT 1").arg(columns), { slot.id }, &ok);
    slot.failed = !ok;
    if (rows.isEmpty())
        return slot;

    slot.taken = true;
    if (!withRow) {
        slot.ours = sameHash(rows.first().value(QStringLiteral("hash")).toString(), hash);
        return slot;
    }

    Torrent stored = rowToTorrent(rows.first());
    slot.ours = sameHash(stored.hash, hash);
    if (slot.ours)
        slot.stored = std::move(stored);
    return slot;
}

QVariantMap TorrentRepository::torrentRow(const Torrent& t)
{
    QVariantMap values;
    values["id"] = static_cast<qlonglong>(rowIdFromHash(t.hash));
    values["hash"] = t.hash;
    values["name"] = t.name;
    values["nameIndex"] = buildNameIndex(t);
    values["size"] = t.size;
    values["files"] = t.files;
    values["piecelength"] = t.pieceLength;
    values["added"] = t.added.isValid() ? t.added.toSecsSinceEpoch() : QDateTime::currentSecsSinceEpoch();
    values["ipv4"] = t.ipv4; // formatValue turns an empty/null string into ''
    values["port"] = t.port;
    values["contentType"] = domain::toId(t.contentType);
    values["contentCategory"] = domain::toId(t.contentCategory);
    values["seeders"] = t.seeders;
    values["leechers"] = t.leechers;
    values["completed"] = t.completed;
    values["trackersChecked"] = t.trackersChecked.isValid() ? t.trackersChecked.toSecsSinceEpoch() : 0;
    values["good"] = t.good;
    values["bad"] = t.bad;
    // A multi-row write takes its column list from the first row, so every row
    // must carry the same keys — "info" is always present, empty object or not.
    values["info"] = t.info;
    return values;
}

QVariantMap TorrentRepository::filesRow(const QString& hash, const QVector<File>& files)
{
    QStringList paths;
    QStringList sizes;
    paths.reserve(files.size());
    sizes.reserve(files.size());
    for (const File& f : files) {
        paths << f.path;
        sizes << QString::number(f.size);
    }

    QVariantMap values;
    // Deliberately the same id as the torrent's row: it turns the export's
    // "files of these 500 torrents" join into the same docid lookup, and makes
    // replacing a file list a single REPLACE instead of a delete + insert that
    // would hand the row a fresh id.
    values["id"] = static_cast<qlonglong>(rowIdFromHash(hash));
    values["hash"] = hash;
    values["path"] = paths.join(QLatin1Char('\n'));
    values["size"] = sizes.join(QLatin1Char('\n'));
    return values;
}

bool TorrentRepository::add(const Torrent& t)
{
    WriteGuard write(this);
    if (!t.isValid())
        return false;
    return add(t, resolve(t.hash));
}

bool TorrentRepository::add(const Torrent& t, const RowSlot& suppliedSlot)
{
    WriteGuard write(this);
    if (!t.isValid())
        return false;

    // A slot resolved for a different hash would send the write to a stranger's
    // row, so it is refused rather than re-resolved: a caller passing the wrong
    // one has a bug, and silently papering over it would hide it.
    if (suppliedSlot.id != rowIdFromHash(t.hash)) {
        qWarning() << "[TorrentRepository] slot does not belong to" << t.hash << "- refusing to store";
        return false;
    }
    const RowSlot slot = resolve(t.hash);
    if (slot.failed || slot.id == 0)
        return false;
    if (slot.collided()) {
        qWarning() << "[TorrentRepository] row id collision, refusing to store" << t.hash;
        return false;
    }
    if (slot.taken) {
        qInfo() << "[TorrentRepository] already present:" << t.hash;
        return true;
    }

    QVariantMap values = torrentRow(t);
    if (t.info.isEmpty())
        values.remove("info");

    // REPLACE rather than INSERT: the id is derived, and Manticore rejects an
    // INSERT onto an id that already exists. The slot check above proved it is
    // free, so this only guards against a concurrent writer claiming it first.
    // Store details first: a failed file write must not leave a new summary
    // looking fully indexed and suppressing subsequent metadata fetches.
    if (!t.fileList.isEmpty() && !saveFiles(t.hash, t.fileList))
        return false;
    if (!db_->replace(kTorrents, values))
        return false;

    bumpStatistics(1, t.files, t.size);
    return true;
}

bool TorrentRepository::update(const Torrent& t)
{
    WriteGuard write(this);
    if (!t.isValid())
        return false;

    // Rewrite the whole row rather than SET the changed columns. Manticore
    // refuses to UPDATE a full-text field, and refuses the *entire* statement
    // when one is named — "attribute 'nameindex' can not be updated (full-text
    // field)" — so the previous partial UPDATE, which had to name nameIndex to
    // keep it in step with a renamed torrent, never applied anything at all: vote
    // merges and rename were both silently dropped. A derived row id makes the
    // honest fix trivial, since REPLACE addresses exactly this torrent's row.
    //
    // Every caller passes a Torrent it just read back with get(), so the row
    // rebuilt here is complete rather than a partial overwrite.
    const RowSlot slot = resolve(t.hash);
    if (slot.failed || !slot.taken || slot.id == 0)
        return false;
    if (slot.collided()) {
        qWarning() << "[TorrentRepository] row id collision, refusing to update" << t.hash;
        return false;
    }

    QVariantMap values = torrentRow(t);
    if (t.info.isEmpty())
        values.remove("info");

    if (!db_->replace(kTorrents, values))
        return false;
    notify([this, hash = t.hash] { emit torrentUpdated(hash); });
    return true;
}

bool TorrentRepository::updateVotes(const QString& hash, int good, int bad, bool keepMaximum)
{
    WriteGuard write(this);
    const auto stored = get(hash);
    if (!stored) return false;
    if (keepMaximum) { good = qMax(good, stored->good); bad = qMax(bad, stored->bad); }
    if (!db_->update(kTorrents, {{"good", good}, {"bad", bad}}, {{"id", stored->id}})) return false;
    notify([this, hash] { emit torrentUpdated(hash); });
    return true;
}

std::optional<Torrent> TorrentRepository::incrementVote(const QString& hash, bool good)
{
    WriteGuard write(this);
    auto stored = get(hash);
    if (!stored) return std::nullopt;
    if (good) ++stored->good; else ++stored->bad;
    if (!updateVotes(hash, stored->good, stored->bad)) return std::nullopt;
    return stored;
}

bool TorrentRepository::remove(const QString& hash)
{
    WriteGuard write(this);
    qint64 removedSize = 0;
    int removedFiles = 0;
    const qint64 id = rowIdFromHash(hash);
    if (id == 0)
        return false;

    // Verify the slot really holds this torrent before deleting it: on a 63-bit
    // id collision the row belongs to somebody else.
    const auto rows = db_->query(QStringLiteral("SELECT hash, size, files FROM torrents WHERE id = ?"), { id });
    if (!rows.isEmpty()) {
        if (!sameHash(rows.first().value(QStringLiteral("hash")).toString(), hash))
            return false;
        removedSize = rows.first().value(QStringLiteral("size")).toLongLong();
        removedFiles = rows.first().value(QStringLiteral("files")).toInt();
    }

    if (!db_->remove(kFiles, {{"id", id}}) || !db_->remove(kTorrents, {{"id", id}}))
        return false;

    if (!rows.isEmpty()) {
        bumpStatistics(-1, -removedFiles, -removedSize);
    }
    return true;
}

bool TorrentRepository::exists(const QString& hash)
{
    const RowSlot slot = resolve(hash);
    // A collided slot is "not present": the torrent is absent, it simply cannot
    // be stored either.
    return slot.taken && slot.ours;
}

std::optional<Torrent> TorrentRepository::get(const QString& hash, bool includeFiles)
{
    const qint64 id = rowIdFromHash(hash);
    if (id == 0)
        return std::nullopt;

    const auto rows = db_->query(QStringLiteral("SELECT * FROM torrents WHERE id = ?"), { id });
    if (rows.isEmpty())
        return std::nullopt;

    Torrent t = rowToTorrent(rows.first());
    if (!sameHash(t.hash, hash))
        return std::nullopt; // id collision: this row is a different torrent
    if (includeFiles)
        t.fileList = filesOf(hash);
    return t;
}

// ---------------------------------------------------------------------------
// Search
// ---------------------------------------------------------------------------

QString TorrentRepository::resolveSortColumn(const QString& key)
{
    // Whitelist: user sort keys -> real columns. Anything else -> no ORDER BY.
    static const QHash<QString, QString> allowed = {
        { QStringLiteral("seeders"), QStringLiteral("seeders") },
        { QStringLiteral("leechers"), QStringLiteral("leechers") },
        { QStringLiteral("name"), QStringLiteral("name") },
        { QStringLiteral("size"), QStringLiteral("size") },
        { QStringLiteral("files"), QStringLiteral("files") },
        { QStringLiteral("added"), QStringLiteral("added") },
        { QStringLiteral("completed"), QStringLiteral("completed") },
    };
    return allowed.value(key.toLower());
}

QString TorrentRepository::contentTypeFilter(const QString& type)
{
    if (type.isEmpty())
        return QString();
    // "application" spans both Software and Games in the UI.
    if (type == QLatin1String("application")) {
        return QStringLiteral("contentType IN (%1, %2)")
            .arg(domain::toId(ContentType::Software))
            .arg(domain::toId(ContentType::Games));
    }
    return QStringLiteral("contentType = %1").arg(domain::toId(domain::contentTypeFromString(type)));
}

QVector<Torrent> TorrentRepository::selectTorrents(const QString& sql, const QVariantList& params)
{
    QVector<Torrent> out;
    for (const auto& row : db_->query(sql, params))
        out.append(rowToTorrent(row));
    return out;
}

QVector<SearchHit> TorrentRepository::searchTorrents(const SearchQuery& q)
{
    if (q.fuzzy && !infohash::isValid(q.text.trimmed()))
        return searchWithFuzzy(q, false);
    return searchTorrentsStrict(q);
}

void TorrentRepository::applySearchFilters(SelectQuery& builder, const SearchQuery& q)
{
    if (q.safeSearch)
        builder.whereRaw(QStringLiteral("contentCategory != %1").arg(domain::toId(ContentCategory::XXX)));
    builder.whereRaw(contentTypeFilter(q.contentType));
    applyRangeFilters(builder, q);
}

QVector<SearchHit> TorrentRepository::searchTorrentsStrict(const SearchQuery& q, bool stableWindow)
{
    QVector<SearchHit> hits;
    const QString text = q.text.trimmed();
    if (text.isEmpty() || q.cancelled())
        return hits;

    SelectQuery builder(kTorrents);

    const bool byHash = infohash::isValid(text);
    if (byHash) {
        builder.whereEq(QStringLiteral("id"), rowIdFromHash(text));
    } else {
        const FullTextQuery fullText = FullTextQuery::parse(text);
        if (fullText.error() != FullTextQuery::Error::None || fullText.isEmpty())
            return hits;
        builder.matchAgainst(fullText, true);

        // Full-text positions treat punctuation as a separator without a gap.
        // Require each unquoted continuous run in the original indexed name
        // before LIMIT so rejected rows cannot consume a page.
        QStringList predicates;
        const QString infoNameLength = sql::quote(QStringLiteral("(?s)^.{1,%1}$").arg(kInfoNameMaxLength - 1));
        for (const QString& term : fullText.continuousTerms()) {
            // The parser takes these runs from a restricted script character
            // class, so each term is also a literal in the regex engine.
            const QString literal = sql::quote(term);
            predicates.append(QStringLiteral("(REGEX(name, %1) OR (REGEX(info.name, %1) AND REGEX(info.name, %2)))")
                .arg(literal, infoNameLength));
        }
        if (!predicates.isEmpty()) {
            builder.columns(QStringLiteral("*, (%1) AS continuous_match").arg(predicates.join(QStringLiteral(" AND "))));
            builder.whereEq(QStringLiteral("continuous_match"), 1);
        }
    }

    applySearchFilters(builder, q);

    const QString sortColumn = resolveSortColumn(q.sort);
    if (!sortColumn.isEmpty())
        builder.orderBy(sortColumn, q.descending);
    if (stableWindow) {
        builder.withStableOrder();
        if (q.limit > 1000) {
            const qint64 available = qMax<qint64>(1000, db_->count(kTorrents));
            builder.maxMatches(static_cast<int>(qMin<qint64>(q.limit, available)));
        }
    }
    builder.limit(q.offset, q.limit);

    for (const auto& row : db_->query(builder.build())) {
        SearchHit hit;
        hit.torrent = rowToTorrent(row);
        // An id lookup can land on a different torrent that shares the slot;
        // a hash search must not answer with it.
        if (byHash && !sameHash(hit.torrent.hash, text))
            continue;
        hits.append(hit);
    }
    return hits;
}

QVector<SearchHit> TorrentRepository::searchFiles(const SearchQuery& q)
{
    if (q.fuzzy && !infohash::isValid(q.text.trimmed()))
        return searchWithFuzzy(q, true);

    const FullTextQuery fullText = FullTextQuery::parse(q.text);
    if (fullText.error() != FullTextQuery::Error::None || fullText.isEmpty() || q.offset < 0 || q.limit <= 0)
        return {};

    const QString expression = fullText.expression(false);
    const QString sortColumn = resolveSortColumn(q.sort);
    const QString sortKey = sortColumn.toLower();
    const qint64 needed = static_cast<qint64>(q.offset) + q.limit;
    const auto better = [&](const RankedHit& a, const RankedHit& b) {
        return betterHit(a, b, sortColumn, q.descending);
    };

    // Page through MATCH candidates by id instead of using an index offset,
    // which stops at max_matches. Explicit sorts keep only the best requested
    // window while examining all candidate batches.
    QVector<RankedHit> window;
    constexpr int batchSize = 128;
    qint64 cursor = 0;
    while (!sortColumn.isEmpty() || window.size() < needed) {
        if (q.cancelled())
            return {};
        bool ok = false;
        const auto candidates = db_->query(SelectQuery(kFiles)
                .columns(QStringLiteral("id, hash, path"))
                .matchAgainst(fullText, false)
                .whereRaw(QStringLiteral("id > %1").arg(cursor))
                .orderBy(QStringLiteral("id"), false)
                .limit(0, batchSize)
                .build(), {}, &ok);
        if (!ok)
            return {};
        if (candidates.isEmpty())
            break;
        cursor = candidates.last().value(QStringLiteral("id")).toLongLong();

        QVector<qint64> parentIds;
        for (const auto& candidate : candidates) {
            const qint64 id = candidate.value(QStringLiteral("id")).toLongLong();
            // The canonical id gives one path document per hash, even when a
            // detached legacy row or an id collision is present.
            if (id == rowIdFromHash(candidate.value(QStringLiteral("hash")).toString()))
                parentIds.append(id);
        }
        if (parentIds.isEmpty())
            continue;

        SelectQuery parents(kTorrents);
        parents.whereInIds(QStringLiteral("id"), parentIds)
            .limit(0, parentIds.size());
        applySearchFilters(parents, q);
        QHash<qint64, QVariantMap> parentsById;
        for (const auto& row : db_->query(parents.build(), {}, &ok))
            parentsById.insert(row.value(QStringLiteral("id")).toLongLong(), row);
        if (!ok)
            return {};

        for (const auto& candidate : candidates) {
            const qint64 id = candidate.value(QStringLiteral("id")).toLongLong();
            const auto parent = parentsById.constFind(id);
            if (parent == parentsById.cend()
                || !sameHash(parent->value(QStringLiteral("hash")).toString(), candidate.value(QStringLiteral("hash")).toString()))
                continue;
            RankedHit ranked;
            ranked.hit.torrent = rowToTorrent(*parent);
            ranked.hit.fromFileMatch = true;
            ranked.sortKey = parent->value(sortKey);
            if (window.size() == needed && !better(ranked, window.front()))
                continue;
            if (!highlightMatchingPaths(*db_, candidate.value(QStringLiteral("path")).toString(), fullText, expression,
                    ranked.hit.matchingPaths, nullptr, nullptr, &q))
                return {};
            if (ranked.hit.matchingPaths.isEmpty())
                continue;

            if (window.size() == needed) {
                std::pop_heap(window.begin(), window.end(), better);
                window.pop_back();
            }
            window.append(std::move(ranked));
            std::push_heap(window.begin(), window.end(), better);
            if (sortColumn.isEmpty() && window.size() == needed)
                break;
        }
    }

    std::sort_heap(window.begin(), window.end(), better);
    QVector<SearchHit> hits;
    for (qsizetype i = q.offset; i < window.size(); ++i)
        hits.append(std::move(window[i].hit));
    return hits;
}

QVector<SearchHit> TorrentRepository::searchWithFuzzy(const SearchQuery& q, bool files)
{
    if (q.offset < 0 || q.limit <= 0 || q.cancelled())
        return {};
    const FullTextQuery query = FullTextQuery::parse(q.text);
    const auto terms = query.fuzzyTerms();
    const qint64 needed = static_cast<qint64>(q.offset) + q.limit;
    SearchQuery strict = q;
    strict.fuzzy = false;
    if (terms.isEmpty() || needed > std::numeric_limits<int>::max())
        return files ? searchFiles(strict) : searchTorrentsStrict(strict);

    // Use the same strict prefix for every page. If it does not fill the
    // requested window, it also contains every hash to exclude from fuzzy hits.
    strict.offset = 0;
    strict.limit = static_cast<int>(needed);
    auto exact = files ? searchFiles(strict) : searchTorrentsStrict(strict, true);
    if (q.cancelled())
        return {};
    if (exact.size() >= needed)
        return exact.mid(q.offset, q.limit);
    QSet<QString> strictHashes;
    for (const auto& hit : exact)
        strictHashes.insert(infohash::normalize(hit.torrent.hash));

    constexpr int termLimit = 4;
    constexpr int rowsPerTerm = 64;
    constexpr int alternativesLimit = 16;
    constexpr int timeLimitMs = 150;
    constexpr int selectLimitMs = 40;
    qsizetype remainingText = 256 * 1024;
    int examined = 0;
    const QDeadlineTimer deadline(timeLimitMs, Qt::PreciseTimer);
    const QString table = files ? kFiles : kTorrents;
    const QString sortColumn = resolveSortColumn(q.sort);
    const QString sortKey = sortColumn.toLower();
    QHash<QString, RankedHit> fuzzyHits;
    QString budgetReason;
    if (terms.size() > termLimit)
        budgetReason = QStringLiteral("term limit");
    bool aborted = false;

    for (const auto& term : terms.first(qMin(terms.size(), qsizetype(termLimit)))) {
        if (q.cancelled())
            return {};
        if (deadline.hasExpired())
            break;
        QStringList alternatives = term.anchors();
        if (!term.continuous) {
            // Probe only exact dictionary keys. This works against both RT
            // tables without an infix index or a dictionary-wide regex.
            bool ok = false;
            auto keywords = db_->query(QStringLiteral("CALL KEYWORDS(?, ?, 1 AS stats)"),
                { alternatives.join(u' '), table }, &ok);
            if (!ok) {
                aborted = true;
                break;
            }
            std::sort(keywords.begin(), keywords.end(), [](const auto& a, const auto& b) {
                const qint64 aDocs = a.value("docs").toLongLong();
                const qint64 bDocs = b.value("docs").toLongLong();
                if (aDocs == bDocs)
                    return a.value("normalized").toString() < b.value("normalized").toString();
                return aDocs > bDocs;
            });
            alternatives.clear();
            for (const auto& row : keywords) {
                if (row.value("docs").toLongLong() <= 0)
                    break;
                if (alternatives.size() == alternativesLimit) {
                    budgetReason = QStringLiteral("dictionary limit");
                    break;
                }
                alternatives.append(row.value("normalized").toString());
            }
        }
        if (deadline.hasExpired())
            break;
        if (alternatives.isEmpty())
            continue;

        const auto variant = query.withAlternatives(term.index, alternatives);
        const QString expression = files ? variant.expression(false) : QString();
        SelectQuery candidates(table);
        candidates.matchAgainst(variant, !files).limit(0, rowsPerTerm + 1);
        if (files) {
            candidates.columns(QStringLiteral("id, hash, path")).orderBy(QStringLiteral("id"), false);
        } else {
            applySearchFilters(candidates, q);
            candidates.orderBy(sortColumn.isEmpty() ? QStringLiteral("id") : sortColumn,
                !sortColumn.isEmpty() && q.descending);
            if (!sortColumn.isEmpty())
                candidates.withStableOrder();
        }
        const int timeLeft = static_cast<int>(deadline.remainingTime());
        const int queryLimit = qBound(1, timeLeft, selectLimitMs);
        bool ok = false;
        const auto rows = db_->query(candidates.maxQueryTime(queryLimit).build(), {}, &ok);
        if (!ok || timeLeft - deadline.remainingTime() >= queryLimit) {
            aborted = true;
            budgetReason = ok ? QStringLiteral("index time limit") : QStringLiteral("index query failed");
            break;
        }
        if (rows.size() > rowsPerTerm)
            budgetReason = QStringLiteral("candidate limit");

        QHash<qint64, QVariantMap> parentsById;
        const auto page = rows.first(qMin(rows.size(), qsizetype(rowsPerTerm)));
        if (files && !page.isEmpty()) {
            QVector<qint64> ids;
            for (const auto& row : page)
                ids.append(row.value("id").toLongLong());
            SelectQuery parents(kTorrents);
            parents.whereInIds("id", ids).limit(0, ids.size());
            applySearchFilters(parents, q);
            const int parentLimit = qBound(1, static_cast<int>(deadline.remainingTime()), selectLimitMs);
            for (const auto& row : db_->query(parents.maxQueryTime(parentLimit).build(), {}, &ok))
                parentsById.insert(row.value("id").toLongLong(), row);
            if (!ok) {
                aborted = true;
                break;
            }
        }

        for (const auto& row : page) {
            if (deadline.hasExpired())
                break;
            ++examined;
            const QString hash = infohash::normalize(row.value("hash").toString());
            if (strictHashes.contains(hash) || (!files && fuzzyHits.contains(hash)))
                continue;
            const qint64 id = row.value("id").toLongLong();
            const auto parent = parentsById.constFind(id);
            if (files && (parent == parentsById.cend() || id != rowIdFromHash(hash)
                             || !sameHash(parent->value("hash").toString(), hash)))
                continue;

            RankedHit ranked;
            const auto& torrentRow = files ? *parent : row;
            ranked.hit.torrent = rowToTorrent(torrentRow);
            ranked.sortKey = torrentRow.value(sortKey);
            ranked.hit.fuzzy = true;
            ranked.hit.fromFileMatch = files;
            const QString source = files ? row.value("path").toString() : buildNameIndex(ranked.hit.torrent);
            if (source.size() > remainingText) {
                budgetReason = QStringLiteral("verification text limit");
                continue;
            }
            remainingText -= source.size();
            if (files) {
                if (!highlightMatchingPaths(*db_, source, variant, expression, ranked.hit.matchingPaths, &term, &deadline, &q)) {
                    aborted = true;
                    break;
                }
                if (ranked.hit.matchingPaths.isEmpty())
                    continue;
            } else if (!term.matches(source) || std::any_of(variant.continuousTerms().cbegin(),
                           variant.continuousTerms().cend(), [&](const QString& word) { return !source.contains(word); })) {
                continue;
            }
            auto existing = fuzzyHits.find(hash);
            if (existing == fuzzyHits.end()) {
                fuzzyHits.insert(hash, std::move(ranked));
            } else {
                for (const QString& path : ranked.hit.matchingPaths) {
                    if (!existing->hit.matchingPaths.contains(path))
                        existing->hit.matchingPaths.append(path);
                }
            }
        }
        if (aborted)
            break;
    }

    if (aborted || deadline.hasExpired()) {
        qDebug() << "[Search] fuzzy pass discarded:" << (deadline.hasExpired() ? QStringLiteral("time limit")
                                        : (budgetReason.isEmpty() ? QStringLiteral("query failed") : budgetReason))
                 << "after" << examined << "candidate rows";
        return exact.mid(q.offset, q.limit);
    }
    if (!budgetReason.isEmpty())
        qDebug() << "[Search] fuzzy pass bounded by" << budgetReason << "after" << examined << "candidate rows";
    auto sorted = fuzzyHits.values();
    std::sort(sorted.begin(), sorted.end(), [&](const RankedHit& a, const RankedHit& b) {
        return betterHit(a, b, sortColumn, q.descending);
    });
    for (const auto& candidate : sorted) {
        if (exact.size() == needed)
            break;
        exact.append(candidate.hit);
    }
    return exact.mid(q.offset, q.limit);
}

QVector<Torrent> TorrentRepository::recent(int limit)
{
    return selectTorrents(QStringLiteral("SELECT * FROM torrents ORDER BY added DESC LIMIT 0,%1").arg(limit));
}

QVector<Torrent> TorrentRepository::top(const QString& type, const QString& time, int offset, int limit)
{
    SelectQuery builder(kTorrents);
    builder.whereRaw(QStringLiteral("seeders > 0"));
    builder.whereRaw(QStringLiteral("contentCategory != %1").arg(domain::toId(ContentCategory::XXX)));
    builder.whereRaw(contentTypeFilter(type));

    if (!time.isEmpty()) {
        const qint64 now = QDateTime::currentSecsSinceEpoch();
        qint64 cutoff = 0;
        if (time == QLatin1String("hours"))
            cutoff = now - 60 * 60 * 24;
        else if (time == QLatin1String("week"))
            cutoff = now - 60 * 60 * 24 * 7;
        else if (time == QLatin1String("month"))
            cutoff = now - 60LL * 60 * 24 * 30;
        if (cutoff > 0)
            builder.whereRaw(QStringLiteral("added > %1").arg(cutoff));
    }

    builder.orderBy(QStringLiteral("seeders"), true);
    builder.limit(offset, limit);
    return selectTorrents(builder.build());
}

QVector<Torrent> TorrentRepository::random(int limit, bool includeFiles)
{
    // RAND() is not an identifier, so this one query is built directly.
    const QString sql = QStringLiteral("SELECT * FROM torrents WHERE seeders > 0 AND contentCategory != %1 "
                                       "ORDER BY RAND() LIMIT %2")
                            .arg(domain::toId(ContentCategory::XXX))
                            .arg(limit);
    QVector<Torrent> results = selectTorrents(sql);

    if (includeFiles && !results.isEmpty()) {
        QStringList hashes;
        for (const Torrent& t : results)
            hashes << t.hash;
        const auto filesMap = filesOf(hashes);
        for (Torrent& t : results)
            t.fileList = filesMap.value(t.hash);
    }
    return results;
}

QHash<QString, Torrent> TorrentRepository::getMany(const QStringList& hashes, QSet<QString>* collided, bool* ok)
{
    if (ok) *ok = true;
    QHash<QString, Torrent> result;
    if (hashes.isEmpty())
        return result;

    QHash<qint64, QStringList> wanted;
    const QVector<qint64> ids = idsFor(hashes, wanted);
    const QString sql = SelectQuery(kTorrents).whereInIds(QStringLiteral("id"), ids).limit(0, ids.size()).build();
    bool queryOk = false;
    const auto rows = db_->query(sql, {}, &queryOk);
    if (ok) *ok = queryOk;
    if (!queryOk) return result;
    for (const auto& row : rows) {
        const Torrent t = rowToTorrent(row);
        // One row answers every hash that asked for its id: the one it actually
        // belongs to is found, and each of the others lost the slot and must be
        // reported so the caller drops it instead of writing over this row.
        for (const QString& want : wanted.value(t.id)) {
            if (sameHash(t.hash, want))
                result.insert(want, t);
            else if (collided)
                *collided += want;
        }
    }
    return result;
}

int TorrentRepository::addMany(const QVector<Torrent>& torrents)
{
    int inserted = 0;
    for (const auto& item : addManyConfirmed(torrents))
        if (item.status == PersistStatus::Inserted) ++inserted;
    return inserted;
}

QVector<TorrentRepository::PersistItem> TorrentRepository::addManyConfirmed(const QVector<Torrent>& torrents)
{
    WriteGuard write(this);
    QVector<PersistItem> result;
    if (torrents.isEmpty()) return result;
    if (statisticsDirty_) primeFromDatabase();
    if (torrents.size() > 500) {
        for (int offset = 0; offset < torrents.size(); offset += 500)
            result += addManyConfirmed(torrents.mid(offset, 500));
        return result;
    }
    QStringList hashes;
    for (const auto& t : torrents) hashes.append(t.hash.toLower());
    QSet<QString> collided;
    bool ok = false;
    const auto stored = getMany(hashes, &collided, &ok);
    QVector<QVariantMap> fileRows, summaryRows;
    QHash<qint64, int> index;
    QHash<qint64, QVariantMap> expectedFiles;
    for (auto t : torrents) {
        t.hash = t.hash.toLower();
        const auto id = rowIdFromHash(t.hash);
        PersistItem item; item.hash = t.hash; item.torrent = t; item.stage = "lookup";
        if (!ok || statisticsDirty_) item.error = "Database lookup/statistics reconciliation failed";
        else if (!t.isValid() || id == 0) { item.status = PersistStatus::Invalid; item.error = "Invalid hash"; }
        else if (collided.contains(t.hash)) { item.status = PersistStatus::Collision; item.error = "Row id collision"; }
        else if (stored.contains(t.hash)) {
            item.torrent = stored.value(t.hash);
            item.status = unconfirmedInserts_.remove(t.hash) ? PersistStatus::Inserted : PersistStatus::Existing;
        } else if (index.contains(id)) {
            item.status = result[index.value(id)].hash == t.hash ? PersistStatus::Duplicate : PersistStatus::Collision;
            item.error = "Duplicate hash or row id inside batch";
        } else {
            unconfirmedInserts_.remove(t.hash); // absent after an earlier uncertain write
            index.insert(id, result.size());
            if (!t.fileList.isEmpty()) {
                auto row = filesRow(t.hash, t.fileList); fileRows.append(row); expectedFiles.insert(id, row);
            }
        }
        result.append(std::move(item));
    }
    if (unconfirmedInserts_.size() + index.size() > 10000) {
        for (auto position : index) {
            result[position].stage = "reconcile";
            result[position].error = "Unconfirmed insertion receipt capacity reached";
        }
        return result;
    }
    const auto files = db_->replaceManyConfirmed(kFiles, fileRows);
    QSet<qint64> confirmedFiles = files.confirmed;
    if (!files.uncertain.isEmpty()) {
        QVector<qint64> ids(files.uncertain.begin(), files.uncertain.end());
        bool readOk = false;
        const auto rows = db_->query(SelectQuery(kFiles).whereInIds("id", ids).limit(0, ids.size()).build(), {}, &readOk);
        if (readOk) for (const auto& row : rows) {
            const auto id = row.value("id").toLongLong(); const auto expected = expectedFiles.value(id);
            if (sameHash(row.value("hash").toString(), expected.value("hash").toString())
                && row.value("path").toString() == expected.value("path").toString()
                && row.value("size").toString() == expected.value("size").toString()) confirmedFiles.insert(id);
        }
    }
    for (auto it = index.cbegin(); it != index.cend(); ++it) {
        auto& item = result[it.value()];
        if (!item.torrent.fileList.isEmpty() && !confirmedFiles.contains(it.key())) {
            item.stage = "files"; item.error = files.rejected.contains(it.key()) ? "File row exceeds SQL byte limit" : "File write unconfirmed";
            item.retryable = !files.rejected.contains(it.key());
        } else summaryRows.append(torrentRow(item.torrent));
    }
    const auto summaries = db_->replaceManyConfirmed(kTorrents, summaryRows);
    QSet<qint64> confirmed = summaries.confirmed;
    if (!summaries.uncertain.isEmpty()) {
        QStringList uncertainHashes;
        for (auto id : summaries.uncertain) uncertainHashes.append(result[index.value(id)].hash);
        bool readOk = false;
        const auto recovered = getMany(uncertainHashes, nullptr, &readOk);
        for (auto id : summaries.uncertain) {
            const auto hash = result[index.value(id)].hash;
            if (readOk && recovered.contains(hash)) confirmed.insert(id);
            else if (!readOk) { unconfirmedInserts_.insert(hash); statisticsDirty_ = true; }
        }
    }
    qint64 added = 0, addedFiles = 0, addedSize = 0;
    for (auto it = index.cbegin(); it != index.cend(); ++it) {
        auto& item = result[it.value()];
        if (confirmed.contains(it.key())) {
            item.status = PersistStatus::Inserted; item.stage = "summary";
            ++added; addedFiles += item.torrent.files; addedSize += item.torrent.size;
        } else if (item.error.isEmpty()) {
            item.stage = "summary"; item.error = summaries.rejected.contains(it.key()) ? "Summary exceeds SQL byte limit" : "Summary write unconfirmed";
            item.retryable = !summaries.rejected.contains(it.key());
        }
    }
    if (added || statisticsDirty_) bumpStatistics(added, addedFiles, addedSize);
    return result;
}

QHash<QString, domain::HashState> TorrentRepository::checkMany(const QStringList& hashes)
{
    QHash<QString, domain::HashState> result;
    if (hashes.size() > 500) {
        for (int offset = 0; offset < hashes.size(); offset += 500) result.insert(checkMany(hashes.mid(offset, 500)));
        return result;
    }
    bool ok = false;
    QSet<QString> collided;
    const auto stored = getMany(hashes, &collided, &ok);
    QStringList details;
    for (auto it = stored.cbegin(); it != stored.cend(); ++it) details.append(it.key());
    bool filesOk = true;
    const auto files = filesOf(details, &filesOk);
    for (const auto& hash : hashes) {
        auto state = domain::HashState::Failed;
        if (ok) {
            if (collided.contains(hash)) state = domain::HashState::Collision;
            else if (!stored.contains(hash)) state = domain::HashState::Missing;
            else if (filesOk) state = stored.value(hash).files > 0 && files.value(hash).size() >= stored.value(hash).files
                ? domain::HashState::Known : domain::HashState::Incomplete;
        }
        result.insert(hash, state);
    }
    return result;
}

QVector<Torrent> TorrentRepository::pageAfterId(qint64 afterId, int limit)
{
    SelectQuery builder(kTorrents);
    if (afterId > 0)
        builder.whereRaw(QStringLiteral("id > %1").arg(afterId));
    builder.orderBy(QStringLiteral("id"), false).limit(0, limit);
    return selectTorrents(builder.build());
}

// ---------------------------------------------------------------------------
// Files
// ---------------------------------------------------------------------------

QVector<File> TorrentRepository::filesOf(const QString& hash)
{
    const qint64 id = rowIdFromHash(hash);
    if (id == 0)
        return {};

    const auto rows = db_->query(QStringLiteral("SELECT * FROM files WHERE id = ?"), { id });
    if (rows.isEmpty())
        return {};
    if (!sameHash(rows.first().value(QStringLiteral("hash")).toString(), hash))
        return {}; // id collision: these files belong to a different torrent
    return parseFileBlob(
        rows.first().value(QStringLiteral("path")).toString(), rows.first().value(QStringLiteral("size")).toString());
}

QHash<QString, QVector<File>> TorrentRepository::filesOf(const QStringList& hashes, bool* ok)
{
    if (ok) *ok = true;
    QHash<QString, QVector<File>> result;
    if (hashes.isEmpty())
        return result;

    QHash<qint64, QStringList> wanted;
    const QVector<qint64> ids = idsFor(hashes, wanted);
    // The LIMIT is not optional: Manticore returns 20 rows for a SELECT without
    // one, which would silently drop most of the batch.
    const QString sql = SelectQuery(kFiles).whereInIds(QStringLiteral("id"), ids).limit(0, ids.size()).build();
    bool queryOk = false;
    const auto rows = db_->query(sql, {}, &queryOk);
    if (ok) *ok = queryOk;
    for (const auto& row : rows) {
        const QString hash = row.value(QStringLiteral("hash")).toString();
        if (!wantedMatches(wanted.value(row.value(QStringLiteral("id")).toLongLong()), hash))
            continue; // id collision
        for (const auto& want : wanted.value(row.value("id").toLongLong()))
            if (sameHash(want, hash)) result[want] = parseFileBlob(row.value("path").toString(), row.value("size").toString());
    }
    return result;
}

bool TorrentRepository::saveFiles(const QString& hash, const QVector<File>& files)
{
    WriteGuard write(this);
    if (files.isEmpty())
        return false;

    // One statement: the files row carries the torrent's own id, so replacing it
    // in place is exact. The old delete + insert handed the row a fresh counter
    // id every time a file list was backfilled.
    return db_->replace(kFiles, filesRow(hash, files));
}

// ---------------------------------------------------------------------------
// Partial updates
// ---------------------------------------------------------------------------

bool TorrentRepository::updateTrackerCounts(const QString& hash, int seeders, int leechers, int completed)
{
    WriteGuard write(this);
    const RowSlot slot = resolve(hash);
    if (!slot.taken || !slot.ours)
        return false;
    return db_->update(kTorrents,
        { { "seeders", seeders }, { "leechers", leechers }, { "completed", completed },
            { "trackersChecked", QDateTime::currentSecsSinceEpoch() } },
        { { "id", slot.id } });
}

bool TorrentRepository::updateFiles(const QString& hash, const QVector<File>& files)
{
    WriteGuard write(this);
    if (files.isEmpty())
        return false;

    const qint64 id = rowIdFromHash(hash);
    if (id == 0)
        return false;

    const auto rows = db_->query(QStringLiteral("SELECT hash, files FROM torrents WHERE id = ?"), { id });
    if (rows.isEmpty())
        return false;
    if (!sameHash(rows.first().value(QStringLiteral("hash")).toString(), hash))
        return false; // id collision
    const int oldCount = rows.first().value(QStringLiteral("files")).toInt();

    if (!saveFiles(hash, files))
        return false;

    if (!db_->update(kTorrents, { { "files", files.size() } }, { { "id", id } }))
        return false;

    bumpStatistics(0, files.size() - oldCount, 0);
    notify([this, hash] { emit torrentUpdated(hash); });
    return true;
}

bool TorrentRepository::mergeInfo(const QString& hash, const QJsonObject& info)
{
    WriteGuard write(this);
    // get() already refuses a collided row, so reaching here means the slot is
    // ours to write.
    const auto existing = get(hash);
    if (!existing)
        return false;

    QJsonObject merged = existing->info;
    for (auto it = info.constBegin(); it != info.constEnd(); ++it)
        merged[it.key()] = it.value();
    return db_->update(kTorrents, { { "info", merged } }, { { "id", existing->id } });
}

bool TorrentRepository::updateClassification(const QString& hash, ContentType type, ContentCategory category)
{
    WriteGuard write(this);
    const RowSlot slot = resolve(hash);
    if (!slot.taken || !slot.ours)
        return false;
    return db_->update(kTorrents,
        { { "contentType", domain::toId(type) }, { "contentCategory", domain::toId(category) } },
        { { "id", slot.id } });
}

// ---------------------------------------------------------------------------
// Row-id migration
// ---------------------------------------------------------------------------

TorrentRepository::RowIdMigrationPage TorrentRepository::migrateRowIdPage(
    const QString& table, qint64& cursor, int limit)
{
    WriteGuard write(this);
    RowIdMigrationPage page;
    const bool isTorrents = (table == kTorrents);
    // Where this page started. Every failure path below rewinds to it, so a
    // page is either fully committed or fully retried — never skipped.
    const qint64 startCursor = cursor;

    bool readOk = false;
    const auto rows = db_->query(
        QStringLiteral("SELECT * FROM %1 WHERE id > %2 ORDER BY id ASC LIMIT %3").arg(table).arg(cursor).arg(limit), {},
        &readOk);
    if (!readOk) {
        // A failed query returns no rows just like an exhausted table does.
        // Telling the two apart is what keeps the migration from declaring
        // itself done after one transient searchd error.
        qWarning() << "[TorrentRepository] migration: failed to read" << table << "page past id" << startCursor;
        page.failed = true;
        return page;
    }
    if (rows.isEmpty()) {
        page.finished = true;
        return page;
    }

    // What each row wants to become. The row itself is rebuilt here, while the
    // source columns are still in hand.
    struct Move {
        qint64 oldId = 0;
        qint64 newId = 0;
        QString hash;
        QVariantMap row;
    };
    QVector<Move> moves;
    QVector<qint64> doomed; // rows to delete outright

    for (const auto& row : rows) {
        const qint64 oldId = row.value(QStringLiteral("id")).toLongLong();
        cursor = oldId;
        ++page.scanned;

        const QString hash = row.value(QStringLiteral("hash")).toString();
        const qint64 newId = rowIdFromHash(hash);
        if (newId == 0) {
            qWarning() << "[TorrentRepository] migration: dropping" << table << "row" << oldId << "with unusable hash"
                       << hash;
            doomed.append(oldId);
            ++page.dropped;
            continue;
        }
        if (newId == oldId)
            continue; // already migrated

        Move move;
        move.oldId = oldId;
        move.newId = newId;
        move.hash = hash;
        if (isTorrents) {
            // Round-trip through the domain type so the rewritten row is byte for
            // byte what a normal insert would have produced — nameIndex rebuilt,
            // info re-encoded as JSON rather than re-quoted as a string.
            move.row = torrentRow(rowToTorrent(row));
        } else {
            // A files row is four plain columns; there is nothing to re-derive.
            move.row = QVariantMap { { "id", static_cast<qlonglong>(newId) }, { "hash", hash },
                { "path", row.value(QStringLiteral("path")) }, { "size", row.value(QStringLiteral("size")) } };
        }
        moves.append(move);
    }

    if (moves.isEmpty() && doomed.isEmpty())
        return page;

    // Refuse a move whose destination is already held by a different torrent.
    QHash<qint64, QString> ownerById;
    if (!moves.isEmpty()) {
        QVector<qint64> targets;
        targets.reserve(moves.size());
        for (const Move& move : moves)
            targets.append(move.newId);
        const QString sql = SelectQuery(table)
                                .columns(QStringLiteral("id, hash"))
                                .whereInIds(QStringLiteral("id"), targets)
                                .limit(0, targets.size())
                                .build();
        bool ownersOk = false;
        const auto owners = db_->query(sql, {}, &ownersOk);
        if (!ownersOk) {
            // Without this lookup a taken destination reads as free, and the move
            // would overwrite a stranger's row. Retry the page instead.
            qWarning() << "[TorrentRepository] migration: failed to check" << table << "destinations past id"
                       << startCursor;
            cursor = startCursor;
            page.failed = true;
            return page;
        }
        for (const auto& row : owners) {
            ownerById.insert(
                row.value(QStringLiteral("id")).toLongLong(), row.value(QStringLiteral("hash")).toString());
        }
    }

    QVector<QVariantMap> rewrites;
    QSet<qint64> claimed; // destinations taken earlier in this same page
    rewrites.reserve(moves.size());

    for (const Move& move : moves) {
        const QString owner = ownerById.value(move.newId);
        if (!owner.isEmpty() && !sameHash(owner, move.hash)) {
            qWarning() << "[TorrentRepository] migration: dropping" << table << "row" << move.oldId << "-"
                       << "row id" << move.newId << "already belongs to a different torrent";
            doomed.append(move.oldId);
            ++page.dropped;
            continue;
        }
        if (claimed.contains(move.newId)) {
            // The same hash stored twice under the old counter ids. The first
            // move carries it; this one is only a duplicate to retire.
            doomed.append(move.oldId);
            continue;
        }
        claimed.insert(move.newId);
        rewrites.append(move.row);
        doomed.append(move.oldId);
        ++page.rewritten;
    }

    // Write the replacements before retiring the originals: interrupted here, the
    // table holds the same torrent under two ids and the next pass re-does the
    // move. Interrupted the other way round, the row would simply be gone.
    if (!rewrites.isEmpty() && !db_->replaceMany(table, rewrites)) {
        qWarning() << "[TorrentRepository] migration: failed to write" << rewrites.size() << table << "rows";
        cursor = startCursor;
        page.rewritten = 0;
        page.failed = true;
        return page;
    }

    if (!doomed.isEmpty()) {
        QStringList ids;
        ids.reserve(doomed.size());
        for (qint64 id : doomed)
            ids << QString::number(id);
        if (!db_->execute(
                QStringLiteral("DELETE FROM %1 WHERE id IN (%2)").arg(table, ids.join(QLatin1String(", "))))) {
            // The replacements are in, the originals are not out: the table now
            // holds some torrents twice. Rewinding re-runs the page, which skips
            // the rows already carrying their derived id and retries this delete.
            qWarning() << "[TorrentRepository] migration: failed to retire" << doomed.size() << table << "rows";
            cursor = startCursor;
            page.failed = true;
            return page;
        }
    }

    return page;
}

// ---------------------------------------------------------------------------
// Mapping helpers
// ---------------------------------------------------------------------------

std::optional<TorrentRepository::SearchReindexPage> TorrentRepository::reindexSearchPage(
    const QString& table, qint64 afterId, int limit)
{
    WriteGuard write(this);
    if (table != kTorrents && table != kFiles)
        return std::nullopt;

    bool ok = false;
    const auto rows = db_->query(SelectQuery(table)
            .whereRaw(QStringLiteral("id > %1").arg(afterId))
            .orderBy(QStringLiteral("id"), false)
            .limit(0, limit)
            .build(), {}, &ok);
    if (!ok)
        return std::nullopt;
    if (rows.isEmpty())
        return SearchReindexPage{ afterId, 0 };

    const auto report = db_->replaceManyConfirmed(table, rows);
    if (!report.uncertain.isEmpty())
        return std::nullopt;
    // The normal bulk writer caps packet size. A pre-existing long file list
    // can still fit as a standalone REPLACE, just as an original single add did.
    for (const auto& row : rows) {
        if (report.rejected.contains(row.value(QStringLiteral("id")).toLongLong()) && !db_->replace(table, row))
            return std::nullopt;
    }
    // A checkpoint can survive a forced exit or later binlog recovery only
    // after the corresponding data is on disk.
    if (!db_->execute(QStringLiteral("FLUSH TABLE %1").arg(table)))
        return std::nullopt;
    return SearchReindexPage{ rows.last().value(QStringLiteral("id")).toLongLong(), rows.size() };
}

Torrent TorrentRepository::rowToTorrent(const QVariantMap& row) const
{
    // Manticore returns every column name lower-cased.
    Torrent t;
    t.id = row.value(QStringLiteral("id")).toLongLong();
    t.hash = row.value(QStringLiteral("hash")).toString();
    t.name = row.value(QStringLiteral("name")).toString();
    t.size = row.value(QStringLiteral("size")).toLongLong();
    t.files = row.value(QStringLiteral("files")).toInt();
    t.pieceLength = row.value(QStringLiteral("piecelength")).toInt();
    t.added = QDateTime::fromSecsSinceEpoch(row.value(QStringLiteral("added")).toLongLong());
    t.ipv4 = row.value(QStringLiteral("ipv4")).toString();
    t.port = row.value(QStringLiteral("port")).toInt();
    t.contentType = domain::contentTypeFromId(row.value(QStringLiteral("contenttype")).toInt());
    t.contentCategory = domain::contentCategoryFromId(row.value(QStringLiteral("contentcategory")).toInt());
    t.seeders = row.value(QStringLiteral("seeders")).toInt();
    t.leechers = row.value(QStringLiteral("leechers")).toInt();
    t.completed = row.value(QStringLiteral("completed")).toInt();

    const qint64 checked = row.value(QStringLiteral("trackerschecked")).toLongLong();
    if (checked > 0)
        t.trackersChecked = QDateTime::fromSecsSinceEpoch(checked);

    t.good = row.value(QStringLiteral("good")).toInt();
    t.bad = row.value(QStringLiteral("bad")).toInt();

    const QString infoStr = row.value(QStringLiteral("info")).toString();
    if (!infoStr.isEmpty()) {
        const QJsonDocument doc = QJsonDocument::fromJson(infoStr.toUtf8());
        if (doc.isObject())
            t.info = doc.object();
    }
    return t;
}

QString TorrentRepository::buildNameIndex(const Torrent& t) const
{
    QString index = t.name;
    if (t.info.contains(QLatin1String("name"))) {
        const QString infoName = t.info.value(QLatin1String("name")).toString();
        if (!infoName.isEmpty() && infoName.length() < kInfoNameMaxLength)
            index += QLatin1Char(' ') + infoName;
    }
    return index;
}

} // namespace rats::data
