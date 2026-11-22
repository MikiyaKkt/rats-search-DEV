#include "services/search_service.h"

#include "common/infohash.h"
#include "data/query.h"

#include <QUrl>
#include <QUrlQuery>

namespace rats::service {

SearchService::SearchService(data::TorrentRepository* repository, QObject* parent)
    : QObject(parent), repository_(repository)
{
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

QVector<domain::SearchHit> SearchService::searchTorrents(const Request& request)
{
    auto query = toRepositoryQuery(request);
    const QString hash = extractInfoHash(request.query);
    if (!hash.isEmpty())
        query.text = hash;
    return repository_->searchTorrents(query);
}

QVector<domain::SearchHit> SearchService::searchFiles(const Request& request)
{
    auto query = toRepositoryQuery(request);
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
