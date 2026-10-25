#pragma once

#include <QString>
#include <QUrl>

class ProtocolRouter final
{
public:
    enum class Protocol {
        Http,
        Https,
        Hls,
        Ftp,
        Ftps,
        Unsupported
    };

    struct Route {
        Protocol protocol = Protocol::Unsupported;
        QUrl url;
        QString error;
    };

    static Route route(const QString &input);
    static QString displayName(Protocol protocol);
    static bool isSupported(Protocol protocol);
    static bool isHttpFamily(Protocol protocol);
    static QString redactedUrl(const QUrl &url);
    static QString redactedUrl(const QString &input);

private:
    static bool looksLikeHls(const QUrl &url);
};
