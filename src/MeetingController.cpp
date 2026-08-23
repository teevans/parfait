#include "MeetingController.h"

#include <QDateTime>
#include <QDir>
#include <QTimer>

#include "audio/AudioEngine.h"
#include "llm/EnhanceService.h"
#include "store/Library.h"
#include "transcribe/TranscribeEngine.h"

namespace parfait {
namespace {

const char* kUntitled = "Untitled meeting";

} // namespace

struct MeetingController::Impl {
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

MeetingController::MeetingController(AudioEngine* audio, TranscribeEngine* transcribe,
                                     EnhanceService* enhance, Library* library,
                                     QObject* parent)
    : QObject(parent), d(std::make_unique<Impl>()) {
    d->audio = audio;
    d->transcribe = transcribe;
    d->enhance = enhance;
    d->library = library;

    d->tick = new QTimer(this);
    d->tick->setInterval(1000);
    connect(d->tick, &QTimer::timeout, this, &MeetingController::elapsedChanged);

    // --- audio -> transcriber -------------------------------------------------
    if (audio && transcribe) {
        connect(audio, &AudioEngine::pcmReady, transcribe, &TranscribeEngine::feed);
    }

    // --- transcriber -> UI + store -------------------------------------------
    if (transcribe) {
        connect(transcribe, &TranscribeEngine::segmentReady, this,
                [this](parfait::TranscriptSegment segment) {
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
            if (d->enhanceId < 0) return;
            d->enhanceBuffer += delta;
            emit enhanceDelta(d->enhanceId, delta);
        });
        connect(enhance, &EnhanceService::enhanceFinished, this, [this](const QString& full) {
            if (d->enhanceId < 0) return;
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
            if (d->titleId < 0) return;
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

MeetingController::~MeetingController() = default;

bool MeetingController::isRecording() const {
    return d->recording;
}

qint64 MeetingController::currentMeetingId() const {
    return d->currentId;
}

double MeetingController::elapsedSeconds() const {
    if (!d->recording || !d->startedAt.isValid()) return d->lastElapsed;
    return d->startedAt.msecsTo(QDateTime::currentDateTime()) / 1000.0;
}

qint64 MeetingController::startMeeting(const QString& title) {
    if (!d->library || d->recording) return d->currentId;

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
    if (!d->recording) return;

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
    if (!d->library || meetingId < 0) return;
    Meeting m = d->library->meeting(meetingId);
    if (m.id < 0 || m.notesMd == cuesMd) return;
    m.notesMd = cuesMd;
    d->library->updateMeeting(m);
}

void MeetingController::enhance(qint64 meetingId, const QString& templateId) {
    if (!d->library || !d->enhance || meetingId < 0) return;
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
    if (!d->library || meetingId < 0) return;
    const QString clean = title.trimmed().isEmpty() ? QLatin1String(kUntitled) : title.trimmed();
    Meeting m = d->library->meeting(meetingId);
    if (m.id < 0 || m.title == clean) return;
    m.title = clean;
    d->library->updateMeeting(m);
}

} // namespace parfait
