#pragma once

#include "EmbeddedProxy.h"
#include "ProtocolRouter.h"

#include <QElapsedTimer>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QUrl>
#include <QVector>

#include <functional>
#include <memory>

class CurlMultiWorker;
class CurlFtpWorker;
class FileHashWorker;
class HlsDownloader;
class QNetworkReply;
class TaskStore;
class QTimer;

class DownloadController final : public QObject
{
    Q_OBJECT

    Q_PROPERTY(QString url READ url NOTIFY taskChanged)
    Q_PROPERTY(QString fileName READ fileName NOTIFY taskChanged)
    Q_PROPERTY(QString downloadDirectory READ downloadDirectory NOTIFY directoryChanged)
    Q_PROPERTY(QString savedPath READ savedPath NOTIFY taskChanged)
    Q_PROPERTY(QString state READ state NOTIFY stateChanged)
    Q_PROPERTY(QString errorString READ errorString NOTIFY errorChanged)
    Q_PROPERTY(QString sha256 READ sha256 NOTIFY taskChanged)
    Q_PROPERTY(bool sourceChanged READ sourceChanged NOTIFY errorChanged)
    Q_PROPERTY(qint64 downloadedBytes READ downloadedBytes NOTIFY progressChanged)
    Q_PROPERTY(qint64 totalBytes READ totalBytes NOTIFY progressChanged)
    Q_PROPERTY(double progress READ progress NOTIFY progressChanged)
    Q_PROPERTY(double speedMBps READ speedMBps NOTIFY progressChanged)
    Q_PROPERTY(int etaSeconds READ etaSeconds NOTIFY progressChanged)
    Q_PROPERTY(int activeLanes READ activeLanes NOTIFY lanesChanged)
    Q_PROPERTY(bool hasTask READ hasTask NOTIFY taskChanged)
    Q_PROPERTY(bool isPaused READ isPaused NOTIFY stateChanged)
    Q_PROPERTY(QString proxyMode READ proxyMode NOTIFY proxyChanged)
    Q_PROPERTY(QString proxyEndpoint READ proxyEndpoint NOTIFY proxyChanged)
    Q_PROPERTY(bool proxyConfigured READ proxyConfigured NOTIFY proxyChanged)
    Q_PROPERTY(bool clipboardHasUrl READ clipboardHasUrl NOTIFY clipboardChanged)
    Q_PROPERTY(QString activeRoute READ activeRoute NOTIFY routeChanged)
    Q_PROPERTY(QString routeNote READ routeNote NOTIFY routeChanged)
    Q_PROPERTY(int connectionLimit READ connectionLimit NOTIFY connectionLimitChanged)
    Q_PROPERTY(bool restoredTask READ restoredTask NOTIFY taskChanged)
    Q_PROPERTY(QString protocolName READ protocolName NOTIFY taskChanged)
    Q_PROPERTY(QStringList hlsVariants READ hlsVariants NOTIFY hlsVariantsChanged)
    Q_PROPERTY(int completedSegments READ completedSegments NOTIFY progressChanged)
    Q_PROPERTY(int totalSegments READ totalSegments NOTIFY progressChanged)
    Q_PROPERTY(bool embeddedProxyConfigured READ embeddedProxyConfigured
               NOTIFY embeddedProxyChanged)
    Q_PROPERTY(bool embeddedProxyRunning READ embeddedProxyRunning
               NOTIFY embeddedProxyChanged)
    Q_PROPERTY(QString embeddedProxyStatus READ embeddedProxyStatus
               NOTIFY embeddedProxyChanged)

public:
    explicit DownloadController(QObject *parent = nullptr);
    ~DownloadController() override;

    QString url() const;
    QString fileName() const;
    QString downloadDirectory() const;
    QString savedPath() const;
    QString state() const;
    QString errorString() const;
    QString sha256() const;
    bool sourceChanged() const;
    qint64 downloadedBytes() const;
    qint64 totalBytes() const;
    double progress() const;
    double speedMBps() const;
    int etaSeconds() const;
    int activeLanes() const;
    bool hasTask() const;
    bool isPaused() const;
    QString proxyMode() const;
    QString proxyEndpoint() const;
    bool proxyConfigured() const;
    bool clipboardHasUrl() const;
    QString activeRoute() const;
    QString routeNote() const;
    int connectionLimit() const;
    bool restoredTask() const;
    QString protocolName() const;
    QStringList hlsVariants() const;
    int completedSegments() const;
    int totalSegments() const;
    bool embeddedProxyConfigured() const;
    bool embeddedProxyRunning() const;
    QString embeddedProxyStatus() const;

    Q_INVOKABLE void startDownload(const QString &url);
    Q_INVOKABLE void pauseDownload();
    Q_INVOKABLE void resumeDownload();
    Q_INVOKABLE void cancelDownload();
    Q_INVOKABLE void retry();
    Q_INVOKABLE void restartDownload();
    Q_INVOKABLE void selectHlsVariant(int index);
    Q_INVOKABLE bool copySha256();
    Q_INVOKABLE bool openDownloadFolder();
    Q_INVOKABLE void setDownloadDirectory(const QUrl &directory);
    void setDownloadDirectoryForSession(const QString &path);
    Q_INVOKABLE void setProxyMode(const QString &mode);
    Q_INVOKABLE void setProxyEndpoint(const QString &endpoint);
    Q_INVOKABLE void cycleProxyMode();
    Q_INVOKABLE void cycleConnectionLimit();
    Q_INVOKABLE void prewarmProxyForUrl(const QString &url);
    Q_INVOKABLE void importEmbeddedSubscription(const QString &subscriptionUrl);
    Q_INVOKABLE void clearEmbeddedSubscription();
    Q_INVOKABLE QString clipboardUrl() const;

signals:
    void taskChanged();
    void directoryChanged();
    void stateChanged();
    void lanesChanged();
    void errorChanged();
    void progressChanged();
    void proxyChanged();
    void routeChanged();
    void embeddedProxyChanged();
    void clipboardChanged();
    void connectionLimitChanged();
    void hlsVariantsChanged();

private:
    struct PlannedSegment {
        qint64 start = 0;
        qint64 end = -1;
        qint64 current = 0;
    };

    void startProbe();
    void startProbeNow();
    void handleProbeFinished();
    void startSegmentedDownload(qint64 totalBytes);
    void startSingleDownload(qint64 totalBytes, bool resume);
    void startWorker(bool resume);
    void startWorkerNow(bool resume);
    void handleWorkerProgress(qint64 bytesWritten,
                              int activeLanes,
                              const QVector<qint64> &offsets);
    void handleWorkerPlanChanged(const QVector<qint64> &starts,
                                 const QVector<qint64> &currents,
                                 const QVector<qint64> &ends);
    void handleWorkerFinished();
    void stopWorker();
    void finishTask();
    void handleHashFinished(const QString &hash, const QString &error);
    void setState(const QString &newState);
    void fail(const QString &message);
    void updateSpeed();
    void resetForNewTask();
    void computeDownloadedBytes();
    void configureNetworkProxy();
    bool prepareRouteForProbe();
    void restorePersistedTask();
    bool persistTaskState();
    void clearPersistedTask();
    void beginHlsTask(const QUrl &url);
    void beginFtpTask(const QUrl &url,
                      ProtocolRouter::Protocol protocol);
    void startHlsEngine(bool restored);
    void cleanupHlsArtifacts();
    void startFtpWorker(bool resume);
    void stopFtpWorker();
    void handleFtpFinished();
    void handleHlsProgress(qint64 bytesWritten,
                           int activeRequests,
                           int completedSegments,
                           int totalSegments);
    void detectSystemProxy();
    void setRouteViaProxy(bool useProxy, const QString &note);
    void setEmbeddedRoute(const QString &note);
    void setIKuuuRoute(const QString &note);
    void setDirectRoute(const QString &note);
    bool canUseProxy() const;
    QString activeProxyEndpoint() const;
    void ensureProxyReady(const QString &reason, std::function<void(bool)> callback);
    void checkProxyPort(const QString &host, quint16 port,
                        std::function<void(bool)> callback);
    void checkProxyTunnel(std::function<void(bool)> callback);
    bool launchIKuuu();
    void closeAutoLaunchedIKuuu();
    void scheduleAutoLaunchedIKuuuClose();
    void cancelIKuuuWarmClose();
    static bool isIKuuuProcessRunning();
    bool isIKuuuEndpoint() const;
    static bool isLocalProxyHost(const QString &host);
    QString chooseTargetPath(const QString &fileName) const;
    static QString safeFileName(const QString &name);
    static QString fileNameFromUrl(const QUrl &url);

    QNetworkAccessManager m_network;
    EmbeddedProxy m_embeddedProxy;
    std::unique_ptr<TaskStore> m_taskStore;
    QPointer<QNetworkReply> m_probeReply;
    CurlMultiWorker *m_worker = nullptr;
    CurlFtpWorker *m_ftpWorker = nullptr;
    HlsDownloader *m_hlsDownloader = nullptr;
    FileHashWorker *m_hashWorker = nullptr;
    QTimer *m_ikuuuWarmTimer = nullptr;

    QUrl m_url;
    QUrl m_effectiveUrl;
    QString m_fileName;
    QString m_downloadDirectory;
    QString m_tempPath;
    QString m_finalPath;
    QString m_state = QStringLiteral("Idle");
    QString m_errorString;
    QString m_sha256;
    QString m_taskId;
    QString m_etag;
    QString m_lastModified;
    QVector<PlannedSegment> m_plan;
    qint64 m_downloadedBytes = 0;
    qint64 m_totalBytes = 0;
    double m_speedMBps = 0.0;
    int m_etaSeconds = 0;
    int m_activeLanes = 0;
    bool m_hasTask = false;
    bool m_userPaused = false;
    bool m_resumePending = false;
    bool m_usingRanges = false;
    QString m_proxyMode = QStringLiteral("auto");
    QString m_proxyEndpoint;
    QString m_routeNote = QStringLiteral("等待资源探测");
    int m_taskGeneration = 0;
    bool m_routeViaProxy = false;
    bool m_useEmbeddedProxy = false;
    bool m_ikuuuLaunchAttempted = false;
    bool m_ikuuuConnectRequested = false;
    bool m_ikuuuLaunchedByApp = false;
    bool m_proxyRecentlyVerified = false;
    QElapsedTimer m_proxyHealthClock;
    bool m_directProbeAttempted = false;
    bool m_proxyProbeAttempted = false;
    int m_probeRetries = 0;
    bool m_proxyTransferFallbackAttempted = false;
    bool m_directTransferFallbackAttempted = false;
    bool m_segmentedRetryAttempted = false;
    bool m_reusePlanOnNextProbe = false;
    bool m_validateResumeOnProbe = false;
    bool m_sourceChanged = false;
    bool m_restoredTask = false;
    bool m_ftpResumePending = false;
    bool m_acceptsRanges = false;
    int m_connectionLimit = 16;
    ProtocolRouter::Protocol m_protocol = ProtocolRouter::Protocol::Http;
    QStringList m_hlsVariants;
    QString m_protocolMetadata;
    QString m_hlsWorkDirectory;
    int m_completedSegments = 0;
    int m_totalSegments = 0;
    QString m_ftpTransferUrl;
    QElapsedTimer m_persistClock;
    qint64 m_lastPersistedBytes = 0;
    QElapsedTimer m_sampleClock;
    qint64 m_lastSampleBytes = 0;
};
