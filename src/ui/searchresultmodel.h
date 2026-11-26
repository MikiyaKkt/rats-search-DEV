#ifndef SEARCHRESULTMODEL_H
#define SEARCHRESULTMODEL_H

#include "domain/torrent.h"
#include <QAbstractTableModel>
#include <QHash>
#include <QSortFilterProxyModel>
#include <QVector>

namespace rats::app {
class FavoritesStore;
}

/**
 * @brief SearchResultModel - Table model for displaying search results
 * Supports both torrent search results and file search results with highlighted
 * paths. Rows are stored as rats::domain::SearchHit (torrent + how it was
 * matched).
 */
class SearchResultModel : public QAbstractTableModel {
    Q_OBJECT

public:
    enum Column { NameColumn = 0, SizeColumn, SeedersColumn, LeechersColumn, DateColumn, ColumnCount };

    // Custom data roles, read by TorrentItemDelegate.
    enum DataRole {
        ContentTypeRole = Qt::UserRole + 1, // domain::ContentType id
        MatchingPathsRole = Qt::UserRole + 2, // QStringList of highlighted file paths
        RemoteRole = Qt::UserRole + 3, // true if the hit came from a peer, not the local index
        HashRole = Qt::UserRole + 4
    };

    explicit SearchResultModel(QObject* parent = nullptr);
    ~SearchResultModel();

    // QAbstractTableModel interface
    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;
    void sort(int column, Qt::SortOrder order = Qt::AscendingOrder) override;

    // Custom methods for torrent results
    void setResults(const QVector<rats::domain::SearchHit>& results);
    void addResult(const rats::domain::SearchHit& result);
    void addResults(const QVector<rats::domain::SearchHit>& results);
    void clearResults();

    // File search results methods
    void addFileResult(const rats::domain::SearchHit& result);
    void addFileResults(const QVector<rats::domain::SearchHit>& results);

    // Access methods
    rats::domain::Torrent getTorrent(int row) const;
    // Full hit including remote provenance (source peer), needed to fetch a
    // remote torrent's file list on demand.
    rats::domain::SearchHit getHit(int row) const;
    int resultCount() const { return results_.size(); }

private:
    void applyCurrentSort();

    QVector<rats::domain::SearchHit> results_;
    QHash<QString, int> rowByHash_;
    int sortColumn_ = -1;
    Qt::SortOrder sortOrder_ = Qt::DescendingOrder;
};

class SearchResultsFilter : public QSortFilterProxyModel {
public:
    explicit SearchResultsFilter(rats::app::FavoritesStore* favorites, QObject* parent = nullptr);
    void setHideFavorites(bool enabled);
    void sort(int column, Qt::SortOrder order) override;

protected:
    bool filterAcceptsRow(int row, const QModelIndex& parent) const override;

private:
    void updateFilter(bool enabled);
    rats::app::FavoritesStore* favorites_;
    bool hideFavorites_ = false;
};

#endif // SEARCHRESULTMODEL_H
