#pragma once

#include <QObject>
#include <QPointer>
#include <QString>

#include <functional>

class QNetworkAccessManager;
class QNetworkReply;
class QProcess;

class EmbeddedProxy final : public QObject
{
    Q_OBJECT

public:
    explicit EmbeddedProxy(QObject *parent = nullptr);
    ~EmbeddedProxy() override;

    bool configured() const;
    bool running() const;
    QString status() const;
    QString endpoint() const;

    void importSubscription(const QString &subscriptionUrl);
    void clearSubscription();
    void ensureRunning(std::function<void(bool)> callback);
    void stop();

signals:
    void changed();
    void importFinished(bool success, const QString &message);

private:
    void fetchSubscription(bool force, std::function<void(bool)> callback);
    void startEngine(std::function<void(bool)> callback);
    bool writeSanitizedConfig(const QByteArray &source);
    bool hasCachedConfig() const;
    bool needsRefresh() const;
    QString binaryPath() const;
    QString configPath() const;
    QString dataDirectory() const;
    void setStatus(const QString &status);

    QNetworkAccessManager *m_network = nullptr;
    QPointer<QNetworkReply> m_reply;
    QProcess *m_process = nullptr;
    QString m_subscriptionUrl;
    QString m_status;
    bool m_subscriptionAvailable = false;
    bool m_starting = false;
};
