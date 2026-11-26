#include "services/search_service.h"

#include "common/performance_metrics.h"
#include "common/infohash.h"
#include "data/query.h"

#include <QPointer>
#include <QScopeGuard>
#include <QUrl>
#include <QUrlQuery>

namespace rats::service {

SearchService::SearchService(data::TorrentRepository* repository, QObject* parent)
    : QObject(parent), repository_(repository)
{
    desktopPool_.setMaxThreadCount(1);
    apiPool_.setMaxThreadCount(2);
    peerPool_.setMaxThreadCount(2);
}

SearchService::~SearchService()
{
    stopAsync();
}

void SearchService::startAsync()
{
    sharedCancellation_ = std::make_shared<std::atomic_bool>(false);
}

void SearchService::stopAsync()
{
    if (!sharedCancellation_)
        return;
    ++generation_;
    sharedCancellation_->store(true, std::memory_order_relaxed);
    sharedCancellation_.reset();
    if (desktopCancellation_)
        desktopCancellation_->store(true, std::memory_order_relaxed);
    desktopPool_.clear();
    apiPool_.clear();
    peerPool_.clear();
    desktopPool_.waitForDone();
    apiPool_.waitForDone();
    peerPool_.waitForDone();
    pendingApi_ = pendingPeer_ = 0;
}

template<typename Work, typename Done>
bool SearchService::submit(QObject* receiver, Lane lane, Work work, Done done)
{
    constexpr qsizetype maxPending = 16;
    if (!sharedCancellation_)
        return false;
    if ((lane == Lane::Api && pendingApi_ >= maxPending) || (lane == Lane::Peer && pendingPeer_ >= maxPending))
        return false;

    QThreadPool* pool = &desktopPool_;
    auto cancellation = sharedCancellation_;
    if (lane == Lane::Desktop) {
        if (desktopCancellation_)
            desktopCancellation_->store(true, std::memory_order_relaxed);
        desktopPool_.clear();
        desktopCancellation_ = std::make_shared<std::atomic_bool>(false);
        cancellation = desktopCancellation_;
    } else if (lane == Lane::Api) {
        ++pendingApi_;
        pool = &apiPool_;
    } else {
        ++pendingPeer_;
        pool = &peerPool_;
    }

    pool->start([this, receiver = QPointer<QObject>(receiver), generation = generation_, lane, cancellation,
                    work = std::move(work), done = std::move(done)]() mutable {
        const auto connection = qScopeGuard([this] { repository_->closeCurrentThreadConnection(); });
        auto result = work(cancellation);
        QMetaObject::invokeMethod(this, [this, receiver, generation, lane, cancellation,
                                            result = std::move(result), done = std::move(done)]() mutable {
            if (generation != generation_)
                return;
            if (lane == Lane::Api)
                --pendingApi_;
            else if (lane == Lane::Peer)
                --pendingPeer_;
            if (receiver && !cancellation->load(std::memory_order_relaxed))
                done(std::move(result));
        }, Qt::QueuedConnection);
    });
    return true;
}

bool SearchService::searchAsync(QObject* receiver, Request request, Mode mode, Lane lane,
    std::function<void(Results)> completed)
{
    return submit(receiver, lane, [this, request = std::move(request), mode](const auto& cancellation) {
        Results results;
        if (mode != Mode::Files && !cancellation->load(std::memory_order_relaxed))
            results.torrents = searchTorrents(request, cancellation);
        if (mode != Mode::Torrents && !cancellation->load(std::memory_order_relaxed))
            results.files = searchFiles(request, cancellation);
        return results;
    }, std::move(completed));
}

bool SearchService::exportAsync(QObject* receiver, ExportWork work,
    std::function<void(QString)> completed)
{
    return submit(receiver, Lane::Api, [this, work = std::move(work)](const auto& cancellation) {
        const auto cancelled = [&] { return cancellation->load(std::memory_order_relaxed); };
        return cancelled() ? QString() : work(*repository_, cancelled);
    }, std::move(completed));
}

bool SearchService::getAsync(QObject* receiver, QString hash, bool includeFiles, Lane lane,
    std::function<void(std::optional<domain::Torrent>)> completed)
{
    return submit(receiver, lane, [this, hash = std::move(hash), includeFiles](const auto& cancellation) {
        return cancellation->load(std::memory_order_relaxed) ? std::optional<domain::Torrent>()
            : repository_->get(hash, includeFiles);
    }, std::move(completed));
}

QString SearchService::extractInfoHash(const QString& query)
{
    const QString text = query.trimmed();
    if (infohash::isValid(text))
        return infohash::normalize(text);
    if (!text.startsWith(QLatin1String("magnet:?"), Qt::CaseInsensitive))
        return {};
    const QLatin1String prefix("urn:btih:");
    const QUrlQuery magnet{ QUrl(text) };
    for (const auto& [key, value] : magnet.queryItems(QUrl::FullyDecoded)) {
        if (key.compare(QLatin1String("xt"), Qt::CaseInsensitive) == 0 && value.startsWith(prefix, Qt::CaseInsensitive)) {
            const QString hash = value.mid(prefix.size());
            if (infohash::isValid(hash))
                return infohash::normalize(hash);
        }
    }
    return {};
}

QString SearchService::queryError(const QString& query, bool requireLongQuery)
{
    if (!requireLongQuery && !extractInfoHash(query).isEmpty())
        return {};

    using data::FullTextQuery;
    switch (FullTextQuery::parse(query).error()) {
    case FullTextQuery::Error::UnclosedQuote:
        return tr("Close the quotation marks to search for a phrase.");
    case FullTextQuery::Error::EmptyPhrase:
        return tr("Enter text between the quotation marks.");
    case FullTextQuery::Error::None:
        break;
    }

    if (requireLongQuery && query.size() <= 2 && !FullTextQuery::allowsShortQuery(query))
        return tr("Query too short");
    return {};
}

bool SearchService::canSendLegacyQuery(const QString& query)
{
    const auto parsed = data::FullTextQuery::parse(query);
    return query.size() > 2 && parsed.error() == data::FullTextQuery::Error::None && !parsed.isEmpty()
        && !parsed.hasExactPhrase() && parsed.continuousTerms().isEmpty();
}

data::TorrentRepository::SearchQuery SearchService::toRepositoryQuery(const Request& r)
{
    data::TorrentRepository::SearchQuery q;
    q.text = r.query;
    q.offset = r.offset;
    q.limit = r.limit;
    q.sort = r.sort;
    q.descending = r.descending;
    q.safeSearch = r.safeSearch;
    q.fuzzy = r.fuzzy;
    q.contentType = r.contentType;
    q.sizeMin = r.sizeMin;
    q.sizeMax = r.sizeMax;
    q.filesMin = r.filesMin;
    q.filesMax = r.filesMax;
    return q;
}

QVector<domain::SearchHit> SearchService::searchTorrents(const Request& request,
    const data::TorrentRepository::SearchCancellation& cancellation)
{
    common::PerformanceTimer timer(QStringLiteral("search.torrents_ms"));
    auto query = toRepositoryQuery(request);
    query.cancellation = cancellation;
    const auto hash = extractInfoHash(request.query);
    if (!hash.isEmpty())
        query.text = hash;
    return repository_->searchTorrents(query);
}

QVector<domain::SearchHit> SearchService::searchFiles(const Request& request,
    const data::TorrentRepository::SearchCancellation& cancellation)
{
    common::PerformanceTimer timer(QStringLiteral("search.files_ms"));
    auto query = toRepositoryQuery(request);
    query.cancellation = cancellation;
    if (query.fuzzy && !extractInfoHash(request.query).isEmpty())
        query.fuzzy = false;
    return repository_->searchFiles(query);
}

QVector<domain::Torrent> SearchService::recent(int limit)
{
    return repository_->recent(limit);
}

QVector<domain::Torrent> SearchService::top(const QString& type, const QString& time, int offset, int limit)
{
    return repository_->top(type, time, offset, limit);
}

std::optional<domain::Torrent> SearchService::get(const QString& hash, bool includeFiles)
{
    return repository_->get(hash, includeFiles);
}

} // namespace rats::service
