#include "common/performance_metrics.h"
#include "data/database.h"
#include "data/manticore.h"
#include "data/torrent_repository.h"
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
#include <QSqlDatabase>
#include <QTemporaryDir>
#include <QTimer>
#include <atomic>
#include <stdexcept>

namespace rats::net {
class CrawlerBenchmark {
public:
    static void start(Crawler& c){c.running_=true;++c.generation_;c.metadataQueueTimer_->start(100);}
    static void announce(Crawler& c,int serial){
        std::array<uint8_t,20> h{}; h[0]=serial>>24;h[1]=serial>>16;h[2]=serial>>8;h[3]=serial;
        c.onAnnounce(h,"127.0.0.1",6881);
    }
    static void pump(Crawler& c){c.processMetadataQueue();}
    static void heldFetch(Crawler& c,const QString& key){ Crawler::MetadataRequest r;r.infoHash=key;c.fetchMetadata(r); }
};
}

class IntermittentDatabase : public rats::data::Database {
public:
    using Database::Database;
    std::atomic<bool> offline{false};
    Rows query(const QString& sql,const QVariantList& params={},bool* ok=nullptr) override {
        if(offline){if(ok)*ok=false;return{};} return Database::query(sql,params,ok);
    }
protected:
    bool runWrite(const QString& sql,const char* op,const QString& table) override {
        return !offline && Database::runWrite(sql,op,table);
    }
};

int main(int argc,char** argv)
{
    QCoreApplication eventLoop(argc,argv); QCommandLineParser parser; parser.addHelpOption();
    parser.addOption({"output","Output JSON.","path"});
    parser.addOption({"seconds","Wall-clock duration (3..7200).","seconds","7200"});
    parser.addOption({"cycles","Stop/start cycles (1..1000).","count","100"});
    parser.addOption({"stop-file","Stop cleanly if this file appears.","path"}); parser.process(eventLoop);
    bool durationOk=false,cyclesOk=false;
    const int seconds=parser.value("seconds").toInt(&durationOk),cycles=parser.value("cycles").toInt(&cyclesOk);
    if(!parser.isSet("output")||!durationOk||seconds<3||seconds>7200||!cyclesOk||cycles<1||cycles>1000)parser.showHelp(2);
    QTemporaryDir directory; if(!directory.isValid())return 2;
    rats::data::Manticore manticore(directory.path()); if(!manticore.start()||!manticore.waitForReady())return 3;
    IntermittentDatabase database(&manticore); rats::data::TorrentRepository repository(&database); repository.primeFromDatabase();
    const int baselineConnections=QSqlDatabase::connectionNames().size();
    rats::service::CrawlerPipeline pipeline(&repository,&manticore); rats::net::Crawler crawler(nullptr);
    crawler.setResultBudget([&](const QString& h){return pipeline.reserveResult(h);},[&](const QString& h){pipeline.releaseResult(h);});
    crawler.setHashLookup([&](const QString& h,auto done){return pipeline.checkState(h,std::move(done))==rats::service::CrawlerPipeline::Admission::Accepted;});
    bool hold=false; rats::net::Crawler::MetadataCompletion held;
    crawler.setMetadataFetcher([&](const QString& hash,const QString&,uint16_t,auto done){
        if(hold){held=std::move(done);return;}
        const int serial=hash.left(8).toInt(nullptr,16);
        const auto finish=[hash,serial,done=std::move(done)]() mutable {
            if(serial%17==0){done(false,{},true);return;}
            rats::domain::Torrent t; t.hash=hash;t.name="soak-"+hash;t.size=120;t.files=2;
            t.fileList={{"folder/one.bin",70},{"folder/two.bin",50}};t.added=QDateTime::currentDateTime();
            done(true,t,false); if(serial%23==0)done(true,t,false);
        };
        if(serial%29==0){auto sync=finish;sync();throw std::runtime_error("after synchronous completion");}
        QTimer::singleShot(15,&crawler,finish);
    });
    int delivered=0,serial=0; bool failed=false;
    QObject::connect(&crawler,&rats::net::Crawler::discovered,&pipeline,[&](const auto& t){
        const auto admission=pipeline.insert(t);if(admission!=rats::service::CrawlerPipeline::Admission::Accepted){pipeline.releaseResult(t.hash);crawler.indexResult(t.hash,false);failed=true;}
    });
    QObject::connect(&pipeline,&rats::service::CrawlerPipeline::torrentIndexed,&eventLoop,[&](const auto&){++delivered;});
    QObject::connect(&pipeline,&rats::service::CrawlerPipeline::indexFinished,&crawler,[&](const QString& h,auto){crawler.indexResult(h,false);});
    auto& metrics=rats::common::PerformanceMetrics::instance();metrics.setEnabled(true);metrics.reset();
    QElapsedTimer elapsed;elapsed.start(); QJsonArray restartRows;
    QFile samples(parser.value("output")+".resources.jsonl"); if(!samples.open(QIODevice::WriteOnly|QIODevice::Truncate))return 5;
    pipeline.start();rats::net::CrawlerBenchmark::start(crawler);
    QTimer producer; producer.setInterval(250);
    QObject::connect(&producer,&QTimer::timeout,&eventLoop,[&]{for(int i=0;i<2;++i)rats::net::CrawlerBenchmark::announce(crawler,++serial);});producer.start();
    QTimer sampler;sampler.setInterval(1000);
    const auto sample=[&]{
        if(parser.isSet("stop-file")&&QFile::exists(parser.value("stop-file")))eventLoop.quit();
        auto row=processResources();row["elapsed_ms"]=elapsed.elapsed();row["cycles"]=restartRows.size();row["delivered"]=delivered;
        row["pending"]=pipeline.pending();row["active"]=crawler.activeMetadataFetches();row["connections"]=QSqlDatabase::connectionNames().size();
        const auto snapshot=metrics.snapshot();row["metrics"]=snapshot;
        const auto gauges=snapshot["gauges"].toObject();
        if(pipeline.pending()>1000 || gauges["crawler.db_pending_bytes"].toObject()["current"].toDouble()>64*1024*1024)failed=true;
        samples.write(QJsonDocument(row).toJson(QJsonDocument::Compact)+"\n");samples.flush();
        // 2 seconds of injected connection/write unavailability per 30-second period.
        database.offline=(elapsed.elapsed()%30000)>=28000;
    };
    QObject::connect(&sampler,&QTimer::timeout,&eventLoop,sample);sampler.start(); sample();
    QTimer restarter;restarter.setTimerType(Qt::PreciseTimer);restarter.setInterval(qMax(1,seconds*1000/(cycles+1)));
    QObject::connect(&restarter,&QTimer::timeout,&eventLoop,[&]{
        // Keep a request in flight across every stop/start, then deliver it late.
        hold=true;const auto key=QString("%1").arg(++serial,8,16,QChar('0'))+QString(32,'0');
        if(pipeline.reserveResult(key)) rats::net::CrawlerBenchmark::heldFetch(crawler,key);
        else failed=true;
        crawler.stop();pipeline.stop();
        const int stoppedConnections=QSqlDatabase::connectionNames().size();
        if(stoppedConnections!=baselineConnections)failed=true;
        pipeline.start();rats::net::CrawlerBenchmark::start(crawler);hold=false;
        if(held){auto old=std::move(held);old(false,{},true);old(false,{},true);}
        crawler.setMetadataConcurrency(restartRows.size()%2?10:20);
        pipeline.configure(1000,restartRows.size()%2?100:50);
        restartRows.append(QJsonObject{{"cycle",restartRows.size()+1},{"elapsed_ms",elapsed.elapsed()},
            {"stopped_connections",stoppedConnections},{"resources",processResources()}});
        if(restartRows.size()>=cycles)restarter.stop();
    });restarter.start();
    QTimer deadline;deadline.setTimerType(Qt::PreciseTimer);deadline.setSingleShot(true);
    QObject::connect(&deadline,&QTimer::timeout,&eventLoop,&QCoreApplication::quit);deadline.start(seconds*1000);
    eventLoop.exec();producer.stop();sampler.stop();restarter.stop(); sample();
    crawler.stop();pipeline.stop();crawler.releaseStoppedFetches();
    const auto final=metrics.snapshot(); database.offline=false;
    repository.reconcileStatistics(); const auto stats=repository.statistics(); const qint64 counted=database.count("torrents");
    manticore.stop();
    const auto counters=final["counters"].toObject();
    const bool accounted=counters["crawler.capacity_accepted"].toInteger()==counters["crawler.capacity_completed"].toInteger()+counters["crawler.capacity_cancelled"].toInteger();
    QJsonObject result{{"kind","controlled sustained metadata + temporary real Manticore"},{"seconds_requested",seconds},{"wall_ms",elapsed.elapsed()},
        {"cycles_requested",cycles},{"restart_cycles",restartRows},{"generated_hashes",serial},{"delivered",delivered},
        {"baseline_connections",baselineConnections},{"connections_after_stop",QSqlDatabase::connectionNames().size()},
        {"database_rows",counted},{"statistics_rows",stats.torrents},{"statistics_exact",stats.exact},{"capacity_accounted",accounted},
        {"final_metrics",final},{"resource_file",samples.fileName()},{"failed",failed}};
    result["cancelled"]=parser.isSet("stop-file")&&QFile::exists(parser.value("stop-file"));
    QFile output(parser.value("output"));if(!output.open(QIODevice::WriteOnly|QIODevice::Truncate))return 5;
    output.write(QJsonDocument(result).toJson(QJsonDocument::Indented));
    return failed||!accounted||pipeline.pending()!=0||crawler.activeMetadataFetches()!=0||restartRows.size()!=cycles||counted!=stats.torrents||!stats.exact
        ||result["cancelled"].toBool()||elapsed.elapsed()<seconds*1000?6:0;
}
