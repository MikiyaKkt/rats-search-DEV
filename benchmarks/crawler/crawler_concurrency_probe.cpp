#include "app/application.h"
#include "app/config_store.h"
#include "common/performance_metrics.h"
#include "data/torrent_repository.h"
#include "data/database.h"
#include "data/manticore.h"
#include "net/crawler.h"
#include "services/crawler_pipeline.h"
#include "process_resources.h"
#include <QCoreApplication>
#include <QCommandLineParser>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTimer>

namespace rats::net {
// Drive the real candidate/check/reserve/fetch/insert path without using DHT.
// Only metadata availability is simulated; SQL and notification delivery are real.
class CrawlerBenchmark {
public:
    static void start(Crawler& crawler) { crawler.running_ = true; ++crawler.generation_; crawler.metadataQueueTimer_->start(Crawler::METADATA_QUEUE_INTERVAL_MS); }
    static void announce(Crawler& crawler, int value) {
        std::array<uint8_t,20> hash{};
        // High prefix digits vary: row IDs discard the low hash bit.
        hash[0] = (value >> 24) & 255; hash[1] = (value >> 16) & 255;
        hash[2] = (value >> 8) & 255; hash[3] = value & 255;
        crawler.onAnnounce(hash,"127.0.0.1",6881);
    }
};
}

int main(int argc, char** argv)
{
    QCoreApplication eventLoop(argc,argv);
    QCommandLineParser parser; parser.addHelpOption();
    parser.addOption({"output","Output JSON.","path"});
    parser.addOption({"concurrency","Configured metadata concurrency (1..64).","count","10"});
    parser.addOption({"count","Synthetic candidates (100..1000).","count","300"});
    parser.process(eventLoop);
    bool limitOk=false,countOk=false;
    const int limit=parser.value("concurrency").toInt(&limitOk), count=parser.value("count").toInt(&countOk);
    if (!parser.isSet("output") || !limitOk || limit<1 || limit>64 || !countOk || count<100 || count>1000) parser.showHelp(2);
    QTemporaryDir directory; if (!directory.isValid()) return 2;
    rats::data::Manticore manticore(directory.path());
    if (!manticore.start() || !manticore.waitForReady()) return 3;
    rats::data::Database database(&manticore);
    rats::data::TorrentRepository repository(&database); repository.primeFromDatabase();
    rats::service::CrawlerPipeline pipeline(&repository,&manticore);
    rats::net::Crawler crawler(nullptr);
    crawler.setMetadataConcurrency(limit);
    crawler.setResultBudget([&](const QString& h){return pipeline.reserveResult(h);},[&](const QString& h){pipeline.releaseResult(h);});
    crawler.setHashLookup([&](const QString& h, auto reply){return pipeline.checkState(h,std::move(reply))==rats::service::CrawlerPipeline::Admission::Accepted;});
    qint64 payloadBytes=0;
    crawler.setMetadataFetcher([&](const QString& hash,const QString&,uint16_t,auto done){
        QTimer::singleShot(50,&crawler,[&,hash,done=std::move(done)]() mutable {
            rats::domain::Torrent torrent; torrent.hash=hash; torrent.name="controlled-"+hash;
            torrent.files=4; torrent.size=4096; torrent.added=QDateTime::currentDateTime();
            for (int i=0;i<4;++i) torrent.fileList.append({QString("folder/file-%1.bin").arg(i),1024});
            payloadBytes += torrent.name.size()*2 + 4*128;
            done(true,std::move(torrent),false);
        });
    });
    QObject::connect(&crawler,&rats::net::Crawler::discovered,&pipeline,[&](const auto& t){
        if (pipeline.insert(t)!=rats::service::CrawlerPipeline::Admission::Accepted) eventLoop.exit(6);
    });
    QObject::connect(&pipeline,&rats::service::CrawlerPipeline::indexFinished,&crawler,[&](const QString& h,auto){crawler.indexResult(h,false);});
    auto& metrics=rats::common::PerformanceMetrics::instance(); metrics.setEnabled(true); metrics.reset();
    QJsonArray resources; QTimer sampler;
    sampler.setInterval(50); QObject::connect(&sampler,&QTimer::timeout,&sampler,[&]{ resources.append(processResources()); });
    resources.append(processResources()); sampler.start();
    int inserted=0; bool timedOut=false; QElapsedTimer elapsed; elapsed.start();
    QHash<QString,qint64> candidateStartNs;
    QObject::connect(&pipeline,&rats::service::CrawlerPipeline::torrentIndexed,&eventLoop,[&](const auto& torrent){
        metrics.observe(QStringLiteral("crawler.end_to_end_ms"), (rats::common::PerformanceMetrics::nowNs()-candidateStartNs.value(torrent.hash))/1000000.0);
        if (++inserted==count) eventLoop.quit();
    });
    pipeline.start(); rats::net::CrawlerBenchmark::start(crawler);
    for (int i=1;i<=count;++i) {
        const auto hash=QString("%1").arg(i,8,16,QChar('0'))+QString(32,'0');
        candidateStartNs.insert(hash,rats::common::PerformanceMetrics::nowNs());
        rats::net::CrawlerBenchmark::announce(crawler,i);
    }
    QTimer::singleShot(30000,&eventLoop,[&]{timedOut=true;eventLoop.exit(4);});
    const int code=eventLoop.exec(); const double seconds=elapsed.nsecsElapsed()/1e9;
    sampler.stop(); resources.append(processResources());
    crawler.stop(); pipeline.stop(); crawler.releaseStoppedFetches();
    const auto snapshot=metrics.snapshot();
    const bool exact=repository.statistics().exact;
    const auto rows=repository.statistics().torrents;
    manticore.stop();
    QJsonObject result{{"kind","controlled metadata + real isolated Manticore"},{"concurrency",limit},{"candidates",count},
        {"metadata_delay_ms",50},{"metadata_tick_ms",100},{"inserted_and_notified",inserted},{"database_rows",rows},{"statistics_exact",exact},
        {"elapsed_seconds",seconds},{"inserted_per_minute",inserted*60/seconds},{"simulated_payload_bytes",payloadBytes},
        {"network_bandwidth",QJsonValue::Null},{"network_note","No DHT or real metadata traffic; simulated payload is not wire bandwidth"},
        {"resources",resources},{"metrics",snapshot},{"timed_out",timedOut}};
    QFile output(parser.value("output")); if (!output.open(QIODevice::WriteOnly)) return 5;
    output.write(QJsonDocument(result).toJson(QJsonDocument::Indented));
    return code || inserted!=count || rows!=count || !exact || pipeline.pending()!=0 ? 6 : 0;
}
