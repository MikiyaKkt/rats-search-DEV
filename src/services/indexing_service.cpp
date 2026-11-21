#include "services/indexing_service.h"

#include "data/torrent_repository.h"
#include "domain/content_classifier.h"
#include "services/filter_policy.h"
#include "common/performance_metrics.h"

#include <QDebug>
#include <QSet>
#include <QStringList>
#include <QScopeGuard>

namespace rats::service {

IndexingService::IndexingService(data::TorrentRepository* repository, FilterPolicy* filter, QObject* parent)
    : QObject(parent), repository_(repository), filter_(filter)
{
}

IndexingService::Result IndexingService::insert(domain::Torrent torrent)
{
    auto& metrics = common::PerformanceMetrics::instance();
    common::PerformanceTimer timing(QStringLiteral("index.single_ms"));
    metrics.increment(QStringLiteral("index.single_attempts"));
    data::TorrentRepository::WriteGuard write(repository_);
    torrent.hash = torrent.hash.toLower();
    Result result;
    if (!repository_->reconcileStatistics()) {
        result.error = QStringLiteral("Database statistics reconciliation failed");
        return result;
    }

    if (!torrent.isValid()) {
        metrics.increment(QStringLiteral("index.invalid"));
        result.error = QStringLiteral("Invalid hash");
        return result;
    }
    if (torrent.name.isEmpty()) {
        metrics.increment(QStringLiteral("index.invalid"));
        result.error = QStringLiteral("Empty torrent name");
        return result;
    }

    // One lookup serves both halves of this method: it answers the dedup question
    // below and is handed to add() further down, so a torrent the crawler has
    // never seen costs a single docid query instead of one here and another
    // inside add().
    const data::TorrentRepository::RowSlot slot = [&] {
        common::PerformanceTimer lookup(QStringLiteral("index.dedupe_ms"));
        return repository_->resolve(torrent.hash, /*withRow*/ true);
    }();
    if (slot.failed) {
        metrics.increment(QStringLiteral("index.failed"));
        result.error = QStringLiteral("Database lookup failed");
        return result;
    }

    // Already indexed? Merge what the incoming copy adds over the stored one
    // (e.g. from a peer) and return the stored entity.
    if (slot.stored) {
        metrics.increment(QStringLiteral("index.merged"));
        domain::Torrent existing = *slot.stored;
        result.success = true;
        result.alreadyExists = true;
        result.torrent = existing;

        // The summary count can be nonzero even when its details row is missing
        // or truncated. Repair from a complete incoming list, never a snippet.
        if (!torrent.fileList.isEmpty()
            && torrent.fileList.size() >= qMax(existing.files, torrent.files)
            && repository_->filesOf(existing.hash).size() < torrent.fileList.size()) {
            if (!repository_->updateFiles(existing.hash, torrent.fileList)) {
                result.success = false;
                result.error = QStringLiteral("Database file merge failed");
                metrics.increment(QStringLiteral("index.failed"));
                return result;
            }
            existing.fileList = torrent.fileList;
            existing.files = torrent.fileList.size();
            result.torrent = existing;
        }

        if (torrent.good > existing.good || torrent.bad > existing.bad) {
            existing.good = qMax(existing.good, torrent.good);
            existing.bad = qMax(existing.bad, torrent.bad);
            if (!repository_->updateVotes(existing.hash, existing.good, existing.bad, true)) {
                result.success = false;
                result.error = QStringLiteral("Database vote merge failed");
                metrics.increment(QStringLiteral("index.failed"));
                return result;
            }
            result.torrent = existing;
        }
        if (result.success && repository_->takeConfirmedInsert(existing.hash)) {
            result.alreadyExists = false;
            write.unlock();
            emit torrentIndexed(result.torrent);
        }
        return result;
    }

    if (torrent.contentType == domain::ContentType::Unknown) {
        common::PerformanceTimer classification(QStringLiteral("index.classify_ms"));
        domain::ContentClassifier::classify(torrent);
    }

    if (filter_) {
        common::PerformanceTimer filtering(QStringLiteral("index.filter_ms"));
        if (const QString reason = filter_->rejectionReason(torrent); !reason.isEmpty()) {
            metrics.increment(QStringLiteral("index.rejected"));
            qCDebug(common::crawlerDetail) << "[Indexing] rejected" << torrent.hash.left(16) << "-" << reason;
            result.error = QStringLiteral("Rejected: ") + reason;
            return result;
        }
    }

    // The slot resolved above is still the one this hash maps to; add() only has
    // to trust it, not look it up again.
    const bool stored = [&] {
        common::PerformanceTimer persistence(QStringLiteral("index.persist_ms"));
        return repository_->add(torrent, slot);
    }();
    if (!stored) {
        metrics.increment(slot.collided() ? QStringLiteral("index.collided") : QStringLiteral("index.failed"));
        result.error = slot.collided() ? QStringLiteral("Row id belongs to a different torrent")
                                       : QStringLiteral("Database insert failed");
        return result;
    }

    result.success = true;
    result.torrent = torrent;
    metrics.increment(QStringLiteral("index.inserted"));
    qCDebug(common::crawlerDetail) << "[Indexing] indexed" << torrent.hash.left(16) << torrent.name.left(50)
            << "size:" << (torrent.size / (1024 * 1024)) << "MB files:" << torrent.files;

    write.unlock();
    emit torrentIndexed(torrent);
    return result;
}

IndexingService::BatchResult IndexingService::insertBatch(QVector<domain::Torrent> torrents, const BatchOptions& options)
{
    data::TorrentRepository::WriteGuard write(repository_);
    BatchResult result;
    if (!repository_->reconcileStatistics()) {
        for (const auto& t : torrents) result.record({t.hash.toLower(), Outcome::Failed, "lookup", "Statistics reconciliation failed", {}});
        return result;
    }
    auto& metrics = common::PerformanceMetrics::instance();
    common::PerformanceTimer timing(QStringLiteral("index.batch_ms"));
    metrics.increment(QStringLiteral("index.batch_attempts"), torrents.size());
    metrics.observe(QStringLiteral("index.batch_size"), torrents.size());
    const auto outcome = qScopeGuard([&] {
        metrics.increment(QStringLiteral("index.batch_inserted_reported"), result.inserted);
        metrics.increment(QStringLiteral("index.batch_merged"), result.merged);
        metrics.increment(QStringLiteral("index.batch_invalid"), result.invalid);
        metrics.increment(QStringLiteral("index.batch_rejected"), result.rejected);
        metrics.increment(QStringLiteral("index.batch_collided"), result.collided);
        metrics.increment(QStringLiteral("index.batch_failed"), result.failed);
    });
    QVector<domain::Torrent> candidates;
    QSet<QString> seen;
    for (auto& t : torrents) {
        t.hash = t.hash.toLower();
        if (!t.isValid() || t.name.isEmpty()) {
            result.record({t.hash, Outcome::Invalid, "validation", "Invalid hash or empty name", {}});
        } else if (seen.contains(t.hash)) {
            result.record({t.hash, Outcome::Duplicate, "validation", "Duplicate inside batch", {}});
            metrics.increment(QStringLiteral("index.batch_duplicates"));
        } else { seen.insert(t.hash); candidates.append(std::move(t)); }
    }
    for (int offset = 0; offset < candidates.size(); offset += 500) {
        const auto chunk = candidates.mid(offset, 500);
        QStringList hashes;
        for (const auto& t : chunk) hashes.append(t.hash);
        QSet<QString> collided;
        bool ok = false;
        const auto existing = repository_->getMany(hashes, &collided, &ok);
        if (!ok) {
            for (const auto& t : chunk) result.record({t.hash, Outcome::Failed, "lookup", "Database lookup failed", {}});
            continue;
        }
        QStringList detailHashes;
        if (options.mergeExisting) for (const auto& t : chunk)
            if (existing.contains(t.hash) && !t.fileList.isEmpty()) detailHashes.append(t.hash);
        bool detailsOk = true;
        const auto details = repository_->filesOf(detailHashes, &detailsOk);
        QVector<domain::Torrent> fresh;
        for (auto incoming : chunk) {
            if (collided.contains(incoming.hash)) {
                result.record({incoming.hash, Outcome::Collision, "lookup", "Row id collision", {}}); continue;
            }
            if (!existing.contains(incoming.hash)) {
                if (incoming.contentType == domain::ContentType::Unknown) domain::ContentClassifier::classify(incoming);
                const auto reason = options.applyFilters && filter_ ? filter_->rejectionReason(incoming) : QString();
                if (!reason.isEmpty()) result.record({incoming.hash, Outcome::Rejected, "filter", reason, {}});
                else fresh.append(std::move(incoming));
                continue;
            }
            auto stored = existing.value(incoming.hash);
            bool merged = true;
            QString stage;
            if (options.mergeExisting) {
                if (!incoming.fileList.isEmpty() && incoming.fileList.size() >= qMax(stored.files, incoming.files)) {
                    stage = "files";
                    merged = detailsOk;
                    if (merged && details.value(stored.hash).size() < incoming.fileList.size()) {
                        merged = repository_->updateFiles(stored.hash, incoming.fileList);
                        if (merged) { stored.files = incoming.fileList.size(); stored.fileList = incoming.fileList; }
                    }
                }
                if (merged && (incoming.good > stored.good || incoming.bad > stored.bad)) {
                    stage = "votes";
                    stored.good = qMax(stored.good, incoming.good); stored.bad = qMax(stored.bad, incoming.bad);
                    merged = repository_->updateVotes(stored.hash, stored.good, stored.bad, true);
                }
            }
            const auto status = !merged ? Outcome::Failed : repository_->takeConfirmedInsert(incoming.hash) ? Outcome::Inserted : Outcome::Merged;
            result.record({incoming.hash, status,
                stage, merged ? QString() : QStringLiteral("Database merge failed"), stored});
        }
        if (!fresh.isEmpty()) {
            common::PerformanceTimer persistence(QStringLiteral("index.batch_persist_ms"));
            for (const auto& item : repository_->addManyConfirmed(fresh)) {
                using Status = data::TorrentRepository::PersistStatus;
                auto status = Outcome::Failed;
                switch (item.status) {
                case Status::Inserted: status = Outcome::Inserted; break;
                case Status::Existing: status = Outcome::Merged; break;
                case Status::Collision: status = Outcome::Collision; break;
                case Status::Invalid: status = Outcome::Invalid; break;
                case Status::Duplicate: status = Outcome::Duplicate; break;
                case Status::Failed: break;
                }
                result.record({item.hash, status, item.stage, item.error, item.torrent, item.retryable});
            }
        }
    }
    return result;
}

bool IndexingService::accepts(const domain::Torrent& torrent) const
{
    return !filter_ || filter_->accepts(torrent);
}

} // namespace rats::service
