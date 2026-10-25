#include "ProtocolRouter.h"

#include <QTest>

class ProtocolRouterTest final : public QObject
{
    Q_OBJECT

private slots:
    void classifiesProtocols();
    void rejectsUnknownSchemes();
    void redactsCredentialsAndQueries();
};

void ProtocolRouterTest::classifiesProtocols()
{
    QCOMPARE(
        ProtocolRouter::route(QStringLiteral("https://example.com/file.bin")).protocol,
        ProtocolRouter::Protocol::Https);
    QCOMPARE(
        ProtocolRouter::route(QStringLiteral("http://example.com/live.m3u8?token=x")).protocol,
        ProtocolRouter::Protocol::Hls);
    QCOMPARE(
        ProtocolRouter::route(QStringLiteral("ftp://user:pass@example.com/file")).protocol,
        ProtocolRouter::Protocol::Ftp);
    QCOMPARE(
        ProtocolRouter::route(QStringLiteral("ftps://user:pass@example.com/file")).protocol,
        ProtocolRouter::Protocol::Ftps);
}

void ProtocolRouterTest::rejectsUnknownSchemes()
{
    const ProtocolRouter::Route route =
        ProtocolRouter::route(QStringLiteral("file:///tmp/a.bin"));
    QCOMPARE(route.protocol, ProtocolRouter::Protocol::Unsupported);
    QVERIFY(!route.error.isEmpty());
}

void ProtocolRouterTest::redactsCredentialsAndQueries()
{
    const QString redacted = ProtocolRouter::redactedUrl(
        QUrl(QStringLiteral(
            "https://user:secret@example.com/file.bin?token=abc#fragment")));
    QVERIFY(!redacted.contains(QStringLiteral("user")));
    QVERIFY(!redacted.contains(QStringLiteral("secret")));
    QVERIFY(!redacted.contains(QStringLiteral("token")));
    QVERIFY(redacted.contains(QStringLiteral("example.com/file.bin")));
}

QTEST_GUILESS_MAIN(ProtocolRouterTest)

#include "test_protocol_router.moc"
