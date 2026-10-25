#include "TaskStore.h"

#include <QFile>
#include <QTemporaryDir>
#include <QTest>

class TaskStoreTest final : public QObject
{
    Q_OBJECT

private slots:
    void persistsAndLoadsTask();
    void marksCompletedAndPrunesSegments();
};

void TaskStoreTest::persistsAndLoadsTask()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("tasks.sqlite"));

    TaskStore::TaskRecord input;
    input.id = QStringLiteral("task-1");
    input.url = QStringLiteral("https://example.com/a.bin?token=secret");
    input.effectiveUrl = QStringLiteral("https://cdn.example.com/a.bin");
    input.fileName = QStringLiteral("a.bin");
    input.targetPath = QStringLiteral("/tmp/a.bin");
    input.tempPath = QStringLiteral("/tmp/a.bin.part");
    input.protocol = QStringLiteral("hls");
    input.protocolMetadata = QStringLiteral("{\"segmentCount\":2}");
    input.totalBytes = 100;
    input.downloadedBytes = 40;
    input.usingRanges = true;
    input.connectionLimit = 16;
    input.state = QStringLiteral("Downloading");
    const QVector<TaskStore::SegmentRecord> segments{
        {0, 20, 49},
        {50, 70, 99}
    };

    {
        TaskStore store(path);
        QVERIFY2(store.ready(), qPrintable(store.lastError()));
        QVERIFY2(store.saveIncomplete(input, segments), qPrintable(store.lastError()));
        QVERIFY(QFile::exists(path));
    }

    TaskStore restoredStore(path);
    QVERIFY2(restoredStore.ready(), qPrintable(restoredStore.lastError()));
    TaskStore::TaskRecord output;
    QVector<TaskStore::SegmentRecord> outputSegments;
    QVERIFY2(
        restoredStore.loadLatestIncomplete(&output, &outputSegments),
        qPrintable(restoredStore.lastError()));
    QCOMPARE(output.id, input.id);
    QCOMPARE(output.protocol, QStringLiteral("hls"));
    QCOMPARE(output.protocolMetadata, input.protocolMetadata);
    QCOMPARE(output.state, QStringLiteral("Paused"));
    QCOMPARE(outputSegments.size(), 2);
    QCOMPARE(outputSegments[1].current, qint64(70));
}

void TaskStoreTest::marksCompletedAndPrunesSegments()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    TaskStore store(directory.filePath(QStringLiteral("tasks.sqlite")));
    QVERIFY(store.ready());

    TaskStore::TaskRecord task;
    task.id = QStringLiteral("task-2");
    task.url = QStringLiteral("https://example.com/file");
    task.state = QStringLiteral("Downloading");
    task.totalBytes = 10;
    QVERIFY(store.saveIncomplete(task, {{0, 5, 9}}));
    QVERIFY(store.markCompleted(
        task.id,
        QStringLiteral("/tmp/file"),
        QString(64, QLatin1Char('a')),
        10));

    TaskStore::TaskRecord output;
    QVector<TaskStore::SegmentRecord> segments;
    QVERIFY(!store.loadLatestIncomplete(&output, &segments));
}

QTEST_GUILESS_MAIN(TaskStoreTest)

#include "test_taskstore.moc"
