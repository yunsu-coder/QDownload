#pragma once

#include <QDateTime>
#include <QSqlDatabase>
#include <QString>
#include <QUrl>

class DownloadMemoryStore final
{
public:
    struct HostMemory {
        QString host;
        QString preferredRoute;
        int recommendedConnections = 0;
        bool supportsRanges = false;
        double expectedSpeedMBps = 0.0;
        double confidence = 0.0;
        int successes = 0;
        int failures = 0;
        QDateTime updatedAt;

        bool isValid() const
        {
            return !preferredRoute.isEmpty() && confidence > 0.0;
        }
    };

    struct ResourceMemory {
        QString host;
        QString fileName;
        qint64 totalBytes = 0;
        bool supportsRanges = false;
        QString etag;
        QString lastModified;
        QDateTime updatedAt;
    };

    struct Outcome {
        QUrl url;
        QString effectiveUrl;
        QString finalPath;
        QString fileHash;
        QString route;
        QString error;
        QString etag;
        QString lastModified;
        qint64 bytes = 0;
        double speedMBps = 0.0;
        int peakConnections = 0;
        bool supportsRanges = false;
        bool success = false;
    };

    explicit DownloadMemoryStore(QString databasePath = {});
    ~DownloadMemoryStore();

    bool ready() const;
    HostMemory lookupHost(const QString &host) const;
    ResourceMemory lookupResource(const QUrl &url) const;
    bool recordOutcome(const Outcome &outcome);
    bool clearAll();
    QString lastError() const;

    static QString routeForProxyUsage(bool useProxy, bool embedded);

private:
    bool initialise();
    bool updateRouteMemory(const QString &host,
                           const QString &route,
                           const Outcome &outcome);
    bool updateResourceMemory(const Outcome &outcome);
    bool refreshPreferredRoute(const QString &host);
    QString databasePath() const;
    QString resourceKey(const QUrl &url) const;
    void setError(const QString &message);

    QString m_connectionName;
    QString m_databasePathOverride;
    QSqlDatabase m_database;
    QString m_lastError;
};
