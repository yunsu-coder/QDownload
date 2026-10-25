#include "DownloadMemoryStore.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QUuid>

#include <utility>

namespace {
constexpr qint64 kMemoryLifetimeDays = 90;
constexpr double kProxyPreferencePenalty = 0.88;

QString normalizedHost(const QUrl &url)
{
    QString host = url.host().trimmed().toLower();
    if (host.startsWith(QStringLiteral("www."))) {
        host = host.mid(4);
    }
    if (url.port() > 0) {
        host += QStringLiteral(":%1").arg(url.port());
    }
    return host;
}

double reverseRank(const QString &route)
{
    if (route == QStringLiteral("direct")) {
        return 1.0;
    }
    if (route == QStringLiteral("builtin")) {
        return 0.95;
    }
    return 0.9;
}
}

DownloadMemoryStore::DownloadMemoryStore(QString databasePath)
    : m_connectionName(
        QStringLiteral("quickdl-memory-%1")
            .arg(QUuid::createUuid().toString(QUuid::WithoutBraces)))
    , m_databasePathOverride(std::move(databasePath))
{
    initialise();
}

DownloadMemoryStore::~DownloadMemoryStore()
{
    if (m_database.isValid()) {
        m_database.close();
    }
    m_database = {};
    if (QSqlDatabase::contains(m_connectionName)) {
        QSqlDatabase::removeDatabase(m_connectionName);
    }
}

bool DownloadMemoryStore::ready() const
{
    return m_database.isValid() && m_database.isOpen();
}

QString DownloadMemoryStore::lastError() const
{
    return m_lastError;
}

QString DownloadMemoryStore::databasePath() const
{
    if (!m_databasePathOverride.isEmpty()) {
        return m_databasePathOverride;
    }
    const QString overrideRoot =
        qEnvironmentVariable("QUICKDL_DATA_DIR").trimmed();
    const QString root = overrideRoot.isEmpty()
        ? QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
        : overrideRoot;
    return QDir(root).filePath(QStringLiteral("download-memory.sqlite"));
}

bool DownloadMemoryStore::initialise()
{
    const QString path = databasePath();
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        setError(QStringLiteral("无法创建下载记忆目录。"));
        return false;
    }
    m_database = QSqlDatabase::addDatabase(
        QStringLiteral("QSQLITE"), m_connectionName);
    m_database.setDatabaseName(path);
    if (!m_database.open()) {
        setError(m_database.lastError().text());
        return false;
    }
    static_cast<void>(QFile::setPermissions(
        path, QFileDevice::ReadOwner | QFileDevice::WriteOwner));

    QSqlQuery pragma(m_database);
    static_cast<void>(pragma.exec(QStringLiteral("PRAGMA journal_mode=WAL")));
    static_cast<void>(pragma.exec(QStringLiteral("PRAGMA synchronous=NORMAL")));
    static_cast<void>(pragma.exec(QStringLiteral("PRAGMA busy_timeout=2000")));

    QSqlQuery query(m_database);
    if (!query.exec(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS route_memory ("
            "host TEXT NOT NULL,"
            "route TEXT NOT NULL,"
            "successes INTEGER NOT NULL DEFAULT 0,"
            "failures INTEGER NOT NULL DEFAULT 0,"
            "avg_speed REAL NOT NULL DEFAULT 0,"
            "best_speed REAL NOT NULL DEFAULT 0,"
            "best_connections INTEGER NOT NULL DEFAULT 0,"
            "last_success_at INTEGER NOT NULL DEFAULT 0,"
            "last_failure_at INTEGER NOT NULL DEFAULT 0,"
            "supports_ranges INTEGER NOT NULL DEFAULT 0,"
            "PRIMARY KEY(host, route)"
            ")"))) {
        setError(query.lastError().text());
        return false;
    }
    if (!query.exec(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS host_memory ("
            "host TEXT PRIMARY KEY,"
            "preferred_route TEXT NOT NULL,"
            "recommended_connections INTEGER NOT NULL DEFAULT 0,"
            "expected_speed REAL NOT NULL DEFAULT 0,"
            "confidence REAL NOT NULL DEFAULT 0,"
            "supports_ranges INTEGER NOT NULL DEFAULT 0,"
            "updated_at INTEGER NOT NULL"
            ")"))) {
        setError(query.lastError().text());
        return false;
    }
    if (!query.exec(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS resource_memory ("
            "resource_key TEXT PRIMARY KEY,"
            "host TEXT NOT NULL,"
            "file_name TEXT,"
            "total_bytes INTEGER NOT NULL DEFAULT 0,"
            "supports_ranges INTEGER NOT NULL DEFAULT 0,"
            "etag TEXT,"
            "last_modified TEXT,"
            "updated_at INTEGER NOT NULL"
            ")"))) {
        setError(query.lastError().text());
        return false;
    }
    return true;
}

void DownloadMemoryStore::setError(const QString &message)
{
    m_lastError = message;
}

DownloadMemoryStore::HostMemory DownloadMemoryStore::lookupHost(
    const QString &hostValue) const
{
    HostMemory memory;
    if (!ready() || hostValue.trimmed().isEmpty()) {
        return memory;
    }
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT preferred_route,recommended_connections,expected_speed,"
        "confidence,supports_ranges,updated_at "
        "FROM host_memory WHERE host=:host"));
    query.bindValue(QStringLiteral(":host"), hostValue.trimmed().toLower());
    if (!query.exec() || !query.next()) {
        return memory;
    }
    const QDateTime updated =
        QDateTime::fromMSecsSinceEpoch(query.value(5).toLongLong());
    if (!updated.isValid()
        || updated.daysTo(QDateTime::currentDateTime()) > kMemoryLifetimeDays) {
        return memory;
    }
    memory.host = hostValue.trimmed().toLower();
    memory.preferredRoute = query.value(0).toString();
    memory.recommendedConnections = query.value(1).toInt();
    memory.expectedSpeedMBps = query.value(2).toDouble();
    memory.confidence = query.value(3).toDouble();
    memory.supportsRanges = query.value(4).toInt() != 0;
    memory.updatedAt = updated;

    QSqlQuery totals(m_database);
    totals.prepare(QStringLiteral(
        "SELECT coalesce(sum(successes),0),coalesce(sum(failures),0) "
        "FROM route_memory WHERE host=:host"));
    totals.bindValue(QStringLiteral(":host"), memory.host);
    if (totals.exec() && totals.next()) {
        memory.successes = totals.value(0).toInt();
        memory.failures = totals.value(1).toInt();
    }
    return memory;
}

DownloadMemoryStore::ResourceMemory DownloadMemoryStore::lookupResource(
    const QUrl &url) const
{
    ResourceMemory memory;
    if (!ready()) {
        return memory;
    }
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT host,file_name,total_bytes,supports_ranges,etag,"
        "last_modified,updated_at FROM resource_memory "
        "WHERE resource_key=:key"));
    query.bindValue(QStringLiteral(":key"), resourceKey(url));
    if (!query.exec() || !query.next()) {
        return memory;
    }
    memory.host = query.value(0).toString();
    memory.fileName = query.value(1).toString();
    memory.totalBytes = query.value(2).toLongLong();
    memory.supportsRanges = query.value(3).toInt() != 0;
    memory.etag = query.value(4).toString();
    memory.lastModified = query.value(5).toString();
    memory.updatedAt =
        QDateTime::fromMSecsSinceEpoch(query.value(6).toLongLong());
    return memory;
}

bool DownloadMemoryStore::recordOutcome(const Outcome &outcome)
{
    m_lastError.clear();
    const QUrl source = outcome.effectiveUrl.isEmpty()
        ? outcome.url : QUrl(outcome.effectiveUrl);
    const QString host = normalizedHost(source);
    if (!ready() || host.isEmpty() || outcome.route.isEmpty()) {
        return false;
    }
    if (!m_database.transaction()) {
        setError(m_database.lastError().text());
        return false;
    }
    bool ok = updateRouteMemory(host, outcome.route, outcome);
    const QString sourceHost = normalizedHost(outcome.url);
    if (ok && !sourceHost.isEmpty() && sourceHost != host) {
        ok = updateRouteMemory(sourceHost, outcome.route, outcome);
    }
    if (ok && outcome.success) {
        ok = updateResourceMemory(outcome);
    }
    if (ok) {
        ok = refreshPreferredRoute(host);
    }
    if (ok && !sourceHost.isEmpty() && sourceHost != host) {
        ok = refreshPreferredRoute(sourceHost);
    }
    if (!ok) {
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

bool DownloadMemoryStore::updateRouteMemory(
    const QString &host,
    const QString &route,
    const Outcome &outcome)
{
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "INSERT INTO route_memory ("
        "host,route,successes,failures,avg_speed,best_speed,"
        "best_connections,last_success_at,last_failure_at,supports_ranges"
        ") VALUES ("
        ":host,:route,:successes,:failures,:avg_speed,:best_speed,"
        ":best_connections,:last_success_at,:last_failure_at,:supports_ranges"
        ") ON CONFLICT(host,route) DO UPDATE SET "
        "successes=successes+:successes,"
        "failures=failures+:failures,"
        "avg_speed=CASE "
        "WHEN :successes > 0 AND avg_speed > 0 "
        "THEN avg_speed*0.7+:avg_speed*0.3 "
        "WHEN :successes > 0 THEN :avg_speed ELSE avg_speed END,"
        "best_speed=max(best_speed,:best_speed),"
        "best_connections=max(best_connections,:best_connections),"
        "last_success_at=max(last_success_at,:last_success_at),"
        "last_failure_at=max(last_failure_at,:last_failure_at),"
        "supports_ranges=max(supports_ranges,:supports_ranges)"));
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    query.bindValue(QStringLiteral(":host"), host);
    query.bindValue(QStringLiteral(":route"), route);
    query.bindValue(QStringLiteral(":successes"), outcome.success ? 1 : 0);
    query.bindValue(QStringLiteral(":failures"), outcome.success ? 0 : 1);
    query.bindValue(QStringLiteral(":avg_speed"), outcome.speedMBps);
    query.bindValue(QStringLiteral(":best_speed"), outcome.speedMBps);
    query.bindValue(
        QStringLiteral(":best_connections"), outcome.peakConnections);
    query.bindValue(
        QStringLiteral(":last_success_at"), outcome.success ? now : 0);
    query.bindValue(
        QStringLiteral(":last_failure_at"), outcome.success ? 0 : now);
    query.bindValue(
        QStringLiteral(":supports_ranges"), outcome.supportsRanges ? 1 : 0);
    if (!query.exec()) {
        setError(query.lastError().text());
        return false;
    }
    return true;
}

bool DownloadMemoryStore::updateResourceMemory(const Outcome &outcome)
{
    const QUrl source = outcome.effectiveUrl.isEmpty()
        ? outcome.url : QUrl(outcome.effectiveUrl);
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "INSERT INTO resource_memory ("
        "resource_key,host,file_name,total_bytes,supports_ranges,"
        "etag,last_modified,updated_at"
        ") VALUES ("
        ":key,:host,:file_name,:total_bytes,:supports_ranges,"
        ":etag,:last_modified,:updated_at"
        ") ON CONFLICT(resource_key) DO UPDATE SET "
        "host=excluded.host,file_name=excluded.file_name,"
        "total_bytes=excluded.total_bytes,"
        "supports_ranges=excluded.supports_ranges,"
        "etag=excluded.etag,last_modified=excluded.last_modified,"
        "updated_at=excluded.updated_at"));
    query.bindValue(QStringLiteral(":key"), resourceKey(outcome.url));
    query.bindValue(QStringLiteral(":host"), normalizedHost(source));
    query.bindValue(
        QStringLiteral(":file_name"), QFileInfo(outcome.finalPath).fileName());
    query.bindValue(QStringLiteral(":total_bytes"), outcome.bytes);
    query.bindValue(
        QStringLiteral(":supports_ranges"), outcome.supportsRanges ? 1 : 0);
    query.bindValue(QStringLiteral(":etag"), outcome.etag);
    query.bindValue(
        QStringLiteral(":last_modified"), outcome.lastModified);
    query.bindValue(
        QStringLiteral(":updated_at"), QDateTime::currentMSecsSinceEpoch());
    if (!query.exec()) {
        setError(query.lastError().text());
        return false;
    }
    return true;
}

bool DownloadMemoryStore::refreshPreferredRoute(const QString &host)
{
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT route,successes,failures,avg_speed,best_connections,"
        "supports_ranges,last_success_at "
        "FROM route_memory WHERE host=:host"));
    query.bindValue(QStringLiteral(":host"), host);
    if (!query.exec()) {
        setError(query.lastError().text());
        return false;
    }

    QString bestRoute;
    int bestConnections = 0;
    double bestSpeed = 0.0;
    double bestConfidence = 0.0;
    bool supportsRanges = false;
    double bestScore = -1.0;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    while (query.next()) {
        const QString route = query.value(0).toString();
        const int successes = query.value(1).toInt();
        const int failures = query.value(2).toInt();
        const double speed = query.value(3).toDouble();
        const qint64 lastSuccess = query.value(6).toLongLong();
        if (successes <= 0 || lastSuccess <= 0) {
            continue;
        }
        const double ageDays =
            qMax<qint64>(0, now - lastSuccess) / 86400000.0;
        const double recency = qMax(0.15, 1.0 - ageDays / kMemoryLifetimeDays);
        const double confidence =
            recency * (static_cast<double>(successes) + 1.0)
            / (successes + failures + 2.0);
        const double penalty =
            route == QStringLiteral("direct") ? 1.0 : kProxyPreferencePenalty;
        const double score = qMax(0.1, speed) * confidence * penalty
            * reverseRank(route);
        if (score > bestScore) {
            bestScore = score;
            bestRoute = route;
            bestConnections = query.value(4).toInt();
            bestSpeed = speed;
            bestConfidence = confidence;
            supportsRanges = query.value(5).toInt() != 0;
        }
    }

    QSqlQuery update(m_database);
    if (bestRoute.isEmpty()) {
        update.prepare(
            QStringLiteral("DELETE FROM host_memory WHERE host=:host"));
        update.bindValue(QStringLiteral(":host"), host);
    } else {
        update.prepare(QStringLiteral(
            "INSERT INTO host_memory ("
            "host,preferred_route,recommended_connections,expected_speed,"
            "confidence,supports_ranges,updated_at"
            ") VALUES ("
            ":host,:route,:connections,:speed,:confidence,"
            ":supports_ranges,:updated_at"
            ") ON CONFLICT(host) DO UPDATE SET "
            "preferred_route=excluded.preferred_route,"
            "recommended_connections=excluded.recommended_connections,"
            "expected_speed=excluded.expected_speed,"
            "confidence=excluded.confidence,"
            "supports_ranges=excluded.supports_ranges,"
            "updated_at=excluded.updated_at"));
        update.bindValue(QStringLiteral(":host"), host);
        update.bindValue(QStringLiteral(":route"), bestRoute);
        update.bindValue(QStringLiteral(":connections"), bestConnections);
        update.bindValue(QStringLiteral(":speed"), bestSpeed);
        update.bindValue(QStringLiteral(":confidence"), bestConfidence);
        update.bindValue(
            QStringLiteral(":supports_ranges"), supportsRanges ? 1 : 0);
        update.bindValue(QStringLiteral(":updated_at"), now);
    }
    if (!update.exec()) {
        setError(update.lastError().text());
        return false;
    }
    return true;
}

bool DownloadMemoryStore::clearAll()
{
    m_lastError.clear();
    if (!ready()) {
        return false;
    }
    if (!m_database.transaction()) {
        setError(m_database.lastError().text());
        return false;
    }
    QSqlQuery query(m_database);
    for (const QString &table : {
             QStringLiteral("host_memory"),
             QStringLiteral("route_memory"),
             QStringLiteral("resource_memory")}) {
        if (!query.exec(QStringLiteral("DELETE FROM %1").arg(table))) {
            setError(query.lastError().text());
            m_database.rollback();
            return false;
        }
    }
    if (!m_database.commit()) {
        setError(m_database.lastError().text());
        m_database.rollback();
        return false;
    }
    return true;
}

QString DownloadMemoryStore::resourceKey(const QUrl &url) const
{
    QUrl safe = url;
    safe.setPassword({});
    safe.setQuery(QString());
    safe.setFragment(QString());
    safe.setUserInfo(QString());
    return QString::fromLatin1(
        QCryptographicHash::hash(
            safe.toString(QUrl::RemoveUserInfo).toUtf8(),
            QCryptographicHash::Sha256)
            .toHex());
}

QString DownloadMemoryStore::routeForProxyUsage(bool useProxy, bool embedded)
{
    if (!useProxy) {
        return QStringLiteral("direct");
    }
    return embedded ? QStringLiteral("builtin") : QStringLiteral("ikuuu");
}
