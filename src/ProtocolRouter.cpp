#include "ProtocolRouter.h"

#include <QFileInfo>

ProtocolRouter::Route ProtocolRouter::route(const QString &input)
{
    Route result;
    const QString trimmed = input.trimmed();
    if (trimmed.isEmpty()) {
        result.error = QStringLiteral("下载链接为空。");
        return result;
    }

    result.url = QUrl::fromUserInput(trimmed);
    if (!result.url.isValid() || result.url.host().isEmpty()) {
        result.error = QStringLiteral("下载链接格式无效。");
        return result;
    }

    const QString scheme = result.url.scheme().toLower();
    if (scheme == QStringLiteral("http")) {
        result.protocol =
            looksLikeHls(result.url) ? Protocol::Hls : Protocol::Http;
    } else if (scheme == QStringLiteral("https")) {
        result.protocol =
            looksLikeHls(result.url) ? Protocol::Hls : Protocol::Https;
    } else if (scheme == QStringLiteral("ftp")) {
        result.protocol = Protocol::Ftp;
    } else if (scheme == QStringLiteral("ftps")) {
        result.protocol = Protocol::Ftps;
    } else {
        result.error = QStringLiteral("目前支持 HTTP、HTTPS、HLS、FTP 和 FTPS。");
        return result;
    }
    return result;
}

QString ProtocolRouter::displayName(Protocol protocol)
{
    switch (protocol) {
    case Protocol::Http: return QStringLiteral("HTTP");
    case Protocol::Https: return QStringLiteral("HTTPS");
    case Protocol::Hls: return QStringLiteral("HLS");
    case Protocol::Ftp: return QStringLiteral("FTP");
    case Protocol::Ftps: return QStringLiteral("FTPS");
    default: return QStringLiteral("未知协议");
    }
}

bool ProtocolRouter::isSupported(Protocol protocol)
{
    return protocol != Protocol::Unsupported;
}

bool ProtocolRouter::isHttpFamily(Protocol protocol)
{
    return protocol == Protocol::Http
        || protocol == Protocol::Https
        || protocol == Protocol::Hls;
}

QString ProtocolRouter::redactedUrl(const QUrl &url)
{
    QUrl safe = url;
    safe.setPassword({});
    safe.setQuery(QString());
    safe.setFragment(QString());
    return safe.toString(QUrl::RemoveUserInfo);
}

QString ProtocolRouter::redactedUrl(const QString &input)
{
    return redactedUrl(QUrl::fromUserInput(input.trimmed()));
}

bool ProtocolRouter::looksLikeHls(const QUrl &url)
{
    const QString path = url.path().toLower();
    return QFileInfo(path).suffix() == QStringLiteral("m3u8");
}
