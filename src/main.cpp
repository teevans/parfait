#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QLocalSocket>
#include <QLocalServer>
#include <QCommandLineParser>

#include "Types.h"
#include "audio/AudioEngine.h"
#include "transcribe/TranscribeEngine.h"
#include "llm/EnhanceService.h"
#include "calendar/CalendarService.h"
#include "models/ModelDownloader.h"
#include "store/Library.h"
#include "theme/ThemeService.h"
#include "MeetingController.h"
#include "MeetingListModel.h"

using namespace parfait;

static const QString kSocketName = QStringLiteral("parfait-ipc");

int main(int argc, char* argv[]) {
    QGuiApplication app(argc, argv);
    app.setApplicationName("parfait");
    app.setOrganizationName("parfait");
    app.setApplicationVersion("0.1.0");
    QQuickStyle::setStyle("Basic");

    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addVersionOption();
    QCommandLineOption retint("retint", "Ask a running instance to reload the Omarchy theme.");
    parser.addOption(retint);
    parser.process(app);

    if (parser.isSet(retint)) {
        QLocalSocket sock;
        sock.connectToServer(kSocketName);
        if (sock.waitForConnected(500)) {
            sock.write("retint\n");
            sock.waitForBytesWritten(500);
        }
        return 0;
    }

    qRegisterMetaType<TranscriptSegment>();
    qRegisterMetaType<Meeting>();
    qRegisterMetaType<CalendarEvent>();

    ThemeService theme;
    Library library;
    library.open();
    AudioEngine audio;
    TranscribeEngine transcribe;
    EnhanceService enhance;
    CalendarService calendar;
    ModelDownloader models;
    MeetingController controller(&audio, &transcribe, &enhance, &library);
    MeetingListModel meetings(&library);

    // Model picked in Settings wins over TranscribeEngine's built-in default,
    // and switching it in the UI applies without a restart.
    if (!models.activeModelPath().isEmpty())
        transcribe.setModelPath(models.activeModelPath());
    QObject::connect(&models, &ModelDownloader::activeModelPathChanged,
                     &transcribe, &TranscribeEngine::setModelPath);

    // Local socket so `parfait --retint` (from the Omarchy theme-set hook) works.
    QLocalServer::removeServer(kSocketName);
    QLocalServer ipc;
    ipc.listen(kSocketName);
    QObject::connect(&ipc, &QLocalServer::newConnection, [&] {
        auto* conn = ipc.nextPendingConnection();
        QObject::connect(conn, &QLocalSocket::readyRead, [&theme, conn] {
            if (conn->readAll().startsWith("retint")) theme.reload();
            conn->deleteLater();
        });
    });

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("Theme", &theme);
    engine.rootContext()->setContextProperty("Controller", &controller);
    engine.rootContext()->setContextProperty("LibraryStore", &library);
    engine.rootContext()->setContextProperty("Meetings", &meetings);
    engine.rootContext()->setContextProperty("Audio", &audio);
    engine.rootContext()->setContextProperty("Transcriber", &transcribe);
    engine.rootContext()->setContextProperty("Enhancer", &enhance);
    engine.rootContext()->setContextProperty("Calendar", &calendar);
    engine.rootContext()->setContextProperty("Models", &models);
    engine.load(QUrl(QStringLiteral("qrc:/Parfait/Main.qml")));
    if (engine.rootObjects().isEmpty()) return 1;
    return app.exec();
}
