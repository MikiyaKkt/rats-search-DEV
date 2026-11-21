#ifndef RATS_COMMON_PERFORMANCE_REPORTER_H
#define RATS_COMMON_PERFORMANCE_REPORTER_H

#include <QObject>
#include <QTimer>
#include <QString>
#include <QJsonObject>

namespace rats::common {

// Lives on the application's main thread; also used by the headless probe.
class PerformanceReporter : public QObject {
    Q_OBJECT
public:
    explicit PerformanceReporter(QObject* parent = nullptr);
    void start();
    void stop();
    QJsonObject report();
signals:
    void beforeSnapshot();
private:
    QTimer summaryTimer_;
    QTimer lagTimer_;
    qint64 previousTickNs_ = 0;
    qint64 previousReportNs_ = 0;
    qint64 previousInserted_ = 0;
    QString runId_;
    bool running_ = false;
};

} // namespace rats::common
#endif
