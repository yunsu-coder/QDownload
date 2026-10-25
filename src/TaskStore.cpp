#include "TaskStore.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QSet>
#include <QUuid>

#include <utility>

namespace {
constexpr int kSchemaVersion = 2;

QString stateForStorage(const QString &state)
{
    if (state == QStringLiteral("Downloading")
        || state == QStringLiteral("Probing")
        || state == QStringLiteral("Completing")) {
        return QStringLiteral("Paused");
    }
    return state;
}
}

TaskStore::TaskStore(QString databasePath)
    : m_connectionName(
        QStringLiteral("quickdl-task-store-%1")
            .arg(QUuid::createUuid().toString(QUuid::WithoutBraces)))
    , m_databasePathOverride(std::move(databasePath))
{
    initialise();
}

TaskStore::~TaskStore()
{
    if (m_database.isValid()) {
        m_database.close();
    }
    m_database = {};
    if (QSqlDatabase::contains(m_connectionName)) {
        QSqlDatabase::removeDatabase(m_connectionName);
    }
}

bool TaskStore::ready() const
{
    return m_database.isValid() && m_database.isOpen();
}

QString TaskStore::lastError() const
{
    return m_lastError;
}

QString TaskStore::databasePath() const
{
    if (!m_databasePathOverride.isEmpty()) {
        return m_databasePathOverride;
    }
    const QString overrideRoot =
        qEnvironmentVariable("QUICKDL_DATA_DIR").trimmed();
    const QString root = overrideRoot.isEmpty()
        ? QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
        : overrideRoot;
    return QDir(root).filePath(QStringLiteral("tasks.sqlite"));
}

bool TaskStore::initialise()
{
    const QString path = databasePath();
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        setError(QStringLiteral("无法创建任务数据库目录。"));
        return false;
    }

    m_database =
        QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connectionName);
    m_database.setDatabaseName(path);
    if (!m_database.open()) {
        setError(m_database.lastError().text());
        return false;
    }
    static_cast<void>(QFile::setPermissions(
        path,
        QFileDevice::ReadOwner | QFileDevice::WriteOwner));
    QSqlQuery pragma(m_database);
    static_cast<void>(
        pragma.exec(QStringLiteral("PRAGMA journal_mode=WAL")));
    static_cast<void>(
        pragma.exec(QStringLiteral("PRAGMA synchronous=NORMAL")));
    static_cast<void>(
        pragma.exec(QStringLiteral("PRAGMA busy_timeout=2000")));
    static_cast<void>(
        pragma.exec(QStringLiteral("PRAGMA foreign_keys=ON")));

    QSqlQuery query(m_database);
    if (!query.exec(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS download_task ("
            "id TEXT PRIMARY KEY,"
            "state TEXT NOT NULL,"
            "url TEXT NOT NULL,"
            "effective_url TEXT,"
            "file_name TEXT,"
            "target_path TEXT,"
            "temp_path TEXT,"
            "total_bytes INTEGER NOT NULL DEFAULT 0,"
            "downloaded_bytes INTEGER NOT NULL DEFAULT 0,"
            "accepts_ranges INTEGER NOT NULL DEFAULT 0,"
            "using_ranges INTEGER NOT NULL DEFAULT 0,"
            "connection_limit INTEGER NOT NULL DEFAULT 16,"
            "etag TEXT,"
            "last_modified TEXT,"
            "error_message TEXT,"
            "sha256 TEXT,"
            "protocol TEXT NOT NULL DEFAULT 'http',"
            "protocol_metadata TEXT,"
            "updated_at INTEGER NOT NULL"
            ")"))) {
        setError(query.lastError().text());
        return false;
    }

    if (!query.exec(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS download_segment ("
            "task_id TEXT NOT NULL,"
            "segment_index INTEGER NOT NULL,"
            "start_byte INTEGER NOT NULL,"
            "current_byte INTEGER NOT NULL,"
            "end_byte INTEGER NOT NULL,"
            "PRIMARY KEY(task_id, segment_index),"
            "FOREIGN KEY(task_id) REFERENCES download_task(id) ON DELETE CASCADE"
            ")"))) {
        setError(query.lastError().text());
        return false;
    }

    QSet<QString> columns;
    if (query.exec(QStringLiteral("PRAGMA table_info(download_task)"))) {
        while (query.next()) {
            columns.insert(query.value(1).toString());
        }
    }
    if (!columns.contains(QStringLiteral("protocol"))
        && !query.exec(QStringLiteral(
            "ALTER TABLE download_task "
            "ADD COLUMN protocol TEXT NOT NULL DEFAULT 'http'"))) {
        setError(query.lastError().text());
        return false;
    }
    if (!columns.contains(QStringLiteral("protocol_metadata"))
        && !query.exec(QStringLiteral(
            "ALTER TABLE download_task "
            "ADD COLUMN protocol_metadata TEXT"))) {
        setError(query.lastError().text());
        return false;
    }

    if (!query.exec(
            QStringLiteral("PRAGMA user_version=%1").arg(kSchemaVersion))) {
        setError(query.lastError().text());
        return false;
    }
    return true;
}

void TaskStore::setError(const QString &message)
{
    m_lastError = message;
}

bool TaskStore::saveIncomplete(
    const TaskRecord &task,
    const QVector<SegmentRecord> &segments)
{
    m_lastError.clear();
    if (!ready() || task.id.isEmpty()) {
        setError(QStringLiteral("任务数据库不可用。"));
        return false;
    }
    if (!m_database.transaction()) {
        setError(m_database.lastError().text());
        return false;
    }

    QSqlQuery cleanup(m_database);
    cleanup.prepare(QStringLiteral(
        "DELETE FROM download_task "
        "WHERE id <> :id AND state <> 'Completed'"));
    cleanup.bindValue(QStringLiteral(":id"), task.id);
    if (!cleanup.exec()) {
        setError(cleanup.lastError().text());
        m_database.rollback();
        return false;
    }
    if (!cleanup.exec(QStringLiteral(
            "DELETE FROM download_segment "
            "WHERE task_id NOT IN (SELECT id FROM download_task)"))) {
        setError(cleanup.lastError().text());
        m_database.rollback();
        return false;
    }

    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "INSERT INTO download_task ("
        "id,state,url,effective_url,file_name,target_path,temp_path,"
        "total_bytes,downloaded_bytes,accepts_ranges,using_ranges,"
        "connection_limit,etag,last_modified,error_message,sha256,"
        "protocol,protocol_metadata,updated_at"
        ") VALUES ("
        ":id,:state,:url,:effective_url,:file_name,:target_path,:temp_path,"
        ":total_bytes,:downloaded_bytes,:accepts_ranges,:using_ranges,"
        ":connection_limit,:etag,:last_modified,:error_message,:sha256,"
        ":protocol,:protocol_metadata,:updated_at"
        ") ON CONFLICT(id) DO UPDATE SET "
        "state=excluded.state,url=excluded.url,"
        "effective_url=excluded.effective_url,file_name=excluded.file_name,"
        "target_path=excluded.target_path,temp_path=excluded.temp_path,"
        "total_bytes=excluded.total_bytes,"
        "downloaded_bytes=excluded.downloaded_bytes,"
        "accepts_ranges=excluded.accepts_ranges,"
        "using_ranges=excluded.using_ranges,"
        "connection_limit=excluded.connection_limit,etag=excluded.etag,"
        "last_modified=excluded.last_modified,"
        "error_message=excluded.error_message,sha256=excluded.sha256,"
        "protocol=excluded.protocol,"
        "protocol_metadata=excluded.protocol_metadata,"
        "updated_at=excluded.updated_at"));
    query.bindValue(QStringLiteral(":id"), task.id);
    query.bindValue(QStringLiteral(":state"), stateForStorage(task.state));
    query.bindValue(QStringLiteral(":url"), task.url);
    query.bindValue(QStringLiteral(":effective_url"), task.effectiveUrl);
    query.bindValue(QStringLiteral(":file_name"), task.fileName);
    query.bindValue(QStringLiteral(":target_path"), task.targetPath);
    query.bindValue(QStringLiteral(":temp_path"), task.tempPath);
    query.bindValue(QStringLiteral(":total_bytes"), task.totalBytes);
    query.bindValue(QStringLiteral(":downloaded_bytes"), task.downloadedBytes);
    query.bindValue(QStringLiteral(":accepts_ranges"), task.acceptsRanges);
    query.bindValue(QStringLiteral(":using_ranges"), task.usingRanges);
    query.bindValue(QStringLiteral(":connection_limit"), task.connectionLimit);
    query.bindValue(QStringLiteral(":etag"), task.etag);
    query.bindValue(QStringLiteral(":last_modified"), task.lastModified);
    query.bindValue(QStringLiteral(":error_message"), task.errorString);
    query.bindValue(QStringLiteral(":sha256"), task.sha256);
    query.bindValue(QStringLiteral(":protocol"), task.protocol);
    query.bindValue(
        QStringLiteral(":protocol_metadata"), task.protocolMetadata);
    query.bindValue(
        QStringLiteral(":updated_at"),
        QDateTime::currentMSecsSinceEpoch());
    if (!query.exec()) {
        setError(query.lastError().text());
        m_database.rollback();
        return false;
    }

    QSqlQuery removeSegments(m_database);
    removeSegments.prepare(
        QStringLiteral("DELETE FROM download_segment WHERE task_id=:task_id"));
    removeSegments.bindValue(QStringLiteral(":task_id"), task.id);
    if (!removeSegments.exec()) {
        setError(removeSegments.lastError().text());
        m_database.rollback();
        return false;
    }

    QSqlQuery insertSegment(m_database);
    insertSegment.prepare(QStringLiteral(
        "INSERT INTO download_segment ("
        "task_id,segment_index,start_byte,current_byte,end_byte"
        ") VALUES (:task_id,:segment_index,:start_byte,:current_byte,:end_byte)"));
    for (int index = 0; index < segments.size(); ++index) {
        insertSegment.bindValue(QStringLiteral(":task_id"), task.id);
        insertSegment.bindValue(QStringLiteral(":segment_index"), index);
        insertSegment.bindValue(QStringLiteral(":start_byte"), segments[index].start);
        insertSegment.bindValue(
            QStringLiteral(":current_byte"), segments[index].current);
        insertSegment.bindValue(QStringLiteral(":end_byte"), segments[index].end);
        if (!insertSegment.exec()) {
            setError(insertSegment.lastError().text());
            m_database.rollback();
            return false;
        }
    }

    if (!m_database.commit()) {
        setError(m_database.lastError().text());
        m_database.rollback();
        return false;
    }
    static_cast<void>(pruneCompleted());
    return true;
}

bool TaskStore::markCompleted(const QString &taskId,
                              const QString &finalPath,
                              const QString &sha256,
                              qint64 totalBytes)
{
    m_lastError.clear();
    if (!ready() || taskId.isEmpty()) {
        setError(QStringLiteral("任务数据库不可用。"));
        return false;
    }
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "UPDATE download_task SET "
        "state='Completed',target_path=:target_path,temp_path='',"
        "total_bytes=:total_bytes,downloaded_bytes=:total_bytes,"
        "error_message='',sha256=:sha256,updated_at=:updated_at "
        "WHERE id=:id"));
    query.bindValue(QStringLiteral(":target_path"), finalPath);
    query.bindValue(QStringLiteral(":total_bytes"), totalBytes);
    query.bindValue(QStringLiteral(":sha256"), sha256);
    query.bindValue(
        QStringLiteral(":updated_at"),
        QDateTime::currentMSecsSinceEpoch());
    query.bindValue(QStringLiteral(":id"), taskId);
    if (!query.exec()) {
        setError(query.lastError().text());
        return false;
    }

    QSqlQuery removeSegments(m_database);
    removeSegments.prepare(
        QStringLiteral("DELETE FROM download_segment WHERE task_id=:task_id"));
    removeSegments.bindValue(QStringLiteral(":task_id"), taskId);
    if (!removeSegments.exec()) {
        setError(removeSegments.lastError().text());
        return false;
    }
    static_cast<void>(pruneCompleted());
    return true;
}

bool TaskStore::removeTask(const QString &taskId)
{
    m_lastError.clear();
    if (!ready() || taskId.isEmpty()) {
        return false;
    }
    if (!m_database.transaction()) {
        setError(m_database.lastError().text());
        return false;
    }
    QSqlQuery removeSegments(m_database);
    removeSegments.prepare(
        QStringLiteral("DELETE FROM download_segment WHERE task_id=:task_id"));
    removeSegments.bindValue(QStringLiteral(":task_id"), taskId);
    if (!removeSegments.exec()) {
        setError(removeSegments.lastError().text());
        m_database.rollback();
        return false;
    }
    QSqlQuery removeTask(m_database);
    removeTask.prepare(
        QStringLiteral("DELETE FROM download_task WHERE id=:task_id"));
    removeTask.bindValue(QStringLiteral(":task_id"), taskId);
    if (!removeTask.exec()) {
        setError(removeTask.lastError().text());
        m_database.rollback();
        return false;
    }
    if (!m_database.commit()) {
        setError(m_database.lastError().text());
        m_database.rollback();
        return false;
    }
    return true;
}

bool TaskStore::loadLatestIncomplete(
    TaskRecord *task,
    QVector<SegmentRecord> *segments)
{
    m_lastError.clear();
    if (!ready() || !task || !segments) {
        return false;
    }
    QSqlQuery query(m_database);
    if (!query.exec(QStringLiteral(
            "SELECT id,state,url,effective_url,file_name,target_path,temp_path,"
            "total_bytes,downloaded_bytes,accepts_ranges,using_ranges,"
            "connection_limit,etag,last_modified,error_message,sha256,"
            "protocol,protocol_metadata "
            "FROM download_task WHERE state <> 'Completed' "
            "ORDER BY updated_at DESC LIMIT 1"))
        || !query.next()) {
        if (query.lastError().isValid()) {
            setError(query.lastError().text());
        }
        return false;
    }

    *task = {};
    task->id = query.value(0).toString();
    task->state = query.value(1).toString();
    task->url = query.value(2).toString();
    task->effectiveUrl = query.value(3).toString();
    task->fileName = query.value(4).toString();
    task->targetPath = query.value(5).toString();
    task->tempPath = query.value(6).toString();
    task->totalBytes = query.value(7).toLongLong();
    task->downloadedBytes = query.value(8).toLongLong();
    task->acceptsRanges = query.value(9).toInt() != 0;
    task->usingRanges = query.value(10).toInt() != 0;
    task->connectionLimit = query.value(11).toInt();
    task->etag = query.value(12).toString();
    task->lastModified = query.value(13).toString();
    task->errorString = query.value(14).toString();
    task->sha256 = query.value(15).toString();
    task->protocol = query.value(16).toString();
    task->protocolMetadata = query.value(17).toString();

    segments->clear();
    QSqlQuery segmentQuery(m_database);
    segmentQuery.prepare(QStringLiteral(
        "SELECT start_byte,current_byte,end_byte "
        "FROM download_segment WHERE task_id=:task_id "
        "ORDER BY segment_index"));
    segmentQuery.bindValue(QStringLiteral(":task_id"), task->id);
    if (!segmentQuery.exec()) {
        setError(segmentQuery.lastError().text());
        return false;
    }
    while (segmentQuery.next()) {
        SegmentRecord segment;
        segment.start = segmentQuery.value(0).toLongLong();
        segment.current = segmentQuery.value(1).toLongLong();
        segment.end = segmentQuery.value(2).toLongLong();
        segments->append(segment);
    }
    return true;
}

bool TaskStore::pruneCompleted()
{
    QSqlQuery query(m_database);
    return query.exec(QStringLiteral(
        "DELETE FROM download_task WHERE state='Completed' AND id NOT IN ("
        "SELECT id FROM download_task WHERE state='Completed' "
        "ORDER BY updated_at DESC LIMIT 20)"));
}
