#include "CurlMultiWorker.h"

#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>

namespace {
constexpr int kReportIntervalMs = 180;
constexpr int kMaxSegmentRetries = 2;
constexpr int kMaxConnections = 64;

bool isRetryableTransferError(CURLcode code)
{
    switch (code) {
    case CURLE_COULDNT_CONNECT:
    case CURLE_OPERATION_TIMEDOUT:
    case CURLE_SSL_CONNECT_ERROR:
    case CURLE_RECV_ERROR:
    case CURLE_SEND_ERROR:
    case CURLE_GOT_NOTHING:
    case CURLE_PARTIAL_FILE:
        return true;
    default:
        return false;
    }
}
}

CurlMultiWorker::CurlMultiWorker(QString url,
                                 QString tempPath,
                                 qint64 totalBytes,
                                 QVector<TransferRange> ranges,
                                 bool useRanges,
                                 bool resume,
                                 QString proxyUrl,
                                 int maxConnections,
                                 QObject *parent)
    : QThread(parent)
    , m_url(std::move(url))
    , m_tempPath(std::move(tempPath))
    , m_totalBytes(totalBytes)
    , m_initialRanges(std::move(ranges))
    , m_useRanges(useRanges)
    , m_resume(resume)
    , m_proxyUrl(std::move(proxyUrl))
    , m_maxConnections(qBound(1, maxConnections, 64))
{
    qRegisterMetaType<QVector<qint64>>("QVector<qint64>");
}

void CurlMultiWorker::requestPause()
{
    m_pauseRequested.store(true);
    if (CURLM *multi = m_multi.load()) {
        curl_multi_wakeup(multi);
    }
}

QVector<qint64> CurlMultiWorker::resumeOffsets() const
{
    return m_resumeOffsets;
}

QVector<qint64> CurlMultiWorker::currentOffsets() const
{
    QVector<qint64> offsets;
    offsets.reserve(m_segments.size());
    for (const Segment &segment : m_segments) {
        offsets.append(segment.current);
    }
    return offsets;
}

QVector<qint64> CurlMultiWorker::currentStarts() const
{
    QVector<qint64> values;
    values.reserve(m_segments.size());
    for (const Segment &segment : m_segments) {
        values.append(segment.start);
    }
    return values;
}

QVector<qint64> CurlMultiWorker::currentEnds() const
{
    QVector<qint64> values;
    values.reserve(m_segments.size());
    for (const Segment &segment : m_segments) {
        values.append(segment.end);
    }
    return values;
}

CurlMultiWorker::Result CurlMultiWorker::result() const
{
    return m_result;
}

QString CurlMultiWorker::errorString() const
{
    return m_errorString;
}

size_t CurlMultiWorker::writeCallback(char *data, size_t size, size_t count, void *userData)
{
    auto *segment = static_cast<Segment *>(userData);
    if (!segment || !segment->file || !segment->owner) {
        return 0;
    }
    if (segment->owner->m_pauseRequested.load()) {
        return 0;
    }

    const qint64 bytes = static_cast<qint64>(size * count);
    if (!segment->file->seek(segment->current)) {
        segment->owner->setFailure(QStringLiteral("无法定位临时文件写入偏移。"));
        return 0;
    }
    const qint64 written = segment->file->write(data, bytes);
    if (written <= 0) {
        segment->owner->setFailure(QStringLiteral("写入本地文件失败。"));
        return 0;
    }

    segment->current += written;
    segment->owner->m_bytesWritten.fetch_add(written, std::memory_order_relaxed);
    return static_cast<size_t>(written);
}

void CurlMultiWorker::setFailure(const QString &message)
{
    if (m_errorString.isEmpty()) {
        m_errorString = message;
    }
    m_result = Result::Failed;
}

void CurlMultiWorker::captureOffsets()
{
    m_resumeOffsets.clear();
    m_resumeOffsets.reserve(m_segments.size());
    for (const Segment &segment : m_segments) {
        m_resumeOffsets.append(segment.current);
    }
}

void CurlMultiWorker::run()
{
    m_result = Result::None;
    m_errorString.clear();
    m_resumeOffsets.clear();
    m_segments.clear();

    QFile file(m_tempPath);
    const auto openMode = m_resume
        ? QIODevice::ReadWrite
        : (QIODevice::ReadWrite | QIODevice::Truncate);
    if (!file.open(openMode)) {
        setFailure(QStringLiteral("无法打开临时文件：%1").arg(file.errorString()));
        captureOffsets();
        return;
    }
    if (m_totalBytes > 0 && file.size() != m_totalBytes) {
        if (!file.resize(m_totalBytes)) {
            setFailure(QStringLiteral("无法预分配临时文件：%1").arg(file.errorString()));
            captureOffsets();
            return;
        }
    }

    qint64 initialBytes = 0;
    m_segments.clear();
    for (int i = 0; i < m_initialRanges.size(); ++i) {
        m_segments.emplace_back();
        Segment &segment = m_segments.back();
        segment.index = i;
        segment.owner = this;
        segment.file = &file;
        segment.start = m_initialRanges[i].start;
        segment.current = m_initialRanges[i].current;
        segment.end = m_initialRanges[i].end;
        segment.hadRange = m_useRanges || segment.current > 0;
        segment.done = segment.end >= 0 && segment.current > segment.end;
        initialBytes += segment.current - segment.start;
        if (segment.hadRange) {
            // CURLOPT_RANGE adds the "bytes=" prefix itself.
            segment.rangeHeader = QByteArray::number(segment.current);
            if (segment.end >= 0) {
                segment.rangeHeader += '-';
                segment.rangeHeader += QByteArray::number(segment.end);
            } else {
                segment.rangeHeader += '-';
            }
        }
    }
    m_bytesWritten.store(initialBytes);

    CURLM *multi = curl_multi_init();
    if (!multi) {
        setFailure(QStringLiteral("libcurl multi 初始化失败。"));
        captureOffsets();
        return;
    }
    m_multi.store(multi);
    curl_multi_setopt(multi, CURLMOPT_MAX_TOTAL_CONNECTIONS,
                      static_cast<long>(kMaxConnections));
    curl_multi_setopt(multi, CURLMOPT_MAX_HOST_CONNECTIONS,
                      static_cast<long>(kMaxConnections));

    curl_slist *headers = nullptr;
    headers = curl_slist_append(headers, "Accept-Encoding: identity");
    const QByteArray urlBytes = m_url.toUtf8();
    const QByteArray userAgent = QByteArrayLiteral("QuickDL/0.3 (curl multi)");
    const QByteArray proxyBytes = m_proxyUrl.toUtf8();
    const bool useSocksProxy =
        m_proxyUrl.startsWith(QStringLiteral("socks5h://"), Qt::CaseInsensitive)
        || m_proxyUrl.startsWith(QStringLiteral("socks5://"), Qt::CaseInsensitive)
        || m_proxyUrl.startsWith(QStringLiteral("socks://"), Qt::CaseInsensitive);

    auto configureSegment = [&](Segment &segment) {
        segment.errorBuffer[0] = '\0';
        segment.done = false;
        segment.started = true;
        segment.easy = curl_easy_init();
        if (!segment.easy) {
            setFailure(QStringLiteral("创建传输通道失败。"));
            return false;
        }

        curl_easy_setopt(segment.easy, CURLOPT_URL, urlBytes.constData());
        curl_easy_setopt(segment.easy, CURLOPT_PRIVATE, &segment);
        curl_easy_setopt(segment.easy, CURLOPT_WRITEFUNCTION,
                         &CurlMultiWorker::writeCallback);
        curl_easy_setopt(segment.easy, CURLOPT_WRITEDATA, &segment);
        curl_easy_setopt(segment.easy, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(segment.easy, CURLOPT_FAILONERROR, 1L);
        curl_easy_setopt(segment.easy, CURLOPT_MAXREDIRS, 10L);
        curl_easy_setopt(segment.easy, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(segment.easy, CURLOPT_CONNECTTIMEOUT_MS, 15000L);
        curl_easy_setopt(segment.easy, CURLOPT_LOW_SPEED_LIMIT, 1L);
        curl_easy_setopt(segment.easy, CURLOPT_LOW_SPEED_TIME, 30L);
        curl_easy_setopt(segment.easy, CURLOPT_BUFFERSIZE, 262144L);
        curl_easy_setopt(segment.easy, CURLOPT_TCP_KEEPALIVE, 1L);
        curl_easy_setopt(segment.easy, CURLOPT_TCP_NODELAY, 1L);
        curl_easy_setopt(segment.easy, CURLOPT_USERAGENT, userAgent.constData());
        curl_easy_setopt(segment.easy, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(segment.easy, CURLOPT_ERRORBUFFER, segment.errorBuffer);
        if (!proxyBytes.isEmpty()) {
            curl_easy_setopt(segment.easy, CURLOPT_PROXY, proxyBytes.constData());
            curl_easy_setopt(
                segment.easy,
                CURLOPT_PROXYTYPE,
                useSocksProxy ? CURLPROXY_SOCKS5_HOSTNAME : CURLPROXY_HTTP);
        } else {
            // Do not let an environment proxy silently change a manual direct route.
            curl_easy_setopt(segment.easy, CURLOPT_NOPROXY, "*");
        }
        curl_easy_setopt(segment.easy, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(segment.easy, CURLOPT_SSL_VERIFYHOST, 2L);
        if (QFileInfo::exists(QStringLiteral("/etc/ssl/cert.pem"))) {
            curl_easy_setopt(segment.easy, CURLOPT_CAINFO, "/etc/ssl/cert.pem");
        }

        segment.rangeHeader.clear();
        if (segment.hadRange) {
            segment.rangeHeader = QByteArray::number(segment.current);
            if (segment.end >= 0) {
                segment.rangeHeader += '-';
                segment.rangeHeader += QByteArray::number(segment.end);
            } else {
                segment.rangeHeader += '-';
            }
            curl_easy_setopt(segment.easy, CURLOPT_RANGE, segment.rangeHeader.constData());
        }

        curl_multi_add_handle(multi, segment.easy);
        return true;
    };

    int nextSegment = 0;
    int inFlight = 0;
    int activeConnectionLimit = qMin(4, m_maxConnections);
    QVector<int> retryQueue;
    QHash<int, qint64> retryDue;
    auto startQueuedSegments = [&]() {
        while (inFlight < activeConnectionLimit
               && nextSegment < static_cast<int>(m_segments.size())) {
            Segment &segment = m_segments[nextSegment++];
            if (segment.done) {
                continue;
            }
            if (!configureSegment(segment)) {
                return false;
            }
            ++inFlight;
        }
        return true;
    };

    int running = 0;
    bool rangeFallback = false;
    QString failure;
    if (!startQueuedSegments()) {
        m_errorString = QStringLiteral("无法启动传输通道。");
    }
    QElapsedTimer reportClock;
    reportClock.start();
    QElapsedTimer schedulerClock;
    schedulerClock.start();
    QElapsedTimer progressClock;
    progressClock.start();
    QElapsedTimer rampClock;
    rampClock.start();
    emit progress(m_bytesWritten.load(), inFlight, currentOffsets());

    while (!m_pauseRequested.load() && m_result != Result::Failed
           && !rangeFallback
           && (inFlight > 0 || !retryQueue.isEmpty())
           && !m_errorString.startsWith(QStringLiteral("无法启动"))) {
        if (inFlight > 0) {
            int numfds = 0;
            const CURLMcode pollCode =
                curl_multi_poll(multi, nullptr, 0, 200, &numfds);
            if (pollCode != CURLM_OK) {
                failure = QString::fromLatin1(curl_multi_strerror(pollCode));
                setFailure(failure);
                break;
            }
            curl_multi_perform(multi, &running);
        } else {
            QThread::msleep(20);
        }

        int messagesLeft = 0;
        while (CURLMsg *message = curl_multi_info_read(multi, &messagesLeft)) {
            if (message->msg != CURLMSG_DONE || !message->easy_handle) {
                continue;
            }

            char *privateData = nullptr;
            curl_easy_getinfo(message->easy_handle, CURLINFO_PRIVATE, &privateData);
            auto *segment = reinterpret_cast<Segment *>(privateData);
            if (!segment) {
                continue;
            }

            long responseCode = 0;
            curl_easy_getinfo(message->easy_handle, CURLINFO_RESPONSE_CODE, &responseCode);
            curl_off_t retryAfter = 0;
            curl_easy_getinfo(
                message->easy_handle, CURLINFO_RETRY_AFTER, &retryAfter);
            const CURLcode transferResult = message->data.result;
            curl_multi_remove_handle(multi, segment->easy);
            curl_easy_cleanup(segment->easy);
            segment->easy = nullptr;
            --inFlight;

            if (responseCode >= 400) {
                const bool retryableHttp =
                    responseCode == 408 || responseCode == 429
                    || responseCode >= 500;
                if (retryableHttp
                    && segment->retries < kMaxSegmentRetries) {
                    ++segment->retries;
                    segment->done = false;
                    segment->started = false;
                    if (responseCode == 429 || responseCode == 503) {
                        activeConnectionLimit =
                            qMax(1, activeConnectionLimit / 2);
                    }
                    qint64 delay = 500LL * (1LL << segment->retries);
                    if (retryAfter > 0 && retryAfter <= 60) {
                        delay = qMax<qint64>(delay, retryAfter * 1000);
                    }
                    retryQueue.append(segment->index);
                    retryDue.insert(
                        segment->index,
                        schedulerClock.elapsed() + delay);
                    continue;
                }
                failure = QStringLiteral("HTTP %1").arg(responseCode);
                setFailure(failure);
                break;
            }
            if (transferResult != CURLE_OK) {
                const bool retryable = isRetryableTransferError(transferResult)
                    && m_result != Result::Failed
                    && segment->retries < kMaxSegmentRetries;
                if (retryable) {
                    ++segment->retries;
                    segment->errorBuffer[0] = '\0';
                    if (segment->hadRange && segment->end >= 0
                        && segment->current > segment->end) {
                        segment->done = true;
                        continue;
                    }
                    if (configureSegment(*segment)) {
                        ++inFlight;
                        continue;
                    }
                }
                const QString detail = segment->errorBuffer[0]
                    ? QString::fromUtf8(segment->errorBuffer)
                    : QString::fromLatin1(curl_easy_strerror(transferResult));
                failure = detail;
                setFailure(failure);
                break;
            }
            if (responseCode >= 400) {
                failure = QStringLiteral("HTTP %1").arg(responseCode);
                setFailure(failure);
                break;
            }
            if (segment->hadRange && responseCode != 206) {
                rangeFallback = true;
                failure = QStringLiteral("服务器忽略了 Range 请求。");
                break;
            }

            segment->done = true;
        }

        if (m_result == Result::Failed || rangeFallback) {
            break;
        }
        const qint64 now = schedulerClock.elapsed();
        for (int index = retryQueue.size() - 1; index >= 0; --index) {
            const int segmentIndex = retryQueue[index];
            if (retryDue.value(segmentIndex) > now) {
                continue;
            }
            retryQueue.removeAt(index);
            retryDue.remove(segmentIndex);
            Segment &segment = m_segments[segmentIndex];
            if (!configureSegment(segment)) {
                setFailure(QStringLiteral("无法重启 HTTP 重试通道。"));
                break;
            }
            ++inFlight;
        }
        if (m_result == Result::Failed) {
            break;
        }
        if (!startQueuedSegments()) {
            setFailure(QStringLiteral("无法启动后续传输通道。"));
            break;
        }
        if (rampClock.elapsed() >= 1500
            && activeConnectionLimit < m_maxConnections) {
            activeConnectionLimit =
                qMin(m_maxConnections, activeConnectionLimit + 4);
            rampClock.restart();
        }
        if (reportClock.elapsed() >= kReportIntervalMs) {
            reportClock.restart();
            emit progress(m_bytesWritten.load(), inFlight, currentOffsets());

            for (Segment &segment : m_segments) {
                if (segment.current != segment.lastObservedCurrent) {
                    segment.lastObservedCurrent = segment.current;
                    segment.lastObservedAt = progressClock.elapsed();
                }
            }
            if (nextSegment >= static_cast<int>(m_segments.size())) {
                for (Segment &segment : m_segments) {
                    if (!segment.started || segment.done || !segment.easy
                        || segment.splits >= 2 || segment.end < 0) {
                        continue;
                    }
                    const qint64 remaining =
                        segment.end - segment.current + 1;
                    if (remaining < 2 * 1024 * 1024
                        || progressClock.elapsed()
                            - segment.lastObservedAt < 4000) {
                        continue;
                    }
                    const qint64 splitAt =
                        segment.current + remaining / 2;
                    if (splitAt <= segment.current
                        || splitAt >= segment.end) {
                        continue;
                    }
                    curl_multi_remove_handle(multi, segment.easy);
                    curl_easy_cleanup(segment.easy);
                    segment.easy = nullptr;
                    --inFlight;
                    Segment child;
                    child.index = static_cast<int>(m_segments.size());
                    child.start = splitAt + 1;
                    child.current = child.start;
                    child.end = segment.end;
                    child.hadRange = true;
                    child.owner = this;
                    child.file = segment.file;
                    segment.end = splitAt;
                    ++segment.splits;
                    child.splits = segment.splits;
                    segment.started = false;
                    m_segments.push_back(child);
                    emit planChanged(
                        currentStarts(), currentOffsets(), currentEnds());
                    if (!configureSegment(segment)) {
                        setFailure(QStringLiteral("无法拆分慢尾传输通道。"));
                    } else {
                        ++inFlight;
                    }
                    break;
                }
            }
        }
    }

    if (m_pauseRequested.load()) {
        m_result = Result::Paused;
    } else if (rangeFallback) {
        m_result = Result::RangeFallback;
        m_errorString = failure;
    } else if (m_result == Result::Failed) {
        // setFailure already recorded the message.
    } else {
        bool complete = !m_segments.empty();
        for (const Segment &segment : m_segments) {
            if (!segment.done) {
                complete = false;
                break;
            }
            if (segment.end >= 0 && segment.current <= segment.end) {
                complete = false;
                break;
            }
        }
        if (complete) {
            m_result = Result::Completed;
        } else {
            setFailure(QStringLiteral("传输结束，但仍有分段未完成。"));
        }
    }

    captureOffsets();
    emit planChanged(currentStarts(), m_resumeOffsets, currentEnds());
    file.flush();
    file.close();
    emit progress(m_bytesWritten.load(), 0, currentOffsets());

    for (Segment &segment : m_segments) {
        if (segment.easy) {
            curl_multi_remove_handle(multi, segment.easy);
            curl_easy_cleanup(segment.easy);
            segment.easy = nullptr;
        }
    }
    curl_slist_free_all(headers);
    m_multi.store(nullptr);
    curl_multi_cleanup(multi);
}
