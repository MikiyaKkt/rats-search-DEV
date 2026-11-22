#include "searchresultmodel.h"
#include "domain/content.h"
#include "format.h"
#include <QDateTime>
#include <QHash>
#include <QPair>
#include <algorithm>

using rats::domain::SearchHit;
using rats::domain::Torrent;

SearchResultModel::SearchResultModel(QObject* parent) : QAbstractTableModel(parent) { }

SearchResultModel::~SearchResultModel() { }

int SearchResultModel::rowCount(const QModelIndex& parent) const
{
    if (parent.isValid()) {
        return 0;
    }
    return results_.size();
}

int SearchResultModel::columnCount(const QModelIndex& parent) const
{
    if (parent.isValid()) {
        return 0;
    }
    return ColumnCount;
}

QVariant SearchResultModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() >= results_.size()) {
        return QVariant();
    }

    const SearchHit& hit = results_[index.row()];
    const Torrent& torrent = hit.torrent;

    if (role == Qt::DisplayRole) {
        switch (index.column()) {
        case NameColumn:
            return hit.fuzzy ? tr("[Fuzzy] %1").arg(torrent.name) : torrent.name;
        case SizeColumn:
            return rats::ui::formatSize(torrent.size);
        case SeedersColumn:
            return torrent.seeders;
        case LeechersColumn:
            return torrent.leechers;
        case DateColumn:
            return rats::ui::formatDate(torrent.added);
        default:
            return QVariant();
        }
    } else if (role == Qt::TextAlignmentRole) {
        switch (index.column()) {
        case SeedersColumn:
        case LeechersColumn:
            return Qt::AlignCenter;
        case SizeColumn:
            return Qt::AlignRight;
        default:
            return Qt::AlignLeft;
        }
    } else if (role == Qt::ToolTipRole) {
        QString tip = QString("Info Hash: %1\nSeeders: %2\nLeechers: %3\nSize: %4\nFiles: %5")
                          .arg(torrent.hash)
                          .arg(torrent.seeders)
                          .arg(torrent.leechers)
                          .arg(rats::ui::formatSize(torrent.size))
                          .arg(torrent.files);
        // The row is tinted for these; say why, and name the peer it came from.
        if (hit.remote) {
            tip += QLatin1Char('\n')
                + (hit.sourcePeerId.isEmpty() ? tr("Not indexed locally — found on a remote peer")
                                              : tr("Not indexed locally — from peer %1").arg(hit.sourcePeerId.left(8)));
        }
        if (hit.fuzzy)
            tip += u'\n' + tr("Fuzzy match");
        return tip;
    }
    // Custom roles for delegate
    else if (role == ContentTypeRole) {
        // Content type as domain enum id (delegate converts back to ContentType)
        return rats::domain::toId(torrent.contentType);
    } else if (role == MatchingPathsRole) {
        // Highlighted file path snippets
        return hit.matchingPaths;
    } else if (role == RemoteRole) {
        return hit.remote;
    }

    return QVariant();
}

QVariant SearchResultModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) {
        return QVariant();
    }

    switch (section) {
    case NameColumn:
        return tr("Name");
    case SizeColumn:
        return tr("Size");
    case SeedersColumn:
        return tr("Seeders");
    case LeechersColumn:
        return tr("Leechers");
    case DateColumn:
        return tr("Date");
    default:
        return QVariant();
    }
}

void SearchResultModel::setResults(const QVector<SearchHit>& results)
{
    beginResetModel();
    results_ = results;
    endResetModel();
    applyCurrentSort();
}

void SearchResultModel::addResult(const SearchHit& result)
{
    for (int i = 0; i < results_.size(); ++i) {
        SearchHit& current = results_[i];
        if (current.torrent.hash != result.torrent.hash)
            continue;
        const bool promoted = current.fuzzy && !result.fuzzy;
        bool changed = promoted;
        if (promoted) {
            current.fuzzy = false;
            current.fromFileMatch = false;
            current.matchingPaths.clear();
        }
        if (current.fuzzy == result.fuzzy) {
            if (result.fromFileMatch && !current.fromFileMatch) {
                current.fromFileMatch = true;
                changed = true;
            }
            for (const QString& path : result.matchingPaths) {
                if (!current.matchingPaths.contains(path)) {
                    current.matchingPaths.append(path);
                    changed = true;
                }
            }
        }
        // A match from this instance means the torrent is available locally.
        if (current.remote && !result.remote) {
            current.remote = false;
            current.sourcePeerId.clear();
            changed = true;
        }
        if (changed) {
            emit dataChanged(index(i, 0), index(i, ColumnCount - 1),
                { Qt::DisplayRole, MatchingPathsRole, RemoteRole, Qt::ToolTipRole, Qt::SizeHintRole });
        }
        if (promoted)
            applyCurrentSort();
        return;
    }

    beginInsertRows(QModelIndex(), results_.size(), results_.size());
    results_.append(result);
    endInsertRows();
    applyCurrentSort();
}

void SearchResultModel::clearResults()
{
    beginResetModel();
    results_.clear();
    endResetModel();
}

Torrent SearchResultModel::getTorrent(int row) const
{
    if (row >= 0 && row < results_.size()) {
        return results_[row].torrent;
    }
    return Torrent();
}

SearchHit SearchResultModel::getHit(int row) const
{
    if (row >= 0 && row < results_.size()) {
        return results_[row];
    }
    return SearchHit();
}

// =========================================================================
// File Search Results Methods
// =========================================================================

void SearchResultModel::addFileResult(const SearchHit& result)
{
    addResult(result);
}

void SearchResultModel::addFileResults(const QVector<SearchHit>& results)
{
    for (const SearchHit& result : results)
        addResult(result);
}

void SearchResultModel::sort(int column, Qt::SortOrder order)
{
    if (column < 0 || column >= ColumnCount) {
        return;
    }

    sortColumn_ = column;
    sortOrder_ = order;
    applyCurrentSort();
}

void SearchResultModel::applyCurrentSort()
{
    if (results_.isEmpty() || (sortColumn_ < 0
            && std::none_of(results_.cbegin(), results_.cend(), [](const SearchHit& hit) { return hit.fuzzy; }))) {
        return;
    }

    const int column = sortColumn_;
    const Qt::SortOrder order = sortOrder_;

    emit layoutAboutToBeChanged();

    // Capture persistent indexes by hash so we can remap them after sorting
    const QModelIndexList oldPersistent = persistentIndexList();
    QVector<QPair<QPersistentModelIndex, QString>> persistentByHash;
    persistentByHash.reserve(oldPersistent.size());
    for (const QModelIndex& idx : oldPersistent) {
        if (idx.row() >= 0 && idx.row() < results_.size()) {
            persistentByHash.append({ QPersistentModelIndex(idx), results_[idx.row()].torrent.hash });
        }
    }

    auto lessThan = [column](const SearchHit& a, const SearchHit& b) {
        switch (column) {
        case NameColumn:
            return a.torrent.name.localeAwareCompare(b.torrent.name) < 0;
        case SizeColumn:
            return a.torrent.size < b.torrent.size;
        case SeedersColumn:
            return a.torrent.seeders < b.torrent.seeders;
        case LeechersColumn:
            return a.torrent.leechers < b.torrent.leechers;
        case DateColumn:
            return a.torrent.added < b.torrent.added;
        }
        return false;
    };

    std::stable_sort(results_.begin(), results_.end(), [&](const SearchHit& a, const SearchHit& b) {
        if (a.fuzzy != b.fuzzy)
            return !a.fuzzy;
        return order == Qt::AscendingOrder ? lessThan(a, b) : lessThan(b, a);
    });

    // Rebuild a hash -> new row map to remap persistent indexes
    QHash<QString, int> rowByHash;
    rowByHash.reserve(results_.size());
    for (int i = 0; i < results_.size(); ++i) {
        rowByHash.insert(results_[i].torrent.hash, i);
    }
    for (const auto& pair : persistentByHash) {
        const int newRow = rowByHash.value(pair.second, -1);
        if (newRow >= 0) {
            changePersistentIndex(pair.first, index(newRow, pair.first.column()));
        } else {
            changePersistentIndex(pair.first, QModelIndex());
        }
    }

    emit layoutChanged();
}
