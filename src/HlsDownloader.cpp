#include "HlsDownloader.h"

#include "Aes128.h"
#include "ProtocolRouter.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSaveFile>
#include <QSet>
#include <QTimer>

#include <algorithm>
#include <utility>

namespace {
constexpr int kMaximumConcurrentSegments = 8;
constexpr int kMaximumSegmentRetries = 3;

QNetworkRequest makeRequest(const QUrl &url)
{
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                         QNetworkRequest::AlwaysNetwork);
    request.setTransferTimeout(30000);
    request.setRawHeader("Accept-Encoding", "identity");
    request.setRawHeader("User-Agent", "QuickDL/0.3 (HLS VOD)");
    return request;
}

bool copyFile(QFile *source, QFile *destination)
{
    if (!source || !destination) {
        return false;
    }
    source->seek(0);
    while (!source->atEnd()) {
        const QByteArray chunk = source->read(1024 * 1024);
        if (chunk.isEmpty()) {
            return source->error() == QFileDevice::NoError;
        }
        if (destination->write(chunk) != chunk.size()) {
            return false;
        }
    }
    return destination->flush();
}
}

HlsDownloader::HlsDownloader(
    QNetworkAccessManager *network,
    QObject *parent)
    : QObject(parent)
    , m_network(network)
{
}

HlsDownloader::~HlsDownloader()
{
    cancel();
}

void HlsDownloader::prepare(
    const QUrl &playlistUrl,
    const QString &workDirectory,
    const QString &outputPath)
{
    m_inputUrl = playlistUrl;
    m_mediaUrl.clear();
    m_workDirectory = workDirectory;
    m_outputPath = outputPath;
    m_expectedSegments = 0;
    m_selectingVariant = false;
    m_paused = false;
    m_failed = false;
    m_finishing = false;
    if (!QDir().mkpath(m_workDirectory)) {
        fail(QStringLiteral("无法创建 HLS 工作目录。"));
        return;
    }
    loadPlaylist(playlistUrl, true);
}

void HlsDownloader::restoreMedia(
    const QUrl &mediaUrl,
    const QString &workDirectory,
    const QString &outputPath,
    int expectedSegments)
{
    m_inputUrl = mediaUrl;
    m_mediaUrl = mediaUrl;
    m_workDirectory = workDirectory;
    m_outputPath = outputPath;
    m_expectedSegments = expectedSegments;
    m_selectingVariant = false;
    m_paused = false;
    m_failed = false;
    m_finishing = false;
    if (!QDir().mkpath(m_workDirectory)) {
        fail(QStringLiteral("无法恢复 HLS 工作目录。"));
        return;
    }
    loadPlaylist(mediaUrl, false);
}

void HlsDownloader::selectVariant(int index)
{
    if (index < 0 || index >= m_variants.size() || m_paused || m_failed) {
        return;
    }
    m_selectingVariant = false;
    m_mediaUrl = m_variants[index].url;
    loadPlaylist(m_mediaUrl, false);
}

void HlsDownloader::loadPlaylist(const QUrl &url, bool selectingVariant)
{
    if (!m_network || !url.isValid()) {
        fail(QStringLiteral("HLS playlist 地址无效。"));
        return;
    }
    m_selectingVariant = selectingVariant;
    m_playlistReply = m_network->get(makeRequest(url));
    QNetworkReply *reply = m_playlistReply;
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, selectingVariant]() {
                handlePlaylistReply(reply, selectingVariant);
            });
}

void HlsDownloader::handlePlaylistReply(
    QNetworkReply *reply,
    bool selectingVariant)
{
    if (!reply || reply != m_playlistReply) {
        return;
    }
    m_playlistReply.clear();
    reply->deleteLater();
    const int status =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (reply->error() != QNetworkReply::NoError || status >= 400) {
        fail(QStringLiteral("无法加载 HLS playlist：HTTP %1 · %2")
                 .arg(status)
                 .arg(reply->errorString()));
        return;
    }

    const QByteArray content = reply->readAll();
    if (HlsPlaylist::isMaster(content)) {
        QString error;
        if (!HlsPlaylist::parseMaster(
                content, reply->url(), &m_variants, &error)) {
            fail(error);
            return;
        }
        m_selectingVariant = true;
        emit variantsReady(variantLabels());
        return;
    }

    HlsPlaylist::Media media;
    QString error;
    if (!HlsPlaylist::parseMedia(content, reply->url(), &media, &error)) {
        fail(error);
        return;
    }
    if (!selectingVariant && m_expectedSegments > 0
        && media.segments.size() != m_expectedSegments) {
        fail(QStringLiteral("HLS 分片数量已变化，无法安全恢复。"));
        return;
    }
    beginMedia(reply->url(), media);
}

void HlsDownloader::beginMedia(
    const QUrl &mediaUrl,
    const HlsPlaylist::Media &media)
{
    m_media = media;
    m_mediaUrl = mediaUrl;
    m_extension = media.extension;
    if (m_extension == QStringLiteral("m4s")
        && media.initUrl.isValid()) {
        m_extension = QStringLiteral("mp4");
    }
    m_totalDuration = media.totalDuration;
    m_mediaSequence = 0;
    m_retries.fill(0, m_media.segments.size());
    emit mediaReady(metadataJsonFor(mediaUrl));
    requestKeys();
}

void HlsDownloader::requestKeys()
{
    m_pendingKeys = 0;
    QSet<QUrl> requested;
    for (const HlsPlaylist::Segment &segment : m_media.segments) {
        if (!segment.key.encrypted || !segment.key.uri.isValid()
            || m_keys.contains(segment.key.uri)
            || requested.contains(segment.key.uri)) {
            continue;
        }
        requested.insert(segment.key.uri);
        ++m_pendingKeys;
        QNetworkReply *reply =
            m_network->get(makeRequest(segment.key.uri));
        m_keyReplies.append(reply);
        const QUrl keyUrl = segment.key.uri;
        connect(reply, &QNetworkReply::finished, this,
                [this, reply, keyUrl]() {
                    m_keyReplies.removeAll(reply);
                    reply->deleteLater();
                    const int status = reply
                        ->attribute(QNetworkRequest::HttpStatusCodeAttribute)
                        .toInt();
                    const QByteArray key = reply->readAll();
                    if (m_failed) {
                        return;
                    }
                    if (reply->error() != QNetworkReply::NoError
                        || status >= 400 || key.size() != 16) {
                        fail(QStringLiteral("HLS AES-128 密钥加载失败。"));
                        return;
                    }
                    m_keys.insert(keyUrl, key);
                    --m_pendingKeys;
                    if (m_pendingKeys == 0) {
                        downloadInitialization();
                    }
                });
    }
    if (m_pendingKeys == 0) {
        downloadInitialization();
    }
}

void HlsDownloader::downloadInitialization()
{
    if (!m_media.initUrl.isValid()) {
        startSegments();
        return;
    }
    const QString path = initPath();
    if (QFileInfo(path).size() > 0) {
        startSegments();
        return;
    }
    m_initReply = m_network->get(makeRequest(m_media.initUrl));
    QNetworkReply *reply = m_initReply;
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        if (!reply || reply != m_initReply) {
            return;
        }
        m_initReply.clear();
        reply->deleteLater();
        const int status =
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray content = reply->readAll();
        if (reply->error() != QNetworkReply::NoError || status >= 400
            || content.isEmpty()) {
            fail(QStringLiteral("HLS 初始化分片下载失败。"));
            return;
        }
        QSaveFile file(initPath());
        if (!file.open(QIODevice::WriteOnly)
            || file.write(content) != content.size()
            || !file.commit()) {
            fail(QStringLiteral("无法写入 HLS 初始化分片。"));
            return;
        }
        startSegments();
    });
}

void HlsDownloader::startSegments()
{
    if (m_paused || m_failed) {
        return;
    }
    m_queue.clear();
    for (int index = 0; index < m_media.segments.size(); ++index) {
        if (QFileInfo(segmentPath(index)).size() <= 0) {
            m_queue.append(index);
        }
    }
    m_finishing = false;
    reportProgress();
    scheduleSegments();
}

void HlsDownloader::scheduleSegments()
{
    if (m_paused || m_failed || m_finishing) {
        return;
    }
    while (m_activeSegments.size() < kMaximumConcurrentSegments
           && !m_queue.isEmpty()) {
        const int index = m_queue.takeFirst();
        requestSegment(index);
    }
    if (m_activeSegments.isEmpty() && m_queue.isEmpty()) {
        concatenateSegments();
    }
}

void HlsDownloader::requestSegment(int index)
{
    if (index < 0 || index >= m_media.segments.size()
        || m_paused || m_failed) {
        return;
    }
    QFile::remove(partialPath(index));
    auto *file = new QFile(partialPath(index), this);
    if (!file->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        delete file;
        fail(QStringLiteral("无法创建 HLS 分片临时文件。"));
        return;
    }
    QNetworkReply *reply =
        m_network->get(makeRequest(m_media.segments[index].url));
    m_activeSegments.insert(reply, index);
    m_activeFiles.insert(index, file);
    connect(reply, &QNetworkReply::readyRead, this, [reply, index, this]() {
        QFile *activeFile = m_activeFiles.value(index);
        if (!activeFile) {
            return;
        }
        const QByteArray chunk = reply->readAll();
        if (!chunk.isEmpty()
            && activeFile->write(chunk) != chunk.size()) {
            reply->abort();
        }
    });
    connect(reply, &QNetworkReply::finished, this,
            [this, reply]() { handleSegmentFinished(reply); });
    reportProgress();
}

void HlsDownloader::handleSegmentFinished(QNetworkReply *reply)
{
    const int index = m_activeSegments.value(reply, -1);
    if (index < 0) {
        reply->deleteLater();
        return;
    }
    m_activeSegments.remove(reply);
    QFile *file = m_activeFiles.take(index);
    if (file) {
        const QByteArray remaining = reply->readAll();
        if (!remaining.isEmpty()) {
            file->write(remaining);
        }
        file->flush();
        file->close();
        file->deleteLater();
    }
    reply->deleteLater();

    const int status =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (m_paused) {
        reportProgress();
        return;
    }
    if (reply->error() != QNetworkReply::NoError || status >= 400) {
        QFile::remove(partialPath(index));
        const int retry = m_retries.value(index, 0);
        if (retry < kMaximumSegmentRetries
            && (status == 0 || isRetryableStatus(status))) {
            ++m_retries[index];
            int delay = 400 * (1 << retry);
            if (status == 429) {
                const QByteArray retryAfter =
                    reply->rawHeader("Retry-After").trimmed();
                bool ok = false;
                const int seconds = retryAfter.toInt(&ok);
                if (ok && seconds >= 0 && seconds <= 60) {
                    delay = seconds * 1000;
                }
            }
            retrySegment(index, retry + 1, delay, m_generation);
            reportProgress();
            return;
        }
        fail(QStringLiteral("HLS 分片 %1 下载失败：HTTP %2")
                 .arg(index + 1)
                 .arg(status));
        return;
    }

    const HlsPlaylist::Segment &segment = m_media.segments[index];
    bool written = false;
    if (segment.key.encrypted) {
        QFile encrypted(partialPath(index));
        if (!encrypted.open(QIODevice::ReadOnly)) {
            fail(QStringLiteral("无法读取 HLS 加密分片。"));
            return;
        }
        const QByteArray key = m_keys.value(segment.key.uri);
        QString error;
        const QByteArray plaintext = Aes128::decryptCbc(
            encrypted.readAll(), key, segment.key.iv, &error);
        if (!error.isEmpty() || plaintext.isEmpty()) {
            fail(error.isEmpty() ? QStringLiteral("HLS 分片解密失败。") : error);
            return;
        }
        QSaveFile output(segmentPath(index));
        written = output.open(QIODevice::WriteOnly)
            && output.write(plaintext) == plaintext.size()
            && output.commit();
        encrypted.close();
        QFile::remove(partialPath(index));
    } else {
        written = QFile::rename(partialPath(index), segmentPath(index));
    }
    if (!written || QFileInfo(segmentPath(index)).size() <= 0) {
        fail(QStringLiteral("无法保存 HLS 分片 %1。").arg(index + 1));
        return;
    }
    reportProgress();
    scheduleSegments();
}

void HlsDownloader::retrySegment(
    int index,
    int retry,
    int delayMs,
    int generation)
{
    QTimer::singleShot(delayMs, this, [this, index, retry, generation]() {
        Q_UNUSED(retry)
        if (generation == m_generation && !m_paused && !m_failed) {
            requestSegment(index);
            scheduleSegments();
        }
    });
}

void HlsDownloader::concatenateSegments()
{
    if (m_finishing || m_paused || m_failed) {
        return;
    }
    m_finishing = true;
    m_concatFiles.clear();
    if (m_media.initUrl.isValid()) {
        m_concatFiles.append(initPath());
    }
    for (int index = 0; index < m_media.segments.size(); ++index) {
        m_concatFiles.append(segmentPath(index));
    }
    m_concatIndex = 0;
    m_concatOutput = new QFile(m_outputPath, this);
    if (!m_concatOutput->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        fail(QStringLiteral("无法创建 HLS 输出文件。"));
        return;
    }
    concatenateNextFile();
}

void HlsDownloader::concatenateNextFile()
{
    if (!m_finishing || m_paused || m_failed) {
        return;
    }
    if (m_concatInput) {
        m_concatInput->close();
        m_concatInput->deleteLater();
        m_concatInput = nullptr;
    }
    if (m_concatIndex >= m_concatFiles.size()) {
        if (!m_concatOutput || !m_concatOutput->flush()) {
            fail(QStringLiteral("刷新 HLS 输出文件失败。"));
            return;
        }
        const qint64 totalBytes = m_concatOutput->size();
        m_concatOutput->close();
        m_concatOutput->deleteLater();
        m_concatOutput = nullptr;
        if (totalBytes <= 0) {
            fail(QStringLiteral("HLS 输出文件为空。"));
            return;
        }
        emit progress(
            totalBytes, 0, m_media.segments.size(), m_media.segments.size());
        emit hashRequired(totalBytes);
        return;
    }

    m_concatInput = new QFile(m_concatFiles[m_concatIndex++], this);
    if (!m_concatInput->open(QIODevice::ReadOnly)) {
        fail(QStringLiteral("无法读取 HLS 分片文件。"));
        return;
    }
    QTimer::singleShot(0, this, &HlsDownloader::concatenateChunk);
}

void HlsDownloader::concatenateChunk()
{
    if (!m_finishing || m_paused || m_failed) {
        return;
    }
    if (!m_concatInput || !m_concatOutput) {
        return;
    }
    const QByteArray chunk = m_concatInput->read(4 * 1024 * 1024);
    if (chunk.isEmpty()) {
        if (m_concatInput->error() != QFileDevice::NoError) {
            fail(QStringLiteral("读取 HLS 分片文件失败。"));
            return;
        }
        concatenateNextFile();
        return;
    }
    if (m_concatOutput->write(chunk) != chunk.size()) {
        fail(QStringLiteral("写入 HLS 输出文件失败。"));
        return;
    }
    QTimer::singleShot(0, this, &HlsDownloader::concatenateChunk);
}

void HlsDownloader::reportProgress()
{
    qint64 bytes = QFileInfo(initPath()).size();
    int completed = 0;
    for (int index = 0; index < m_media.segments.size(); ++index) {
        const qint64 finalSize = QFileInfo(segmentPath(index)).size();
        if (finalSize > 0) {
            bytes += finalSize;
            ++completed;
        } else {
            const qint64 partial = QFileInfo(partialPath(index)).size();
            if (partial > 0) {
                bytes += partial;
            }
        }
    }
    emit progress(
        bytes,
        m_activeSegments.size(),
        completed,
        m_media.segments.size());
}

void HlsDownloader::pause()
{
    if (m_paused || m_failed) {
        return;
    }
    m_paused = true;
    ++m_generation;
    stopNetwork();
    if (m_concatInput) {
        m_concatInput->close();
        m_concatInput->deleteLater();
        m_concatInput = nullptr;
    }
    if (m_concatOutput) {
        m_concatOutput->close();
        m_concatOutput->deleteLater();
        m_concatOutput = nullptr;
    }
    m_finishing = false;
    emit paused();
}

void HlsDownloader::resume()
{
    if (!m_paused || m_failed) {
        return;
    }
    m_paused = false;
    ++m_generation;
    startSegments();
}

void HlsDownloader::cancel()
{
    m_paused = true;
    ++m_generation;
    stopNetwork();
}

QStringList HlsDownloader::variantLabels() const
{
    QStringList labels;
    labels.reserve(m_variants.size());
    for (const HlsPlaylist::Variant &variant : m_variants) {
        labels.append(variant.label);
    }
    return labels;
}

int HlsDownloader::completedSegments() const
{
    int completed = 0;
    for (int index = 0; index < m_media.segments.size(); ++index) {
        if (QFileInfo(segmentPath(index)).size() > 0) {
            ++completed;
        }
    }
    return completed;
}

int HlsDownloader::totalSegments() const
{
    return m_media.segments.size();
}

QString HlsDownloader::metadataJson() const
{
    return metadataJsonFor(m_mediaUrl);
}

QString HlsDownloader::metadataJsonFor(const QUrl &mediaUrl) const
{
    QJsonObject metadata;
    metadata.insert(QStringLiteral("protocol"), QStringLiteral("hls"));
    metadata.insert(
        QStringLiteral("mediaUrl"), mediaUrl.toString());
    metadata.insert(QStringLiteral("workDirectory"), m_workDirectory);
    metadata.insert(QStringLiteral("outputPath"), m_outputPath);
    metadata.insert(QStringLiteral("extension"), m_extension);
    metadata.insert(
        QStringLiteral("segmentCount"), m_media.segments.size());
    metadata.insert(QStringLiteral("duration"), m_totalDuration);
    metadata.insert(
        QStringLiteral("encrypted"),
        std::any_of(
            m_media.segments.cbegin(),
            m_media.segments.cend(),
            [](const HlsPlaylist::Segment &segment) {
                return segment.key.encrypted;
            }));
    return QString::fromUtf8(
        QJsonDocument(metadata).toJson(QJsonDocument::Compact));
}

void HlsDownloader::fail(const QString &error)
{
    if (m_failed) {
        return;
    }
    m_failed = true;
    ++m_generation;
    stopNetwork();
    emit failed(error);
}

void HlsDownloader::stopNetwork()
{
    if (m_playlistReply) {
        m_playlistReply->abort();
        m_playlistReply.clear();
    }
    if (m_initReply) {
        m_initReply->abort();
        m_initReply.clear();
    }
    for (const QPointer<QNetworkReply> &reply : std::as_const(m_keyReplies)) {
        if (reply) {
            reply->abort();
        }
    }
    m_keyReplies.clear();
    const auto replies = m_activeSegments.keys();
    for (QNetworkReply *reply : replies) {
        reply->abort();
    }
    for (QFile *file : std::as_const(m_activeFiles)) {
        file->flush();
        file->close();
        file->deleteLater();
    }
    m_activeFiles.clear();
}

void HlsDownloader::removePartialFiles()
{
    for (int index = 0; index < m_media.segments.size(); ++index) {
        QFile::remove(partialPath(index));
    }
}

QString HlsDownloader::segmentPath(int index) const
{
    return QDir(m_workDirectory)
        .filePath(QStringLiteral("segment-%1.%2")
                      .arg(index, 8, 10, QLatin1Char('0'))
                      .arg(m_extension));
}

QString HlsDownloader::partialPath(int index) const
{
    return segmentPath(index) + QStringLiteral(".download");
}

QString HlsDownloader::initPath() const
{
    return QDir(m_workDirectory).filePath(
        QStringLiteral("init.%1").arg(m_extension));
}

bool HlsDownloader::isRetryableStatus(int status) const
{
    return status == 408 || status == 429 || status >= 500;
}
