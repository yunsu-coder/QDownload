#pragma once

#include "HlsPlaylist.h"

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QUrl>
#include <QVector>

class QFile;
class QNetworkAccessManager;
class QNetworkReply;

class HlsDownloader final : public QObject
{
    Q_OBJECT

public:
    explicit HlsDownloader(QNetworkAccessManager *network,
                           QObject *parent = nullptr);
    ~HlsDownloader() override;

    void prepare(const QUrl &playlistUrl,
                 const QString &workDirectory,
                 const QString &outputPath);
    void selectVariant(int index);
    void restoreMedia(const QUrl &mediaUrl,
                      const QString &workDirectory,
                      const QString &outputPath,
                      int expectedSegments);
    void pause();
    void resume();
    void cancel();

    QStringList variantLabels() const;
    QString metadataJson() const;
    int completedSegments() const;
    int totalSegments() const;

signals:
    void variantsReady(const QStringList &labels);
    void mediaReady(const QString &metadataJson);
    void progress(qint64 bytesWritten,
                  int activeRequests,
                  int completedSegments,
                  int totalSegments);
    void hashRequired(qint64 totalBytes);
    void failed(const QString &error);
    void paused();

private:
    void loadPlaylist(const QUrl &url, bool selectingVariant);
    void handlePlaylistReply(QNetworkReply *reply, bool selectingVariant);
    void beginMedia(const QUrl &mediaUrl, const HlsPlaylist::Media &media);
    void requestKeys();
    void downloadInitialization();
    void startSegments();
    void scheduleSegments();
    void requestSegment(int index);
    void handleSegmentFinished(QNetworkReply *reply);
    void retrySegment(int index, int retry, int delayMs, int generation);
    void concatenateSegments();
    void concatenateNextFile();
    void concatenateChunk();
    void reportProgress();
    void fail(const QString &error);
    void stopNetwork();
    void removePartialFiles();
    QString segmentPath(int index) const;
    QString partialPath(int index) const;
    QString initPath() const;
    QString metadataJsonFor(const QUrl &mediaUrl) const;
    bool isRetryableStatus(int status) const;

    QNetworkAccessManager *m_network = nullptr;
    QPointer<QNetworkReply> m_playlistReply;
    QPointer<QNetworkReply> m_initReply;
    QVector<QPointer<QNetworkReply>> m_keyReplies;
    QHash<QNetworkReply *, int> m_activeSegments;
    QHash<int, QFile *> m_activeFiles;
    QFile *m_concatOutput = nullptr;
    QFile *m_concatInput = nullptr;
    QStringList m_concatFiles;
    int m_concatIndex = 0;

    QUrl m_inputUrl;
    QUrl m_mediaUrl;
    QString m_workDirectory;
    QString m_outputPath;
    QString m_extension;
    QVector<HlsPlaylist::Variant> m_variants;
    HlsPlaylist::Media m_media;
    QHash<QUrl, QByteArray> m_keys;
    QVector<int> m_retries;
    QVector<int> m_queue;
    qint64 m_totalDuration = 0.0;
    int m_mediaSequence = 0;
    int m_expectedSegments = 0;
    int m_pendingKeys = 0;
    int m_generation = 0;
    bool m_selectingVariant = false;
    bool m_paused = false;
    bool m_failed = false;
    bool m_finishing = false;
};
