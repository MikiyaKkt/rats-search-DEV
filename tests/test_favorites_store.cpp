#include "app/favorites_store.h"
#include "app/favorites_xlsx.h"
#include "domain/torrent_codec.h"

#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTimeZone>
#include <QtTest/QtTest>

using rats::app::FavoritesStore;
using rats::domain::Torrent;

namespace {
Torrent sample(QChar hash, const QString& title)
{
    Torrent torrent;
    torrent.hash = QString(40, hash);
    torrent.name = title;
    torrent.files = 1;
    return torrent;
}

QByteArray readFile(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}
} // namespace

class TestFavoritesStore : public QObject {
    Q_OBJECT

private slots:
    void oldFileAndBatchOperations();
    void exportOptionalFields();
    void failedMutationKeepsData();
    void exportExcel();
};

void TestFavoritesStore::oldFileAndBatchOperations()
{
    QTemporaryDir dir;
    const auto original = sample(u'a', QStringLiteral("\u65e7\u6536\u85cf"));
    const auto second = sample(u'b', "Second");
    const auto third = sample(u'c', "Third");
    const qint64 date = 1700000000123LL;
    {
        QFile file(dir.filePath("favorites.json"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        auto json = rats::domain::codec::toJson(original);
        json["favoritedAt"] = date;
        QVERIFY(file.write(QJsonDocument(QJsonArray{ json }).toJson()) > 0);
    }
    const auto folder = QStringLiteral("\u7535\u5f71");
    FavoritesStore store(dir.path());
    QCOMPARE(store.favorites().size(), 1);
    QCOMPARE(store.favorites().first().favoritedAt.toMSecsSinceEpoch(), date);
    QVERIFY(store.favorites().first().folder.isEmpty());
    QVERIFY(store.createFolder(folder));
    QVERIFY(store.createFolder("Empty"));
    QVERIFY(!store.createFolder("empty"));
    QSignalSpy changed(&store, &FavoritesStore::favoritesChanged);
    QCOMPARE(store.addMany({ original, second, third, third }, folder), 2);
    QCOMPARE(changed.size(), 1);
    QVERIFY(store.isFavorite(second.hash.toUpper()));
    QVERIFY(store.renameFolder(folder, "Renamed"));
    QCOMPARE(store.moveMany({ original.hash.toUpper(), second.hash }, "Empty"), 2);

    FavoritesStore reloaded(dir.path());
    QCOMPARE(reloaded.folders(), QStringList({ "Renamed", "Empty" }));
    QCOMPARE(reloaded.favorites().size(), 3);
    QCOMPARE(reloaded.favorites()[0].folder, QString("Empty"));
    QCOMPARE(reloaded.favorites()[2].folder, QString("Renamed"));
    QCOMPARE(reloaded.favorites()[0].favoritedAt.toMSecsSinceEpoch(), date);
    QVERIFY(reloaded.removeFolder("Empty"));
    QVERIFY(reloaded.favorites()[0].folder.isEmpty());
    QCOMPARE(reloaded.removeMany({ second.hash, third.hash }), 2);
    FavoritesStore afterRemoval(dir.path());
    QCOMPARE(afterRemoval.favorites().size(), 1);
    QVERIFY(afterRemoval.isFavorite(original.hash));
    QCOMPARE(afterRemoval.folders(), QStringList{ "Renamed" });
}

void TestFavoritesStore::exportOptionalFields()
{
    QTemporaryDir dir;
    auto known = sample(u'a', QStringLiteral("\u7535\u5f71"));
    known.fileList.append({ "folder/video.mkv", 1024 });
    const auto indexOnly = sample(u'b', "In index");
    const auto remoteOnly = sample(u'c', "Remote");
    FavoritesStore store(dir.path());
    QVERIFY(store.createFolder("Video"));
    QCOMPARE(store.addMany({ known, indexOnly, remoteOnly }, "Video"), 3);

    FavoritesStore reloaded(dir.path());
    const auto entries = reloaded.favorites();
    QCOMPARE(entries.first().torrent.fileList.size(), 1);
    int lookups = 0;
    const auto filePath = dir.filePath("export.json");
    const auto lookup = [&](const QString& hash) -> QVector<rats::domain::File> {
        ++lookups;
        return hash == indexOnly.hash ? QVector<rats::domain::File>{ { "local.txt", 10 } }
                                      : QVector<rats::domain::File>();
    };
    const auto keepGoing = [] { return false; };
    QVERIFY(FavoritesStore::exportToFile(filePath, entries, { true, true }, lookup, keepGoing).isEmpty());
    auto rows = QJsonDocument::fromJson(readFile(filePath)).object()["favorites"].toArray();
    QCOMPARE(rows.size(), 3);
    const auto first = rows[0].toObject();
    QCOMPARE(first["title"].toString(), known.name);
    QCOMPARE(first["magnet"].toString(), known.magnetLink());
    QCOMPARE(first["folder"].toString(), QString("Video"));
    QVERIFY(QDateTime::fromString(first["favoritedAt"].toString(), Qt::ISODateWithMs).isValid());
    QCOMPARE(first["files"].toArray().first().toObject()["path"].toString(), QString("folder/video.mkv"));
    QCOMPARE(rows[1].toObject()["files"].toArray().size(), 1);
    QVERIFY(rows[2].toObject()["files"].isNull());
    QCOMPARE(lookups, 2);

    QVERIFY(FavoritesStore::exportToFile(filePath, { entries.last() }, {}, lookup, keepGoing).isEmpty());
    const auto saved = readFile(filePath);
    rows = QJsonDocument::fromJson(saved).object()["favorites"].toArray();
    QCOMPARE(rows.size(), 1);
    QVERIFY(!rows.first().toObject().contains("files"));
    QVERIFY(!rows.first().toObject().contains("favoritedAt"));
    QCOMPARE(lookups, 2);
    QVERIFY(!FavoritesStore::exportToFile(filePath, entries, {}, lookup, [] { return true; }).isEmpty());
    QCOMPARE(readFile(filePath), saved);
}

void TestFavoritesStore::failedMutationKeepsData()
{
    QTemporaryDir dir;
    const auto path = dir.filePath("favorites.json");
    {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QVERIFY(file.write("interrupted legacy write") > 0);
    }
    const auto stored = readFile(path);
    FavoritesStore store(dir.path());
    QSignalSpy failed(&store, &FavoritesStore::saveFailed);
    QSignalSpy changed(&store, &FavoritesStore::favoritesChanged);
    QVERIFY(!store.add(sample(u'd', "Protected")));
    QCOMPARE(failed.size(), 1);
    QVERIFY(changed.isEmpty());
    QCOMPARE(readFile(path), stored);
}

void TestFavoritesStore::exportExcel()
{
    QTemporaryDir dir;
    auto known = sample(u'a', QStringLiteral("=\u6536\u85cf & <tag> _x000A_"));
    known.fileList.append({ QStringLiteral("\u7535\u5f71/\u96ea.mkv"), 2048 });
    const auto unknown = sample(u'b', QStringLiteral("Other"));
    const auto date = QDateTime::fromMSecsSinceEpoch(1700000000123LL, QTimeZone::utc());
    const QVector<FavoritesStore::Entry> entries{ { known, date, "Video" }, { unknown, date, {} } };
    const auto lookup = [](const QString&) { return QVector<rats::domain::File>(); };
    const auto working = [] { return false; };
    const auto path = qEnvironmentVariable("RATS_FAVORITES_XLSX_FIXTURE", dir.filePath("export.xlsx"));
    QVERIFY(rats::app::exportFavoritesXlsx(path, entries, { true, true }, lookup, working).isEmpty());
    const auto saved = readFile(path);
    QVERIFY(saved.startsWith("PK\x03\x04"));
    QVERIFY(saved.contains("xl/worksheets/sheet2.xml"));
    QVERIFY(saved.contains(unknown.magnetLink().toHtmlEscaped().toUtf8()));
    QVERIFY(saved.contains(QStringLiteral("\u7535\u5f71/\u96ea.mkv").toUtf8()));
    QVERIFY(saved.contains("_x005F_x000A_"));
    QVERIFY(saved.contains("Unavailable"));
    QVERIFY(!rats::app::exportFavoritesXlsx(path, entries, {}, lookup, [] { return true; }).isEmpty());
    QCOMPARE(readFile(path), saved);

    const auto basicPath = dir.filePath("basic.xlsx");
    QVERIFY(rats::app::exportFavoritesXlsx(basicPath, entries, {}, lookup, working).isEmpty());
    QVERIFY(!readFile(basicPath).contains("xl/worksheets/sheet2.xml"));
    QVERIFY(!readFile(basicPath).contains("Added to Favorites (UTC)"));
}

QTEST_GUILESS_MAIN(TestFavoritesStore)
#include "test_favorites_store.moc"
