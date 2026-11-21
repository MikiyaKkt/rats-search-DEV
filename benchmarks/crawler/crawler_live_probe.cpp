#include "app/application.h"
#include "app/config_store.h"
#include "common/performance_metrics.h"
#include "net/crawler.h"
#include "net/p2p_transport.h"
#include "peer/peer_api.h"
#include "version.h"
#include "process_resources.h"
#pragma push_macro("emit")
#pragma push_macro("slots")
#pragma push_macro("signals")
#undef emit
#undef slots
#undef signals
#include "librats/subsystems/bittorrent.h"
#pragma pop_macro("signals")
#pragma pop_macro("slots")
#pragma pop_macro("emit")
#include <atomic>

#include <QCoreApplication>
#include <QJsonArray>
#include <QElapsedTimer>
#include <QSqlDatabase>
#include <QCommandLineParser>
#include <QDateTime>
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTcpServer>
#include <QUdpSocket>
#include <QTimer>

// A bounded live probe with an isolated temporary database. Never opens user data.
int main(int argc, char** argv)
{
    QCoreApplication eventLoop(argc, argv);
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOption({ "output", "Output JSON file (required).", "path" });
    parser.addOption({ "seconds", "Measurement seconds (1..7200).", "seconds", "60" });
    parser.addOption({ "warmup", "Warmup seconds (0..3600).", "seconds", "0" });
    parser.addOption({ "concurrency", "Crawler metadata concurrency (1..64).", "count", "10" });
    parser.addOption({ "profile-waiters", "Profile N library timeout watchers instead of crawling (0..40).", "count", "0" });
    parser.addOption({ "resource-interval", "Resource sampling interval in milliseconds (100..60000).", "ms", "100" });
    parser.addOption({ "restart-cycles", "Crawler/pipeline stop-start cycles distributed across the run (0..1000).", "count", "0" });
    parser.addOption({ "stop-file", "Stop cleanly if this file appears.", "path" });
    parser.process(eventLoop);
    bool secondsOk = false, warmupOk = false;
    const int seconds = parser.value("seconds").toInt(&secondsOk);
    const int warmup = parser.value("warmup").toInt(&warmupOk);
    bool concurrencyOk = false, waitersOk = false;
    const int concurrency = parser.value("concurrency").toInt(&concurrencyOk);
    const int waiters = parser.value("profile-waiters").toInt(&waitersOk);
    bool intervalOk=false; const int resourceInterval=parser.value("resource-interval").toInt(&intervalOk);
    bool cyclesOk=false; const int restartCycles=parser.value("restart-cycles").toInt(&cyclesOk);
    if (!parser.isSet("output") || !secondsOk || !warmupOk || seconds < 1 || seconds > 7200 || warmup < 0 || warmup > 3600
        || !concurrencyOk || concurrency < 1 || concurrency > 64 || !waitersOk || waiters < 0 || waiters > 40
        || !intervalOk || resourceInterval<100 || resourceInterval>60000 || !cyclesOk || restartCycles<0 || restartCycles>1000)
        parser.showHelp(2);
    QTemporaryDir temp;
    if (!temp.isValid()) return 2;
    QTcpServer tcp;
    QUdpSocket udp;
    if (!tcp.listen(QHostAddress::AnyIPv4, 0) || !udp.bind(QHostAddress::AnyIPv4, quint16(0))) return 3;
    rats::app::Application::Options options;
    options.dataDirectory = temp.path();
    options.headless = true;
    options.forceSpider = waiters == 0 && restartCycles == 0;
    options.shareDatabase = false;
    options.p2pPort = tcp.serverPort();
    options.dhtPort = udp.localPort();
    tcp.close(); udp.close();
    rats::app::Application app(options);
    app.config()->setSpiderMetadataConcurrency(concurrency);
    if (waiters) app.config()->setIndexerEnabled(false);
    // Keep tracker/replication traffic out of the crawler baseline.
    app.config()->setValue("trackers", false);
    app.config()->setValue("p2pReplication", false);
    app.config()->setValue("p2pReplicationServer", false);
    app.config()->setValue("restApi", false);
    app.config()->setValue("upnp", false);
    app.config()->setValue("holePunch", false);
    app.config()->setValue("relay", false);
    app.config()->setValue("relayServe", false);
    // Replication=false alone still permits feed sync and incoming announces.
    // Isolate the benchmark, not the production protocol: keep DHT enabled but
    // prevent every peer payload that could insert torrents into this fixture DB.
    QObject::disconnect(app.transport(), nullptr, app.peerApi(), nullptr);
    for (const auto& type : { "feed_response", "randomTorrents_response", "torrentAnnounce", "torrent_response" })
        app.transport()->registerHandler(QLatin1String(type), [](const QString&, const QJsonObject&) {});
    if (!app.start()) return 4;
    QJsonArray restarts; QTimer restartTimer;
    if(restartCycles) {
        restartTimer.setTimerType(Qt::PreciseTimer);
        restartTimer.setInterval(qMax(1,(warmup+seconds)*1000/(restartCycles+1)));
        QObject::connect(&restartTimer,&QTimer::timeout,&eventLoop,[&]{
            app.config()->setIndexerEnabled(false);
            const int stoppedConnections=QSqlDatabase::connectionNames().size();
            app.config()->setIndexerEnabled(true);
            restarts.append(QJsonObject{{"cycle",restarts.size()+1},{"stopped_connections",stoppedConnections},
                {"running",app.crawler()->isRunning()},{"active",app.crawler()->activeMetadataFetches()}});
            if(restarts.size()>=restartCycles)restartTimer.stop();
        });
        restartTimer.start();
    }
    QJsonArray resources;
    QFile resourceOutput(parser.value("output")+".resources.jsonl");
    if (!resourceOutput.open(QIODevice::WriteOnly|QIODevice::Truncate)) return 5;
    qint64 resourceSamples=0;
    QElapsedTimer resourceClock; resourceClock.start();
    const auto sample = [&] {
        if(parser.isSet("stop-file")&&QFile::exists(parser.value("stop-file")))eventLoop.quit();
        auto value=processResources(); value["elapsed_ms"]=qint64(resourceClock.elapsed()); ++resourceSamples;
        resourceOutput.write(QJsonDocument(value).toJson(QJsonDocument::Compact)+"\n"); resourceOutput.flush();
        if (resources.size()>=1024) resources.removeAt(1); // Preserve baseline and a bounded recent tail.
        resources.append(value);
    };
    sample(); QTimer resourceTimer; resourceTimer.setInterval(resourceInterval);
    QObject::connect(&resourceTimer,&QTimer::timeout,&eventLoop,sample); resourceTimer.start();
    std::atomic<int> waiterCompletions{0};
    if (waiters) {
        for (int i=0;i<waiters;++i) {
            const QString hash=QString("%1").arg(i+1,8,16,QChar('0'))+QString(32,'a');
            app.transport()->bittorrent()->get_torrent_metadata_from_peer(hash.toStdString(),"127.0.0.1",9,
                [&](const auto&,bool,const auto&){++waiterCompletions;},1500);
        }
    }
    QJsonObject start;
    QString startUtc;
    QTimer::singleShot(warmup * 1000, &eventLoop, [&] {
        startUtc = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
        start = rats::common::PerformanceMetrics::instance().snapshot();
    });
    QTimer deadline; deadline.setSingleShot(true); deadline.setTimerType(Qt::PreciseTimer);
    QObject::connect(&deadline,&QTimer::timeout,&eventLoop,&QCoreApplication::quit);
    deadline.start((warmup+seconds)*1000);
    eventLoop.exec();
    const auto end = rats::common::PerformanceMetrics::instance().snapshot();
    const auto endUtc = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    const auto dhtNodes = app.transport()->dhtNodeCount();
    const bool crawlerRunning = app.crawler()->isRunning();
    resourceTimer.stop(); sample();
    app.stop();
    QFile output(parser.value("output"));
    if (!output.open(QIODevice::WriteOnly | QIODevice::Truncate)) return 5;
    const QJsonObject result { { "schema", 1 }, { "source", RATSSEARCH_GIT_DESCRIBE },
        { "utc", QDateTime::currentDateTimeUtc().toString(Qt::ISODate) }, { "measurement_seconds", seconds },
        { "warmup_seconds", warmup }, { "trackers", false }, { "replication", false },
        { "measurement_start_utc", startUtc }, { "measurement_end_utc", endUtc },
        { "dht_nodes_at_end", qint64(dhtNodes) }, { "crawler_running", crawlerRunning },
        { "isolated_database", true }, { "peer_ingestion_disabled", true },
        { "metadata_concurrency", concurrency }, { "profile_waiters", waiters }, { "waiter_completions", waiterCompletions.load() },
        { "resources", resources }, { "network_bandwidth", QJsonValue::Null },
        { "resource_samples", resourceSamples }, { "resource_interval_ms", resourceInterval },
        { "resource_file", resourceOutput.fileName() }, { "wall_ms", resourceClock.elapsed() },
        { "post_stop_snapshot", rats::common::PerformanceMetrics::instance().snapshot() },
        { "restart_cycles", restarts }, { "connections_after_stop", QSqlDatabase::connectionNames().size() },
        { "cancelled", parser.isSet("stop-file")&&QFile::exists(parser.value("stop-file")) },
        { "start_snapshot", start }, { "end_snapshot", end } };
    const auto bytes = QJsonDocument(result).toJson(QJsonDocument::Indented);
    return output.write(bytes) == bytes.size() ? (result["cancelled"].toBool()?7:0) : 6;
}
