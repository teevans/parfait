#pragma once
#include <QDateTime>
#include <QObject>
#include <QString>
#include <QStringList>

namespace parfait {

// Which capture stream a piece of audio/text came from.
enum class Stream { Mic = 0, System = 1 };

struct TranscriptSegment {
    Q_GADGET
public:
    qint64 meetingId = -1;
    int stream = 0;          // 0 = Mic ("Me"), 1 = System ("Them")
    double t0 = 0.0;         // seconds from meeting start
    double t1 = 0.0;
    QString text;
    bool final = false;      // partials update in place until final
    // Speaker turn index within the System stream (tinydiarize): 0,1,2,... as
    // turns change; -1 = unknown/not diarized. Mic stream is always -1 ("Me").
    // Turn indices are NOT stable identities — index 2 may be the same person
    // as index 0; they only mark that the voice changed.
    int speaker = -1;
    Q_PROPERTY(int stream MEMBER stream)
    Q_PROPERTY(int speaker MEMBER speaker)
    Q_PROPERTY(double t0 MEMBER t0)
    Q_PROPERTY(double t1 MEMBER t1)
    Q_PROPERTY(QString text MEMBER text)
    Q_PROPERTY(bool final MEMBER final)
};

struct Meeting {
    Q_GADGET
public:
    qint64 id = -1;
    QString title;
    QDateTime startedAt;
    QDateTime endedAt;
    QString calendarUid;
    QStringList attendees;
    QString audioPath;       // ogg opus, stereo: L=mic R=system; empty if not retained
    QString notesMd;         // raw cues typed during the meeting
    QString enhancedMd;      // LLM-enhanced note
    QString templateId;      // e.g. "general", "one-on-one", "standup"
    QString state;           // "recording" | "done" | "enhanced"
    Q_PROPERTY(qint64 id MEMBER id)
    Q_PROPERTY(QString title MEMBER title)
    Q_PROPERTY(QDateTime startedAt MEMBER startedAt)
    Q_PROPERTY(QDateTime endedAt MEMBER endedAt)
    Q_PROPERTY(QStringList attendees MEMBER attendees)
    Q_PROPERTY(QString audioPath MEMBER audioPath)
    Q_PROPERTY(QString notesMd MEMBER notesMd)
    Q_PROPERTY(QString enhancedMd MEMBER enhancedMd)
    Q_PROPERTY(QString templateId MEMBER templateId)
    Q_PROPERTY(QString state MEMBER state)
};

struct CalendarEvent {
    Q_GADGET
public:
    QString uid;
    QString title;
    QDateTime start;
    QDateTime end;
    QStringList attendees;
    QString meetingUrl;      // zoom/meet/teams/... link if detected
    Q_PROPERTY(QString uid MEMBER uid)
    Q_PROPERTY(QString title MEMBER title)
    Q_PROPERTY(QDateTime start MEMBER start)
    Q_PROPERTY(QDateTime end MEMBER end)
    Q_PROPERTY(QStringList attendees MEMBER attendees)
    Q_PROPERTY(QString meetingUrl MEMBER meetingUrl)
};

} // namespace parfait

Q_DECLARE_METATYPE(parfait::TranscriptSegment)
Q_DECLARE_METATYPE(parfait::Meeting)
Q_DECLARE_METATYPE(parfait::CalendarEvent)
