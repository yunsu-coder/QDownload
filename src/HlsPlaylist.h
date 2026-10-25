#pragma once

#include <QByteArray>
#include <QString>
#include <QUrl>
#include <QVector>

class HlsPlaylist final
{
public:
    struct Key {
        bool encrypted = false;
        QString method;
        QUrl uri;
        QByteArray iv;
    };

    struct Variant {
        QUrl url;
        qint64 bandwidth = 0;
        QString resolution;
        QString codecs;
        QString label;
    };

    struct Segment {
        QUrl url;
        qint64 sequence = 0;
        double duration = 0.0;
        Key key;
    };

    struct Media {
        QVector<Segment> segments;
        QUrl initUrl;
        bool endList = false;
        double totalDuration = 0.0;
        QString extension;
    };

    static bool isMaster(const QByteArray &content);
    static bool parseMaster(const QByteArray &content,
                            const QUrl &playlistUrl,
                            QVector<Variant> *variants,
                            QString *error);
    static bool parseMedia(const QByteArray &content,
                           const QUrl &playlistUrl,
                           Media *media,
                           QString *error);

private:
    static QVector<QPair<QString, QString>> parseAttributes(const QString &line);
};
