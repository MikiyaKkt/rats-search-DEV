#pragma once

#include <QString>
#include <functional>

namespace rats::app::relocation {

struct Request {
    QString destination;
    QString token;
};

// Stored beside the source profile so a restart with an explicit data path
// can also resume an interrupted relocation.
Request pending(const QString& source);
QString schedule(const QString& source, const QString& destination);
QString clear(const QString& source);
QString validate(const QString& source, const QString& destination);

using Progress = std::function<bool(qint64 copied, qint64 total)>;
// The caller holds the source profile lock and has checked that searchd is stopped.
QString prepare(const QString& source, const Request& request, const Progress& progress);
void finish(const QString& source, const Request& request);

} // namespace rats::app::relocation
