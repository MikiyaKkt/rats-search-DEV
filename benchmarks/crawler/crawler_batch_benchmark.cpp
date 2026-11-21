#include "common/performance_metrics.h"
#include "data/database.h"
#include "data/manticore.h"
#include "data/torrent_repository.h"
#include "services/crawler_pipeline.h"
#include <QCoreApplication>
#include <QCommandLineParser>
#include <QTemporaryDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QCommandLineParser parser; parser.addHelpOption();
    parser.addOption({"output", "Write controlled pipeline measurements.", "path"}); parser.process(app);
    if (!parser.isSet("output")) parser.showHelp(2);
    QTemporaryDir temp;
    rats::data::Manticore manticore(temp.path());
    if (!temp.isValid() || !manticore.start() || !manticore.waitForReady()) return 3;
    rats::data::Database database(&manticore);
    rats::data::TorrentRepository repository(&database);
    repository.primeFromDatabase();
    QJsonArray runs;
    for (int repeat = 0; repeat < 3; ++repeat) {
        rats::service::CrawlerPipeline pipeline(&repository, &manticore);
        QVector<rats::domain::Torrent> input;
        for (int i = 0; i < 300; ++i) {
            rats::domain::Torrent t;
            t.hash = QString("%1").arg(200000 + repeat * 300 + i, 8, 16, QChar('0')) + QString(32, 'a');
            t.name = QStringLiteral("Controlled pipeline %1").arg(i); t.files = 4; t.size = 400;
            t.contentType = rats::domain::ContentType::Video;
            for (int j = 0; j < 4; ++j) t.fileList.append({QStringLiteral("folder/file-%1.mkv").arg(j),100});
            input.append(t);
        }
        auto& metrics = rats::common::PerformanceMetrics::instance();
        metrics.setEnabled(true); metrics.reset();
        QEventLoop loop; QTimer deadline; deadline.setSingleShot(true);
        QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
        int next = 0, delivered = 0; bool failed = false;
        auto pump = [&] {
            while (next < input.size() && pipeline.pending() < pipeline.MaxJobs) {
                if (pipeline.insert(input[next]) != rats::service::CrawlerPipeline::Admission::Accepted) { failed = true; loop.quit(); return; }
                ++next;
            }
            if (next == input.size() && pipeline.pending() == 0) loop.quit();
        };
        QObject::connect(&pipeline, &rats::service::CrawlerPipeline::torrentIndexed, &loop, [&](const auto&) { ++delivered; });
        QObject::connect(&pipeline, &rats::service::CrawlerPipeline::settled, &loop, [&](quint64, bool cancelled) { failed |= cancelled; pump(); });
        const auto before = repository.statistics();
        pipeline.start(); QElapsedTimer elapsed; elapsed.start(); deadline.start(30000); pump(); loop.exec();
        const auto snapshot = metrics.snapshot();
        const auto elapsedMs = elapsed.nsecsElapsed() / 1000000.0;
        pipeline.stop();
        if (failed || next != 300 || delivered != 300 || repository.statistics().torrents != before.torrents + 300) return 4;
        const auto counters = snapshot["counters"].toObject();
        if (counters["db.main_query_calls"].toInteger() || counters["db.main_write_calls"].toInteger()) return 5;
        runs.append(QJsonObject{{"repeat", repeat+1}, {"inputs",300}, {"delivered",delivered},
            {"elapsed_ms",elapsedMs}, {"delivered_per_second",300000.0/elapsedMs}, {"metrics",snapshot}});
    }
    manticore.stop();
    QFile file(parser.value("output")); if (!file.open(QIODevice::WriteOnly)) return 6;
    const auto bytes = QJsonDocument(QJsonObject{{"phase","P2"},{"network","disabled"},
        {"batch_size",100},{"batch_wait_ms",100},{"notification_limit_per_50ms",5},{"runs",runs}}).toJson();
    return file.write(bytes) == bytes.size() ? 0 : 7;
}
