#include "app/data_relocation.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest/QtTest>

namespace relocation = rats::app::relocation;

class TestDataRelocation : public QObject {
    Q_OBJECT
private slots:
    void wholeProfileAndRetry();
    void rejectsNonemptyAndCancels();
};

static bool write(const QString& path, const QByteArray& data)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
}

static QByteArray read(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

void TestDataRelocation::wholeProfileAndRetry()
{
    QTemporaryDir directory;
    const auto source = directory.filePath("old");
    const auto target = directory.filePath(QStringLiteral("\u65b0 database"));
    QVERIFY(QDir().mkpath(QDir(source).filePath("database/sub")));
    QVERIFY(QDir().mkpath(target));
    const QByteArray bytes(3 * 1024 * 1024 + 2, 'd');
    QVERIFY(write(QDir(source).filePath("database/sub/table.ram"), bytes));
    QVERIFY(write(QDir(source).filePath("binlog.meta"), "journal"));
    QVERIFY(write(QDir(source).filePath("favorites.json"), "favorites"));
    QVERIFY(write(QDir(source).filePath("sphinx.conf"), "old absolute paths"));
    QVERIFY(write(QDir(source).filePath("rats-search.log"), "generated"));
    QVERIFY(relocation::schedule(source, target).isEmpty());
    const auto request = relocation::pending(source);
    QVERIFY(!request.token.isEmpty());
    qint64 copied = 0;
    QVERIFY(relocation::prepare(source, request, [&](qint64 done, qint64) { copied = done; return true; }).isEmpty());
    QVERIFY(copied > bytes.size());
    QCOMPARE(read(QDir(target).filePath("database/sub/table.ram")), bytes);
    QCOMPARE(read(QDir(target).filePath("binlog.meta")), QByteArray("journal"));
    QCOMPARE(read(QDir(target).filePath("favorites.json")), QByteArray("favorites"));
    QVERIFY(!QFile::exists(QDir(target).filePath("sphinx.conf")));
    QVERIFY(!QFile::exists(QDir(target).filePath("rats-search.log")));
    QCOMPARE(read(QDir(source).filePath("database/sub/table.ram")), bytes);
    QVERIFY(relocation::prepare(source, request, [](qint64, qint64) { return false; }).isEmpty());
    relocation::finish(source, request);
    QVERIFY(relocation::pending(source).token.isEmpty());
    QVERIFY(!relocation::prepare(source, request, [](qint64, qint64) { return true; }).isEmpty());
}

void TestDataRelocation::rejectsNonemptyAndCancels()
{
    QTemporaryDir directory;
    const auto source = directory.filePath("src");
    const auto target = directory.filePath("dst");
    QVERIFY(QDir().mkpath(QDir(source).filePath("database")));
    QVERIFY(QDir().mkpath(target));
    const auto marker = QDir(target).filePath("existing");
    QVERIFY(write(marker, "preserve"));
    QVERIFY(!relocation::schedule(source, target).isEmpty());
    QCOMPARE(read(marker), QByteArray("preserve"));
    QVERIFY(!relocation::schedule(source, QDir(source).filePath("nested")).isEmpty());
    QVERIFY(!relocation::schedule(source, directory.path()).isEmpty());
    QVERIFY(QFile::remove(marker));
    QVERIFY(write(QDir(source).filePath("database/table"), "source"));
    QVERIFY(relocation::schedule(source, target).isEmpty());
    QVERIFY(!relocation::prepare(source, relocation::pending(source),
        [](qint64 done, qint64) { return done == 0; }).isEmpty());
    QCOMPARE(QDir(target).entryList(QDir::Files | QDir::Hidden).size(), 0);
    QCOMPARE(read(QDir(source).filePath("database/table")), QByteArray("source"));
    QVERIFY(relocation::clear(source).isEmpty());
}

QTEST_GUILESS_MAIN(TestDataRelocation)
#include "test_data_relocation.moc"
