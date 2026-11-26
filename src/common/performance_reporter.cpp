#include "common/performance_reporter.h"
#include "common/performance_metrics.h"

#include <QDebug>
#include <QJsonDocument>
#include <QUuid>

namespace rats::common {

PerformanceReporter::PerformanceReporter(QObject* parent) : QObject(parent)
{
    summaryTimer_.setInterval(5000);
    lagTimer_.setInterval(200);
    lagTimer_.setTimerType(Qt::PreciseTimer);
    connect(&summaryTimer_, &QTimer::timeout, this, [this] { report(); });
    connect(&lagTimer_, &QTimer::timeout, this, [this] {
        const auto now = PerformanceMetrics::nowNs();
        const double lag = qMax(0.0, (now - previousTickNs_) / 1000000.0 - lagTimer_.interval());
        PerformanceMetrics::instance().observe(QStringLiteral("main.event_loop_drift_ms"), lag);
        if (lag >= 500)
            qWarning() << "[MainThread] event loop delayed" << qRound64(lag) << "ms";
        previousTickNs_ = now;
    });
}

void PerformanceReporter::start()
{
    if (running_ || !PerformanceMetrics::instance().enabled()) return;
    running_ = true;
    runId_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
    previousTickNs_ = previousReportNs_ = PerformanceMetrics::nowNs();
    previousInserted_ = PerformanceMetrics::instance().snapshot().value("counters").toObject()
        .value("crawler.index_inserted").toInteger();
    summaryTimer_.start();
    lagTimer_.start();
}

QJsonObject PerformanceReporter::report()
{
    emit beforeSnapshot();
    auto data = PerformanceMetrics::instance().snapshot();
    const auto now = PerformanceMetrics::nowNs();
    const auto counters = data.value("counters").toObject();
    const auto inserted = counters.value("crawler.index_inserted").toInteger();
    const double seconds = (now - previousReportNs_) / 1e9;
    const auto succeeded = counters.value("crawler.metadata_succeeded").toInteger();
    const auto failed = counters.value("crawler.metadata_failed").toInteger();
    data["run_id"] = runId_;
    data["interval_ms"] = seconds * 1000;
    data["inserted_per_minute"] = seconds > 0 ? (inserted - previousInserted_) * 60.0 / seconds : 0;
    data["metadata_success_ratio"] = succeeded + failed > 0
        ? QJsonValue(double(succeeded) / (succeeded + failed)) : QJsonValue();
    data["metadata_timeout_ratio"] = succeeded + failed > 0
        ? QJsonValue(double(counters.value("crawler.metadata_timeouts").toInteger()) / (succeeded + failed)) : QJsonValue();
    previousReportNs_ = now;
    previousInserted_ = inserted;
    qInfo().noquote() << "[CrawlerMetrics]" << QJsonDocument(data).toJson(QJsonDocument::Compact);
    return data;
}

void PerformanceReporter::stop()
{
    if (!running_) return;
    summaryTimer_.stop();
    lagTimer_.stop();
    report();
    running_ = false;
}

} // namespace rats::common
