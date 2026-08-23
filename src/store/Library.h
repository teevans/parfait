#pragma once
#include <QList>
#include <QObject>
#include "Types.h"

namespace gromarch {

// SQLite-backed meeting store with FTS5 search over segments + notes.
// DB: ~/.local/share/gromarch/gromarch.db (WAL). Note files are also written
// to ~/Meetings/<yyyy-mm-dd> <title>/note.md on save — files are the source of
// truth for notes, the DB is the index.
class Library : public QObject {
    Q_OBJECT
public:
    explicit Library(QObject* parent = nullptr);
    ~Library() override;

    bool open();                                   // creates schema on first run
    qint64 createMeeting(const Meeting& m);        // returns new id
    void updateMeeting(const Meeting& m);
    Meeting meeting(qint64 id) const;
    QList<Meeting> allMeetings() const;            // newest first
    void appendSegment(const TranscriptSegment& s);        // final segments only
    QList<TranscriptSegment> segments(qint64 meetingId) const;
    QString transcriptText(qint64 meetingId) const;        // "Me:/Them:" lines
    QList<Meeting> search(const QString& query) const;     // FTS5 across everything
    // Directory for a meeting's files (created on demand): audio.ogg, note.md
    QString meetingDir(const Meeting& m) const;
    void writeNoteFile(const Meeting& m);          // enhancedMd (or notesMd) -> note.md

signals:
    void meetingsChanged();
    void error(const QString& message);
};

} // namespace gromarch
