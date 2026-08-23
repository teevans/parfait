#include "MeetingController.h"

#include <QDateTime>
#include <QDir>
#include <QHash>
#include <QTimer>

#include "audio/AudioEngine.h"
#include "llm/EnhanceService.h"
#include "store/Library.h"
#include "transcribe/TranscribeEngine.h"

namespace gromarch {
namespace {

const char* kUntitled = "Untitled meeting";

// The header is a frozen contract with no data members, so per-instance state
// lives in this side table, torn down when the controller is destroyed.
struct Priv {
    AudioEngine* audio = nullptr;
    TranscribeEngine* transcribe = nullptr;
    EnhanceService* enhance = nullptr;
    Library* library = nullptr;

    qint64 currentId = -1;      // meeting being recorded (-1 when idle)
    qint64 enhanceId = -1;      // meeting an enhance job belongs to (-1 when none)
    qint64 titleId = -1;        // meeting awaiting an AI title (-1 when none)
    bool recording = false;
    QDateTime startedAt;
    double lastElapsed = 0.0;
    QString enhanceBuffer;
    QTimer* tick = nullptr;
};

QHash<const MeetingController*, Priv*>& registry() {
    static QHash<const MeetingController*, Priv*> map;
    return map;
}

Priv* priv(const MeetingController* self) {
    return registry().value(self);
}

} // namespace

MeetingController::MeetingController(AudioEngine* audio, TranscribeEngine* transcribe,
                                     EnhanceService* enhance, Library* library,
                                     QObject* parent)
    : QObject(parent) {
    auto* d = new Priv;
    d->audio = audio;
    d->transcribe = transcribe;
    d->enhance = enhance;
    d->library = library;
    registry().insert(this, d);

    d->tick = new QTimer(this);
    d->tick->setInterval(1000);
    connect(d->tick, &QTimer::timeout, this, &MeetingController::elapsedChanged);

    connect(this, &QObject::destroyed, [](QObject* obj) {
        delete registry().take(static_cast<MeetingController*>(obj));
    });

    // --- audio -> transcriber -------------------------------------------------
    if (audio && transcribe) {
        connect(audio, &AudioEngine::pcmReady, transcribe, &TranscribeEngine::feed);
    }

    // --- transcriber -> UI + store -------------------------------------------
    if (transcribe) {
        connect(transcribe, &TranscribeEngine::segmentReady, this,
                [this](gromarch::TranscriptSegment segment) {
                    Priv* d = priv(this);
                    if (!d) return;
                    if (segment.meetingId < 0) segment.meetingId = d->currentId;
                    emit liveSegment(segment);
                    if (segment.final && d->library && segment.meetingId >= 0 &&
                        !segment.text.trimmed().isEmpty()) {
                        d->library->appendSegment(segment);
                    }
                });
        connect(transcribe, &TranscribeEngine::error, this, &MeetingController::error);
    }

    if (audio) {
        connect(audio, &AudioEngine::error, this, &MeetingController::error);
    }

    // --- enhance --------------------------------------------------------------
    if (enhance) {
        connect(enhance, &EnhanceService::enhanceDelta, this, [this](const QString& delta) {
            Priv* d = priv(this);
            if (!d || d->enhanceId < 0) return;
            d->enhanceBuffer += delta;
            emit enhanceDelta(d->enhanceId, delta);
        });
        connect(enhance, &EnhanceService::enhanceFinished, this, [this](const QString& full) {
            Priv* d = priv(this);
            if (!d || d->enhanceId < 0) return;
            const qint64 id = d->enhanceId;
            d->enhanceId = -1;
            const QString text = full.isEmpty() ? d->enhanceBuffer : full;
            d->enhanceBuffer.clear();
            if (d->library) {
                Meeting m = d->library->meeting(id);
                if (m.id >= 0) {
                    m.enhancedMd = text;
                    m.state = QStringLiteral("enhanced");
                    d->library->updateMeeting(m);
                    d->library->writeNoteFile(m);
                }
            }
            emit enhanceFinished(id);
        });
        connect(enhance, &EnhanceService::titleReady, this, [this](const QString& title) {
            Priv* d = priv(this);
            if (!d || d->titleId < 0) return;
            const qint64 id = d->titleId;
            d->titleId = -1;
            const QString clean = title.trimmed();
            if (clean.isEmpty() || !d->library) return;
            const Meeting m = d->library->meeting(id);
            if (m.id >= 0 && m.title == QLatin1String(kUntitled)) setTitle(id, clean);
        });
        connect(enhance, &EnhanceService::error, this, &MeetingController::error);
    }
}

bool MeetingController::isRecording() const {
    Priv* d = priv(this);
    return d && d->recording;
}

qint64 MeetingController::currentMeetingId() const {
    Priv* d = priv(this);
    return d ? d->currentId : -1;
}

double MeetingController::elapsedSeconds() const {
    Priv* d = priv(this);
    if (!d) return 0.0;
    if (!d->recording || !d->startedAt.isValid()) return d->lastElapsed;
    return d->startedAt.msecsTo(QDateTime::currentDateTime()) / 1000.0;
}

qint64 MeetingController::startMeeting(const QString& title) {
    Priv* d = priv(this);
    if (!d || !d->library || d->recording) return d ? d->currentId : -1;

    Meeting m;
    m.title = title.trimmed().isEmpty() ? QLatin1String(kUntitled) : title.trimmed();
    m.startedAt = QDateTime::currentDateTime();
    m.state = QStringLiteral("recording");
    m.templateId = QStringLiteral("general");

    const qint64 id = d->library->createMeeting(m);
    if (id < 0) {
        emit error(tr("Could not create the meeting."));
        return -1;
    }
    m.id = id;

    d->currentId = id;
    d->recording = true;
    d->startedAt = m.startedAt;
    d->lastElapsed = 0.0;

    if (d->transcribe) d->transcribe->begin();

    const QString dir = d->library->meetingDir(m);
    if (!dir.isEmpty()) {
        m.audioPath = dir + QStringLiteral("/audio.ogg");
        d->library->updateMeeting(m);
    }
    if (d->audio) d->audio->start(m.audioPath);

    d->tick->start();
    emit stateChanged();
    emit elapsedChanged();
    return id;
}

void MeetingController::stopMeeting() {
    Priv* d = priv(this);
    if (!d || !d->recording) return;

    const qint64 id = d->currentId;
    d->lastElapsed = elapsedSeconds();
    d->recording = false;
    d->tick->stop();

    if (d->audio) d->audio->stop();
    if (d->transcribe) d->transcribe->finish();

    if (d->library && id >= 0) {
        Meeting m = d->library->meeting(id);
        if (m.id >= 0) {
            m.endedAt = QDateTime::currentDateTime();
            m.state = QStringLiteral("done");
            d->library->updateMeeting(m);

            // Ask for a title only if the user never gave one.
            if (d->enhance && d->enhance->isConfigured() &&
                m.title == QLatin1String(kUntitled)) {
                const QString transcript = d->library->transcriptText(id);
                if (!transcript.trimmed().isEmpty()) {
                    d->titleId = id;
                    d->enhance->suggestTitle(transcript);
                }
            }
        }
    }

    emit stateChanged();
    emit elapsedChanged();
}

void MeetingController::saveCues(qint64 meetingId, const QString& cuesMd) {
    Priv* d = priv(this);
    if (!d || !d->library || meetingId < 0) return;
    Meeting m = d->library->meeting(meetingId);
    if (m.id < 0 || m.notesMd == cuesMd) return;
    m.notesMd = cuesMd;
    d->library->updateMeeting(m);
}

void MeetingController::enhance(qint64 meetingId, const QString& templateId) {
    Priv* d = priv(this);
    if (!d || !d->library || !d->enhance || meetingId < 0) return;
    if (!d->enhance->isConfigured()) {
        emit error(tr("No LLM endpoint configured — set one in Settings."));
        return;
    }
    Meeting m = d->library->meeting(meetingId);
    if (m.id < 0) return;

    const QString tpl = templateId.isEmpty() ? QStringLiteral("general") : templateId;
    if (m.templateId != tpl) {
        m.templateId = tpl;
        d->library->updateMeeting(m);
    }

    d->enhanceId = meetingId;
    d->enhanceBuffer.clear();
    d->enhance->enhance(m.notesMd, d->library->transcriptText(meetingId), tpl);
}

void MeetingController::setTitle(qint64 meetingId, const QString& title) {
    Priv* d = priv(this);
    if (!d || !d->library || meetingId < 0) return;
    const QString clean = title.trimmed().isEmpty() ? QLatin1String(kUntitled) : title.trimmed();
    Meeting m = d->library->meeting(meetingId);
    if (m.id < 0 || m.title == clean) return;
    m.title = clean;
    d->library->updateMeeting(m);
}

} // namespace gromarch
