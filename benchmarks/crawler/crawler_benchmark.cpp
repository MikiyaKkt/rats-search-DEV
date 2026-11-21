#include "common/performance_metrics.h"
#include "data/database.h"
#include "data/manticore.h"
#include "data/torrent_repository.h"
#include "services/filter_policy.h"
#include "services/indexing_service.h"
#include "version.h"

#include <QCoreApplication>
#include <QCommandLineParser>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMutexLocker>
#include <QSysInfo>
#include <QTemporaryDir>
#include <cstdio>
#include <stdexcept>

using rats::common::PerformanceMetrics;
using rats::domain::Torrent;
namespace {
QMutex logMutex;
QFile* detailLog = nullptr;
qint64 logLines = 0;
void logMessage(QtMsgType type, const QMessageLogContext&, const QString& text)
{
    QMutexLocker lock(&logMutex);
    if (detailLog) { detailLog->write(text.toUtf8()); detailLog->write("\n"); ++logLines; }
    if (type == QtWarningMsg || type == QtCriticalMsg || type == QtFatalMsg)
        std::fprintf(stderr, "%s\n", text.toUtf8().constData());
}
void require(bool ok, const char* reason) { if (!ok) throw std::runtime_error(reason); }
Torrent fixture(int serial, int files)
{
    Torrent t;
    t.hash = QString("%1").arg(serial, 8, 16, QChar('0')) + QString(32, 'a');
    t.name = QString("P0 reproducible collection %1").arg(serial);
    t.files = files;
    t.contentType = rats::domain::ContentType::Video;
    for (int i = 0; i < files; ++i) {
        const auto bytes = qint64(1024 + i);
        t.fileList.append({ QString("collection/subfolder/file-%1.mkv").arg(i), bytes });
        t.size += bytes;
    }
    return t;
}
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOption({ "output", "Write JSON results to this file (required).", "path" });
    parser.process(app);
    if (!parser.isSet("output")) parser.showHelp(2);
    qInstallMessageHandler(logMessage);
    QJsonArray runs;
    int serial = 100000;
    try {
        // Rotate order across repetitions to reduce systematic warm-cache bias.
        const QStringList modes { "metrics_off", "metrics_on", "verbose" };
        for (int repeat = 0; repeat < 3; ++repeat) for (int mi = 0; mi < modes.size(); ++mi) {
            const auto mode = modes[(repeat + mi) % modes.size()];
            QTemporaryDir temp;
            require(temp.isValid(), "temporary directory failed");
            rats::data::Manticore manticore(temp.path());
            require(manticore.start() && manticore.waitForReady(), "Manticore failed to start");
            rats::data::Database db(&manticore);
            rats::data::TorrentRepository repository(&db);
            rats::service::FilterPolicy filter;
            rats::service::IndexingService indexing(&repository, &filter);
            for (int i = 0; i < 12; ++i) require(indexing.insert(fixture(++serial, 2)).success, "warmup failed");
            for (const auto& scenario : { QString("new_single"), QString("duplicate_single"),
                     QString("large_single"), QString("new_batch") }) {
                QVector<Torrent> input;
                const int count = scenario == "large_single" ? 60 : 300;
                const int files = scenario == "large_single" ? 256 : 4;
                for (int i = 0; i < count; ++i) input.append(fixture(++serial, files));
                const bool duplicate = scenario == "duplicate_single";
                if (duplicate) for (const auto& t : input)
                    require(indexing.insert(t).success, "duplicate seed failed");

                auto& metrics = PerformanceMetrics::instance();
                metrics.setEnabled(mode != "metrics_off");
                metrics.reset(); // No background tasks are active in this controlled driver.
                QLoggingCategory::setFilterRules(mode == "verbose"
                    ? QStringLiteral("rats.crawler.detail.debug=true")
                    : QStringLiteral("rats.crawler.detail.debug=false"));
                QFile log(temp.filePath("details.log"));
                require(log.open(QIODevice::WriteOnly | QIODevice::Truncate), "log file failed");
                logLines = 0;
                detailLog = &log;
                const auto before = repository.statistics();
                QElapsedTimer timer;
                timer.start();
                int inserted = 0, merged = 0;
                if (scenario == "new_batch") {
                    for (int offset = 0; offset < input.size(); offset += 100) {
                        const auto result = indexing.insertBatch(input.mid(offset, 100));
                        inserted += result.inserted;
                        merged += result.merged;
                    }
                } else for (const auto& t : input) {
                    const auto result = indexing.insert(t);
                    require(result.success, "single insert failed");
                    if (result.alreadyExists) ++merged; else ++inserted;
                }
                require(log.flush(), "log flush failed");
                const double elapsedMs = timer.nsecsElapsed() / 1e6;
                detailLog = nullptr;
                const auto snapshot = metrics.snapshot();
                require(inserted == (duplicate ? 0 : count) && merged == (duplicate ? count : 0), "outcome mismatch");
                require(repository.statistics().torrents - before.torrents == inserted, "statistics mismatch");
                bool queryOk = false;
                const auto rows = db.query("SELECT COUNT(*) AS cnt FROM torrents", {}, &queryOk);
                require(queryOk && !rows.isEmpty()
                    && rows.first().value("cnt").toLongLong() == repository.statistics().torrents, "stored count mismatch");
                for (const auto i : { 0, count - 1 })
                    require(repository.filesOf(input[i].hash).size() == files, "file list mismatch");
                runs.append(QJsonObject { { "repeat", repeat + 1 }, { "mode", mode }, { "scenario", scenario },
                    { "inputs", count }, { "files_per_torrent", files }, { "inserted", inserted }, { "merged", merged },
                    { "elapsed_ms", elapsedMs }, { "inputs_per_second", count * 1000.0 / elapsedMs },
                    { "inserted_per_minute", inserted * 60000.0 / elapsedMs }, { "log_lines", logLines },
                    { "log_bytes", log.size() }, { "metrics", snapshot } });
            }
            manticore.stop();
        }
    } catch (const std::exception& e) {
        detailLog = nullptr;
        std::fprintf(stderr, "Benchmark failed: %s\n", e.what());
        return 1;
    }
    QFile output(parser.value("output"));
    if (!output.open(QIODevice::WriteOnly | QIODevice::Truncate)) return 2;
    const QJsonObject result { { "schema", 1 }, { "utc", QDateTime::currentDateTimeUtc().toString(Qt::ISODate) },
        { "source", RATSSEARCH_GIT_DESCRIBE }, { "qt", qVersion() }, { "os", QSysInfo::prettyProductName() },
        { "cpu_arch", QSysInfo::currentCpuArchitecture() }, { "warmup_per_instance", 12 },
        { "network", "disabled; fixtures enter IndexingService, not DHT" }, { "runs", runs } };
    const auto bytes = QJsonDocument(result).toJson(QJsonDocument::Indented);
    return output.write(bytes) == bytes.size() ? 0 : 3;
}
