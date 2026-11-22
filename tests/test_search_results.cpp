#include <QtTest/QtTest>

#include "ui/searchresultmodel.h"

class TestSearchResults : public QObject {
    Q_OBJECT

private slots:
    void strictBeforeFuzzyInBothSortDirections();
    void strictFileHitPromotesFuzzyResult();
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

QTEST_GUILESS_MAIN(TestSearchResults)
#include "test_search_results.moc"
