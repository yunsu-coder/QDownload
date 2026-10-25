#include "Aes128.h"
#include "HlsPlaylist.h"

#include <QTest>

class HlsTest final : public QObject
{
    Q_OBJECT

private slots:
    void decryptsAes128Cbc();
    void parsesMasterVariants();
    void parsesEncryptedMediaPlaylist();
    void rejectsLivePlaylist();
};

void HlsTest::decryptsAes128Cbc()
{
    const QByteArray key =
        QByteArray::fromHex("000102030405060708090a0b0c0d0e0f");
    const QByteArray iv =
        QByteArray::fromHex("101112131415161718191a1b1c1d1e1f");
    const QByteArray ciphertext = QByteArray::fromHex(
        "d57008dab74413361be17f45d3139560"
        "58a3dc433843563c10a272317d984b33"
        "29f27d9a9c001067c5ea8dbc8156eaff");
    QString error;
    const QByteArray plaintext =
        Aes128::decryptCbc(ciphertext, key, iv, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(plaintext, QByteArray("QuickDL HLS AES-128 test segment"));
}

void HlsTest::parsesMasterVariants()
{
    const QByteArray content =
        "#EXTM3U\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=800000,RESOLUTION=1280x720\n"
        "720/index.m3u8\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=2400000,RESOLUTION=1920x1080\n"
        "1080/index.m3u8\n";
    QVector<HlsPlaylist::Variant> variants;
    QString error;
    QVERIFY2(
        HlsPlaylist::parseMaster(
            content, QUrl("https://cdn.example/master.m3u8"), &variants, &error),
        qPrintable(error));
    QCOMPARE(variants.size(), 2);
    QCOMPARE(variants[0].resolution, QStringLiteral("1920x1080"));
    QCOMPARE(
        variants[0].url,
        QUrl("https://cdn.example/1080/index.m3u8"));
}

void HlsTest::parsesEncryptedMediaPlaylist()
{
    const QByteArray content =
        "#EXTM3U\n"
        "#EXT-X-VERSION:3\n"
        "#EXT-X-TARGETDURATION:6\n"
        "#EXT-X-MEDIA-SEQUENCE:42\n"
        "#EXT-X-MAP:URI=\"init.mp4\"\n"
        "#EXT-X-KEY:METHOD=AES-128,URI=\"key.bin\","
        "IV=0x000102030405060708090a0b0c0d0e0f\n"
        "#EXTINF:5.5,\n"
        "segment-42.m4s\n"
        "#EXTINF:5.0,\n"
        "segment-43.m4s\n"
        "#EXT-X-ENDLIST\n";
    HlsPlaylist::Media media;
    QString error;
    QVERIFY2(
        HlsPlaylist::parseMedia(
            content, QUrl("https://cdn.example/video/index.m3u8"), &media, &error),
        qPrintable(error));
    QCOMPARE(media.segments.size(), 2);
    QCOMPARE(media.extension, QStringLiteral("m4s"));
    QCOMPARE(media.initUrl, QUrl("https://cdn.example/video/init.mp4"));
    QCOMPARE(media.segments[0].sequence, qint64(42));
    QVERIFY(media.segments[0].key.encrypted);
    QCOMPARE(
        media.segments[0].key.uri,
        QUrl("https://cdn.example/video/key.bin"));
}

void HlsTest::rejectsLivePlaylist()
{
    const QByteArray content =
        "#EXTM3U\n"
        "#EXT-X-TARGETDURATION:6\n"
        "#EXTINF:6,\n"
        "live-1.ts\n";
    HlsPlaylist::Media media;
    QString error;
    QVERIFY(!HlsPlaylist::parseMedia(
        content, QUrl("https://cdn.example/live.m3u8"), &media, &error));
    QVERIFY(error.contains(QStringLiteral("仅支持 VOD")));
}

QTEST_GUILESS_MAIN(HlsTest)

#include "test_hls.moc"
