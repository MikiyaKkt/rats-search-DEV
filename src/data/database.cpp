#include "data/database.h"

#include "data/manticore.h"
#include "data/query.h"
#include "common/performance_metrics.h"

#include <QDebug>
#include <QElapsedTimer>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QCoreApplication>
#include <QScopeGuard>
#include <QThread>
#include <optional>

namespace rats::data {

Database::Database(Manticore* manticore, QObject* parent) : QObject(parent), manticore_(manticore) { }

void Database::closeCurrentThreadConnection() { manticore_->closeCurrentThreadConnection(); }

bool Database::isConnected() const
{
    return manticore_ && manticore_->isRunning();
}

Database::Rows Database::query(const QString& sql, const QVariantList& params, bool* ok)
{
    auto& metrics = common::PerformanceMetrics::instance();
    common::PerformanceTimer timing(QStringLiteral("db.query_ms"));
    std::optional<common::PerformanceTimer> mainTiming;
    if (QCoreApplication::instance() && QThread::currentThread() == QCoreApplication::instance()->thread()) {
        mainTiming.emplace(QStringLiteral("db.main_query_ms"));
        metrics.increment(QStringLiteral("db.main_query_calls"));
    }
    metrics.increment(QStringLiteral("db.query_calls"));
    bool succeeded = false;
    const auto outcome = qScopeGuard([&] {
        metrics.increment(succeeded ? QStringLiteral("db.query_ok") : QStringLiteral("db.query_failed"));
    });
    Rows rows;
    if (ok)
        *ok = false;

    QSqlDatabase db = manticore_->getDatabase();
    if (!db.isOpen() && !db.open()) {
        qWarning() << "Database: connection not open:" << db.lastError().text() << "| SQL:" << sql.left(200);
        return rows;
    }

    const QString finalSql = sql::substitute(sql, params);

    QElapsedTimer timer;
    timer.start();
    QSqlQuery q(db);
    if (!q.exec(finalSql)) {
        qWarning() << "Database: query failed:" << q.lastError().text() << "| SQL:" << finalSql.left(200);
        return rows;
    }

    const QSqlRecord record = q.record();
    while (q.next()) {
        Row row;
        for (int i = 0; i < record.count(); ++i)
            row[record.fieldName(i)] = q.value(i);
        rows.append(row);
    }

    if (ok)
        *ok = true;
    succeeded = true;
    metrics.increment(QStringLiteral("db.rows_returned"), rows.size());
    qCDebug(common::crawlerDetail).noquote()
        << QStringLiteral("Database: %1 rows in %2ms | %3").arg(rows.size()).arg(timer.elapsed()).arg(sql.left(120));
    return rows;
}

bool Database::runWrite(const QString& sqlText, const char* op, const QString& table)
{
    auto& metrics = common::PerformanceMetrics::instance();
    common::PerformanceTimer timing(QStringLiteral("db.write_ms"));
    std::optional<common::PerformanceTimer> mainTiming;
    if (QCoreApplication::instance() && QThread::currentThread() == QCoreApplication::instance()->thread()) {
        mainTiming.emplace(QStringLiteral("db.main_write_ms"));
        metrics.increment(QStringLiteral("db.main_write_calls"));
    }
    metrics.increment(QStringLiteral("db.write_calls"));
    if (metrics.enabled())
        metrics.observe(QStringLiteral("db.statement_bytes"), sqlText.toUtf8().size());
    bool succeeded = false;
    const auto outcome = qScopeGuard([&] {
        metrics.increment(succeeded ? QStringLiteral("db.write_ok") : QStringLiteral("db.write_failed"));
    });
    QSqlDatabase db = manticore_->getDatabase();
    if (!db.isOpen() && !db.open()) {
        qWarning() << "Database:" << op << "connection not open:" << db.lastError().text();
        return false;
    }

    QElapsedTimer timer;
    timer.start();
    QSqlQuery q(db);
    if (!q.exec(sqlText)) {
        qWarning() << "Database:" << op << "failed:" << q.lastError().text() << "| SQL:" << sqlText.left(200);
        return false;
    }

    succeeded = true;
    qCDebug(common::crawlerDetail).noquote() << QStringLiteral("Database: %1 %2 | %3ms | affected %4")
                             .arg(QString::fromLatin1(op), table)
                             .arg(timer.elapsed())
                             .arg(q.numRowsAffected());
    return true;
}

bool Database::execute(const QString& sql)
{
    return runWrite(sql, "EXEC", QString());
}

bool Database::insert(const QString& table, const QVariantMap& values)
{
    return writeRow("INSERT", table, values);
}

bool Database::replace(const QString& table, const QVariantMap& values)
{
    return writeRow("REPLACE", table, values);
}

bool Database::insertMany(const QString& table, const QVector<QVariantMap>& rows)
{
    return writeRows("INSERT", table, rows);
}

bool Database::replaceMany(const QString& table, const QVector<QVariantMap>& rows)
{
    return writeRows("REPLACE", table, rows);
}

bool Database::writeRow(const char* verb, const QString& table, const QVariantMap& values)
{
    QStringList columns;
    QStringList literals;
    for (auto it = values.constBegin(); it != values.constEnd(); ++it) {
        columns << it.key();
        literals << sql::formatValue(it.value());
    }
    const QString stmt
        = QStringLiteral("%1 INTO %2 (%3) VALUES (%4)")
              .arg(QLatin1String(verb), table, columns.join(QLatin1String(", ")), literals.join(QLatin1String(", ")));
    common::PerformanceMetrics::instance().observe(QStringLiteral("db.rows_per_statement"), 1);
    return runWrite(stmt, verb, table);
}

bool Database::writeRows(const char* verb, const QString& table, const QVector<QVariantMap>& rows)
{
    return writeRowsConfirmed(verb, table, rows).success();
}

Database::WriteReport Database::replaceManyConfirmed(const QString& table, const QVector<QVariantMap>& rows)
{
    return writeRowsConfirmed("REPLACE", table, rows);
}

Database::WriteReport Database::writeRowsConfirmed(const char* verb, const QString& table, const QVector<QVariantMap>& rows)
{
    WriteReport result;
    if (rows.isEmpty()) return result;
    QStringList columns;
    for (auto it = rows.first().cbegin(); it != rows.first().cend(); ++it) columns << it.key();
    const QString prefix = QStringLiteral("%1 INTO %2 (%3) VALUES ")
        .arg(QLatin1String(verb), table, columns.join(QLatin1String(", ")));
    QString statement = prefix;
    qint64 bytes = prefix.toUtf8().size();
    QSet<qint64> pending;
    auto flush = [&] {
        if (pending.isEmpty()) return;
        common::PerformanceMetrics::instance().observe(QStringLiteral("db.rows_per_statement"), pending.size());
        if (runWrite(statement, verb, table)) result.confirmed.unite(pending);
        else result.uncertain.unite(pending);
        pending.clear(); statement = prefix; bytes = prefix.toUtf8().size();
    };
    for (const auto& row : rows) {
        QStringList literals;
        for (const auto& column : columns) literals << sql::formatValue(row.value(column));
        const QString tuple = QStringLiteral("(%1)").arg(literals.join(QLatin1String(", ")));
        const auto tupleBytes = tuple.toUtf8().size();
        const auto id = row.value(QStringLiteral("id")).toLongLong();
        if (prefix.toUtf8().size() + tupleBytes > MaxStatementBytes) {
            result.rejected.insert(id);
            continue;
        }
        if (bytes + tupleBytes + 1 > MaxStatementBytes || pending.size() >= 500) flush();
        if (!pending.isEmpty()) { statement += QLatin1Char(','); ++bytes; }
        statement += tuple; bytes += tupleBytes; pending.insert(id);
    }
    flush();
    return result;
}

bool Database::update(const QString& table, const QVariantMap& values, const QVariantMap& where)
{
    QStringList setParts;
    for (auto it = values.constBegin(); it != values.constEnd(); ++it)
        setParts << QStringLiteral("%1 = %2").arg(it.key(), sql::formatValue(it.value()));

    QStringList whereParts;
    for (auto it = where.constBegin(); it != where.constEnd(); ++it)
        whereParts << QStringLiteral("%1 = %2").arg(it.key(), sql::formatValue(it.value()));

    const QString stmt = QStringLiteral("UPDATE %1 SET %2 WHERE %3")
                             .arg(table, setParts.join(QLatin1String(", ")), whereParts.join(QLatin1String(" AND ")));
    return runWrite(stmt, "UPDATE", table);
}

bool Database::remove(const QString& table, const QVariantMap& where)
{
    QStringList whereParts;
    for (auto it = where.constBegin(); it != where.constEnd(); ++it)
        whereParts << QStringLiteral("%1 = %2").arg(it.key(), sql::formatValue(it.value()));

    const QString stmt = QStringLiteral("DELETE FROM %1 WHERE %2").arg(table, whereParts.join(QLatin1String(" AND ")));
    return runWrite(stmt, "DELETE", table);
}

qint64 Database::count(const QString& table, const QString& whereRaw)
{
    QString stmt = QStringLiteral("SELECT COUNT(*) AS cnt FROM %1").arg(table);
    if (!whereRaw.isEmpty())
        stmt += QStringLiteral(" WHERE ") + whereRaw;
    const Rows rows = query(stmt);
    return rows.isEmpty() ? 0 : rows.first().value(QStringLiteral("cnt")).toLongLong();
}

} // namespace rats::data
