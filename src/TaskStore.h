#pragma once

#include <QSqlDatabase>
#include <QString>
#include <QVector>

class TaskStore final
{
public:
    struct TaskRecord {
        QString id;
        QString url;
        QString effectiveUrl;
        QString fileName;
        QString targetPath;
        QString tempPath;
        QString etag;
        QString lastModified;
        QString state;
        QString errorString;
        QString sha256;
        QString protocol = QStringLiteral("http");
        QString protocolMetadata;
        qint64 totalBytes = 0;
        qint64 downloadedBytes = 0;
        bool acceptsRanges = false;
        bool usingRanges = false;
        int connectionLimit = 16;
    };

    struct SegmentRecord {
        qint64 start = 0;
        qint64 current = 0;
        qint64 end = -1;
    };

    explicit TaskStore(QString databasePath = {});
    ~TaskStore();

    bool ready() const;
    bool saveIncomplete(const TaskRecord &task, const QVector<SegmentRecord> &segments);
    bool markCompleted(const QString &taskId,
                       const QString &finalPath,
                       const QString &sha256,
                       qint64 totalBytes);
    bool removeTask(const QString &taskId);
    bool loadLatestIncomplete(TaskRecord *task, QVector<SegmentRecord> *segments);
    QString lastError() const;

private:
    bool initialise();
    bool pruneCompleted();
    QString databasePath() const;
    void setError(const QString &message);

    QString m_connectionName;
    QString m_databasePathOverride;
    QSqlDatabase m_database;
    QString m_lastError;
};
