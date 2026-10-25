#include "DownloadMemoryStore.h"

#include <QTemporaryDir>
#include <QTest>

class DownloadMemoryTest final : public QObject
{
    Q_OBJECT

private slots:
    void selectsFastestSuccessfulRoute();
    void recordsResourceMetadata();
    void clearsMemory();
};

namespace {
DownloadMemoryStore::Outcome outcome(
    const QString &route,
    double speed,
    int connections,
    bool success = true)
{
    DownloadMemoryStore::Outcome value;
    value.url = QUrl(QStringLiteral("https://cdn.example/file.bin?token=secret"));
    value.effectiveUrl = QStringLiteral("https://cdn.example/file.bin");
    value.finalPath = QStringLiteral("/tmp/file.bin");
    value.route = route;
    value.bytes = 1024;
    value.speedMBps = speed;
    value.peakConnections = connections;
    value.supportsRanges = true;
    value.etag = QStringLiteral("\"v1\"");
    value.success = success;
    return value;
}
}

void DownloadMemoryTest::selectsFastestSuccessfulRoute()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    DownloadMemoryStore store(
        directory.filePath(QStringLiteral("memory.sqlite")));
    QVERIFY2(store.ready(), qPrintable(store.lastError()));

    QVERIFY(store.recordOutcome(
        outcome(QStringLiteral("direct"), 12.0, 8)));
    auto memory = store.lookupHost(QStringLiteral("cdn.example"));
    QVERIFY(memory.isValid());
    QCOMPARE(memory.preferredRoute, QStringLiteral("direct"));
    QCOMPARE(memory.recommendedConnections, 8);

    QVERIFY(store.recordOutcome(
        outcome(QStringLiteral("builtin"), 48.0, 16)));
    memory = store.lookupHost(QStringLiteral("cdn.example"));
    QVERIFY(memory.isValid());
    QCOMPARE(memory.preferredRoute, QStringLiteral("builtin"));
    QCOMPARE(memory.recommendedConnections, 16);
}

void DownloadMemoryTest::recordsResourceMetadata()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    DownloadMemoryStore store(
        directory.filePath(QStringLiteral("memory.sqlite")));
    QVERIFY(store.recordOutcome(
        outcome(QStringLiteral("direct"), 20.0, 12)));

    const auto resource = store.lookupResource(
        QUrl(QStringLiteral("https://cdn.example/file.bin?other=value")));
    QCOMPARE(resource.host, QStringLiteral("cdn.example"));
    QCOMPARE(resource.totalBytes, qint64(1024));
    QVERIFY(resource.supportsRanges);
    QCOMPARE(resource.etag, QStringLiteral("\"v1\""));
}

void DownloadMemoryTest::clearsMemory()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    DownloadMemoryStore store(
        directory.filePath(QStringLiteral("memory.sqlite")));
    QVERIFY(store.recordOutcome(
        outcome(QStringLiteral("direct"), 20.0, 12)));
    QVERIFY(store.clearAll());
    QVERIFY(!store.lookupHost(QStringLiteral("cdn.example")).isValid());
}

QTEST_GUILESS_MAIN(DownloadMemoryTest)

#include "test_download_memory.moc"
