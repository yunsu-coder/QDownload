#include "DownloadController.h"

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTimer>
#include <QDebug>

#include <curl/curl.h>

int main(int argc, char *argv[])
{
    curl_global_init(CURL_GLOBAL_DEFAULT);
    qInfo().noquote() << "libcurl version:" << curl_version();
    QGuiApplication app(argc, argv);
    app.setOrganizationName(QStringLiteral("QuickDL"));
    app.setApplicationName(QStringLiteral("QuickDL"));
    app.setApplicationDisplayName(QStringLiteral("快下载"));

    QQuickStyle::setStyle(QStringLiteral("Basic"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("快下载单任务下载器原型"));
    parser.addHelpOption();

    QCommandLineOption urlOption(
        QStringList{QStringLiteral("u"), QStringLiteral("url")},
        QStringLiteral("URL to prefill and optionally start."),
        QStringLiteral("url"));
    QCommandLineOption autoStartOption(
        QStringList{QStringLiteral("a"), QStringLiteral("autostart")},
        QStringLiteral("Start the URL immediately."));
    QCommandLineOption screenshotOption(
        QStringList{QStringLiteral("screenshot")},
        QStringLiteral("Save a window screenshot and exit."),
        QStringLiteral("path"));
    QCommandLineOption screenshotDelayOption(
        QStringList{QStringLiteral("screenshot-delay")},
        QStringLiteral("Delay before taking the screenshot, in milliseconds."),
        QStringLiteral("ms"),
        QStringLiteral("2500"));
    QCommandLineOption pauseAfterOption(
        QStringList{QStringLiteral("pause-after")},
        QStringLiteral("Pause the active transfer after the given milliseconds."),
        QStringLiteral("ms"));
    QCommandLineOption resumeAfterOption(
        QStringList{QStringLiteral("resume-after")},
        QStringLiteral("Resume the paused transfer after the given milliseconds."),
        QStringLiteral("ms"));
    QCommandLineOption retryAfterOption(
        QStringList{QStringLiteral("retry-after")},
        QStringLiteral("Retry a failed transfer after the given milliseconds."),
        QStringLiteral("ms"));
    QCommandLineOption hlsVariantOption(
        QStringList{QStringLiteral("hls-variant")},
        QStringLiteral("Select an HLS master playlist variant by zero-based index."),
        QStringLiteral("index"),
        QStringLiteral("-1"));
    QCommandLineOption proxyModeOption(
        QStringList{QStringLiteral("p"), QStringLiteral("proxy-mode")},
        QStringLiteral("Routing mode: auto, proxy, or direct."),
        QStringLiteral("mode"));
    QCommandLineOption proxyEndpointOption(
        QStringList{QStringLiteral("proxy-endpoint")},
        QStringLiteral("Local HTTP or SOCKS proxy, for example http://127.0.0.1:7890."),
        QStringLiteral("url"));
    QCommandLineOption downloadDirectoryOption(
        QStringList{QStringLiteral("download-dir")},
        QStringLiteral("Use a session-only download directory."),
        QStringLiteral("path"));

    parser.addOption(urlOption);
    parser.addOption(autoStartOption);
    parser.addOption(screenshotOption);
    parser.addOption(screenshotDelayOption);
    parser.addOption(pauseAfterOption);
    parser.addOption(resumeAfterOption);
    parser.addOption(retryAfterOption);
    parser.addOption(hlsVariantOption);
    parser.addOption(proxyModeOption);
    parser.addOption(proxyEndpointOption);
    parser.addOption(downloadDirectoryOption);
    parser.process(app);

    const QString initialUrl = parser.value(urlOption);
    const bool autoStart = parser.isSet(autoStartOption) && !initialUrl.isEmpty();
    QUrl initialDisplayUrl = QUrl::fromUserInput(initialUrl);
    initialDisplayUrl.setPassword({});

    DownloadController controller;
    if (parser.isSet(downloadDirectoryOption)) {
        controller.setDownloadDirectoryForSession(
            parser.value(downloadDirectoryOption));
    }
    if (parser.isSet(proxyEndpointOption)) {
        controller.setProxyEndpoint(parser.value(proxyEndpointOption));
    }
    if (parser.isSet(proxyModeOption)) {
        controller.setProxyMode(parser.value(proxyModeOption));
    }

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("downloadController"), &controller);
    engine.rootContext()->setContextProperty(QStringLiteral("initialUrl"), initialUrl);
    engine.rootContext()->setContextProperty(
        QStringLiteral("initialDisplayUrl"),
        initialDisplayUrl.toString());
    engine.rootContext()->setContextProperty(QStringLiteral("autoStart"), autoStart);

    QObject::connect(
        &engine,
        &QQmlApplicationEngine::objectCreationFailed,
        &app,
        []() { QCoreApplication::exit(-1); },
        Qt::QueuedConnection);

    engine.addImportPath(QStringLiteral("qrc:/"));
    engine.load(QUrl(QStringLiteral("qrc:/QuickDL/qml/Main.qml")));

    const int hlsVariant = parser.value(hlsVariantOption).toInt();
    if (hlsVariant >= 0) {
        QObject::connect(
            &controller,
            &DownloadController::stateChanged,
            &controller,
            [&controller, hlsVariant]() {
                if (controller.state() == QStringLiteral("Selecting")) {
                    QTimer::singleShot(
                        0,
                        &controller,
                        [&controller, hlsVariant]() {
                            controller.selectHlsVariant(hlsVariant);
                        });
                }
            });
    }

    const int pauseAfter = parser.value(pauseAfterOption).toInt();
    if (pauseAfter > 0) {
        QTimer::singleShot(pauseAfter, &controller, [&controller]() {
            controller.pauseDownload();
        });
    }
    const int resumeAfter = parser.value(resumeAfterOption).toInt();
    if (resumeAfter > 0) {
        QTimer::singleShot(resumeAfter, &controller, [&controller]() {
            controller.resumeDownload();
        });
    }
    const int retryAfter = parser.value(retryAfterOption).toInt();
    if (retryAfter > 0) {
        QTimer::singleShot(retryAfter, &controller, [&controller]() {
            controller.retry();
        });
    }

    const QString screenshotPath = parser.value(screenshotOption);
    if (!screenshotPath.isEmpty() && !engine.rootObjects().isEmpty()) {
        if (auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst())) {
            window->requestActivate();
            window->raise();
            const int delay = qMax(250, parser.value(screenshotDelayOption).toInt());
            QTimer::singleShot(delay, window, [window, screenshotPath]() {
                window->grabWindow().save(screenshotPath);
                QCoreApplication::quit();
            });
        }
    }

    const int exitCode = app.exec();
    curl_global_cleanup();
    return exitCode;
}
