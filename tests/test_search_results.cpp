#include <QtTest/QtTest>

#include "app/favorites_store.h"
#include "ui/searchresultmodel.h"

class TestSearchResults : public QObject {
    Q_OBJECT

private slots:
    void strictBeforeFuzzyInBothSortDirections();
    void strictFileHitPromotesFuzzyResult();
    void batchMergeKeepsSelectionAfterSorting();
    void filterTracksBatchFavoritesAndStreamedHits();
};

using rats::domain::SearchHit;

void TestSearchResults::strictBeforeFuzzyInBothSortDirections()
{
    SearchHit strict;
    strict.torrent.hash = "strict";
    strict.torrent.name = "Ordinary";
    strict.torrent.seeders = 10;
    SearchHit fuzzy;
    fuzzy.torrent.hash = "fuzzy";
    fuzzy.torrent.name = "Correction";
    fuzzy.torrent.seeders = 50;
    fuzzy.fuzzy = true;

    SearchResultModel model;
    model.setResults({ fuzzy, strict });
    for (Qt::SortOrder order : { Qt::AscendingOrder, Qt::DescendingOrder }) {
        model.sort(SearchResultModel::SeedersColumn, order);
        QCOMPARE(model.getHit(0).torrent.hash, strict.torrent.hash);
        QCOMPARE(model.getHit(1).torrent.hash, fuzzy.torrent.hash);
    }
    QCOMPARE(model.data(model.index(1, SearchResultModel::NameColumn)).toString(), QString("[Fuzzy] Correction"));
}

void TestSearchResults::strictFileHitPromotesFuzzyResult()
{
    SearchHit strict;
    strict.torrent.hash = "strict";
    strict.torrent.seeders = 10;
    SearchHit fuzzy;
    fuzzy.torrent.hash = "fuzzy";
    fuzzy.torrent.seeders = 50;
    fuzzy.fuzzy = true;
    fuzzy.fromFileMatch = true;
    fuzzy.matchingPaths = { "path/<b>correction</b>.mkv" };

    SearchResultModel model;
    model.sort(SearchResultModel::SeedersColumn, Qt::DescendingOrder);
    model.setResults({ strict, fuzzy });
    SearchHit sameHash = fuzzy;
    sameHash.fuzzy = false;
    sameHash.matchingPaths = { "path/<b>exact</b>.mkv" };
    model.addFileResult(sameHash);
    model.addFileResult(fuzzy);
    model.addFileResult(sameHash);
    QCOMPARE(model.rowCount(), 2);
    QCOMPARE(model.getHit(0).torrent.hash, sameHash.torrent.hash);
    QVERIFY(!model.getHit(0).fuzzy);
    QCOMPARE(model.getHit(0).matchingPaths, sameHash.matchingPaths);
}

void TestSearchResults::batchMergeKeepsSelectionAfterSorting()
{
    SearchHit saved;
    saved.torrent.hash = "saved";
    saved.torrent.seeders = 5;
    SearchHit streamed;
    streamed.torrent.hash = "streamed";
    streamed.torrent.seeders = 20;
    streamed.remote = true;
    auto local = streamed;
    local.remote = false;
    local.fromFileMatch = true;
    local.matchingPaths = { "path/<b>local</b>.mp4" };

    SearchResultModel model;
    QItemSelectionModel selection(&model);
    model.sort(SearchResultModel::SeedersColumn, Qt::DescendingOrder);
    model.addResults({ saved });
    selection.select(model.index(0, 0), QItemSelectionModel::Select | QItemSelectionModel::Rows);
    model.addResults({ streamed, local, streamed, local });
    QCOMPARE(model.rowCount(), 2);
    QCOMPARE(model.getHit(0).matchingPaths, local.matchingPaths);
    QVERIFY(!model.getHit(0).remote);
    QCOMPARE(selection.selectedRows().size(), 1);
    QCOMPARE(selection.selectedRows().front().data(SearchResultModel::HashRole).toString(), saved.torrent.hash);
}

void TestSearchResults::filterTracksBatchFavoritesAndStreamedHits()
{
    QTemporaryDir dir;
    rats::app::FavoritesStore favorites(dir.path());
    SearchResultModel model;
    SearchResultsFilter proxy(&favorites);
    proxy.setSourceModel(&model);
    QItemSelectionModel selection(&proxy);

    SearchHit first;
    first.torrent.hash = QString(40, u'a');
    first.torrent.seeders = 10;
    SearchHit saved;
    saved.torrent.hash = QString(40, u'b');
    saved.torrent.seeders = 20;
    SearchHit streamed;
    streamed.torrent.hash = QString(40, u'c');
    streamed.torrent.seeders = 30;
    model.addResults({ first, saved });
    proxy.sort(SearchResultModel::SeedersColumn, Qt::AscendingOrder);
    selection.select(proxy.index(0, 0), QItemSelectionModel::Select | QItemSelectionModel::Rows);
    selection.select(proxy.index(1, 0), QItemSelectionModel::Select | QItemSelectionModel::Rows);

    proxy.setHideFavorites(true);
    QCOMPARE(favorites.addMany({ saved.torrent, streamed.torrent }), 2);
    QCOMPARE(proxy.rowCount(), 1);
    QCOMPARE(selection.selectedRows().size(), 1);
    QCOMPARE(selection.selectedRows().first().data(SearchResultModel::HashRole).toString(), first.torrent.hash);
    model.addResults({ streamed });
    QCOMPARE(proxy.rowCount(), 1);

    QVERIFY(favorites.remove(saved.torrent.hash));
    QCOMPARE(proxy.rowCount(), 2);
    QCOMPARE(proxy.index(1, 0).data(SearchResultModel::HashRole).toString(), saved.torrent.hash);
    proxy.setHideFavorites(false);
    QCOMPARE(proxy.rowCount(), 3);
}

QTEST_GUILESS_MAIN(TestSearchResults)
#include "test_search_results.moc"
