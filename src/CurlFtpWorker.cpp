#include "CurlFtpWorker.h"

#include <QFile>
#include <QFileInfo>
#include <QtGlobal>
#include <QUrl>

#include <algorithm>

CurlFtpWorker::CurlFtpWorker(
    QString url,
    QString tempPath,
    QString proxyUrl,
    QObject *parent)
    : QThread(parent)
    , m_url(std::move(url))
    , m_tempPath(std::move(tempPath))
    , m_proxyUrl(std::move(proxyUrl))
{
}

void CurlFtpWorker::requestPause()
{
    m_pauseRequested.store(true);
}

CurlFtpWorker::Result CurlFtpWorker::result() const
{
    return m_result;
}

QString CurlFtpWorker::errorString() const
{
    return m_errorString;
}

void CurlFtpWorker::configureCommon(CURL *easy) const
{
    const QUrl url(m_url);
    const bool ftps = url.scheme().compare(
                          QStringLiteral("ftps"), Qt::CaseInsensitive) == 0;
    QUrl transferUrl = url;
    if (ftps) {
        // libcurl treats ftps:// as implicit TLS. QuickDL exposes explicit
        // FTPS, which is ftp:// followed by AUTH TLS.
        transferUrl.setScheme(QStringLiteral("ftp"));
    }
    const QByteArray urlBytes = transferUrl.toString().toUtf8();
    const QByteArray proxyBytes = m_proxyUrl.toUtf8();

    curl_easy_setopt(easy, CURLOPT_URL, urlBytes.constData());
    curl_easy_setopt(easy, CURLOPT_PROTOCOLS_STR, "ftp,ftps");
    curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(easy, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT_MS, 20000L);
    curl_easy_setopt(easy, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(easy, CURLOPT_LOW_SPEED_TIME, 30L);
    curl_easy_setopt(easy, CURLOPT_USERAGENT, "QuickDL/0.3 (FTP)");
    curl_easy_setopt(easy, CURLOPT_FTP_USE_EPSV, 1L);
    curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(easy, CURLOPT_SSL_VERIFYHOST, 2L);
    if (ftps) {
        curl_easy_setopt(easy, CURLOPT_USE_SSL, CURLUSESSL_ALL);
    }
    const QString caBundle =
        qEnvironmentVariable("QUICKDL_CA_BUNDLE").trimmed();
    if (!caBundle.isEmpty() && QFileInfo::exists(caBundle)) {
        curl_easy_setopt(easy, CURLOPT_CAINFO, caBundle.toUtf8().constData());
    }
#ifdef Q_OS_MACOS
    else if (QFileInfo::exists(QStringLiteral("/etc/ssl/cert.pem"))) {
        curl_easy_setopt(easy, CURLOPT_CAINFO, "/etc/ssl/cert.pem");
    }
#endif
    if (!proxyBytes.isEmpty()) {
        curl_easy_setopt(easy, CURLOPT_PROXY, proxyBytes.constData());
    } else {
        curl_easy_setopt(easy, CURLOPT_NOPROXY, "*");
    }
}

QString CurlFtpWorker::curlError(CURLcode code, const char *buffer) const
{
    if (buffer && *buffer) {
        return QString::fromUtf8(buffer);
    }
    return QString::fromLatin1(curl_easy_strerror(code));
}

size_t CurlFtpWorker::writeCallback(
    char *data,
    size_t size,
    size_t count,
    void *userData)
{
    auto *context = static_cast<WriteContext *>(userData);
    if (!context || !context->file || !context->owner) {
        return 0;
    }
    if (context->owner->m_pauseRequested.load()) {
        return 0;
    }
    const qint64 bytes = static_cast<qint64>(size * count);
    const qint64 written = context->file->write(data, bytes);
    if (written != bytes) {
        return 0;
    }
    context->resumeFrom += written;
    return static_cast<size_t>(written);
}

size_t CurlFtpWorker::discardCallback(
    char *,
    size_t size,
    size_t count,
    void *)
{
    return size * count;
}

int CurlFtpWorker::progressCallback(
    void *userData,
    curl_off_t downloadTotal,
    curl_off_t downloadNow,
    curl_off_t,
    curl_off_t)
{
    auto *context = static_cast<WriteContext *>(userData);
    if (!context || !context->owner) {
        return 1;
    }
    if (context->owner->m_pauseRequested.load()) {
        return 1;
    }
    const qint64 knownTotal = downloadTotal > 0
        ? context->initialOffset + downloadTotal : 0;
    emit context->owner->progress(
        context->initialOffset + std::max<qint64>(0, downloadNow),
        knownTotal);
    return 0;
}

void CurlFtpWorker::run()
{
    CURL *probe = curl_easy_init();
    if (!probe) {
        m_errorString = QStringLiteral("无法初始化 FTP 探测。");
        m_result = Result::Failed;
        return;
    }
    configureCommon(probe);
    curl_easy_setopt(probe, CURLOPT_NOBODY, 1L);
    curl_easy_setopt(probe, CURLOPT_HEADER, 0L);
    curl_easy_setopt(probe, CURLOPT_WRITEFUNCTION, &discardCallback);
    char probeError[CURL_ERROR_SIZE] = {};
    curl_easy_setopt(probe, CURLOPT_ERRORBUFFER, probeError);
    const CURLcode probeCode = curl_easy_perform(probe);
    curl_off_t probeLength = -1;
    curl_easy_getinfo(probe, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &probeLength);
    curl_easy_cleanup(probe);
    if (m_pauseRequested.load()) {
        m_result = Result::Paused;
        return;
    }
    if (probeCode != CURLE_OK) {
        m_errorString = curlError(probeCode, probeError);
        m_result = Result::Failed;
        return;
    }
    qint64 totalBytes = probeLength > 0 ? probeLength : 0;
    emit metadata(totalBytes);

    QFile file(m_tempPath);
    if (!file.open(QIODevice::ReadWrite | QIODevice::Append)) {
        m_errorString =
            QStringLiteral("无法打开 FTP 临时文件：%1").arg(file.errorString());
        m_result = Result::Failed;
        return;
    }
    qint64 resumeFrom = file.size();
    if (totalBytes > 0 && resumeFrom > totalBytes) {
        m_errorString = QStringLiteral("FTP 临时文件大于远端文件。");
        m_result = Result::Failed;
        return;
    }
    WriteContext context{&file, this, resumeFrom, resumeFrom};

    bool allowResume = resumeFrom > 0;
    bool restartedFromZero = false;
    for (int attempt = 0; attempt < 4; ++attempt) {
        if (m_pauseRequested.load()) {
            m_result = Result::Paused;
            break;
        }
        CURL *easy = curl_easy_init();
        if (!easy) {
            m_errorString = QStringLiteral("无法初始化 FTP 下载。");
            m_result = Result::Failed;
            break;
        }
        configureCommon(easy);
        char errorBuffer[CURL_ERROR_SIZE] = {};
        curl_easy_setopt(easy, CURLOPT_ERRORBUFFER, errorBuffer);
        curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, &writeCallback);
        curl_easy_setopt(easy, CURLOPT_WRITEDATA, &context);
        curl_easy_setopt(easy, CURLOPT_XFERINFOFUNCTION, &progressCallback);
        curl_easy_setopt(easy, CURLOPT_XFERINFODATA, &context);
        curl_easy_setopt(easy, CURLOPT_NOPROGRESS, 0L);
        const qint64 offset = allowResume ? context.resumeFrom : 0;
        if (offset > 0) {
            curl_easy_setopt(easy, CURLOPT_RESUME_FROM_LARGE, offset);
        }

        const CURLcode code = curl_easy_perform(easy);
        curl_easy_cleanup(easy);
        file.flush();
        if (code == CURLE_OK) {
            m_result = Result::Completed;
            break;
        }
        if (m_pauseRequested.load() || code == CURLE_ABORTED_BY_CALLBACK) {
            m_result = Result::Paused;
            break;
        }
        const QString detail = curlError(code, errorBuffer);
        if (allowResume && !restartedFromZero
            && (code == CURLE_FTP_COULDNT_RETR_FILE
                || code == CURLE_REMOTE_ACCESS_DENIED)) {
            allowResume = false;
            restartedFromZero = true;
            file.resize(0);
            context.resumeFrom = 0;
            context.initialOffset = 0;
            emit resumeUnsupported();
            continue;
        }
        if (code == CURLE_PEER_FAILED_VERIFICATION
            || code == CURLE_SSL_CONNECT_ERROR
            || code == CURLE_REMOTE_FILE_NOT_FOUND
            || code == CURLE_LOGIN_DENIED
            || code == CURLE_REMOTE_ACCESS_DENIED) {
            m_errorString = detail;
            m_result = Result::Failed;
            break;
        }
        if (attempt >= 3) {
            m_errorString = detail;
            m_result = Result::Failed;
            break;
        }
        QThread::msleep(static_cast<unsigned long>(400 * (1 << attempt)));
    }
    file.flush();
    file.close();
}
