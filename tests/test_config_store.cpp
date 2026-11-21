#include "app/config_store.h"
#include "app/application.h"
#include "net/crawler.h"
#include "rest/api_router.h"

#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

using rats::app::ConfigStore;

// ConfigStore is the only write path for settings, and Application::applyConfig()
// hangs off configChanged(). These tests pin that contract: every successful
// write notifies, whichever entry point produced it (typed setter from the
// settings dialog, or fromJson from the `config.set` API method).
class TestConfigStore : public QObject {
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void typedSetter_emitsConfigChanged();
    void typedSetter_nestedKey_emitsConfigChanged();
    void unchangedValue_doesNotEmit();
    void fromJson_emitsOneBatchedSignal();
    void p2pConnections_isClamped();
    void replicationImpliesReplicationServer();
    void disablingReplicationServer_disablesReplication();
    void languageAndDarkMode_haveDedicatedSignals();
    void uiFont_defaultsAndClamping();
    void saveThenLoad_roundTrips();
    void spiderLimits_defaultsClampAndNotify();
    void spiderLimits_legacyAndMalformedLoad();
    void spiderLimits_apiAppliesToServicesAndStats();

private:
    QTemporaryDir* dir_ = nullptr;
    ConfigStore* config_ = nullptr;
    QString path_ = {};
};

void TestConfigStore::init()
{
    dir_ = new QTemporaryDir();
    QVERIFY(dir_->isValid());
    path_ = dir_->path() + "/rats.json";
    config_ = new ConfigStore(path_);
}

void TestConfigStore::spiderLimits_defaultsClampAndNotify()
{
    QCOMPARE(config_->spiderMetadataConcurrency(), 10);
    QCOMPARE(config_->spiderPendingLimit(), 1000);
    QCOMPARE(config_->spiderBatchSize(), 100);
    QSignalSpy changed(config_, &ConfigStore::configChanged);
    config_->setSpiderMetadataConcurrency(20);
    QCOMPARE(changed.count(), 1);
    QCOMPARE(changed.first().first().toStringList(), QStringList{"spider.metadataConcurrency"});
    config_->setSpiderMetadataConcurrency(1000); QCOMPARE(config_->spiderMetadataConcurrency(), 64);
    changed.clear(); config_->setSpiderMetadataConcurrency(65); QCOMPARE(changed.count(), 0);
    config_->setSpiderMetadataConcurrency(-5); QCOMPARE(config_->spiderMetadataConcurrency(), 1);
    config_->setValue("spider.metadataConcurrency", "invalid"); QCOMPARE(config_->spiderMetadataConcurrency(), 10);
    config_->setValue("spider.metadataConcurrency", 2.5); QCOMPARE(config_->spiderMetadataConcurrency(), 10);
    config_->setValue("spider.metadataConcurrency", true); QCOMPARE(config_->spiderMetadataConcurrency(), 10);
    changed.clear();
    config_->fromJson({{"spider", QJsonObject{{"metadataConcurrency",40},{"pendingLimit",0},{"batchSize",500}}}});
    QCOMPARE(changed.count(), 1); QCOMPARE(config_->spiderMetadataConcurrency(), 40);
    QCOMPARE(config_->spiderPendingLimit(), 1); QCOMPARE(config_->spiderBatchSize(), 100);
    QVERIFY(config_->save()); ConfigStore loaded(path_); QVERIFY(loaded.load());
    QCOMPARE(loaded.spiderMetadataConcurrency(), 40); QCOMPARE(loaded.spiderPendingLimit(), 1);
}

void TestConfigStore::spiderLimits_legacyAndMalformedLoad()
{
    QFile file(path_); QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(R"({"spider":{"walkInterval":250}})"); file.close();
    QVERIFY(config_->load()); QCOMPARE(config_->spiderWalkInterval(), 250);
    QCOMPARE(config_->spiderMetadataConcurrency(), 10); QCOMPARE(config_->spiderBatchSize(), 100);
    QVERIFY(file.open(QIODevice::WriteOnly|QIODevice::Truncate));
    file.write(R"({"spider":{"metadataConcurrency":"bad","pendingLimit":-10,"batchSize":2.5}})"); file.close();
    QVERIFY(config_->load()); QCOMPARE(config_->spiderMetadataConcurrency(), 10);
    QCOMPARE(config_->spiderPendingLimit(), 1); QCOMPARE(config_->spiderBatchSize(), 100);
    QCOMPARE(config_->toJson()["spider"].toObject()["metadataConcurrency"].toInt(), 10);
}

void TestConfigStore::spiderLimits_apiAppliesToServicesAndStats()
{
    rats::app::Application::Options options;
    options.dataDirectory = dir_->path() + "/application"; options.headless = true;
    rats::app::Application application(options);
    rats::rest::ApiRouter router(&application);
    bool replied = false;
    router.call("config.set", {{"spider.metadataConcurrency",20},{"spider.pendingLimit",200},{"spider.batchSize",20}},
        [&](const rats::Result& result) { QVERIFY(result.ok()); replied = true; });
    QVERIFY(replied); QCOMPARE(application.crawler()->metadataConcurrency(),20);
    QJsonObject stats;
    router.call("stats.get",{},[&](const rats::Result& result){QVERIFY(result.ok()); stats=result.data().toObject();});
    QVERIFY(stats.contains("torrents"));
    const auto crawler=stats["crawler"].toObject();
    QCOMPARE(crawler["metadataConcurrency"].toInt(),20); QCOMPARE(crawler["activeMetadata"].toInt(),0);
    QVERIFY(!crawler["running"].toBool()); QVERIFY(!crawler["waitingForCapacity"].toBool());
}

void TestConfigStore::cleanup()
{
    delete config_;
    config_ = nullptr;
    delete dir_;
    dir_ = nullptr;
}

void TestConfigStore::typedSetter_emitsConfigChanged()
{
    QSignalSpy spy(config_, &ConfigStore::configChanged);
    config_->setIndexerEnabled(false);

    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(0).toStringList(), QStringList { "indexer" });
    QVERIFY(!config_->indexerEnabled());
}

void TestConfigStore::typedSetter_nestedKey_emitsConfigChanged()
{
    QSignalSpy spy(config_, &ConfigStore::configChanged);
    config_->setFiltersAdultFilter(true);

    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(0).toStringList(), QStringList { "filters.adultFilter" });
    QVERIFY(config_->filtersAdultFilter());
}

void TestConfigStore::unchangedValue_doesNotEmit()
{
    config_->setSpiderWalkInterval(250);

    QSignalSpy spy(config_, &ConfigStore::configChanged);
    config_->setSpiderWalkInterval(250); // same value
    QCOMPARE(spy.count(), 0);
}

void TestConfigStore::fromJson_emitsOneBatchedSignal()
{
    QSignalSpy spy(config_, &ConfigStore::configChanged);
    const QStringList changed = config_->fromJson(QJsonObject { { "trackers", false }, { "restApi", true } });

    QCOMPARE(spy.count(), 1); // batched, not one per key
    QCOMPARE(changed.size(), 2);
    QVERIFY(!config_->trackersEnabled());
    QVERIFY(config_->restApiEnabled());
}

void TestConfigStore::p2pConnections_isClamped()
{
    config_->setP2pConnections(5);
    QCOMPARE(config_->p2pConnections(), 10);

    config_->setP2pConnections(50000);
    QCOMPARE(config_->p2pConnections(), 1000);

    // A second out-of-range write dedupes against the already-clamped value.
    QSignalSpy spy(config_, &ConfigStore::configChanged);
    config_->setP2pConnections(50001);
    QCOMPARE(spy.count(), 0);
}

void TestConfigStore::replicationImpliesReplicationServer()
{
    config_->setP2pReplicationServer(false);
    QVERIFY(!config_->p2pReplicationServer());

    config_->setP2pReplication(true);
    QVERIFY(config_->p2pReplication());
    QVERIFY(config_->p2pReplicationServer()); // pulled along
}

void TestConfigStore::disablingReplicationServer_disablesReplication()
{
    config_->setP2pReplication(true);
    config_->setP2pReplicationServer(false);

    QVERIFY(!config_->p2pReplicationServer());
    QVERIFY(!config_->p2pReplication());
}

void TestConfigStore::languageAndDarkMode_haveDedicatedSignals()
{
    QSignalSpy languageSpy(config_, &ConfigStore::languageChanged);
    QSignalSpy darkModeSpy(config_, &ConfigStore::darkModeChanged);

    config_->setLanguage("ru");
    config_->setDarkMode(true);

    QCOMPARE(languageSpy.count(), 1);
    QCOMPARE(languageSpy.at(0).at(0).toString(), QStringLiteral("ru"));
    QCOMPARE(darkModeSpy.count(), 1);
    QCOMPARE(darkModeSpy.at(0).at(0).toBool(), true);
}

void TestConfigStore::uiFont_defaultsAndClamping()
{
    QCOMPARE(config_->uiFontFamily(), QString());
    QCOMPARE(config_->uiFontScale(), ConfigStore::kDefaultUiFontScale);

    QSignalSpy spy(config_, &ConfigStore::configChanged);
    config_->setUiFontFamily(QStringLiteral("  Noto Sans  "));
    QCOMPARE(config_->uiFontFamily(), QStringLiteral("Noto Sans"));
    QCOMPARE(spy.at(0).at(0).toStringList(), QStringList { "uiFontFamily" });

    config_->setUiFontScale(1000);
    QCOMPARE(config_->uiFontScale(), ConfigStore::kMaxUiFontScale);
    config_->setUiFontScale(1);
    QCOMPARE(config_->uiFontScale(), ConfigStore::kMinUiFontScale);

    // `config.set` goes through the same clamp.
    config_->fromJson(QJsonObject { { "uiFontScale", 5000 } });
    QCOMPARE(config_->uiFontScale(), ConfigStore::kMaxUiFontScale);

    // So does a hand-edited file.
    QFile file(path_);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(R"({ "uiFontScale": 3 })");
    file.close();
    ConfigStore reloaded(path_);
    QVERIFY(reloaded.load());
    QCOMPARE(reloaded.uiFontScale(), ConfigStore::kMinUiFontScale);
}

void TestConfigStore::saveThenLoad_roundTrips()
{
    config_->setFiltersSizeMin(1024);
    config_->setDownloadPath("/tmp/rats-downloads");
    QVERIFY(config_->save());

    ConfigStore reloaded(path_);
    QVERIFY(reloaded.load());
    QCOMPARE(reloaded.filtersSizeMin(), 1024LL);
    QCOMPARE(reloaded.downloadPath(), QStringLiteral("/tmp/rats-downloads"));
}

QTEST_MAIN(TestConfigStore)
#include "test_config_store.moc"
