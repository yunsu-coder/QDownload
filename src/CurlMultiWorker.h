#pragma once

#include <QByteArray>
#include <QPair>
#include <QString>
#include <QThread>
#include <QVector>

#include <curl/curl.h>

#include <atomic>
#include <deque>

class QFile;

class CurlMultiWorker final : public QThread
{
    Q_OBJECT

public:
    struct TransferRange {
        qint64 start = 0;
        qint64 current = 0;
        qint64 end = -1;
    };

    enum class Result {
        None,
        Completed,
        Paused,
        Failed,
        RangeFallback
    };

    CurlMultiWorker(QString url,
                    QString tempPath,
                    qint64 totalBytes,
                    QVector<TransferRange> ranges,
                    bool useRanges,
                    bool resume,
                    QString proxyUrl,
                    int maxConnections,
                    QObject *parent = nullptr);

    void requestPause();
    QVector<qint64> resumeOffsets() const;
    Result result() const;
    QString errorString() const;

signals:
    void progress(qint64 bytesWritten,
                  int activeLanes,
                  const QVector<qint64> &offsets);
    void planChanged(const QVector<qint64> &starts,
                     const QVector<qint64> &currents,
                     const QVector<qint64> &ends);

protected:
    void run() override;

private:
    struct Segment {
        int index = -1;
        qint64 start = 0;
        qint64 current = 0;
        qint64 end = -1;
        QByteArray rangeHeader;
        CURL *easy = nullptr;
        QFile *file = nullptr;
        CurlMultiWorker *owner = nullptr;
        char errorBuffer[CURL_ERROR_SIZE] = {};
        bool done = false;
        bool started = false;
        bool hadRange = false;
        int retries = 0;
        int splits = 0;
        qint64 lastObservedAt = 0;
        qint64 lastObservedCurrent = 0;
    };

    static size_t writeCallback(char *data, size_t size, size_t count, void *userData);
    void setFailure(const QString &message);
    void captureOffsets();
    QVector<qint64> currentOffsets() const;
    QVector<qint64> currentStarts() const;
    QVector<qint64> currentEnds() const;

    QString m_url;
    QString m_tempPath;
    qint64 m_totalBytes = 0;
    QVector<TransferRange> m_initialRanges;
    bool m_useRanges = false;
    bool m_resume = false;
    QString m_proxyUrl;
    int m_maxConnections = 16;

    std::atomic_bool m_pauseRequested{false};
    std::atomic<qint64> m_bytesWritten{0};
    std::atomic<CURLM *> m_multi{nullptr};

    std::deque<Segment> m_segments;
    QVector<qint64> m_resumeOffsets;
    Result m_result = Result::None;
    QString m_errorString;
};

Q_DECLARE_METATYPE(CurlMultiWorker::Result)
