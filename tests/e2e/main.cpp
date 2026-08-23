// Headless end-to-end driver: AudioEngine -> TranscribeEngine, no GUI.
// Built as `gromarch-e2e`; intended to run inside a container with a headless
// PipeWire and virtual devices feeding the default source / sink monitor.
//
//   gromarch-e2e --model ggml-base.en.bin --seconds 30 [--audio out.ogg] [--out t.txt]
//
// Exit codes: 0 = at least one final segment, 2 = ran but produced none,
//             1 = error (model load failure, audio error, bad arguments).

#include <QByteArray>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QFile>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTextStream>
#include <QTimer>
#include <QVector>

#include <algorithm>

#include "Types.h"
#include "audio/AudioEngine.h"
#include "transcribe/TranscribeEngine.h"

using namespace gromarch;

namespace {

constexpr int kFinishTimeoutMs = 60000;   // grace period for finish() -> finished()

QString speakerOf(int stream) {
    return stream == int(Stream::System) ? QStringLiteral("Them") : QStringLiteral("Me");
}

// "Them#0 [1.0-2.5]: ..." when the segment carries a diarized turn index,
// plain "Them [...]" / "Me [...]" when it does not.
QString formatSegment(const TranscriptSegment& seg) {
    QString who = speakerOf(seg.stream);
    if (seg.speaker >= 0) who += QStringLiteral("#") + QString::number(seg.speaker);
    return QStringLiteral("%1 [%2-%3]: %4")
        .arg(who)
        .arg(seg.t0, 0, 'f', 1)
        .arg(seg.t1, 0, 'f', 1)
        .arg(seg.text);
}

} // namespace

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    app.setApplicationName("gromarch-e2e");
    app.setApplicationVersion("0.1.0");

    QCommandLineParser parser;
    parser.setApplicationDescription("Headless AudioEngine -> TranscribeEngine end-to-end test.");
    parser.addHelpOption();
    parser.addVersionOption();
    QCommandLineOption modelOpt(QStringLiteral("model"),
                                QStringLiteral("Whisper ggml model file (required)."),
                                QStringLiteral("path"));
    QCommandLineOption secondsOpt(QStringLiteral("seconds"),
                                  QStringLiteral("Capture duration in seconds (default 30)."),
                                  QStringLiteral("N"), QStringLiteral("30"));
    QCommandLineOption audioOpt(QStringLiteral("audio"),
                                QStringLiteral("Write the captured audio to this Ogg Opus file."),
                                QStringLiteral("path"));
    QCommandLineOption outOpt(QStringLiteral("out"),
                              QStringLiteral("Also write the final transcript to this file."),
                              QStringLiteral("path"));
    parser.addOption(modelOpt);
    parser.addOption(secondsOpt);
    parser.addOption(audioOpt);
    parser.addOption(outOpt);
    parser.process(app);

    QTextStream out(stdout);
    QTextStream err(stderr);

    if (!parser.isSet(modelOpt) || parser.value(modelOpt).isEmpty()) {
        err << "error: --model <path> is required\n" << Qt::flush;
        return 1;
    }
    const QString modelPath = parser.value(modelOpt);
    const QString audioPath = parser.value(audioOpt);
    const QString outPath = parser.value(outOpt);

    bool secondsOk = false;
    const double seconds = parser.value(secondsOpt).toDouble(&secondsOk);
    if (!secondsOk || seconds <= 0.0) {
        err << "error: --seconds must be a positive number\n" << Qt::flush;
        return 1;
    }

    qRegisterMetaType<TranscriptSegment>();

    AudioEngine audio;
    TranscribeEngine transcribe;

    QVector<TranscriptSegment> finals;
    bool done = false;      // results already reported; ignore anything later
    bool failed = false;

    auto report = [&](const char* why) {
        if (done) return;
        done = true;
        if (why) err << "note: " << why << "\n";

        std::stable_sort(finals.begin(), finals.end(),
                         [](const TranscriptSegment& a, const TranscriptSegment& b) {
                             return a.t0 < b.t0;
                         });

        QStringList lines;
        int me = 0;
        int them = 0;
        QSet<int> speakers;      // distinct turn indices seen on the System stream
        for (const TranscriptSegment& seg : finals) {
            lines << formatSegment(seg);
            if (seg.stream == int(Stream::System)) {
                ++them;
                if (seg.speaker >= 0) speakers.insert(seg.speaker);
            } else {
                ++me;
            }
        }
        for (const QString& line : lines) out << line << "\n";
        out.flush();
        err << "segments=" << finals.size() << " me=" << me << " them=" << them
            << " speakers=" << speakers.size() << "\n" << Qt::flush;

        if (!outPath.isEmpty()) {
            QFile file(outPath);
            if (file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
                QTextStream ts(&file);
                for (const QString& line : lines) ts << line << "\n";
            } else {
                err << "warning: could not write " << outPath << ": " << file.errorString() << "\n"
                    << Qt::flush;
            }
        }
        app.exit(finals.isEmpty() ? 2 : 0);
    };

    auto fail = [&](const QString& message) {
        if (done || failed) return;
        failed = true;
        done = true;
        err << "error: " << message << "\n" << Qt::flush;
        audio.stop();
        app.exit(1);
    };

    QObject::connect(&audio, &AudioEngine::pcmReady, &transcribe, &TranscribeEngine::feed);
    QObject::connect(&audio, &AudioEngine::error, &app, [&](const QString& message) {
        fail(QStringLiteral("audio: %1").arg(message));
    });
    QObject::connect(&transcribe, &TranscribeEngine::error, &app, [&](const QString& message) {
        fail(QStringLiteral("transcribe: %1").arg(message));
    });
    QObject::connect(&transcribe, &TranscribeEngine::segmentReady, &app,
                     [&](const gromarch::TranscriptSegment& seg) {
                         if (seg.final) {
                             finals.append(seg);
                             err << formatSegment(seg) << "\n" << Qt::flush;
                         } else {
                             err << "~" << formatSegment(seg) << "\n" << Qt::flush;
                         }
                     });
    QObject::connect(&transcribe, &TranscribeEngine::finished, &app, [&] { report(nullptr); });

    // Everything below runs once the event loop is live, so an error raised during
    // start() can exit cleanly instead of racing app.exec().
    QTimer::singleShot(0, &app, [&] {
        transcribe.setModelPath(modelPath);
        transcribe.begin();
        if (done) return;

        audio.start(audioPath);
        if (done) return;
        err << "recording for " << QString::number(seconds, 'f', 1) << "s"
            << (audioPath.isEmpty() ? QString() : QStringLiteral(" -> ") + audioPath) << "\n"
            << Qt::flush;

        QTimer::singleShot(int(seconds * 1000.0), &app, [&] {
            if (done) return;
            audio.stop();
            transcribe.finish();
            QTimer::singleShot(kFinishTimeoutMs, &app, [&] {
                report("timed out waiting for finished()");
            });
        });
    });

    const int rc = app.exec();
    return rc;
}
