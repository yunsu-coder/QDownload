#include "DownloadController.h"

#include "CurlFtpWorker.h"
#include "CurlMultiWorker.h"
#include "FileHashWorker.h"
#include "HlsDownloader.h"
#include "SecureStore.h"
#include "TaskStore.h"

#include <QDateTime>
#include <QClipboard>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QNetworkReply>
#include <QNetworkProxyFactory>
#include <QNetworkProxyQuery>
#include <QNetworkRequest>
#include <QProcess>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QTcpSocket>
#include <QTextStream>
#include <QTimer>
#include <QUuid>

#include <cmath>
#include <memory>

namespace {
constexpr qint64 kSampleIntervalMs = 260;
constexpr qint64 kMinimumRangeFileSize = 16 * 1024 * 1024;
constexpr qint64 kMebibyte = 1024 * 1024;
constexpr qint64 kMinimumChunkSize = 4 * kMebibyte;
constexpr qint64 kMaximumChunkSize = 32 * kMebibyte;
constexpr int kProxyHealthCacheMs = 30000;
constexpr int kIKuuuWarmIdleMs = 120000;

struct ProxyTarget {
    bool valid = false;
    QNetworkProxy::ProxyType type = QNetworkProxy::NoProxy;
    QString host;
    quint16 port = 0;
    QString user;
    QString password;
    QString normalizedUrl;
};

ProxyTarget parseProxyTarget(QString endpoint)
{
    ProxyTarget target;
    endpoint = endpoint.trimmed();
    if (endpoint.isEmpty()) {
        return target;
    }
    if (!endpoint.contains(QStringLiteral("://"))) {
        endpoint.prepend(QStringLiteral("http://"));
    }

    const QUrl url(endpoint);
    if (!url.isValid() || url.host().isEmpty() || url.port() <= 0) {
        return target;
    }

    const QString scheme = url.scheme().toLower();
    if (scheme == QStringLiteral("http") || scheme == QStringLiteral("https")) {
        target.type = QNetworkProxy::HttpProxy;
        target.normalizedUrl =
            QStringLiteral("http://%1:%2").arg(url.host()).arg(url.port());
    } else if (scheme == QStringLiteral("socks5")
               || scheme == QStringLiteral("socks5h")
               || scheme == QStringLiteral("socks")) {
        target.type = QNetworkProxy::Socks5Proxy;
        target.normalizedUrl =
            QStringLiteral("socks5h://%1:%2").arg(url.host()).arg(url.port());
    } else {
        return target;
    }

    target.valid = true;
    target.host = url.host();
    target.port = static_cast<quint16>(url.port());
    target.user = url.userName();
    target.password = url.password();
    return target;
}

bool hostMatches(const QString &host, const QString &suffix)
{
    return host == suffix || host.endsWith(QLatin1Char('.') + suffix);
}

bool isLikelyOverseas(const QUrl &url)
{
    const QString host = url.host().toLower();
    if (host.isEmpty()) {
        return false;
    }

    const QHostAddress address(host);
    if (!address.isNull()) {
        return !(address.isLoopback()
                 || address.isInSubnet(QHostAddress(QStringLiteral("10.0.0.0")), 8)
                 || address.isInSubnet(QHostAddress(QStringLiteral("172.16.0.0")), 12)
                 || address.isInSubnet(QHostAddress(QStringLiteral("192.168.0.0")), 16));
    }
    if (hostMatches(host, QStringLiteral("local"))
        || hostMatches(host, QStringLiteral("localhost"))) {
        return false;
    }
    if (hostMatches(host, QStringLiteral("cn"))) {
        return false;
    }

    static const QStringList domesticDomains{
        QStringLiteral("aliyun.com"),
        QStringLiteral("alicdn.com"),
        QStringLiteral("baidu.com"),
        QStringLiteral("bdstatic.com"),
        QStringLiteral("bilibili.com"),
        QStringLiteral("bilivideo.com"),
        QStringLiteral("bytedance.com"),
        QStringLiteral("byteimg.com"),
        QStringLiteral("csdn.net"),
        QStringLiteral("douban.com"),
        QStringLiteral("doubanio.com"),
        QStringLiteral("douyin.com"),
        QStringLiteral("gtimg.com"),
        QStringLiteral("hdslb.com"),
        QStringLiteral("huawei.com"),
        QStringLiteral("iqiyi.com"),
        QStringLiteral("jd.com"),
        QStringLiteral("kuaishou.com"),
        QStringLiteral("meituan.com"),
        QStringLiteral("mi.com"),
        QStringLiteral("myqcloud.com"),
        QStringLiteral("pinduoduo.com"),
        QStringLiteral("qcloud.com"),
        QStringLiteral("qq.com"),
        QStringLiteral("sina.com"),
        QStringLiteral("sinaimg.cn"),
        QStringLiteral("taobao.com"),
        QStringLiteral("tencent.com"),
        QStringLiteral("tmall.com"),
        QStringLiteral("weibo.com"),
        QStringLiteral("xiaomi.com"),
        QStringLiteral("xhscdn.com"),
        QStringLiteral("xiaohongshu.com"),
        QStringLiteral("youku.com"),
        QStringLiteral("zhihu.com"),
        QStringLiteral("zhimg.com")
    };
    for (const QString &domain : domesticDomains) {
        if (hostMatches(host, domain)) {
            return false;
        }
    }
    return true;
}

void logEvent(const QString &scope, const QString &message)
{
    static QMutex mutex;
    QMutexLocker locker(&mutex);
    const QString dataOverride =
        qEnvironmentVariable("QUICKDL_DATA_DIR").trimmed();
    const QString logDirectory =
        (dataOverride.isEmpty()
             ? QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
             : dataOverride)
        + QStringLiteral("/logs");
    QDir().mkpath(logDirectory);
    QFile file(logDirectory + QStringLiteral("/quickdl.log"));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        return;
    }
    QTextStream stream(&file);
    stream << QDateTime::currentDateTime().toString(Qt::ISODate)
           << " [" << scope << "] " << message << '\n';
}

QNetworkRequest makeProbeRequest(const QUrl &url)
{
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                         QNetworkRequest::AlwaysNetwork);
    request.setTransferTimeout(20000);
    request.setRawHeader("Accept-Encoding", "identity");
    request.setRawHeader("User-Agent", "QuickDL/0.3 (curl multi)");
    return request;
}

QString ftpSecretAccount(const QUrl &url)
{
    if (url.userName().isEmpty()) {
        return {};
    }
    return QStringLiteral("ftp:%1:%2")
        .arg(url.host().toLower(), url.userName());
}

QString memoryHostForUrl(const QUrl &url)
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

bool isRouteRelatedFailure(const QString &message)
{
    const QString lower = message.toLower();
    if (lower.contains(QStringLiteral("源文件已变化"))
        || lower.contains(QStringLiteral("保存"))
        || lower.contains(QStringLiteral("写入"))
        || lower.contains(QStringLiteral("重命名"))
        || lower.contains(QStringLiteral("空间不足"))
        || lower.contains(QStringLiteral("权限"))) {
        return false;
    }
    static const QStringList markers{
        QStringLiteral("connect"),
        QStringLiteral("connection"),
        QStringLiteral("proxy"),
        QStringLiteral("代理"),
        QStringLiteral("timeout"),
        QStringLiteral("超时"),
        QStringLiteral("reset"),
        QStringLiteral("tls"),
        QStringLiteral("ssl"),
        QStringLiteral("certificate"),
        QStringLiteral("dns"),
        QStringLiteral("http 403"),
        QStringLiteral("http 407"),
        QStringLiteral("http 408"),
        QStringLiteral("http 429"),
        QStringLiteral("http 500"),
        QStringLiteral("http 502"),
        QStringLiteral("http 503"),
        QStringLiteral("http 504")
    };
    for (const QString &marker : markers) {
        if (lower.contains(marker)) {
            return true;
        }
    }
    return false;
}
}

DownloadController::DownloadController(QObject *parent)
    : QObject(parent)
    , m_taskStore(std::make_unique<TaskStore>())
    , m_memoryStore(std::make_unique<DownloadMemoryStore>())
{
    m_ikuuuWarmTimer = new QTimer(this);
    m_ikuuuWarmTimer->setSingleShot(true);
    m_ikuuuWarmTimer->setInterval(kIKuuuWarmIdleMs);
    connect(m_ikuuuWarmTimer, &QTimer::timeout, this, [this]() {
        if (m_state == QStringLiteral("Downloading")
            || m_state == QStringLiteral("Probing")
            || m_state == QStringLiteral("Paused")) {
            return;
        }
        closeAutoLaunchedIKuuu();
    });
    connect(&m_embeddedProxy, &EmbeddedProxy::changed,
            this, &DownloadController::embeddedProxyChanged);
    if (QClipboard *clipboard = QGuiApplication::clipboard()) {
        connect(clipboard, &QClipboard::dataChanged,
                this, &DownloadController::clipboardChanged);
    }
    connect(&m_embeddedProxy, &EmbeddedProxy::importFinished,
            this, [this](bool success, const QString &message) {
                m_routeNote = message;
                if (success) {
                    setProxyMode(QStringLiteral("auto"));
                } else {
                    emit routeChanged();
                }
            });

    QSettings settings;
    m_downloadDirectory = settings.value(QStringLiteral("downloadDirectory")).toString();
    m_proxyMode =
        settings.value(QStringLiteral("proxyMode"), QStringLiteral("auto")).toString().toLower();
    if (m_proxyMode != QStringLiteral("auto")
        && m_proxyMode != QStringLiteral("builtin")
        && m_proxyMode != QStringLiteral("proxy")
        && m_proxyMode != QStringLiteral("direct")) {
        m_proxyMode = QStringLiteral("auto");
    }
    m_proxyEndpoint = settings.value(QStringLiteral("proxyEndpoint")).toString();
    m_connectionLimit =
        qBound(4,
               settings.value(QStringLiteral("connectionLimit"), 16).toInt(),
               32);
    if (!parseProxyTarget(m_proxyEndpoint).valid) {
        detectSystemProxy();
    }
    if (!parseProxyTarget(m_proxyEndpoint).valid
        && QFileInfo::exists(QStringLiteral("/Applications/iKuuuVPN.app"))) {
        m_proxyEndpoint = QStringLiteral("http://127.0.0.1:7890");
        logEvent(QStringLiteral("route"),
                 QStringLiteral("using iKuuu default proxy %1").arg(m_proxyEndpoint));
    }

    if (m_downloadDirectory.isEmpty()) {
        m_downloadDirectory = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    }
    if (m_downloadDirectory.isEmpty()) {
        m_downloadDirectory = QDir::homePath();
    }
    if (!m_taskStore->ready()) {
        logEvent(
            QStringLiteral("store"),
            QStringLiteral("task database unavailable: %1")
                .arg(m_taskStore->lastError()));
    } else {
        restorePersistedTask();
    }
    if (!m_memoryStore->ready()) {
        logEvent(
            QStringLiteral("memory"),
            QStringLiteral("download memory unavailable: %1")
                .arg(m_memoryStore->lastError()));
    }
}

DownloadController::~DownloadController()
{
    stopWorker();
    stopFtpWorker();
    if (m_hlsDownloader) {
        m_hlsDownloader->cancel();
        m_hlsDownloader->deleteLater();
        m_hlsDownloader = nullptr;
    }
    persistTaskState();
    if (m_hashWorker) {
        m_hashWorker->requestInterruption();
        m_hashWorker->wait(5000);
        m_hashWorker->deleteLater();
        m_hashWorker = nullptr;
    }
    if (m_probeReply) {
        QObject::disconnect(m_probeReply, nullptr, this, nullptr);
        m_probeReply->abort();
        m_probeReply->deleteLater();
    }
    closeAutoLaunchedIKuuu();
}

QString DownloadController::url() const { return m_url.toString(); }
QString DownloadController::fileName() const { return m_fileName; }
QString DownloadController::downloadDirectory() const { return m_downloadDirectory; }
QString DownloadController::savedPath() const { return m_finalPath; }
QString DownloadController::state() const { return m_state; }
QString DownloadController::errorString() const { return m_errorString; }
QString DownloadController::sha256() const { return m_sha256; }
bool DownloadController::sourceChanged() const { return m_sourceChanged; }
qint64 DownloadController::downloadedBytes() const { return m_downloadedBytes; }
qint64 DownloadController::totalBytes() const { return m_totalBytes; }
double DownloadController::progress() const
{
    if (m_totalBytes <= 0) {
        return 0.0;
    }
    return qBound(0.0, static_cast<double>(m_downloadedBytes) / static_cast<double>(m_totalBytes), 1.0);
}
double DownloadController::speedMBps() const { return m_speedMBps; }
int DownloadController::etaSeconds() const { return m_etaSeconds; }
int DownloadController::activeLanes() const { return m_activeLanes; }
bool DownloadController::hasTask() const { return m_hasTask; }
bool DownloadController::isPaused() const { return m_state == QStringLiteral("Paused"); }
QString DownloadController::proxyMode() const { return m_proxyMode; }
QString DownloadController::proxyEndpoint() const
{
    if (m_useEmbeddedProxy
        || ((m_proxyMode == QStringLiteral("builtin")
             || m_proxyMode == QStringLiteral("auto"))
            && m_embeddedProxy.configured())) {
        return m_embeddedProxy.endpoint();
    }
    return m_proxyEndpoint;
}
bool DownloadController::proxyConfigured() const
{
    if (m_proxyMode == QStringLiteral("builtin")) {
        return m_embeddedProxy.configured();
    }
    if (m_proxyMode == QStringLiteral("auto") && m_embeddedProxy.configured()) {
        return true;
    }
    return parseProxyTarget(m_proxyEndpoint).valid;
}
bool DownloadController::clipboardHasUrl() const
{
    return !clipboardUrl().isEmpty();
}
QString DownloadController::activeRoute() const
{
    if (!m_routeViaProxy) {
        return QStringLiteral("直连");
    }
    const ProxyTarget target = parseProxyTarget(activeProxyEndpoint());
    if (m_useEmbeddedProxy) {
        return target.valid
            ? QStringLiteral("内置代理 · %1:%2").arg(target.host).arg(target.port)
            : QStringLiteral("内置代理");
    }
    return target.valid
        ? QStringLiteral("iKuuu · %1:%2").arg(target.host).arg(target.port)
        : QStringLiteral("代理");
}
QString DownloadController::routeNote() const { return m_routeNote; }
int DownloadController::connectionLimit() const { return m_connectionLimit; }
bool DownloadController::restoredTask() const { return m_restoredTask; }
QString DownloadController::protocolName() const
{
    return ProtocolRouter::displayName(m_protocol);
}
QStringList DownloadController::hlsVariants() const { return m_hlsVariants; }
int DownloadController::completedSegments() const { return m_completedSegments; }
int DownloadController::totalSegments() const { return m_totalSegments; }
QString DownloadController::memoryHint() const { return m_memoryHint; }
bool DownloadController::embeddedProxyConfigured() const
{
    return m_embeddedProxy.configured();
}
bool DownloadController::embeddedProxyRunning() const
{
    return m_embeddedProxy.running();
}
QString DownloadController::embeddedProxyStatus() const
{
    return m_embeddedProxy.status();
}

void DownloadController::setState(const QString &newState)
{
    if (m_state == newState) {
        return;
    }
    m_state = newState;
    emit stateChanged();
}

void DownloadController::startDownload(const QString &url)
{
    const ProtocolRouter::Route route = ProtocolRouter::route(url);
    if (!ProtocolRouter::isSupported(route.protocol)) {
        fail(route.error);
        return;
    }
    if (route.protocol == ProtocolRouter::Protocol::Hls) {
        beginHlsTask(route.url);
        return;
    }
    if (route.protocol == ProtocolRouter::Protocol::Ftp
        || route.protocol == ProtocolRouter::Protocol::Ftps) {
        beginFtpTask(route.url, route.protocol);
        return;
    }
    QUrl parsed = route.url;

    stopWorker();
    if (m_hlsDownloader) {
        m_hlsDownloader->cancel();
        m_hlsDownloader->deleteLater();
        m_hlsDownloader = nullptr;
    }
    cancelIKuuuWarmClose();
    if (m_probeReply) {
        QObject::disconnect(m_probeReply, nullptr, this, nullptr);
        m_probeReply->abort();
        m_probeReply->deleteLater();
        m_probeReply.clear();
    }
    if (!m_tempPath.isEmpty() && QFileInfo::exists(m_tempPath)) {
        QFile::remove(m_tempPath);
    }

    resetForNewTask();
    m_protocol =
        parsed.scheme() == QStringLiteral("http")
        ? ProtocolRouter::Protocol::Http
        : ProtocolRouter::Protocol::Https;
    m_taskId =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_url = parsed;
    m_effectiveUrl = parsed;
    m_fileName = safeFileName(fileNameFromUrl(parsed));
    if (m_fileName.isEmpty()) {
        m_fileName = QStringLiteral("download.bin");
    }
    m_finalPath = chooseTargetPath(m_fileName);
    m_tempPath = m_finalPath + QStringLiteral(".")
        + m_taskId.left(8) + QStringLiteral(".part");
    m_hasTask = true;
    m_errorString.clear();
    m_sha256.clear();
    m_etag.clear();
    m_lastModified.clear();
    m_sourceChanged = false;
    m_restoredTask = false;
    m_acceptsRanges = false;
    m_directProbeAttempted = false;
    m_proxyProbeAttempted = false;
    m_proxyTransferFallbackAttempted = false;
    m_directTransferFallbackAttempted = false;
    m_segmentedRetryAttempted = false;

    if (!prepareRouteForProbe()) {
        return;
    }

    emit taskChanged();
    emit errorChanged();
    setState(QStringLiteral("Probing"));
    emit progressChanged();
    persistTaskState();
    logEvent(QStringLiteral("task"),
             QStringLiteral("probe original=%1")
                 .arg(ProtocolRouter::redactedUrl(parsed)));
    startProbe();
}

void DownloadController::beginHlsTask(const QUrl &url)
{
    stopWorker();
    stopFtpWorker();
    if (m_hlsDownloader) {
        m_hlsDownloader->cancel();
        m_hlsDownloader->deleteLater();
        m_hlsDownloader = nullptr;
    }
    if (m_probeReply) {
        QObject::disconnect(m_probeReply, nullptr, this, nullptr);
        m_probeReply->abort();
        m_probeReply->deleteLater();
        m_probeReply.clear();
    }
    cleanupHlsArtifacts();
    if (!m_tempPath.isEmpty() && QFileInfo::exists(m_tempPath)) {
        QFile::remove(m_tempPath);
    }

    resetForNewTask();
    m_protocol = ProtocolRouter::Protocol::Hls;
    m_taskId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_url = url;
    m_effectiveUrl = url;
    QString baseName =
        QFileInfo(fileNameFromUrl(url)).completeBaseName();
    if (baseName.isEmpty()) {
        baseName = QStringLiteral("video");
    }
    m_fileName = safeFileName(baseName + QStringLiteral(".ts"));
    m_finalPath = chooseTargetPath(m_fileName);
    m_tempPath = m_finalPath + QStringLiteral(".")
        + m_taskId.left(8) + QStringLiteral(".part");
    m_hlsWorkDirectory = QDir(QFileInfo(m_finalPath).absolutePath())
        .filePath(QStringLiteral(".%1.hls").arg(m_taskId));
    m_hasTask = true;
    m_restoredTask = false;
    m_errorString.clear();
    m_sha256.clear();
    m_totalBytes = 0;
    m_downloadedBytes = 0;
    m_totalSegments = 0;
    m_completedSegments = 0;
    m_protocolMetadata.clear();
    m_ftpTransferUrl.clear();
    m_ftpResumePending = false;
    m_hlsVariants.clear();
    emit hlsVariantsChanged();
    emit taskChanged();
    emit errorChanged();
    emit progressChanged();
    if (!prepareRouteForProbe()) {
        return;
    }
    setState(QStringLiteral("Probing"));
    persistTaskState();
    logEvent(
        QStringLiteral("task"),
        QStringLiteral("hls original=%1")
            .arg(ProtocolRouter::redactedUrl(url)));
    startHlsEngine(false);
}

void DownloadController::startHlsEngine(bool restored)
{
    if (!m_hlsDownloader) {
        m_hlsDownloader = new HlsDownloader(&m_network, this);
        connect(
            m_hlsDownloader,
            &HlsDownloader::variantsReady,
            this,
            [this](const QStringList &labels) {
                m_hlsVariants = labels;
                setState(QStringLiteral("Selecting"));
                emit hlsVariantsChanged();
                emit taskChanged();
                persistTaskState();
            });
        connect(
            m_hlsDownloader,
            &HlsDownloader::mediaReady,
            this,
            [this](const QString &metadataJson) {
                m_protocolMetadata = metadataJson;
                const QJsonObject metadata =
                    QJsonDocument::fromJson(metadataJson.toUtf8()).object();
                QString extension =
                    metadata.value(QStringLiteral("extension"))
                        .toString(QStringLiteral("ts"));
                if (extension == QStringLiteral("m4s")) {
                    extension = QStringLiteral("mp4");
                }
                QString baseName =
                    QFileInfo(fileNameFromUrl(m_url)).completeBaseName();
                if (baseName.isEmpty()) {
                    baseName = QStringLiteral("video");
                }
                m_fileName = safeFileName(
                    baseName + QLatin1Char('.') + extension);
                m_finalPath = chooseTargetPath(m_fileName);
                m_totalSegments =
                    metadata.value(QStringLiteral("segmentCount")).toInt();
                m_completedSegments =
                    m_hlsDownloader
                    ? m_hlsDownloader->completedSegments() : 0;
                setState(QStringLiteral("Downloading"));
                emit taskChanged();
                emit progressChanged();
                persistTaskState();
            });
        connect(
            m_hlsDownloader,
            &HlsDownloader::progress,
            this,
            &DownloadController::handleHlsProgress);
        connect(
            m_hlsDownloader,
            &HlsDownloader::failed,
            this,
            [this](const QString &error) {
                fail(error);
            });
        connect(
            m_hlsDownloader,
            &HlsDownloader::paused,
            this,
            [this]() {
                m_activeLanes = 0;
                m_speedMBps = 0.0;
                m_etaSeconds = 0;
                setState(QStringLiteral("Paused"));
                emit lanesChanged();
                emit progressChanged();
                persistTaskState();
            });
        connect(
            m_hlsDownloader,
            &HlsDownloader::hashRequired,
            this,
            [this](qint64 totalBytes) {
                m_totalBytes = totalBytes;
                m_downloadedBytes = totalBytes;
                m_completedSegments = m_totalSegments;
                HlsDownloader *downloader = m_hlsDownloader;
                m_hlsDownloader = nullptr;
                if (downloader) {
                    downloader->deleteLater();
                }
                emit progressChanged();
                finishTask();
            });
    }
    configureNetworkProxy();

    if (restored && !m_protocolMetadata.isEmpty()) {
        const QJsonObject metadata =
            QJsonDocument::fromJson(m_protocolMetadata.toUtf8()).object();
        const QUrl mediaUrl(metadata.value(QStringLiteral("mediaUrl")).toString());
        const int expectedSegments =
            metadata.value(QStringLiteral("segmentCount")).toInt();
        if (mediaUrl.isValid() && expectedSegments > 0) {
            m_hlsDownloader->restoreMedia(
                mediaUrl,
                m_hlsWorkDirectory,
                m_tempPath,
                expectedSegments);
            return;
        }
    }
    m_hlsDownloader->prepare(m_url, m_hlsWorkDirectory, m_tempPath);
}

void DownloadController::selectHlsVariant(int index)
{
    if (m_protocol != ProtocolRouter::Protocol::Hls
        || m_state != QStringLiteral("Selecting")
        || !m_hlsDownloader) {
        return;
    }
    setState(QStringLiteral("Probing"));
    persistTaskState();
    m_hlsDownloader->selectVariant(index);
}

void DownloadController::handleHlsProgress(
    qint64 bytesWritten,
    int activeRequests,
    int completedSegments,
    int totalSegments)
{
    if (m_protocol != ProtocolRouter::Protocol::Hls
        || m_state == QStringLiteral("Paused")) {
        return;
    }
    m_downloadedBytes = bytesWritten;
    m_completedSegments = completedSegments;
    m_totalSegments = totalSegments;
    m_activeLanes = activeRequests;
    m_peakActiveLanes = qMax(m_peakActiveLanes, activeRequests);
    updateSpeed();
    m_peakSpeedMBps = qMax(m_peakSpeedMBps, m_speedMBps);
    emit lanesChanged();
    emit progressChanged();
    if (m_persistClock.elapsed() >= 1000
        || qAbs(m_downloadedBytes - m_lastPersistedBytes)
            >= 8 * 1024 * 1024) {
        persistTaskState();
        m_lastPersistedBytes = m_downloadedBytes;
        m_persistClock.restart();
    }
}

void DownloadController::cleanupHlsArtifacts()
{
    if (!m_hlsWorkDirectory.isEmpty()) {
        QDir directory(m_hlsWorkDirectory);
        if (directory.exists()
            && directory.dirName().startsWith(QLatin1Char('.'))
            && directory.dirName().endsWith(QStringLiteral(".hls"))) {
            directory.removeRecursively();
        }
    }
    m_hlsWorkDirectory.clear();
}

void DownloadController::beginFtpTask(
    const QUrl &url,
    ProtocolRouter::Protocol protocol)
{
    stopWorker();
    stopFtpWorker();
    if (m_hlsDownloader) {
        m_hlsDownloader->cancel();
        m_hlsDownloader->deleteLater();
        m_hlsDownloader = nullptr;
    }
    cleanupHlsArtifacts();
    if (!m_tempPath.isEmpty() && QFileInfo::exists(m_tempPath)) {
        QFile::remove(m_tempPath);
    }

    resetForNewTask();
    m_protocol = protocol;
    m_taskId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QUrl displayUrl = url;
    const QString password = displayUrl.password();
    displayUrl.setPassword({});
    m_url = displayUrl;
    m_effectiveUrl = displayUrl;
    m_ftpTransferUrl = url.toString();
    if (!password.isEmpty()) {
        const QString account = ftpSecretAccount(url);
        if (account.isEmpty()
            || !SecureStore::saveSecret(account, password)) {
            fail(QStringLiteral("无法将 FTP 密码写入系统安全存储。"));
            return;
        }
    }

    m_fileName = safeFileName(fileNameFromUrl(url));
    if (m_fileName.isEmpty()) {
        m_fileName = QStringLiteral("download.bin");
    }
    m_finalPath = chooseTargetPath(m_fileName);
    m_tempPath = m_finalPath + QStringLiteral(".")
        + m_taskId.left(8) + QStringLiteral(".part");
    m_plan.clear();
    m_plan.append({0, -1, 0});
    m_hasTask = true;
    m_usingRanges = false;
    m_acceptsRanges = false;
    m_restoredTask = false;
    m_errorString.clear();
    m_sha256.clear();
    emit taskChanged();
    emit errorChanged();
    emit progressChanged();
    if (!prepareRouteForProbe()) {
        return;
    }
    setState(QStringLiteral("Probing"));
    persistTaskState();
    logEvent(
        QStringLiteral("task"),
        QStringLiteral("ftp original=%1 protocol=%2")
            .arg(
                ProtocolRouter::redactedUrl(url),
                ProtocolRouter::displayName(protocol)));
    startFtpWorker(false);
}

void DownloadController::startFtpWorker(bool resume)
{
    stopFtpWorker();
    if (!m_hasTask || m_taskId.isEmpty()) {
        return;
    }

    QUrl transferUrl(m_url);
    const QString account = ftpSecretAccount(transferUrl);
    if (!account.isEmpty() && transferUrl.password().isEmpty()) {
        const QString password = SecureStore::loadSecret(account);
        if (!password.isEmpty()) {
            transferUrl.setPassword(password);
        }
    }
    m_ftpTransferUrl = transferUrl.toString();
    const QString proxyUrl =
        m_routeViaProxy
        ? parseProxyTarget(activeProxyEndpoint()).normalizedUrl : QString();

    auto launch = [this, resume, proxyUrl]() {
        if (!m_hasTask || m_taskId.isEmpty()) {
            return;
        }
        m_ftpWorker =
            new CurlFtpWorker(m_ftpTransferUrl, m_tempPath, proxyUrl, this);
        connect(
            m_ftpWorker,
            &CurlFtpWorker::metadata,
            this,
            [this](qint64 totalBytes) {
                if (totalBytes <= 0) {
                    return;
                }
                m_totalBytes = totalBytes;
                if (m_plan.isEmpty()) {
                    m_plan.append({0, totalBytes - 1, 0});
                } else {
                    m_plan[0].end = totalBytes - 1;
                }
                setState(QStringLiteral("Downloading"));
                emit progressChanged();
                persistTaskState();
            });
        connect(
            m_ftpWorker,
            &CurlFtpWorker::progress,
            this,
            [this](qint64 bytesWritten, qint64 totalBytes) {
                if (totalBytes > 0) {
                    m_totalBytes = totalBytes;
                }
                m_downloadedBytes = bytesWritten;
                if (!m_plan.isEmpty()) {
                    m_plan[0].current = bytesWritten;
                    if (m_totalBytes > 0) {
                        m_plan[0].end = m_totalBytes - 1;
                    }
                }
                m_activeLanes = 1;
                m_peakActiveLanes = qMax(m_peakActiveLanes, 1);
                updateSpeed();
                m_peakSpeedMBps = qMax(m_peakSpeedMBps, m_speedMBps);
                emit lanesChanged();
                emit progressChanged();
                if (m_persistClock.elapsed() >= 1000
                    || qAbs(m_downloadedBytes - m_lastPersistedBytes)
                        >= 8 * 1024 * 1024) {
                    persistTaskState();
                    m_lastPersistedBytes = m_downloadedBytes;
                    m_persistClock.restart();
                }
            });
        connect(
            m_ftpWorker,
            &CurlFtpWorker::resumeUnsupported,
            this,
            [this]() {
                m_routeNote =
                    QStringLiteral("FTP 服务器不支持续传 · 已从头下载");
                emit routeChanged();
                logEvent(QStringLiteral("ftp"), m_routeNote);
            });
        connect(
            m_ftpWorker,
            &CurlFtpWorker::finished,
            this,
            &DownloadController::handleFtpFinished,
            Qt::QueuedConnection);
        setState(resume ? QStringLiteral("Downloading")
                        : QStringLiteral("Probing"));
        emit taskChanged();
        m_lastPersistedBytes = m_downloadedBytes;
        m_persistClock.restart();
        m_ftpWorker->start();
    };

    if (m_routeViaProxy) {
        const int generation = m_taskGeneration;
        ensureProxyReady(
            QStringLiteral("FTP 代理连接"),
            [this, generation, launch = std::move(launch)](bool ready) mutable {
                if (generation != m_taskGeneration) {
                    return;
                }
                if (!ready) {
                    fail(QStringLiteral("FTP 代理没有在可用时间内启动。"));
                    return;
                }
                launch();
            });
        return;
    }
    launch();
}

void DownloadController::handleFtpFinished()
{
    auto *worker = qobject_cast<CurlFtpWorker *>(sender());
    if (!worker || worker != m_ftpWorker) {
        return;
    }
    const CurlFtpWorker::Result result = worker->result();
    const QString error = worker->errorString();
    m_ftpWorker = nullptr;
    worker->deleteLater();
    m_activeLanes = 0;
    emit lanesChanged();
    const qint64 fileSize = QFileInfo(m_tempPath).size();
    m_downloadedBytes = fileSize;
    if (!m_plan.isEmpty()) {
        m_plan[0].current = fileSize;
    }
    if (result == CurlFtpWorker::Result::Paused) {
        m_speedMBps = 0.0;
        m_etaSeconds = 0;
        setState(QStringLiteral("Paused"));
        emit progressChanged();
        persistTaskState();
        if (m_ftpResumePending) {
            m_ftpResumePending = false;
            startFtpWorker(true);
        }
        return;
    }
    if (result == CurlFtpWorker::Result::Failed) {
        fail(error.isEmpty() ? QStringLiteral("FTP 下载失败。") : error);
        return;
    }
    if (m_totalBytes <= 0) {
        m_totalBytes = fileSize;
    }
    if (fileSize <= 0) {
        fail(QStringLiteral("FTP 下载结果为空。"));
        return;
    }
    m_downloadedBytes = fileSize;
    m_totalBytes = fileSize;
    if (!m_plan.isEmpty()) {
        m_plan[0].end = fileSize - 1;
        m_plan[0].current = fileSize;
    }
    finishTask();
}

void DownloadController::stopFtpWorker()
{
    if (!m_ftpWorker) {
        return;
    }
    CurlFtpWorker *worker = m_ftpWorker;
    m_ftpWorker = nullptr;
    QObject::disconnect(worker, nullptr, this, nullptr);
    worker->requestPause();
    worker->wait(5000);
    worker->deleteLater();
}

bool DownloadController::applyMemoryPolicy(const QUrl &url)
{
    m_routeFromMemory = false;
    m_initialConnectionLimit = 4;
    m_memoryHint.clear();
    if (!m_memoryStore || !m_memoryStore->ready()) {
        return false;
    }
    const QString host = memoryHostForUrl(url);
    if (host.isEmpty()) {
        return false;
    }
    const DownloadMemoryStore::HostMemory hostMemory =
        m_memoryStore->lookupHost(host);
    const DownloadMemoryStore::ResourceMemory resourceMemory =
        m_memoryStore->lookupResource(url);

    if (hostMemory.recommendedConnections > 0) {
        m_initialConnectionLimit = qBound(
            4, hostMemory.recommendedConnections, m_connectionLimit);
    }
    if (hostMemory.isValid()) {
        QString routeLabel;
        if (hostMemory.preferredRoute == QStringLiteral("direct")) {
            routeLabel = QStringLiteral("直连");
        } else if (hostMemory.preferredRoute == QStringLiteral("builtin")) {
            routeLabel = QStringLiteral("内置代理");
        } else {
            routeLabel = QStringLiteral("iKuuu");
        }
        m_memoryHint = QStringLiteral("历史：%1 · %2 路 · %3 MB/s")
            .arg(routeLabel)
            .arg(m_initialConnectionLimit)
            .arg(hostMemory.expectedSpeedMBps, 0, 'f', 1);
        if (resourceMemory.supportsRanges && m_memoryHint.size() < 96) {
            m_memoryHint += QStringLiteral(" · Range");
        }
    } else if (resourceMemory.totalBytes > 0) {
        m_memoryHint = QStringLiteral("历史：已知资源元数据");
    }

    if (m_proxyMode != QStringLiteral("auto") || !hostMemory.isValid()) {
        return false;
    }
    if (hostMemory.preferredRoute == QStringLiteral("direct")) {
        m_routeFromMemory = true;
        logEvent(
            QStringLiteral("memory"),
            QStringLiteral("reuse host=%1 route=direct connections=%2")
                .arg(host)
                .arg(m_initialConnectionLimit));
        setDirectRoute(QStringLiteral("历史记忆 · 直连 · %1 路")
                           .arg(m_initialConnectionLimit));
        return true;
    }
    if (hostMemory.preferredRoute == QStringLiteral("builtin")
        && m_embeddedProxy.configured()) {
        m_routeFromMemory = true;
        logEvent(
            QStringLiteral("memory"),
            QStringLiteral("reuse host=%1 route=builtin connections=%2")
                .arg(host)
                .arg(m_initialConnectionLimit));
        setEmbeddedRoute(QStringLiteral("历史记忆 · 内置代理 · %1 路")
                             .arg(m_initialConnectionLimit));
        return true;
    }
    if (hostMemory.preferredRoute == QStringLiteral("ikuuu")
        && canUseProxy()) {
        m_routeFromMemory = true;
        logEvent(
            QStringLiteral("memory"),
            QStringLiteral("reuse host=%1 route=ikuuu connections=%2")
                .arg(host)
                .arg(m_initialConnectionLimit));
        setIKuuuRoute(QStringLiteral("历史记忆 · iKuuu · %1 路")
                          .arg(m_initialConnectionLimit));
        return true;
    }
    return false;
}

bool DownloadController::prepareRouteForProbe()
{
    if (applyMemoryPolicy(m_url)) {
        return true;
    }
    if (m_proxyMode == QStringLiteral("builtin") && !m_embeddedProxy.configured()) {
        fail(QStringLiteral("请先导入 Clash 订阅，再使用内置代理。"));
        return false;
    }
    if (m_proxyMode == QStringLiteral("proxy") && !canUseProxy()) {
        fail(QStringLiteral("代理模式已开启，但没有检测到可用的本地代理。"));
        return false;
    }
    if (m_proxyMode == QStringLiteral("builtin")) {
        setEmbeddedRoute(QStringLiteral("内置代理 · 全部通过 mihomo"));
    } else if (m_proxyMode == QStringLiteral("proxy")) {
        setIKuuuRoute(QStringLiteral("手动模式 · 全部通过 iKuuu"));
    } else if (m_proxyMode == QStringLiteral("direct") || !canUseProxy()) {
        setDirectRoute(
            canUseProxy() ? QStringLiteral("手动模式 · 始终直连")
                          : QStringLiteral("未检测到 iKuuu 代理 · 使用直连"));
    } else {
        setRouteViaProxy(false, QStringLiteral("智能路由 · 正在探测资源"));
    }
    return true;
}

bool DownloadController::clearDownloadMemory()
{
    if (!m_memoryStore || !m_memoryStore->ready()) {
        return false;
    }
    const bool cleared = m_memoryStore->clearAll();
    if (cleared) {
        m_memoryHint.clear();
        m_routeFromMemory = false;
        emit routeChanged();
        logEvent(QStringLiteral("memory"), QStringLiteral("cleared"));
    }
    return cleared;
}

void DownloadController::rememberCurrentOutcome(bool success)
{
    if (!m_memoryStore || !m_memoryStore->ready()
        || !m_hasTask || m_url.isEmpty()) {
        return;
    }
    DownloadMemoryStore::Outcome outcome;
    outcome.url = m_url;
    outcome.effectiveUrl = m_effectiveUrl.toString();
    outcome.finalPath = m_finalPath;
    outcome.fileHash = m_sha256;
    outcome.route = DownloadMemoryStore::routeForProxyUsage(
        m_routeViaProxy, m_useEmbeddedProxy);
    outcome.error = m_errorString;
    outcome.bytes = m_downloadedBytes;
    outcome.speedMBps = m_peakSpeedMBps;
    outcome.peakConnections = qMax(1, m_peakActiveLanes);
    outcome.supportsRanges = m_acceptsRanges;
    outcome.etag = m_etag;
    outcome.lastModified = m_lastModified;
    outcome.success = success;
    if (!m_memoryStore->recordOutcome(outcome)) {
        logEvent(
            QStringLiteral("memory"),
            QStringLiteral("record failed: %1")
                .arg(m_memoryStore->lastError()));
        return;
    }
    logEvent(
        QStringLiteral("memory"),
        QStringLiteral("recorded host=%1 route=%2 success=%3 "
                       "connections=%4 speed=%5")
            .arg(
                memoryHostForUrl(
                    m_effectiveUrl.isEmpty() ? m_url : m_effectiveUrl),
                outcome.route)
            .arg(success)
            .arg(outcome.peakConnections)
            .arg(outcome.speedMBps, 0, 'f', 1));
}

bool DownloadController::persistTaskState()
{
    if (!m_taskStore || !m_taskStore->ready() || !m_hasTask
        || m_taskId.isEmpty() || m_state == QStringLiteral("Completed")) {
        return false;
    }

    TaskStore::TaskRecord task;
    task.id = m_taskId;
    task.url = m_url.toString();
    task.effectiveUrl = m_effectiveUrl.toString();
    task.fileName = m_fileName;
    task.targetPath = m_finalPath;
    task.tempPath = m_tempPath;
    task.etag = m_etag;
    task.lastModified = m_lastModified;
    task.state = m_state;
    task.errorString = m_errorString;
    task.sha256 = m_sha256;
    switch (m_protocol) {
    case ProtocolRouter::Protocol::Hls:
        task.protocol = QStringLiteral("hls");
        break;
    case ProtocolRouter::Protocol::Ftp:
        task.protocol = QStringLiteral("ftp");
        break;
    case ProtocolRouter::Protocol::Ftps:
        task.protocol = QStringLiteral("ftps");
        break;
    default:
        task.protocol = QStringLiteral("http");
        break;
    }
    task.protocolMetadata = m_protocolMetadata;
    task.totalBytes = m_totalBytes;
    task.downloadedBytes = m_downloadedBytes;
    task.acceptsRanges = m_acceptsRanges;
    task.usingRanges = m_usingRanges;
    task.connectionLimit = m_connectionLimit;

    QVector<TaskStore::SegmentRecord> segments;
    segments.reserve(m_plan.size());
    for (const PlannedSegment &segment : m_plan) {
        segments.append({segment.start, segment.current, segment.end});
    }

    const bool saved = m_taskStore->saveIncomplete(task, segments);
    if (!saved) {
        logEvent(
            QStringLiteral("store"),
            QStringLiteral("save failed: %1").arg(m_taskStore->lastError()));
    }
    return saved;
}

void DownloadController::clearPersistedTask()
{
    if (m_taskStore && m_taskStore->ready() && !m_taskId.isEmpty()) {
        if (!m_taskStore->removeTask(m_taskId)) {
            logEvent(
                QStringLiteral("store"),
                QStringLiteral("remove failed: %1")
                    .arg(m_taskStore->lastError()));
        }
    }
}

void DownloadController::restorePersistedTask()
{
    TaskStore::TaskRecord task;
    QVector<TaskStore::SegmentRecord> segments;
    if (!m_taskStore->loadLatestIncomplete(&task, &segments)) {
        return;
    }
    const bool restoredHls = task.protocol == QStringLiteral("hls");
    const bool restoredFtp = task.protocol == QStringLiteral("ftp")
        || task.protocol == QStringLiteral("ftps");
    if (!restoredHls
        && (task.tempPath.isEmpty() || !QFileInfo::exists(task.tempPath)
            || segments.isEmpty())) {
        logEvent(
            QStringLiteral("store"),
            QStringLiteral("skip restore missing temp or segments task=%1")
                .arg(task.id));
        m_taskStore->removeTask(task.id);
        return;
    }

    const QFileInfo temporaryInfo(task.tempPath);
    if (!restoredHls && !restoredFtp && task.totalBytes > 0
        && temporaryInfo.size() != task.totalBytes) {
        logEvent(
            QStringLiteral("store"),
            QStringLiteral("skip restore size mismatch task=%1 expected=%2 actual=%3")
                .arg(task.id)
                .arg(task.totalBytes)
                .arg(temporaryInfo.size()));
        return;
    }
    if (restoredFtp && task.totalBytes > 0
        && temporaryInfo.size() > task.totalBytes) {
        logEvent(
            QStringLiteral("store"),
            QStringLiteral("skip restore oversized ftp temp task=%1")
                .arg(task.id));
        return;
    }

    m_taskId = task.id;
    if (restoredHls) {
        m_protocol = ProtocolRouter::Protocol::Hls;
    } else if (task.protocol == QStringLiteral("ftp")) {
        m_protocol = ProtocolRouter::Protocol::Ftp;
    } else if (task.protocol == QStringLiteral("ftps")) {
        m_protocol = ProtocolRouter::Protocol::Ftps;
    } else {
        m_protocol = QUrl(task.url).scheme() == QStringLiteral("https")
            ? ProtocolRouter::Protocol::Https
            : ProtocolRouter::Protocol::Http;
    }
    m_url = QUrl(task.url);
    m_ftpTransferUrl = m_url.toString();
    m_effectiveUrl = QUrl(task.effectiveUrl);
    if (!m_effectiveUrl.isValid()) {
        m_effectiveUrl = m_url;
    }
    m_fileName = task.fileName;
    m_finalPath = task.targetPath;
    m_tempPath = task.tempPath;
    m_etag = task.etag;
    m_lastModified = task.lastModified;
    m_sha256 = task.sha256;
    m_protocolMetadata = task.protocolMetadata;
    m_totalBytes = task.totalBytes;
    m_acceptsRanges = task.acceptsRanges;
    m_usingRanges = task.usingRanges;
    m_connectionLimit = qBound(4, task.connectionLimit, 32);
    m_plan.clear();
    if (restoredHls) {
        const QJsonObject metadata =
            QJsonDocument::fromJson(m_protocolMetadata.toUtf8()).object();
        m_hlsWorkDirectory =
            metadata.value(QStringLiteral("workDirectory")).toString();
        if (m_hlsWorkDirectory.isEmpty()) {
            m_hlsWorkDirectory =
                QDir(QFileInfo(m_tempPath).absolutePath())
                    .filePath(QStringLiteral(".%1.hls").arg(m_taskId));
        }
        m_totalSegments =
            metadata.value(QStringLiteral("segmentCount")).toInt();
        m_completedSegments = 0;
        m_usingRanges = false;
        m_acceptsRanges = false;
    } else {
        m_plan.reserve(segments.size());
        for (const TaskStore::SegmentRecord &segment : segments) {
            m_plan.append({segment.start, segment.end, segment.current});
        }
        computeDownloadedBytes();
    }
    m_hasTask = true;
    m_restoredTask = true;
    m_errorString = task.errorString;
    m_sourceChanged =
        m_errorString.contains(QStringLiteral("源文件已变化"));
    m_state = task.state == QStringLiteral("Failed")
        ? QStringLiteral("Failed") : QStringLiteral("Paused");
    m_speedMBps = 0.0;
    m_etaSeconds = 0;
    m_activeLanes = 0;
    logEvent(
        QStringLiteral("store"),
        QStringLiteral("restored paused task=%1 segments=%2 bytes=%3")
            .arg(m_taskId)
            .arg(m_plan.size())
            .arg(m_downloadedBytes));
    emit taskChanged();
    emit stateChanged();
    emit errorChanged();
    emit lanesChanged();
    emit progressChanged();
}

void DownloadController::startProbe()
{
    if (m_routeViaProxy) {
        const int generation = m_taskGeneration;
        ensureProxyReady(
            QStringLiteral("代理探测"),
            [this, generation](bool ready) {
                if (generation != m_taskGeneration || m_userPaused) {
                    return;
                }
                if (ready) {
                    startProbeNow();
                    return;
                }
                if (m_proxyMode == QStringLiteral("auto")
                    && !m_directProbeAttempted) {
                    setRouteViaProxy(
                        false,
                        QStringLiteral("代理启动超时 · 回退直连探测"));
                    startProbeNow();
                    return;
                }
                fail(QStringLiteral("代理没有在可用时间内启动。"));
            });
        return;
    }
    startProbeNow();
}

void DownloadController::startProbeNow()
{
    configureNetworkProxy();
    if (m_routeViaProxy) {
        m_proxyProbeAttempted = true;
    } else {
        m_directProbeAttempted = true;
    }
    logEvent(QStringLiteral("route"),
             QStringLiteral("probe route=%1 endpoint=%2")
                 .arg(activeRoute(), activeProxyEndpoint()));
    m_probeReply = m_network.head(makeProbeRequest(m_url));
    connect(m_probeReply, &QNetworkReply::finished,
            this, &DownloadController::handleProbeFinished);
}

void DownloadController::handleProbeFinished()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
    if (!reply || reply != m_probeReply) {
        return;
    }

    if (m_userPaused) {
        reply->deleteLater();
        m_probeReply.clear();
        return;
    }

    const int statusCode =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const qint64 contentLength =
        reply->header(QNetworkRequest::ContentLengthHeader).toLongLong();
    const bool supportsRanges =
        reply->rawHeader("Accept-Ranges").toLower().contains("bytes");
    const QString error = reply->errorString();
    const auto networkError = reply->error();
    const bool routeSensitiveFailure =
        networkError != QNetworkReply::NoError
        || statusCode == 403
        || statusCode == 407
        || statusCode == 451
        || statusCode >= 500;

    if (m_proxyMode == QStringLiteral("auto") && routeSensitiveFailure) {
        if (!m_routeViaProxy && !m_proxyProbeAttempted) {
            if (m_embeddedProxy.configured()) {
                logEvent(QStringLiteral("route"),
                         QStringLiteral("probe fallback direct-to-builtin status=%1")
                             .arg(statusCode));
                reply->deleteLater();
                m_probeReply.clear();
                rememberCurrentOutcome(false);
                setEmbeddedRoute(QStringLiteral("直连失败 · 自动切换内置代理"));
                startProbe();
                return;
            }
            if (canUseProxy()) {
                logEvent(QStringLiteral("route"),
                         QStringLiteral("probe fallback direct-to-proxy status=%1 error=%2")
                             .arg(statusCode)
                             .arg(error));
                reply->deleteLater();
                m_probeReply.clear();
                rememberCurrentOutcome(false);
                setIKuuuRoute(QStringLiteral("直连探测失败 · 自动切换 iKuuu"));
                startProbe();
                return;
            }
        }
        if (m_routeViaProxy && !m_directProbeAttempted) {
            logEvent(QStringLiteral("route"),
                     QStringLiteral("probe fallback proxy-to-direct status=%1 error=%2")
                         .arg(statusCode)
                         .arg(error));
            reply->deleteLater();
            m_probeReply.clear();
            rememberCurrentOutcome(false);
            setRouteViaProxy(false, QStringLiteral("iKuuu 探测失败 · 回退直连"));
            startProbe();
            return;
        }
    }

    const bool transientProbeFailure =
        networkError != QNetworkReply::NoError && statusCode == 0;
    if (transientProbeFailure && m_probeRetries < 2) {
        ++m_probeRetries;
        logEvent(QStringLiteral("probe"),
                 QStringLiteral("retry n=%1 error=%2")
                     .arg(m_probeRetries)
                     .arg(error));
        reply->deleteLater();
        m_probeReply.clear();
        const int generation = m_taskGeneration;
        QTimer::singleShot(350, this, [this, generation]() {
            if (generation == m_taskGeneration
                && m_state == QStringLiteral("Probing")
                && !m_userPaused) {
                startProbeNow();
            }
        });
        return;
    }

    const QUrl probedEffectiveUrl =
        reply->url().isValid() ? reply->url() : m_url;
    const QString probeEtag =
        QString::fromLatin1(reply->rawHeader("ETag"));
    const QString probeLastModified =
        QString::fromLatin1(reply->rawHeader("Last-Modified"));
    const bool sameEffectiveUrl = probedEffectiveUrl == m_effectiveUrl;
    const bool validatorsMatch =
        (m_etag.isEmpty() || probeEtag == m_etag)
        && (m_lastModified.isEmpty()
            || probeLastModified == m_lastModified);
    m_effectiveUrl = probedEffectiveUrl;
    m_acceptsRanges = supportsRanges;

    const QString effectiveName = safeFileName(fileNameFromUrl(m_effectiveUrl));
    const bool sameTargetName =
        effectiveName.isEmpty() || effectiveName == m_fileName;
    const bool canReusePlan =
        (m_reusePlanOnNextProbe || m_validateResumeOnProbe)
        && sameTargetName
        && sameEffectiveUrl
        && validatorsMatch
        && contentLength > 0
        && contentLength == m_totalBytes
        && !m_plan.isEmpty()
        && !m_tempPath.isEmpty()
        && QFileInfo::exists(m_tempPath);
    if (!effectiveName.isEmpty() && effectiveName != m_fileName) {
        m_fileName = effectiveName;
        m_finalPath = chooseTargetPath(m_fileName);
        m_tempPath = m_finalPath + QStringLiteral(".")
            + m_taskId.left(8) + QStringLiteral(".part");
        emit taskChanged();
    }

    logEvent(QStringLiteral("probe"),
             QStringLiteral("status=%1 length=%2 ranges=%3 final=%4")
                 .arg(statusCode)
                 .arg(contentLength)
                 .arg(supportsRanges)
                 .arg(ProtocolRouter::redactedUrl(m_effectiveUrl)));

    reply->deleteLater();
    m_probeReply.clear();

    if (statusCode >= 400 && statusCode != 405 && statusCode != 501) {
        fail(QStringLiteral("HTTP %1 · %2").arg(statusCode).arg(error));
        return;
    }

    if (networkError != QNetworkReply::NoError
        && statusCode != 405 && statusCode != 501) {
        // Some servers reject HEAD; use GET below so the real failure is visible.
        startSingleDownload(
            0,
            m_validateResumeOnProbe || m_reusePlanOnNextProbe);
        return;
    }

    if ((m_validateResumeOnProbe || m_reusePlanOnNextProbe)
        && (!sameEffectiveUrl || !validatorsMatch || !canReusePlan)) {
        m_validateResumeOnProbe = false;
        m_reusePlanOnNextProbe = false;
        m_sourceChanged = true;
        fail(QStringLiteral(
            "源文件已变化，无法安全续传；未完成文件已保留，可重新下载。"));
        return;
    }

    m_etag = probeEtag;
    m_lastModified = probeLastModified;
    m_validateResumeOnProbe = false;
    m_reusePlanOnNextProbe = false;
    m_sourceChanged = false;
    if (m_restoredTask) {
        m_restoredTask = false;
        emit taskChanged();
    }

    if (m_proxyMode == QStringLiteral("auto")
        && !m_routeViaProxy
        && !m_routeFromMemory
        && isLikelyOverseas(m_effectiveUrl)) {
        if (m_embeddedProxy.configured()) {
            setEmbeddedRoute(QStringLiteral("识别为海外资源 · 自动切换内置代理"));
            logEvent(QStringLiteral("route"),
                     QStringLiteral("auto selected builtin final=%1")
                             .arg(ProtocolRouter::redactedUrl(m_effectiveUrl)));
        } else if (canUseProxy()) {
            setIKuuuRoute(
                QStringLiteral("识别为海外资源 · 自动切换 iKuuu"));
            logEvent(QStringLiteral("route"),
                     QStringLiteral("auto selected proxy final=%1 endpoint=%2")
                         .arg(
                             ProtocolRouter::redactedUrl(m_effectiveUrl),
                             m_proxyEndpoint));
        }
    } else if (!m_routeViaProxy) {
        m_routeNote = m_proxyMode == QStringLiteral("auto")
            ? QStringLiteral("识别为国内或本地资源 · 使用直连")
            : m_routeNote;
        emit routeChanged();
    }

    if (canReusePlan && m_usingRanges
        && contentLength >= kMinimumRangeFileSize && supportsRanges) {
        logEvent(QStringLiteral("task"), QStringLiteral("retry resumed existing plan"));
        startWorker(true);
        return;
    }
    if (canReusePlan && !m_usingRanges) {
        logEvent(QStringLiteral("task"), QStringLiteral("retry resumed single connection"));
        startSingleDownload(contentLength, true);
        return;
    }

    if (contentLength >= kMinimumRangeFileSize && supportsRanges) {
        startSegmentedDownload(contentLength);
    } else {
        startSingleDownload(contentLength, false);
    }
}

void DownloadController::startSegmentedDownload(qint64 totalBytes)
{
    m_usingRanges = true;
    m_acceptsRanges = true;
    m_totalBytes = totalBytes;
    m_plan.clear();

    const qint64 targetChunk =
        totalBytes / qMax(1, m_connectionLimit * 4);
    const qint64 alignedChunk =
        ((targetChunk + kMebibyte - 1) / kMebibyte) * kMebibyte;
    const qint64 chunkSize =
        qBound(kMinimumChunkSize, alignedChunk, kMaximumChunkSize);
    const int chunkCount = static_cast<int>(
        (totalBytes + chunkSize - 1) / chunkSize);
    m_plan.reserve(chunkCount);
    for (int i = 0; i < chunkCount; ++i) {
        PlannedSegment segment;
        segment.start = static_cast<qint64>(i) * chunkSize;
        segment.end = qMin(totalBytes, segment.start + chunkSize) - 1;
        segment.current = segment.start;
        m_plan.append(segment);
    }

    persistTaskState();
    startWorker(false);
}

void DownloadController::startSingleDownload(qint64 totalBytes, bool resume)
{
    m_usingRanges = false;
    if (totalBytes > 0) {
        m_totalBytes = totalBytes;
    }

    if (!resume || m_plan.isEmpty()) {
        m_plan.clear();
        PlannedSegment segment;
        segment.start = 0;
        segment.end = m_totalBytes > 0 ? m_totalBytes - 1 : -1;
        segment.current = 0;
        m_plan.append(segment);
    }

    persistTaskState();
    startWorker(resume);
}

void DownloadController::startWorker(bool resume)
{
    if (m_routeViaProxy) {
        const int generation = m_taskGeneration;
        ensureProxyReady(
            QStringLiteral("开始代理下载"),
            [this, generation, resume](bool ready) {
                if (generation != m_taskGeneration || m_userPaused) {
                    return;
                }
                if (ready) {
                    startWorkerNow(resume);
                    return;
                }
                if (m_proxyMode == QStringLiteral("auto")
                    && !m_directTransferFallbackAttempted) {
                    m_directTransferFallbackAttempted = true;
                    rememberCurrentOutcome(false);
                    setRouteViaProxy(
                        false,
                        QStringLiteral("代理启动超时 · 本次回退直连"));
                    startWorkerNow(true);
                    return;
                }
                fail(QStringLiteral("代理没有在可用时间内启动，无法继续下载。"));
            });
        return;
    }
    startWorkerNow(resume);
}

void DownloadController::startWorkerNow(bool resume)
{
    stopWorker();
    configureNetworkProxy();

    QVector<CurlMultiWorker::TransferRange> ranges;
    ranges.reserve(m_plan.size());
    m_downloadedBytes = 0;
    for (const PlannedSegment &segment : m_plan) {
        ranges.append({segment.start, segment.current, segment.end});
        m_downloadedBytes += segment.current - segment.start;
    }

    m_errorString.clear();
    emit errorChanged();
    m_activeLanes = 0;
    emit lanesChanged();
    m_speedMBps = 0.0;
    m_etaSeconds = 0;
    m_lastSampleBytes = m_downloadedBytes;
    m_sampleClock.restart();
    setState(QStringLiteral("Downloading"));
    emit progressChanged();
    m_lastPersistedBytes = m_downloadedBytes;
    m_persistClock.restart();
    persistTaskState();

    auto *worker = new CurlMultiWorker(
        m_effectiveUrl.toString(),
        m_tempPath,
        m_totalBytes,
        ranges,
        m_usingRanges,
        resume,
        m_routeViaProxy ? parseProxyTarget(activeProxyEndpoint()).normalizedUrl : QString(),
        m_connectionLimit,
        m_initialConnectionLimit,
        this);
    m_worker = worker;
    connect(worker, &CurlMultiWorker::progress,
            this, &DownloadController::handleWorkerProgress,
            Qt::QueuedConnection);
    connect(worker, &CurlMultiWorker::planChanged,
            this, &DownloadController::handleWorkerPlanChanged,
            Qt::QueuedConnection);
    connect(worker, &QThread::finished,
            this, &DownloadController::handleWorkerFinished,
            Qt::QueuedConnection);
    worker->start();
    logEvent(QStringLiteral("worker"),
             QStringLiteral("started resume=%1 ranges=%2 total=%3 final=%4 route=%5")
                 .arg(resume)
                 .arg(ranges.size())
                 .arg(m_totalBytes)
                 .arg(
                     ProtocolRouter::redactedUrl(m_effectiveUrl),
                     activeRoute()));
}

void DownloadController::handleWorkerProgress(
    qint64 bytesWritten,
    int activeLanes,
    const QVector<qint64> &offsets)
{
    if (!m_worker || m_userPaused) {
        return;
    }
    if (offsets.size() == m_plan.size()) {
        for (int index = 0; index < m_plan.size(); ++index) {
            m_plan[index].current = offsets[index];
        }
        computeDownloadedBytes();
    } else {
        m_downloadedBytes = bytesWritten;
    }
    if (m_activeLanes != activeLanes) {
        m_activeLanes = activeLanes;
        m_peakActiveLanes = qMax(m_peakActiveLanes, activeLanes);
        emit lanesChanged();
    }
    updateSpeed();
    m_peakSpeedMBps = qMax(m_peakSpeedMBps, m_speedMBps);
    emit progressChanged();
    if (m_persistClock.elapsed() >= 1000
        || qAbs(m_downloadedBytes - m_lastPersistedBytes)
            >= 8 * 1024 * 1024) {
        persistTaskState();
        m_lastPersistedBytes = m_downloadedBytes;
        m_persistClock.restart();
    }
}

void DownloadController::handleWorkerPlanChanged(
    const QVector<qint64> &starts,
    const QVector<qint64> &currents,
    const QVector<qint64> &ends)
{
    if (starts.size() != currents.size()
        || starts.size() != ends.size()) {
        return;
    }
    m_plan.clear();
    m_plan.reserve(starts.size());
    for (int index = 0; index < starts.size(); ++index) {
        m_plan.append({starts[index], ends[index], currents[index]});
    }
    computeDownloadedBytes();
    emit progressChanged();
    persistTaskState();
}

void DownloadController::handleWorkerFinished()
{
    auto *worker = qobject_cast<CurlMultiWorker *>(sender());
    if (!worker || worker != m_worker) {
        return;
    }

    const auto offsets = worker->resumeOffsets();
    if (offsets.size() == m_plan.size()) {
        for (int i = 0; i < m_plan.size(); ++i) {
            m_plan[i].current = offsets[i];
        }
        computeDownloadedBytes();
    }

    const auto result = worker->result();
    const QString error = worker->errorString();
    logEvent(QStringLiteral("worker"),
             QStringLiteral("finished result=%1 offsets=%2 route=%3 error=%4")
                 .arg(static_cast<int>(result))
                 .arg(offsets.size())
                 .arg(activeRoute())
                 .arg(error));
    m_worker = nullptr;
    worker->deleteLater();
    m_activeLanes = 0;
    emit lanesChanged();

    if (m_resumePending && result == CurlMultiWorker::Result::Paused) {
        m_resumePending = false;
        m_userPaused = false;
        startWorker(true);
        return;
    }

    if (m_userPaused || result == CurlMultiWorker::Result::Paused) {
        m_userPaused = false;
        m_activeLanes = 0;
        m_speedMBps = 0.0;
        m_etaSeconds = 0;
        emit lanesChanged();
        setState(QStringLiteral("Paused"));
        emit progressChanged();
        persistTaskState();
        return;
    }

    if (result == CurlMultiWorker::Result::RangeFallback) {
        m_usingRanges = false;
        m_plan.clear();
        startSingleDownload(m_totalBytes, false);
        return;
    }

    if (result == CurlMultiWorker::Result::Failed
        && m_usingRanges
        && !m_segmentedRetryAttempted) {
        m_segmentedRetryAttempted = true;
        logEvent(QStringLiteral("worker"),
                 QStringLiteral("retrying unfinished segmented ranges error=%1")
                     .arg(error));
        startWorker(true);
        return;
    }

    if (result == CurlMultiWorker::Result::Failed) {
        if (m_proxyMode == QStringLiteral("auto")) {
            if (!m_routeViaProxy
                && !m_proxyTransferFallbackAttempted
                && (m_embeddedProxy.configured() || canUseProxy())) {
                m_proxyTransferFallbackAttempted = true;
                logEvent(QStringLiteral("route"),
                         QStringLiteral("transfer fallback direct-to-proxy error=%1")
                             .arg(error));
                if (m_embeddedProxy.configured()) {
                    rememberCurrentOutcome(false);
                    setEmbeddedRoute(
                        QStringLiteral("下载连接失败 · 自动重试内置代理"));
                } else {
                    rememberCurrentOutcome(false);
                    setIKuuuRoute(
                        QStringLiteral("下载连接失败 · 自动重试 iKuuu"));
                }
                startWorker(true);
                return;
            }
            if (m_routeViaProxy && !m_directTransferFallbackAttempted) {
                m_directTransferFallbackAttempted = true;
                logEvent(QStringLiteral("route"),
                         QStringLiteral("transfer fallback proxy-to-direct error=%1")
                             .arg(error));
                rememberCurrentOutcome(false);
                setDirectRoute(
                    QStringLiteral("代理下载失败 · 自动回退直连"));
                startWorker(true);
                return;
            }
        }
        fail(error.isEmpty() ? QStringLiteral("传输失败。") : error);
        return;
    }

    if (result == CurlMultiWorker::Result::Completed) {
        finishTask();
    }
}

void DownloadController::pauseDownload()
{
    if (m_protocol == ProtocolRouter::Protocol::Ftp
        || m_protocol == ProtocolRouter::Protocol::Ftps) {
        if (m_state != QStringLiteral("Downloading")
            && m_state != QStringLiteral("Probing")) {
            return;
        }
        m_userPaused = true;
        if (m_ftpWorker) {
            m_ftpWorker->requestPause();
        }
        m_activeLanes = 0;
        m_speedMBps = 0.0;
        m_etaSeconds = 0;
        emit lanesChanged();
        setState(QStringLiteral("Paused"));
        emit progressChanged();
        persistTaskState();
        return;
    }
    if (m_protocol == ProtocolRouter::Protocol::Hls) {
        if ((m_state != QStringLiteral("Downloading")
             && m_state != QStringLiteral("Probing"))
            || !m_hlsDownloader) {
            return;
        }
        logEvent(
            QStringLiteral("task"),
            QStringLiteral("hls pause requested downloaded=%1")
                .arg(m_downloadedBytes));
        m_userPaused = true;
        m_hlsDownloader->pause();
        m_activeLanes = 0;
        m_speedMBps = 0.0;
        m_etaSeconds = 0;
        emit lanesChanged();
        setState(QStringLiteral("Paused"));
        emit progressChanged();
        persistTaskState();
        return;
    }
    if (m_state != QStringLiteral("Downloading")
        && m_state != QStringLiteral("Probing")) {
        return;
    }

    m_userPaused = true;
    logEvent(QStringLiteral("task"),
             QStringLiteral("pause requested downloaded=%1").arg(m_downloadedBytes));
    if (m_probeReply) {
        QObject::disconnect(m_probeReply, nullptr, this, nullptr);
        m_probeReply->abort();
        m_probeReply->deleteLater();
        m_probeReply.clear();
    }
    if (m_worker) {
        m_worker->requestPause();
    }

    m_activeLanes = 0;
    m_speedMBps = 0.0;
    m_etaSeconds = 0;
    emit lanesChanged();
    setState(QStringLiteral("Paused"));
    emit progressChanged();
    persistTaskState();
}

void DownloadController::resumeDownload()
{
    if (m_state != QStringLiteral("Paused")) {
        return;
    }

    m_userPaused = false;
    logEvent(QStringLiteral("task"), QStringLiteral("resume requested"));
    m_errorString.clear();
    emit errorChanged();

    if (m_protocol == ProtocolRouter::Protocol::Ftp
        || m_protocol == ProtocolRouter::Protocol::Ftps) {
        if (m_ftpWorker && m_ftpWorker->isRunning()) {
            m_ftpResumePending = true;
            return;
        }
        m_userPaused = false;
        startFtpWorker(true);
        return;
    }

    if (m_protocol == ProtocolRouter::Protocol::Hls) {
        m_userPaused = false;
        if (m_hlsDownloader) {
            m_hlsDownloader->resume();
            setState(QStringLiteral("Downloading"));
            return;
        }
        m_routeViaProxy = false;
        m_useEmbeddedProxy = false;
        if (!prepareRouteForProbe()) {
            return;
        }
        setState(QStringLiteral("Probing"));
        persistTaskState();
        startHlsEngine(true);
        return;
    }

    if (m_worker && m_worker->isRunning()) {
        m_resumePending = true;
        return;
    }

    if (m_restoredTask || m_plan.isEmpty()) {
        m_validateResumeOnProbe =
            m_restoredTask && !m_plan.isEmpty();
        m_reusePlanOnNextProbe =
            !m_plan.isEmpty()
            && !m_tempPath.isEmpty()
            && QFileInfo::exists(m_tempPath);
        m_routeViaProxy = false;
        m_useEmbeddedProxy = false;
        m_proxyProbeAttempted = false;
        m_directProbeAttempted = false;
        m_probeRetries = 0;
        setState(QStringLiteral("Probing"));
        persistTaskState();
        if (prepareRouteForProbe()) {
            startProbe();
        }
        return;
    }

    startWorker(true);
}

void DownloadController::cancelDownload()
{
    logEvent(QStringLiteral("task"),
             QStringLiteral("cancel requested downloaded=%1").arg(m_downloadedBytes));
    stopWorker();
    stopFtpWorker();
    if (m_hlsDownloader) {
        m_hlsDownloader->cancel();
        m_hlsDownloader->deleteLater();
        m_hlsDownloader = nullptr;
    }
    cleanupHlsArtifacts();
    if (m_hashWorker) {
        m_hashWorker->requestInterruption();
        m_hashWorker->wait(5000);
        m_hashWorker->deleteLater();
        m_hashWorker = nullptr;
    }
    if (m_probeReply) {
        QObject::disconnect(m_probeReply, nullptr, this, nullptr);
        m_probeReply->abort();
        m_probeReply->deleteLater();
        m_probeReply.clear();
    }
    if (!m_tempPath.isEmpty() && QFileInfo::exists(m_tempPath)) {
        QFile::remove(m_tempPath);
    }
    clearPersistedTask();
    scheduleAutoLaunchedIKuuuClose();
    resetForNewTask();
    setState(QStringLiteral("Idle"));
    emit taskChanged();
    emit progressChanged();
}

void DownloadController::stopWorker()
{
    if (!m_worker) {
        return;
    }
    CurlMultiWorker *worker = m_worker;
    m_worker = nullptr;
    QObject::disconnect(worker, nullptr, this, nullptr);
    worker->requestPause();
    worker->wait(4000);
    worker->deleteLater();
}

void DownloadController::retry()
{
    if (m_state != QStringLiteral("Failed")
        || !m_url.isValid() || m_sourceChanged) {
        return;
    }

    m_reusePlanOnNextProbe =
        !m_plan.isEmpty()
        && !m_tempPath.isEmpty()
        && QFileInfo::exists(m_tempPath);
    m_proxyProbeAttempted = false;
    m_directProbeAttempted = false;
    m_probeRetries = 0;
    m_proxyTransferFallbackAttempted = false;
    m_directTransferFallbackAttempted = false;
    m_segmentedRetryAttempted = false;
    m_routeViaProxy = false;
    m_useEmbeddedProxy = false;
    m_errorString.clear();
    m_speedMBps = 0.0;
    m_etaSeconds = 0;
    m_activeLanes = 0;
    emit errorChanged();
    emit lanesChanged();
    emit progressChanged();
    setState(QStringLiteral("Probing"));
    persistTaskState();
    logEvent(
        QStringLiteral("task"),
        QStringLiteral("retry requested resume=%1").arg(m_reusePlanOnNextProbe));
    if (prepareRouteForProbe()) {
        startProbe();
    }
}

void DownloadController::restartDownload()
{
    if (!m_hasTask || !m_url.isValid()) {
        return;
    }
    const QString restartUrl = m_url.toString();
    logEvent(
        QStringLiteral("task"),
        QStringLiteral("restart from zero requested"));
    stopWorker();
    if (m_probeReply) {
        QObject::disconnect(m_probeReply, nullptr, this, nullptr);
        m_probeReply->abort();
        m_probeReply->deleteLater();
        m_probeReply.clear();
    }
    clearPersistedTask();
    m_sourceChanged = false;
    startDownload(restartUrl);
}

bool DownloadController::copySha256()
{
    if (m_sha256.isEmpty()) {
        return false;
    }
    if (QClipboard *clipboard = QGuiApplication::clipboard()) {
        clipboard->setText(m_sha256);
        return true;
    }
    return false;
}

bool DownloadController::openDownloadFolder()
{
    QString path = m_finalPath;
    if (path.isEmpty() || !QFileInfo::exists(path)) {
        path = m_tempPath;
    }
    QString directory = path.isEmpty() ? m_downloadDirectory : QFileInfo(path).absolutePath();
    if (directory.isEmpty()) {
        directory = QDir::homePath();
    }
    const bool opened =
        QDesktopServices::openUrl(QUrl::fromLocalFile(directory));
    if (!opened) {
        logEvent(
            QStringLiteral("task"),
            QStringLiteral("open folder failed path=%1").arg(directory));
    }
    return opened;
}

void DownloadController::setDownloadDirectory(const QUrl &directory)
{
    if (!directory.isLocalFile() || !QFileInfo(directory.toLocalFile()).isDir()) {
        return;
    }
    m_downloadDirectory = directory.toLocalFile();
    QSettings settings;
    settings.setValue(QStringLiteral("downloadDirectory"), m_downloadDirectory);
    emit directoryChanged();
}

void DownloadController::setDownloadDirectoryForSession(const QString &path)
{
    const QFileInfo directory(path);
    if (!directory.exists() || !directory.isDir()) {
        return;
    }
    m_downloadDirectory = directory.absoluteFilePath();
    emit directoryChanged();
}

void DownloadController::setProxyMode(const QString &mode)
{
    const QString normalized = mode.trimmed().toLower();
    if (normalized != QStringLiteral("auto")
        && normalized != QStringLiteral("builtin")
        && normalized != QStringLiteral("proxy")
        && normalized != QStringLiteral("direct")) {
        return;
    }
    if (m_proxyMode == normalized) {
        return;
    }
    if (m_state == QStringLiteral("Downloading")
        || m_state == QStringLiteral("Probing")) {
        return;
    }

    m_proxyMode = normalized;
    QSettings settings;
    settings.setValue(QStringLiteral("proxyMode"), m_proxyMode);
    m_routeNote = m_proxyMode == QStringLiteral("auto")
        ? QStringLiteral("智能路由 · 下次任务自动选择")
        : (m_proxyMode == QStringLiteral("builtin")
            ? QStringLiteral("内置代理 · 下次任务使用 mihomo")
            : (m_proxyMode == QStringLiteral("proxy")
                ? QStringLiteral("iKuuu 模式 · 下次任务全部走代理")
                : QStringLiteral("直连模式 · 下次任务不使用代理")));
    emit proxyChanged();
    emit routeChanged();
}

void DownloadController::setProxyEndpoint(const QString &endpoint)
{
    const ProxyTarget target = parseProxyTarget(endpoint);
    if (!target.valid) {
        return;
    }
    m_proxyEndpoint = target.normalizedUrl;
    QSettings settings;
    settings.setValue(QStringLiteral("proxyEndpoint"), m_proxyEndpoint);
    emit proxyChanged();
    emit routeChanged();
}

void DownloadController::cycleProxyMode()
{
    if (m_state == QStringLiteral("Downloading")
        || m_state == QStringLiteral("Probing")) {
        return;
    }
    if (m_proxyMode == QStringLiteral("auto")) {
        setProxyMode(
            m_embeddedProxy.configured()
                ? QStringLiteral("builtin")
                : QStringLiteral("proxy"));
    } else if (m_proxyMode == QStringLiteral("builtin")) {
        setProxyMode(QStringLiteral("proxy"));
    } else if (m_proxyMode == QStringLiteral("proxy")) {
        setProxyMode(QStringLiteral("direct"));
    } else {
        setProxyMode(QStringLiteral("auto"));
    }
}

void DownloadController::cycleConnectionLimit()
{
    if (m_state == QStringLiteral("Downloading")
        || m_state == QStringLiteral("Probing")
        || m_state == QStringLiteral("Paused")) {
        return;
    }
    static constexpr int values[] = {4, 8, 16, 24, 32};
    int nextIndex = 0;
    for (int index = 0; index < 5; ++index) {
        if (m_connectionLimit == values[index]) {
            nextIndex = (index + 1) % 5;
            break;
        }
    }
    m_connectionLimit = values[nextIndex];
    QSettings settings;
    settings.setValue(QStringLiteral("connectionLimit"), m_connectionLimit);
    emit connectionLimitChanged();
    logEvent(
        QStringLiteral("settings"),
        QStringLiteral("connection limit=%1").arg(m_connectionLimit));
}

void DownloadController::prewarmProxyForUrl(const QString &url)
{
    if (m_proxyMode == QStringLiteral("direct")
        || !canUseProxy()
        || m_state == QStringLiteral("Downloading")
        || m_state == QStringLiteral("Probing")
        || m_state == QStringLiteral("Paused")) {
        return;
    }

    const QUrl parsed = QUrl::fromUserInput(url.trimmed());
    if (!parsed.isValid()
        || (parsed.scheme() != QStringLiteral("http")
            && parsed.scheme() != QStringLiteral("https"))
        || parsed.host().isEmpty()
        || !parsed.host().contains(QLatin1Char('.'))
        || !isLikelyOverseas(parsed)) {
        return;
    }

    cancelIKuuuWarmClose();
    if (m_proxyMode == QStringLiteral("builtin")
        || (m_proxyMode == QStringLiteral("auto")
            && m_embeddedProxy.configured())) {
        setEmbeddedRoute(QStringLiteral("海外资源预热 · 内置代理"));
    } else if (!m_useEmbeddedProxy) {
        setIKuuuRoute(QStringLiteral("海外资源预热 · iKuuu"));
    }
    const int generation = m_taskGeneration;
    ensureProxyReady(
        QStringLiteral("海外资源预热"),
        [this, generation](bool ready) {
            if (generation != m_taskGeneration || !ready) {
                return;
            }
            scheduleAutoLaunchedIKuuuClose();
        });
}

void DownloadController::importEmbeddedSubscription(const QString &subscriptionUrl)
{
    m_embeddedProxy.importSubscription(subscriptionUrl);
}

void DownloadController::clearEmbeddedSubscription()
{
    m_embeddedProxy.clearSubscription();
    if (m_proxyMode == QStringLiteral("builtin")) {
        setProxyMode(QStringLiteral("auto"));
    }
}

QString DownloadController::clipboardUrl() const
{
    const QClipboard *clipboard = QGuiApplication::clipboard();
    if (!clipboard) {
        return {};
    }
    const QString text = clipboard->text().trimmed();
    if (text.isEmpty()) {
        return {};
    }
    const QUrl url = QUrl::fromUserInput(text);
    if (!url.isValid()
        || url.host().isEmpty()
        || (url.scheme() != QStringLiteral("http")
            && url.scheme() != QStringLiteral("https"))) {
        return {};
    }
    return url.toString();
}

void DownloadController::finishTask()
{
    m_downloadedBytes = m_totalBytes > 0 ? m_totalBytes : m_downloadedBytes;
    const QFileInfo temporaryInfo(m_tempPath);
    if (!temporaryInfo.exists()
        || (m_totalBytes > 0 && temporaryInfo.size() != m_totalBytes)) {
        logEvent(
            QStringLiteral("task"),
            QStringLiteral("completion size check failed expected=%1 actual=%2")
                .arg(m_totalBytes)
                .arg(temporaryInfo.size()));
        fail(QStringLiteral("文件大小校验失败，已保留未完成文件。"));
        return;
    }
    m_activeLanes = 0;
    m_speedMBps = 0.0;
    m_etaSeconds = 0;
    emit lanesChanged();
    setState(QStringLiteral("Completing"));
    emit taskChanged();
    emit progressChanged();
    persistTaskState();

    m_hashWorker = new FileHashWorker(m_tempPath, this);
    connect(
        m_hashWorker,
        &FileHashWorker::hashFinished,
        this,
        &DownloadController::handleHashFinished,
        Qt::QueuedConnection);
    m_hashWorker->start();
    logEvent(
        QStringLiteral("task"),
        QStringLiteral("verifying sha256 temp=%1").arg(m_tempPath));
}

void DownloadController::handleHashFinished(
    const QString &hash,
    const QString &error)
{
    FileHashWorker *worker = qobject_cast<FileHashWorker *>(sender());
    if (!worker || worker != m_hashWorker) {
        return;
    }
    m_hashWorker = nullptr;
    worker->deleteLater();
    if (m_state != QStringLiteral("Completing")) {
        return;
    }
    if (!error.isEmpty() || hash.isEmpty()) {
        fail(error.isEmpty() ? QStringLiteral("SHA-256 计算失败。") : error);
        return;
    }

    const QFileInfo temporaryInfo(m_tempPath);
    if (!temporaryInfo.exists()
        || (m_totalBytes > 0 && temporaryInfo.size() != m_totalBytes)) {
        fail(QStringLiteral("校验期间临时文件发生变化，已保留未完成文件。"));
        return;
    }
    if (QFileInfo::exists(m_finalPath)) {
        m_finalPath = chooseTargetPath(m_fileName);
    }
    if (!QFile::rename(m_tempPath, m_finalPath)) {
        logEvent(
            QStringLiteral("task"),
            QStringLiteral("rename failed temp=%1 final=%2")
                .arg(m_tempPath, m_finalPath));
        fail(QStringLiteral("下载完成，但无法重命名临时文件。"));
        return;
    }

    m_sha256 = hash;
    m_downloadedBytes = m_totalBytes > 0 ? m_totalBytes : m_downloadedBytes;
    m_plan.clear();
    m_restoredTask = false;
    m_sourceChanged = false;
    logEvent(
        QStringLiteral("task"),
        QStringLiteral("completed path=%1 bytes=%2 sha256=%3")
            .arg(m_finalPath)
            .arg(m_downloadedBytes)
            .arg(m_sha256));
    rememberCurrentOutcome(true);
    if (m_protocol == ProtocolRouter::Protocol::Hls) {
        cleanupHlsArtifacts();
    }
    if (m_taskStore && m_taskStore->ready()) {
        if (!m_taskStore->markCompleted(
                m_taskId, m_finalPath, m_sha256, m_downloadedBytes)) {
            logEvent(
                QStringLiteral("store"),
                QStringLiteral("mark completed failed: %1")
                    .arg(m_taskStore->lastError()));
        }
    }
    scheduleAutoLaunchedIKuuuClose();
    setState(QStringLiteral("Completed"));
    emit taskChanged();
    emit progressChanged();
}

void DownloadController::updateSpeed()
{
    const qint64 elapsed = m_sampleClock.elapsed();
    if (elapsed < kSampleIntervalMs) {
        return;
    }

    const qint64 delta = m_downloadedBytes - m_lastSampleBytes;
    const double instant =
        (static_cast<double>(delta) / 1024.0 / 1024.0)
        / (static_cast<double>(elapsed) / 1000.0);
    m_speedMBps = m_speedMBps <= 0.01
        ? instant
        : (m_speedMBps * 0.62 + instant * 0.38);
    m_lastSampleBytes = m_downloadedBytes;
    m_sampleClock.restart();

    if (m_totalBytes > 0 && m_speedMBps > 0.01) {
        const double remainingMb =
            static_cast<double>(qMax<qint64>(0, m_totalBytes - m_downloadedBytes))
            / 1024.0 / 1024.0;
        m_etaSeconds = static_cast<int>(std::ceil(remainingMb / m_speedMBps));
    } else {
        m_etaSeconds = 0;
    }
}

void DownloadController::fail(const QString &message)
{
    if (m_state == QStringLiteral("Failed")) {
        return;
    }
    m_errorString = message;
    logEvent(QStringLiteral("error"), message);
    m_speedMBps = 0.0;
    m_etaSeconds = 0;
    m_activeLanes = 0;
    m_userPaused = false;
    scheduleAutoLaunchedIKuuuClose();
    emit lanesChanged();
    setState(QStringLiteral("Failed"));
    emit errorChanged();
    emit progressChanged();
    if (isRouteRelatedFailure(message)) {
        rememberCurrentOutcome(false);
    }
    persistTaskState();
}

void DownloadController::resetForNewTask()
{
    ++m_taskGeneration;
    const bool stateWasChanged = m_state != QStringLiteral("Idle");
    m_url.clear();
    m_fileName.clear();
    m_tempPath.clear();
    m_finalPath.clear();
    m_taskId.clear();
    m_state = QStringLiteral("Idle");
    m_protocol = ProtocolRouter::Protocol::Http;
    m_errorString.clear();
    m_sha256.clear();
    m_etag.clear();
    m_lastModified.clear();
    m_plan.clear();
    m_downloadedBytes = 0;
    m_totalBytes = 0;
    m_speedMBps = 0.0;
    m_etaSeconds = 0;
    m_activeLanes = 0;
    m_hasTask = false;
    m_userPaused = false;
    m_resumePending = false;
    m_usingRanges = false;
    m_acceptsRanges = false;
    m_sourceChanged = false;
    m_restoredTask = false;
    m_protocolMetadata.clear();
    m_hlsVariants.clear();
    m_hlsWorkDirectory.clear();
    m_completedSegments = 0;
    m_totalSegments = 0;
    m_routeViaProxy = false;
    m_useEmbeddedProxy = false;
    if (!m_ikuuuLaunchedByApp) {
        m_ikuuuLaunchAttempted = false;
        m_ikuuuConnectRequested = false;
    }
    m_directProbeAttempted = false;
    m_proxyProbeAttempted = false;
    m_probeRetries = 0;
    m_proxyTransferFallbackAttempted = false;
    m_directTransferFallbackAttempted = false;
    m_segmentedRetryAttempted = false;
    m_reusePlanOnNextProbe = false;
    m_validateResumeOnProbe = false;
    m_lastPersistedBytes = 0;
    m_lastSampleBytes = 0;
    m_routeFromMemory = false;
    m_initialConnectionLimit = 4;
    m_peakActiveLanes = 0;
    m_peakSpeedMBps = 0.0;
    m_memoryHint.clear();
    if (stateWasChanged) {
        emit stateChanged();
    }
    emit lanesChanged();
}

void DownloadController::computeDownloadedBytes()
{
    m_downloadedBytes = 0;
    for (const PlannedSegment &segment : m_plan) {
        m_downloadedBytes += segment.current - segment.start;
    }
}

void DownloadController::configureNetworkProxy()
{
    QNetworkProxy proxy(QNetworkProxy::NoProxy);
    if (m_routeViaProxy) {
        const ProxyTarget target = parseProxyTarget(activeProxyEndpoint());
        if (target.valid) {
            proxy.setType(target.type);
            proxy.setHostName(target.host);
            proxy.setPort(target.port);
            proxy.setUser(target.user);
            proxy.setPassword(target.password);
        }
    }
    m_network.setProxy(proxy);
}

void DownloadController::ensureProxyReady(
    const QString &reason,
    std::function<void(bool)> callback)
{
    const int generation = m_taskGeneration;
    if (m_useEmbeddedProxy && !m_embeddedProxy.running()) {
        if (!m_embeddedProxy.configured()) {
            QTimer::singleShot(0, this, [callback = std::move(callback)]() {
                callback(false);
            });
            return;
        }
        m_embeddedProxy.ensureRunning(
            [this, generation, reason, callback = std::move(callback)](bool ready) mutable {
                if (generation != m_taskGeneration || m_userPaused) {
                    return;
                }
                if (!ready) {
                    setRouteViaProxy(
                        true,
                        QStringLiteral("内置代理启动失败"));
                    callback(false);
                    return;
                }
                m_proxyRecentlyVerified = false;
                ensureProxyReady(reason, std::move(callback));
            });
        return;
    }

    const ProxyTarget target = parseProxyTarget(activeProxyEndpoint());
    if (!target.valid) {
        QTimer::singleShot(0, this, [callback = std::move(callback)]() {
            callback(false);
        });
        return;
    }
    if (m_proxyRecentlyVerified
        && m_proxyHealthClock.isValid()
        && m_proxyHealthClock.elapsed() < kProxyHealthCacheMs) {
        QTimer::singleShot(
            0,
            this,
            [callback = std::move(callback)]() { callback(true); });
        return;
    }

    auto *timer = new QTimer(this);
    auto *elapsed = new QElapsedTimer();
    auto finished = std::make_shared<bool>(false);
    auto completion =
        std::make_shared<std::function<void(bool)>>(std::move(callback));
    auto stop = std::make_shared<std::function<void(bool)>>();
    *stop = [this, timer, elapsed, finished, completion](bool ready) {
        if (*finished) {
            return;
        }
        *finished = true;
        timer->stop();
        timer->deleteLater();
        delete elapsed;
        (*completion)(ready);
    };

    auto requestConnect = [this, elapsed]() {
        if (m_useEmbeddedProxy
            || !isIKuuuEndpoint()
            || m_ikuuuConnectRequested
            || elapsed->elapsed() < 2000) {
            return;
        }
        m_ikuuuConnectRequested = true;
        const bool requested = QProcess::startDetached(
            QStringLiteral("/usr/bin/open"),
            QStringList{QStringLiteral("ikuuu:///vpn/toggle")});
        logEvent(QStringLiteral("route"),
                 QStringLiteral("requested iKuuu VPN connect success=%1")
                     .arg(requested));
        setRouteViaProxy(
            true,
            QStringLiteral("iKuuu 未连接 · 正在连接节点"));
    };

    auto attempt = std::make_shared<std::function<void()>>();
    *attempt = [this, target, reason, generation, timer, elapsed, stop,
                requestConnect, attempt]() {
        if (generation != m_taskGeneration || m_userPaused) {
            timer->stop();
            timer->deleteLater();
            delete elapsed;
            return;
        }
        checkProxyPort(
            target.host,
            target.port,
            [this, target, reason, generation, timer, elapsed, stop,
             requestConnect, attempt](bool portOnline) {
                if (generation != m_taskGeneration || m_userPaused) {
                    timer->deleteLater();
                    delete elapsed;
                    return;
                }

                if (!portOnline) {
                    m_proxyRecentlyVerified = false;
                    if (!m_ikuuuLaunchAttempted) {
                        logEvent(QStringLiteral("route"),
                                 QStringLiteral("proxy unavailable endpoint=%1 reason=%2")
                                     .arg(activeProxyEndpoint(), reason));
                        if (!isIKuuuEndpoint()) {
                            (*stop)(false);
                            return;
                        }
                        m_ikuuuLaunchAttempted = true;
                        if (!launchIKuuu()) {
                            logEvent(QStringLiteral("route"),
                                     QStringLiteral("failed to launch iKuuu"));
                            setRouteViaProxy(
                                true,
                                QStringLiteral("iKuuu 未运行，且自动启动失败"));
                            (*stop)(false);
                            return;
                        }
                        setRouteViaProxy(
                            true,
                            m_ikuuuLaunchedByApp
                                ? QStringLiteral("QuickDL 正在打开 iKuuu · 等待代理就绪")
                                : QStringLiteral("iKuuu 正在初始化 · 等待代理就绪"));
                    }
                    requestConnect();
                    if (elapsed->elapsed() >= 45000) {
                        logEvent(QStringLiteral("route"),
                                 QStringLiteral("proxy startup timeout endpoint=%1")
                                     .arg(activeProxyEndpoint()));
                        setRouteViaProxy(
                            true,
                            QStringLiteral("iKuuu 连接超时 · 代理端口不可用"));
                        (*stop)(false);
                        return;
                    }
                    timer->start();
                    return;
                }

                auto handleTunnel = [this, reason, generation, elapsed, timer, stop,
                                     requestConnect](bool healthy) {
                    if (generation != m_taskGeneration || m_userPaused) {
                        timer->stop();
                        timer->deleteLater();
                        delete elapsed;
                        return;
                    }
                    if (healthy) {
                        m_proxyRecentlyVerified = true;
                        m_proxyHealthClock.restart();
                        logEvent(QStringLiteral("route"),
                                 QStringLiteral("proxy ready endpoint=%1")
                                     .arg(activeProxyEndpoint()));
                        setRouteViaProxy(
                            true,
                            reason + QStringLiteral(" · 代理已就绪"));
                        (*stop)(true);
                        return;
                    }
                    requestConnect();
                    if (elapsed->elapsed() >= 45000) {
                        logEvent(QStringLiteral("route"),
                                 QStringLiteral("proxy tunnel timeout endpoint=%1")
                                     .arg(activeProxyEndpoint()));
                        setRouteViaProxy(
                            true,
                            QStringLiteral("iKuuu 连接超时 · 隧道不可用"));
                        (*stop)(false);
                        return;
                    }
                    timer->start();
                };

                if (isIKuuuEndpoint()) {
                    checkProxyTunnel(handleTunnel);
                } else {
                    handleTunnel(true);
                }
            });
    };

    elapsed->start();
    timer->setInterval(1000);
    timer->setSingleShot(true);
    connect(timer, &QTimer::timeout, this, [attempt]() { (*attempt)(); });
    (*attempt)();
}

void DownloadController::checkProxyPort(
    const QString &host,
    quint16 port,
    std::function<void(bool)> callback)
{
    QTcpSocket socket;
    socket.connectToHost(host, port);
    const bool online = socket.waitForConnected(650);
    if (online) {
        socket.disconnectFromHost();
    } else {
        socket.abort();
    }
    if (online) {
        logEvent(QStringLiteral("route"),
                 QStringLiteral("proxy port online host=%1 port=%2")
                     .arg(host)
                     .arg(port));
    }
    QTimer::singleShot(
        0,
        this,
        [callback = std::move(callback), online]() { callback(online); });
}

void DownloadController::checkProxyTunnel(std::function<void(bool)> callback)
{
    const ProxyTarget target = parseProxyTarget(activeProxyEndpoint());
    if (!target.valid) {
        QTimer::singleShot(
            0,
            this,
            [callback = std::move(callback)]() { callback(false); });
        return;
    }

    QNetworkProxy proxy(target.type, target.host, target.port);
    proxy.setUser(target.user);
    proxy.setPassword(target.password);

    auto *manager = new QNetworkAccessManager(this);
    manager->setProxy(proxy);
    QNetworkRequest request(QUrl(QStringLiteral("https://cp.cloudflare.com/")));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                         QNetworkRequest::AlwaysNetwork);
    request.setTransferTimeout(6000);
    request.setRawHeader("User-Agent", "QuickDL/0.3 proxy-health-check");

    QNetworkReply *reply = manager->head(request);
    connect(reply, &QNetworkReply::finished, this,
            [manager, reply, callback = std::move(callback)]() mutable {
                const int status =
                    reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                const bool healthy =
                    reply->error() == QNetworkReply::NoError
                    && status >= 200
                    && status < 400;
                logEvent(QStringLiteral("route"),
                         QStringLiteral("proxy tunnel health status=%1 error=%2")
                             .arg(status)
                             .arg(reply->errorString()));
                callback(healthy);
                reply->deleteLater();
                manager->deleteLater();
            });
}

bool DownloadController::launchIKuuu()
{
#ifdef Q_OS_MACOS
    if (isIKuuuProcessRunning()) {
        logEvent(QStringLiteral("route"),
                 QStringLiteral("iKuuu process already running; waiting for proxy"));
        return true;
    }

    const QString appPath = QStringLiteral("/Applications/iKuuuVPN.app");
    if (!QFileInfo::exists(appPath)) {
        return false;
    }
    const bool started =
        QProcess::startDetached(QStringLiteral("/usr/bin/open"), QStringList{appPath});
    if (started) {
        m_ikuuuLaunchedByApp = true;
        logEvent(QStringLiteral("route"),
                 QStringLiteral("opened iKuuu automatically path=%1").arg(appPath));
    }
    return started;
#else
    logEvent(QStringLiteral("route"),
             QStringLiteral("automatic iKuuu launch is not configured on this platform"));
    return false;
#endif
}

void DownloadController::scheduleAutoLaunchedIKuuuClose()
{
    if (!m_ikuuuLaunchedByApp || !m_ikuuuWarmTimer) {
        return;
    }
    m_ikuuuWarmTimer->start();
    m_routeNote =
        QStringLiteral("iKuuu 热连接保留 120 秒 · 新任务将直接复用");
    emit routeChanged();
    logEvent(QStringLiteral("route"),
             QStringLiteral("iKuuu warm retention armed seconds=120"));
}

void DownloadController::cancelIKuuuWarmClose()
{
    if (m_ikuuuWarmTimer) {
        m_ikuuuWarmTimer->stop();
    }
}

void DownloadController::closeAutoLaunchedIKuuu()
{
    if (!m_ikuuuLaunchedByApp) {
        return;
    }
    cancelIKuuuWarmClose();
    m_ikuuuLaunchedByApp = false;
    m_proxyRecentlyVerified = false;
#ifdef Q_OS_MACOS
    const bool requested = QProcess::startDetached(
        QStringLiteral("/usr/bin/osascript"),
        QStringList{
            QStringLiteral("-e"),
            QStringLiteral("tell application id \"org.ikuuu.vpn\" to quit")
        });
    logEvent(QStringLiteral("route"),
             QStringLiteral("closing iKuuu opened by QuickDL success=%1")
                 .arg(requested));
#endif
    m_routeNote = QStringLiteral("iKuuu 已关闭 · 下次任务会重新启动");
    emit routeChanged();
}

bool DownloadController::isIKuuuProcessRunning()
{
#ifdef Q_OS_MACOS
    QProcess process;
    process.start(QStringLiteral("/usr/bin/pgrep"),
                  QStringList{QStringLiteral("-x"), QStringLiteral("iKuuuVPN")});
    if (!process.waitForFinished(800)) {
        process.kill();
        return false;
    }
    return process.exitStatus() == QProcess::NormalExit
        && process.exitCode() == 0;
#else
    return false;
#endif
}

bool DownloadController::isIKuuuEndpoint() const
{
    if (m_useEmbeddedProxy) {
        return false;
    }
    const ProxyTarget target = parseProxyTarget(m_proxyEndpoint);
    return target.valid
        && isLocalProxyHost(target.host)
        && (target.port == 7890 || target.port == 7891)
#ifdef Q_OS_MACOS
        && QFileInfo::exists(QStringLiteral("/Applications/iKuuuVPN.app"))
#endif
        ;
}

bool DownloadController::isLocalProxyHost(const QString &host)
{
    const QString normalized = host.trimmed().toLower();
    return normalized == QStringLiteral("127.0.0.1")
        || normalized == QStringLiteral("localhost")
        || normalized == QStringLiteral("::1");
}

void DownloadController::detectSystemProxy()
{
    const auto proxies = QNetworkProxyFactory::systemProxyForQuery(
        QNetworkProxyQuery(QUrl(QStringLiteral("https://www.google.com"))));
    for (const QNetworkProxy &proxy : proxies) {
        if ((proxy.type() != QNetworkProxy::HttpProxy
             && proxy.type() != QNetworkProxy::Socks5Proxy)
            || proxy.hostName().isEmpty()
            || proxy.port() <= 0) {
            continue;
        }
        const QString scheme =
            proxy.type() == QNetworkProxy::Socks5Proxy
            ? QStringLiteral("socks5h")
            : QStringLiteral("http");
        m_proxyEndpoint = QStringLiteral("%1://%2:%3")
                              .arg(scheme, proxy.hostName())
                              .arg(proxy.port());
        logEvent(QStringLiteral("route"),
                 QStringLiteral("detected system proxy %1").arg(m_proxyEndpoint));
        return;
    }
    logEvent(QStringLiteral("route"), QStringLiteral("no system proxy detected"));
}

void DownloadController::setRouteViaProxy(bool useProxy, const QString &note)
{
    const bool changed = m_routeViaProxy != useProxy || m_routeNote != note;
    m_routeViaProxy = useProxy;
    m_routeNote = note;
    if (changed) {
        emit routeChanged();
    }
}

void DownloadController::setEmbeddedRoute(const QString &note)
{
    m_useEmbeddedProxy = true;
    m_proxyRecentlyVerified = false;
    setRouteViaProxy(true, note);
}

void DownloadController::setIKuuuRoute(const QString &note)
{
    m_useEmbeddedProxy = false;
    m_proxyRecentlyVerified = false;
    if (!parseProxyTarget(m_proxyEndpoint).valid) {
        detectSystemProxy();
    }
    setRouteViaProxy(true, note);
}

void DownloadController::setDirectRoute(const QString &note)
{
    m_useEmbeddedProxy = false;
    setRouteViaProxy(false, note);
}

bool DownloadController::canUseProxy() const
{
    if (m_useEmbeddedProxy || m_proxyMode == QStringLiteral("builtin")) {
        return m_embeddedProxy.configured();
    }
    return parseProxyTarget(m_proxyEndpoint).valid;
}

QString DownloadController::activeProxyEndpoint() const
{
    return m_useEmbeddedProxy ? m_embeddedProxy.endpoint() : m_proxyEndpoint;
}

QString DownloadController::chooseTargetPath(const QString &fileName) const
{
    const QDir dir(m_downloadDirectory);
    const QFileInfo info(fileName);
    const QString stem = info.completeBaseName();
    const QString suffix = info.suffix().isEmpty()
        ? QString()
        : QStringLiteral(".") + info.suffix();

    QString candidate = dir.filePath(fileName);
    if (!QFileInfo::exists(candidate)) {
        return candidate;
    }
    for (int i = 1; i < 10000; ++i) {
        candidate = dir.filePath(QStringLiteral("%1 (%2)%3").arg(stem).arg(i).arg(suffix));
        if (!QFileInfo::exists(candidate)) {
            return candidate;
        }
    }
    return dir.filePath(QStringLiteral("%1-%2%3")
                            .arg(stem)
                            .arg(QDateTime::currentMSecsSinceEpoch())
                            .arg(suffix));
}

QString DownloadController::safeFileName(const QString &name)
{
    QString result = name;
    result.replace(QRegularExpression(QStringLiteral(R"([\\/:*?"<>|\x00-\x1f])")),
                   QStringLiteral("_"));
    result.replace(QRegularExpression(QStringLiteral(R"([. ]+$)")), QString());
    if (result.isEmpty() || result == QStringLiteral(".") || result == QStringLiteral("..")) {
        return QStringLiteral("download.bin");
    }
    return result;
}

QString DownloadController::fileNameFromUrl(const QUrl &url)
{
    QString name = url.path().section(QLatin1Char('/'), -1);
    name = QUrl::fromPercentEncoding(name.toUtf8());
    return name.isEmpty() ? QStringLiteral("download.bin") : name;
}
