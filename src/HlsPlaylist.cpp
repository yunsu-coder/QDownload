#include "HlsPlaylist.h"

#include <QFileInfo>
#include <QPair>
#include <QRegularExpression>
#include <QStringList>

#include <algorithm>

namespace {
QString tagValue(const QString &line, const QString &tag)
{
    if (!line.startsWith(tag + QLatin1Char(':'))) {
        return {};
    }
    return line.mid(tag.size() + 1).trimmed();
}

QByteArray parseHexBytes(QString value, bool *ok)
{
    *ok = false;
    value = value.trimmed();
    if (value.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive)) {
        value = value.mid(2);
    }
    if (value.isEmpty() || value.size() % 2 != 0) {
        return {};
    }
    const QByteArray bytes = QByteArray::fromHex(value.toLatin1());
    *ok = bytes.size() * 2 == value.size();
    return bytes;
}

QByteArray sequenceIv(qint64 sequence)
{
    QByteArray iv(16, '\0');
    for (int index = 0; index < 8; ++index) {
        iv[15 - index] =
            static_cast<char>((sequence >> (index * 8)) & 0xff);
    }
    return iv;
}
}

bool HlsPlaylist::isMaster(const QByteArray &content)
{
    const QString text = QString::fromUtf8(content);
    return text.contains(QStringLiteral("#EXT-X-STREAM-INF"))
        || text.contains(QStringLiteral("#EXT-X-I-FRAME-STREAM-INF"));
}

QVector<QPair<QString, QString>> HlsPlaylist::parseAttributes(
    const QString &line)
{
    QVector<QPair<QString, QString>> attributes;
    int index = 0;
    while (index < line.size()) {
        while (index < line.size()
               && (line[index].isSpace() || line[index] == QLatin1Char(','))) {
            ++index;
        }
        const int keyStart = index;
        while (index < line.size()
               && line[index] != QLatin1Char('=')
               && line[index] != QLatin1Char(',')) {
            ++index;
        }
        if (index >= line.size() || line[index] != QLatin1Char('=')) {
            break;
        }
        const QString key = line.mid(keyStart, index - keyStart).trimmed();
        ++index;

        QString value;
        if (index < line.size() && line[index] == QLatin1Char('"')) {
            ++index;
            while (index < line.size()) {
                if (line[index] == QLatin1Char('"')) {
                    ++index;
                    break;
                }
                if (line[index] == QLatin1Char('\\')
                    && index + 1 < line.size()) {
                    ++index;
                }
                value.append(line[index++]);
            }
        } else {
            const int valueStart = index;
            while (index < line.size()
                   && line[index] != QLatin1Char(',')) {
                ++index;
            }
            value = line.mid(valueStart, index - valueStart).trimmed();
        }
        attributes.append({key, value});
    }
    return attributes;
}

bool HlsPlaylist::parseMaster(
    const QByteArray &content,
    const QUrl &playlistUrl,
    QVector<Variant> *variants,
    QString *error)
{
    if (!variants) {
        return false;
    }
    variants->clear();
    const QStringList lines =
        QString::fromUtf8(content).split(QRegularExpression(QStringLiteral("[\\r\\n]+")),
                                         Qt::SkipEmptyParts);
    Variant pending;
    bool hasPending = false;
    for (const QString &rawLine : lines) {
        const QString line = rawLine.trimmed();
        if (line.startsWith(QStringLiteral("#EXT-X-STREAM-INF:"))) {
            pending = {};
            hasPending = true;
            const auto attributes = parseAttributes(tagValue(line,
                QStringLiteral("#EXT-X-STREAM-INF")));
            for (const auto &[key, value] : attributes) {
                if (key == QStringLiteral("BANDWIDTH")) {
                    pending.bandwidth = value.toLongLong();
                } else if (key == QStringLiteral("RESOLUTION")) {
                    pending.resolution = value;
                } else if (key == QStringLiteral("CODECS")) {
                    pending.codecs = value;
                }
            }
            continue;
        }
        if (line.startsWith(QLatin1Char('#')) || line.isEmpty()
            || !hasPending) {
            continue;
        }
        pending.url = playlistUrl.resolved(QUrl(line));
        if (pending.resolution.isEmpty()) {
            pending.label = QStringLiteral("%1 Mbps")
                .arg(pending.bandwidth / 1000000.0, 0, 'f', 1);
        } else {
            pending.label = pending.resolution + QStringLiteral(" · %1 Mbps")
                .arg(pending.bandwidth / 1000000.0, 0, 'f', 1);
        }
        variants->append(pending);
        hasPending = false;
    }

    if (variants->isEmpty()) {
        if (error) {
            *error = QStringLiteral("master playlist 中没有可用码率。");
        }
        return false;
    }
    std::sort(variants->begin(), variants->end(),
              [](const Variant &left, const Variant &right) {
                  return left.bandwidth > right.bandwidth;
              });
    return true;
}

bool HlsPlaylist::parseMedia(
    const QByteArray &content,
    const QUrl &playlistUrl,
    Media *media,
    QString *error)
{
    if (!media) {
        return false;
    }
    *media = {};
    const QStringList lines =
        QString::fromUtf8(content).split(QRegularExpression(QStringLiteral("[\\r\\n]+")),
                                         Qt::SkipEmptyParts);
    qint64 mediaSequence = 0;
    double pendingDuration = -1.0;
    Key currentKey;

    for (const QString &rawLine : lines) {
        const QString line = rawLine.trimmed();
        if (line.isEmpty()) {
            continue;
        }
        if (line.startsWith(QStringLiteral("#EXT-X-MEDIA-SEQUENCE:"))) {
            mediaSequence =
                tagValue(line, QStringLiteral("#EXT-X-MEDIA-SEQUENCE")).toLongLong();
            continue;
        }
        if (line.startsWith(QStringLiteral("#EXTINF:"))) {
            const QString value =
                tagValue(line, QStringLiteral("#EXTINF"));
            pendingDuration = value.section(QLatin1Char(','), 0, 0).toDouble();
            continue;
        }
        if (line.startsWith(QStringLiteral("#EXT-X-KEY:"))) {
            const auto attributes = parseAttributes(
                tagValue(line, QStringLiteral("#EXT-X-KEY")));
            currentKey = {};
            for (const auto &[key, value] : attributes) {
                if (key == QStringLiteral("METHOD")) {
                    currentKey.method = value.toUpper();
                } else if (key == QStringLiteral("URI")) {
                    currentKey.uri = playlistUrl.resolved(QUrl(value));
                } else if (key == QStringLiteral("IV")) {
                    bool ok = false;
                    currentKey.iv = parseHexBytes(value, &ok);
                    if (!ok) {
                        if (error) {
                            *error = QStringLiteral("HLS 密钥 IV 无效。");
                        }
                        return false;
                    }
                }
            }
            currentKey.encrypted =
                currentKey.method == QStringLiteral("AES-128");
            if (!currentKey.method.isEmpty()
                && currentKey.method != QStringLiteral("NONE")
                && !currentKey.encrypted) {
                if (error) {
                    *error = QStringLiteral("不支持的 HLS 加密方式：%1")
                        .arg(currentKey.method);
                }
                return false;
            }
            continue;
        }
        if (line.startsWith(QStringLiteral("#EXT-X-MAP:"))) {
            const auto attributes = parseAttributes(
                tagValue(line, QStringLiteral("#EXT-X-MAP")));
            for (const auto &[key, value] : attributes) {
                if (key == QStringLiteral("URI")) {
                    media->initUrl = playlistUrl.resolved(QUrl(value));
                }
            }
            continue;
        }
        if (line == QStringLiteral("#EXT-X-ENDLIST")) {
            media->endList = true;
            continue;
        }
        if (line.startsWith(QLatin1Char('#'))) {
            continue;
        }
        if (pendingDuration < 0.0) {
            continue;
        }

        Segment segment;
        segment.url = playlistUrl.resolved(QUrl(line));
        segment.sequence =
            mediaSequence + static_cast<qint64>(media->segments.size());
        segment.duration = pendingDuration;
        segment.key = currentKey;
        if (segment.key.encrypted && segment.key.iv.size() != 16) {
            segment.key.iv = sequenceIv(segment.sequence);
        }
        media->segments.append(segment);
        media->totalDuration += pendingDuration;
        pendingDuration = -1.0;
    }

    if (!media->endList) {
        if (error) {
            *error = QStringLiteral("检测到直播或缺少 ENDLIST 的 HLS playlist，"
                                    "V2.0 仅支持 VOD。");
        }
        return false;
    }
    if (media->segments.isEmpty()) {
        if (error) {
            *error = QStringLiteral("HLS playlist 中没有媒体分片。");
        }
        return false;
    }
    for (const Segment &segment : media->segments) {
        const QString suffix = QFileInfo(segment.url.path()).suffix().toLower();
        if (suffix == QStringLiteral("m4s")
            || suffix == QStringLiteral("ts")
            || suffix == QStringLiteral("aac")
            || suffix == QStringLiteral("vtt")) {
            media->extension = suffix;
            break;
        }
    }
    if (media->extension.isEmpty()) {
        media->extension =
            media->initUrl.isValid() ? QStringLiteral("mp4")
                                     : QStringLiteral("ts");
    }
    return true;
}
