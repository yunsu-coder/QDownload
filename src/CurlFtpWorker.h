#pragma once

#include <QString>
#include <QThread>

#include <curl/curl.h>

#include <atomic>

class QFile;

class CurlFtpWorker final : public QThread
{
    Q_OBJECT

public:
    enum class Result {
        Completed,
        Paused,
        Failed
    };

    CurlFtpWorker(QString url,
                  QString tempPath,
                  QString proxyUrl,
                  QObject *parent = nullptr);

    void requestPause();
    Result result() const;
    QString errorString() const;

signals:
    void metadata(qint64 totalBytes);
    void progress(qint64 bytesWritten, qint64 totalBytes);
    void resumeUnsupported();

protected:
    void run() override;

private:
    struct WriteContext {
        QFile *file = nullptr;
        CurlFtpWorker *owner = nullptr;
        qint64 resumeFrom = 0;
        qint64 initialOffset = 0;
    };

    static size_t writeCallback(char *data,
                                size_t size,
                                size_t count,
                                void *userData);
    static size_t discardCallback(char *data,
                                  size_t size,
                                  size_t count,
                                  void *userData);
    static int progressCallback(void *userData,
                                curl_off_t downloadTotal,
                                curl_off_t downloadNow,
                                curl_off_t uploadTotal,
                                curl_off_t uploadNow);
    void configureCommon(CURL *easy) const;
    QString curlError(CURLcode code, const char *buffer) const;

    QString m_url;
    QString m_tempPath;
    QString m_proxyUrl;
    std::atomic_bool m_pauseRequested{false};
    Result m_result = Result::Failed;
    QString m_errorString;
};
