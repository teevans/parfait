#pragma once
#include <QObject>
#include <QString>
#include "Types.h"

namespace gromarch {

class AudioEngine;
class TranscribeEngine;
class EnhanceService;
class Library;

// Orchestrates one meeting lifecycle: start/stop recording, route PCM to the
// transcriber, persist final segments, run enhance. Exposed to QML as
// `Controller`. Also owns the list/selection state the UI binds to.
class MeetingController : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool recording READ isRecording NOTIFY stateChanged)
    Q_PROPERTY(qint64 currentMeetingId READ currentMeetingId NOTIFY stateChanged)
    Q_PROPERTY(double elapsedSeconds READ elapsedSeconds NOTIFY elapsedChanged)
public:
    MeetingController(AudioEngine* audio, TranscribeEngine* transcribe,
                      EnhanceService* enhance, Library* library,
                      QObject* parent = nullptr);

    bool isRecording() const;
    qint64 currentMeetingId() const;
    double elapsedSeconds() const;

public slots:
    qint64 startMeeting(const QString& title);   // returns meeting id
    void stopMeeting();
    void saveCues(qint64 meetingId, const QString& cuesMd);   // autosave from notepad
    void enhance(qint64 meetingId, const QString& templateId);
    void setTitle(qint64 meetingId, const QString& title);

signals:
    void stateChanged();
    void elapsedChanged();
    // Forwarded live segments (partials included) for the transcript view.
    void liveSegment(gromarch::TranscriptSegment segment);
    void enhanceDelta(qint64 meetingId, const QString& textDelta);
    void enhanceFinished(qint64 meetingId);
    void error(const QString& message);
};

} // namespace gromarch
