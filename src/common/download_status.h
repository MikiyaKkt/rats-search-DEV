#pragma once

#include <QJsonObject>
#include <QString>

namespace rats {

enum class DownloadPhase { Stopped, Metadata, Checking, Downloading, Seeding, Waiting, Error, PartialSeeding };
inline QString phaseName(DownloadPhase phase) {
    switch (phase) {
    case DownloadPhase::Metadata: return QStringLiteral("metadata");
    case DownloadPhase::Checking: return QStringLiteral("checking");
    case DownloadPhase::Downloading: return QStringLiteral("downloading");
    case DownloadPhase::Seeding: return QStringLiteral("seeding");
    case DownloadPhase::PartialSeeding: return QStringLiteral("partialSeeding");
    case DownloadPhase::Waiting: return QStringLiteral("waiting");
    case DownloadPhase::Error: return QStringLiteral("error");
    default: return QStringLiteral("stopped");
    }
}

struct DownloadError {
    QString code, message, operation, path;
    int systemCode = 0;
    bool retryable = false;
    bool isRetryable() const { return retryable; }
    bool isError() const { return !code.isEmpty(); }
    QJsonObject toJson() const {
        return {{"code", code}, {"message", message}, {"operation", operation},
            {"path", path}, {"systemCode", systemCode}, {"retryable", retryable}};
    }
    static DownloadError fromJson(const QJsonObject& json) {
        return {json["code"].toString(), json["message"].toString(), json["operation"].toString(),
            json["path"].toString(), json["systemCode"].toInt(), json["retryable"].toBool()};
    }
};

struct OperationResult {
    enum class Outcome { Applied, Accepted, Rejected, AppliedNotPersisted };
    Outcome outcome = Outcome::Rejected;
    DownloadError error;
    QString hash, operationId;
    qint64 revision = 0;
    bool applied() const { return outcome == Outcome::Applied || outcome == Outcome::AppliedNotPersisted; }
    QJsonObject toJson() const {
        const char* state = outcome == Outcome::Applied ? "applied" : outcome == Outcome::Accepted ? "accepted"
            : outcome == Outcome::AppliedNotPersisted ? "appliedNotPersisted" : "rejected";
        return {{"outcome", state}, {"hash", hash}, {"operationId", operationId},
            {"revision", revision}, {"error", error.toJson()}};
    }
    static OperationResult fromJson(const QJsonObject& json) {
        OperationResult result;
        const auto state = json["outcome"].toString();
        if (state == "applied") result.outcome = Outcome::Applied;
        else if (state == "accepted") result.outcome = Outcome::Accepted;
        else if (state == "appliedNotPersisted") result.outcome = Outcome::AppliedNotPersisted;
        result.hash = json["hash"].toString();
        result.operationId = json["operationId"].toString();
        result.revision = json["revision"].toInteger();
        result.error = DownloadError::fromJson(json["error"].toObject());
        return result;
    }
};

} // namespace rats
