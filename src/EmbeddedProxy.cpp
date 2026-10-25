#include "EmbeddedProxy.h"

#include "SecureStore.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>

#include <memory>

namespace {
constexpr quint16 kHttpPort = 18890;
constexpr quint16 kSocksPort = 18891;
constexpr quint16 kControllerPort = 19090;
constexpr qint64 kRefreshIntervalMs = 6LL * 60 * 60 * 1000;

QString defaultStatus()
{
    return QStringLiteral("内置代理尚未配置");
}

bool portIsOpen(quint16 port)
{
    QTcpSocket socket;
    socket.connectToHost(QHostAddress::LocalHost, port);
    const bool open = socket.waitForConnected(120);
    if (open) {
        socket.disconnectFromHost();
    } else {
        socket.abort();
    }
    return open;
}
}

EmbeddedProxy::EmbeddedProxy(QObject *parent)
    : QObject(parent)
    , m_network(new QNetworkAccessManager(this))
    , m_status(defaultStatus())
{
    QSettings settings;
    m_subscriptionAvailable =
        settings.value(QStringLiteral("embeddedProxySubscription"), false).toBool();
    if (hasCachedConfig()) {
        m_status = QStringLiteral("内置代理已配置");
    }
}

EmbeddedProxy::~EmbeddedProxy()
{
    if (m_reply) {
        m_reply->abort();
    }
    stop();
}

bool EmbeddedProxy::configured() const
{
    return hasCachedConfig();
}

bool EmbeddedProxy::running() const
{
    return m_process
        && m_process->state() == QProcess::Running
        && portIsOpen(kHttpPort);
}

QString EmbeddedProxy::status() const
{
    return m_status;
}

QString EmbeddedProxy::endpoint() const
{
    return QStringLiteral("http://127.0.0.1:%1").arg(kHttpPort);
}

void EmbeddedProxy::importSubscription(const QString &subscriptionUrl)
{
    const QUrl url = QUrl::fromUserInput(subscriptionUrl.trimmed());
    if (!url.isValid()
        || (url.scheme() != QStringLiteral("http")
            && url.scheme() != QStringLiteral("https"))
        || url.host().isEmpty()) {
        setStatus(QStringLiteral("订阅链接无效"));
        emit importFinished(false, m_status);
        return;
    }

    m_subscriptionUrl = url.toString();
    if (!SecureStore::saveClashSubscription(m_subscriptionUrl)) {
        setStatus(QStringLiteral("无法写入系统钥匙串"));
        emit importFinished(false, m_status);
        return;
    }
    m_subscriptionAvailable = true;
    QSettings settings;
    settings.setValue(QStringLiteral("embeddedProxySubscription"), true);

    setStatus(QStringLiteral("正在下载并验证 Clash 订阅"));
    if (m_process && m_process->state() != QProcess::NotRunning) {
        stop();
    }
    fetchSubscription(true, [this](bool success) {
        if (success) {
            setStatus(QStringLiteral("内置代理订阅已导入"));
        }
        emit importFinished(success, m_status);
    });
}

void EmbeddedProxy::clearSubscription()
{
    stop();
    m_subscriptionUrl.clear();
    SecureStore::clearClashSubscription();
    m_subscriptionAvailable = false;
    QSettings settings;
    settings.remove(QStringLiteral("embeddedProxySubscription"));
    QFile::remove(configPath());
    setStatus(QStringLiteral("内置代理配置已清除"));
}

void EmbeddedProxy::ensureRunning(std::function<void(bool)> callback)
{
    if (running()) {
        QTimer::singleShot(
            0,
            this,
            [callback = std::move(callback)]() { callback(true); });
        return;
    }
    if (m_starting) {
        QTimer::singleShot(
            100,
            this,
            [this, callback = std::move(callback)]() mutable {
                ensureRunning(std::move(callback));
            });
        return;
    }

    if (!hasCachedConfig()) {
        if (!m_subscriptionAvailable) {
            setStatus(QStringLiteral("请先导入 Clash 订阅"));
            QTimer::singleShot(
                0,
                this,
                [callback = std::move(callback)]() { callback(false); });
            return;
        }
        fetchSubscription(false, [this, callback = std::move(callback)](bool success) mutable {
            if (!success) {
                callback(false);
                return;
            }
            startEngine(std::move(callback));
        });
        return;
    }

    startEngine(callback);
    if (!m_subscriptionUrl.isEmpty() && needsRefresh()) {
        fetchSubscription(false, {});
    }
}

void EmbeddedProxy::stop()
{
    m_starting = false;
    if (!m_process) {
        return;
    }
    if (m_process->state() != QProcess::NotRunning) {
        m_process->terminate();
        if (!m_process->waitForFinished(1500)) {
            m_process->kill();
            m_process->waitForFinished(500);
        }
    }
    m_process->deleteLater();
    m_process = nullptr;
    setStatus(QStringLiteral("内置代理已停止"));
}

void EmbeddedProxy::fetchSubscription(
    bool force,
    std::function<void(bool)> callback)
{
    if (m_reply) {
        if (callback) {
            QTimer::singleShot(
                0,
                this,
                [callback = std::move(callback)]() { callback(false); });
        }
        return;
    }
    if (!force && hasCachedConfig() && !needsRefresh()) {
        if (callback) {
            QTimer::singleShot(
                0,
                this,
                [callback = std::move(callback)]() { callback(true); });
        }
        return;
    }

    if (m_subscriptionUrl.isEmpty()) {
        m_subscriptionUrl = SecureStore::loadClashSubscription();
    }
    const QUrl url(m_subscriptionUrl);
    if (!url.isValid()) {
        if (callback) {
            callback(false);
        }
        return;
    }

    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                         QNetworkRequest::AlwaysNetwork);
    request.setTransferTimeout(30000);
    request.setRawHeader("User-Agent", "clash-verge/v2.0");
    request.setRawHeader("Accept", "application/yaml,text/yaml,*/*");

    setStatus(QStringLiteral("正在更新内置代理订阅"));
    m_reply = m_network->get(request);
    connect(m_reply, &QNetworkReply::finished, this,
            [this, force, callback = std::move(callback)]() mutable {
                QPointer<QNetworkReply> reply = m_reply;
                m_reply.clear();
                if (!reply) {
                    if (callback) {
                        callback(false);
                    }
                    return;
                }

                const int status =
                    reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                const QByteArray payload = reply->readAll();
                const bool networkOk = reply->error() == QNetworkReply::NoError
                    && status >= 200 && status < 400;
                reply->deleteLater();

                bool success = false;
                if (networkOk) {
                    success = writeSanitizedConfig(payload);
                }
                if (success) {
                    setStatus(QStringLiteral("内置代理配置已更新"));
                } else if (!force && hasCachedConfig()) {
                    setStatus(QStringLiteral("订阅更新失败，继续使用本地缓存"));
                    success = true;
                } else {
                    setStatus(QStringLiteral("订阅导入失败，请检查链接或网络"));
                }

                if (callback) {
                    callback(success);
                }
            });
}

void EmbeddedProxy::startEngine(std::function<void(bool)> callback)
{
    if (running()) {
        callback(true);
        return;
    }
    m_starting = true;

    const QString binary = binaryPath();
    if (!QFileInfo::exists(binary) || !hasCachedConfig()) {
        m_starting = false;
        setStatus(QStringLiteral("内置代理文件不完整"));
        callback(false);
        return;
    }

    QDir().mkpath(dataDirectory());
    QFile::setPermissions(
        dataDirectory(),
        QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);

    m_process = new QProcess(this);
    m_process->setProgram(QStringLiteral("/bin/sh"));
    m_process->setArguments({
        QStringLiteral("-c"),
        QStringLiteral(
            "parent=$1; shift; "
            "\"$@\" & child=$!; "
            "trap 'kill \"$child\" 2>/dev/null; wait \"$child\" 2>/dev/null; exit 0' TERM INT; "
            "while kill -0 \"$parent\" 2>/dev/null; do sleep 1; done; "
            "kill \"$child\" 2>/dev/null; wait \"$child\" 2>/dev/null"),
        QStringLiteral("quickdl-mihomo"),
        QString::number(QCoreApplication::applicationPid()),
        binary,
        QStringLiteral("-f"),
        configPath(),
        QStringLiteral("-d"),
        dataDirectory()
    });
    m_process->setProcessChannelMode(QProcess::MergedChannels);
    connect(m_process, &QProcess::readyRead, m_process, [this]() {
        if (m_process) {
            m_process->readAll();
        }
    });
    connect(m_process, &QProcess::errorOccurred, this,
            [this](QProcess::ProcessError) {
                if (!m_starting) {
                    setStatus(QStringLiteral("内置代理进程异常退出"));
                }
            });
    connect(m_process,
            qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            this,
            [this](int, QProcess::ExitStatus) {
                m_starting = false;
                if (m_process) {
                    setStatus(QStringLiteral("内置代理进程已停止"));
                }
            });

    setStatus(QStringLiteral("正在启动内置代理"));
    m_process->start();

    auto attempts = std::make_shared<int>(0);
    auto poll = std::make_shared<std::function<void()>>();
    auto completed = std::make_shared<bool>(false);
    auto finish = [this, callback = std::move(callback), completed](bool success) mutable {
        if (*completed) {
            return;
        }
        *completed = true;
        m_starting = false;
        if (success) {
            setStatus(QStringLiteral("内置代理已就绪"));
        } else if (m_status != QStringLiteral("内置代理进程已停止")) {
            setStatus(QStringLiteral("内置代理启动失败"));
        }
        callback(success);
    };

    *poll = [this, attempts, poll, finish]() mutable {
        if (portIsOpen(kHttpPort)) {
            finish(true);
            return;
        }
        ++(*attempts);
        if (*attempts >= 30
            || !m_process
            || m_process->state() == QProcess::NotRunning) {
            finish(false);
            return;
        }
        QTimer::singleShot(100, this, [poll]() { (*poll)(); });
    };
    QTimer::singleShot(50, this, [poll]() { (*poll)(); });
}

bool EmbeddedProxy::writeSanitizedConfig(const QByteArray &source)
{
    QString yaml = QString::fromUtf8(source);
    if (yaml.size() < 100
        || !yaml.contains(QStringLiteral("proxies:"))
        || !yaml.contains(QStringLiteral("proxy-groups:"))) {
        return false;
    }

    const auto replaceLine = [&yaml](const QString &pattern, const QString &replacement) {
        yaml.replace(
            QRegularExpression(pattern, QRegularExpression::MultilineOption),
            replacement);
    };
    replaceLine(QStringLiteral(R"(^port:\s*[^\r\n]*$)"),
                QStringLiteral("port: %1").arg(kHttpPort));
    replaceLine(QStringLiteral(R"(^socks-port:\s*[^\r\n]*$)"),
                QStringLiteral("socks-port: %1").arg(kSocksPort));
    replaceLine(QStringLiteral(R"(^allow-lan:\s*[^\r\n]*$)"),
                QStringLiteral("allow-lan: false"));
    replaceLine(QStringLiteral(R"(^mode:\s*[^\r\n]*$)"),
                QStringLiteral("mode: global"));
    replaceLine(QStringLiteral(R"(^log-level:\s*[^\r\n]*$)"),
                QStringLiteral("log-level: warning"));
    replaceLine(QStringLiteral(R"(^external-controller:\s*[^\r\n]*$)"),
                QStringLiteral("external-controller: 127.0.0.1:%1").arg(kControllerPort));

    const QRegularExpression rulesPattern(
        QStringLiteral(R"(^rules:\s*$)"),
        QRegularExpression::MultilineOption);
    const QRegularExpressionMatch rulesMatch = rulesPattern.match(yaml);
    if (rulesMatch.hasMatch()) {
        yaml.truncate(rulesMatch.capturedStart());
    }
    yaml += QStringLiteral("\nrules: []\n");

    QDir().mkpath(QFileInfo(configPath()).absolutePath());
    QSaveFile file(configPath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return false;
    }
    file.setPermissions(
        QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    if (file.write(yaml.toUtf8()) < 0 || !file.commit()) {
        return false;
    }
    QFile::setPermissions(
        configPath(),
        QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    return true;
}

bool EmbeddedProxy::hasCachedConfig() const
{
    const QFileInfo info(configPath());
    return info.exists() && info.size() > 1024;
}

bool EmbeddedProxy::needsRefresh() const
{
    const QFileInfo info(configPath());
    return !info.exists()
        || info.lastModified().msecsTo(QDateTime::currentDateTime()) > kRefreshIntervalMs;
}

QString EmbeddedProxy::binaryPath() const
{
    const QDir appDir(QCoreApplication::applicationDirPath());
#ifdef Q_OS_MACOS
    const QString bundled =
        appDir.filePath(QStringLiteral("../Resources/bin/mihomo"));
#else
    const QString bundled = appDir.filePath(QStringLiteral("bin/mihomo"));
#endif
    if (QFileInfo::exists(bundled)) {
        return QFileInfo(bundled).canonicalFilePath();
    }
    return {};
}

QString EmbeddedProxy::configPath() const
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
        .filePath(QStringLiteral("mihomo/config.yaml"));
}

QString EmbeddedProxy::dataDirectory() const
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
        .filePath(QStringLiteral("mihomo/data"));
}

void EmbeddedProxy::setStatus(const QString &status)
{
    if (m_status == status) {
        return;
    }
    m_status = status;
    emit changed();
}
