#include "store/Library.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QStandardPaths>
#include <QStringList>
#include <QVariant>
#include <QtGlobal>

// All Library methods must be called from the main (GUI) thread: the QSqlDatabase
// connection is bound to the thread that created it and the meetingsChanged()/error()
// signals are consumed directly by QML. Worker threads should queue calls instead
// (e.g. QMetaObject::invokeMethod on the Library's thread).

namespace parfait {
namespace {

// Bumped whenever the on-disk schema changes; see the migration ladder in open().
// v1: initial schema.  v2: segments.speaker (diarization turn index, -1 = unknown).
constexpr int kSchemaVersion = 2;

enum class FtsMode {
    None,           // no FTS5 in this SQLite build -> LIKE fallback
    Contentless,    // fts5(..., content='', contentless_delete=1)  [SQLite >= 3.43]
    Standard        // plain fts5(...) storing its own copy of the text
};

QString isoOrEmpty(const QDateTime& dt) {
    return dt.isValid() ? dt.toString(Qt::ISODate) : QString();
}

QDateTime fromIso(const QString& s) {
    return s.isEmpty() ? QDateTime() : QDateTime::fromString(s, Qt::ISODate);
}

QString attendeesToJson(const QStringList& list) {
    QJsonArray arr;
    for (const QString& a : list) arr.append(a);
    return QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact));
}

QStringList attendeesFromJson(const QString& json) {
    QStringList out;
    const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8());
    if (doc.isArray()) {
        const QJsonArray arr = doc.array();
        for (const QJsonValue& v : arr)
            if (v.isString()) out << v.toString();
    }
    return out;
}

Meeting meetingFromRecord(const QSqlRecord& r) {
    Meeting m;
    m.id = r.value("id").toLongLong();
    m.title = r.value("title").toString();
    m.startedAt = fromIso(r.value("started_at").toString());
    m.endedAt = fromIso(r.value("ended_at").toString());
    m.calendarUid = r.value("calendar_uid").toString();
    m.attendees = attendeesFromJson(r.value("attendees_json").toString());
    m.audioPath = r.value("audio_path").toString();
    m.notesMd = r.value("notes_md").toString();
    m.enhancedMd = r.value("enhanced_md").toString();
    m.templateId = r.value("template_id").toString();
    m.state = r.value("state").toString();
    return m;
}

// Turn user input into a safe FTS5 MATCH expression: every whitespace-separated
// term becomes a quoted phrase, so operators/punctuation can never blow up.
QString ftsEscape(const QString& query) {
    QStringList terms;
    const QStringList raw = query.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
    for (QString t : raw) {
        t.replace('"', "\"\"");
        terms << QString("\"%1\"").arg(t);
    }
    return terms.join(' ');
}

QString sanitizeTitle(const QString& title) {
    QString s;
    for (const QChar c : title) {
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' ||
            c == '<' || c == '>' || c == '|' || c.unicode() < 0x20)
            s.append('-');
        else
            s.append(c);
    }
    s = s.simplified().trimmed();
    while (s.endsWith('.')) s.chop(1);
    if (s.size() > 80) s = s.left(80).trimmed();
    if (s.isEmpty()) s = QStringLiteral("Untitled");
    return s;
}

} // namespace

struct Library::Impl {
    QString connName;
    FtsMode fts = FtsMode::None;
    QSet<qint64> dirty;      // meetings whose transcript index is stale
    bool opened = false;

    void reindexMeeting(qint64 id);
    void flushDirty();
};

Library::Library(QObject* parent) : QObject(parent), d(std::make_unique<Impl>()) {
    d->connName = QString("parfait_library_%1").arg(reinterpret_cast<quintptr>(this));
}

Library::~Library() {
    Impl* s = d.get();
    if (QSqlDatabase::contains(s->connName)) {
        {
            QSqlDatabase db = QSqlDatabase::database(s->connName, false);
            if (db.isOpen()) db.close();
        }
        QSqlDatabase::removeDatabase(s->connName);
    }
}

bool Library::open() {
    Impl* s = d.get();
    if (s->opened) return true;

    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (dir.isEmpty()) dir = QDir::homePath() + "/.local/share/parfait";
    if (!QDir().mkpath(dir)) {
        const QString msg = QString("Cannot create data directory %1").arg(dir);
        qWarning("parfait: %s", qPrintable(msg));
        emit error(msg);
        return false;
    }

    QSqlDatabase db = QSqlDatabase::contains(s->connName)
                          ? QSqlDatabase::database(s->connName, false)
                          : QSqlDatabase::addDatabase("QSQLITE", s->connName);
    if (!db.isValid()) {
        const QString msg = QStringLiteral("SQLite driver (QSQLITE) is not available");
        qWarning("parfait: %s", qPrintable(msg));
        emit error(msg);
        return false;
    }
    db.setDatabaseName(dir + "/parfait.db");
    if (!db.open()) {
        const QString msg = QString("Cannot open %1/parfait.db: %2").arg(dir, db.lastError().text());
        qWarning("parfait: %s", qPrintable(msg));
        emit error(msg);
        return false;
    }

    QSqlQuery q(db);
    q.exec("PRAGMA journal_mode=WAL");
    q.exec("PRAGMA foreign_keys=ON");
    q.exec("PRAGMA synchronous=NORMAL");

    const char* schema[] = {
        "CREATE TABLE IF NOT EXISTS schema_version (version INTEGER NOT NULL)",
        "CREATE TABLE IF NOT EXISTS meetings ("
        " id INTEGER PRIMARY KEY,"
        " title TEXT,"
        " started_at TEXT,"
        " ended_at TEXT,"
        " calendar_uid TEXT,"
        " attendees_json TEXT,"
        " audio_path TEXT,"
        " notes_md TEXT,"
        " enhanced_md TEXT,"
        " template_id TEXT,"
        " state TEXT)",
        "CREATE TABLE IF NOT EXISTS segments ("
        " id INTEGER PRIMARY KEY,"
        " meeting_id INTEGER REFERENCES meetings(id) ON DELETE CASCADE,"
        " stream INTEGER,"
        " t0 REAL,"
        " t1 REAL,"
        " text TEXT,"
        " speaker INTEGER DEFAULT -1)",
        "CREATE INDEX IF NOT EXISTS idx_segments_meeting ON segments(meeting_id, t0)",
        "CREATE INDEX IF NOT EXISTS idx_meetings_started ON meetings(started_at DESC)",
    };
    for (const char* sql : schema) {
        if (!q.exec(QString::fromLatin1(sql))) {
            const QString msg = QString("Schema creation failed: %1").arg(q.lastError().text());
            qWarning("parfait: %s", qPrintable(msg));
            emit error(msg);
            db.close();
            return false;
        }
    }
    // Version ladder. A fresh DB was just created at kSchemaVersion by the schema
    // above, so it only needs the version row; older DBs are migrated step by step.
    int version = 0;
    bool haveVersion = false;
    if (q.exec("SELECT version FROM schema_version") && q.next()) {
        version = q.value(0).toInt();
        haveVersion = true;
    }
    const int oldVersion = version;
    if (!haveVersion) {
        QSqlQuery ins(db);
        ins.prepare("INSERT INTO schema_version(version) VALUES(?)");
        ins.addBindValue(kSchemaVersion);
        ins.exec();
        version = kSchemaVersion;
    }
    if (version < 2) {
        // v1 -> v2: diarization turn index on segments; existing rows read back -1.
        QSqlQuery mig(db);
        if (!mig.exec("ALTER TABLE segments ADD COLUMN speaker INTEGER DEFAULT -1")) {
            const QString msg = QString("Schema migration to v2 failed: %1")
                                    .arg(mig.lastError().text());
            qWarning("parfait: %s", qPrintable(msg));
            emit error(msg);
            db.close();
            return false;
        }
        version = 2;
    }
    if (haveVersion && version != oldVersion) {
        QSqlQuery upd(db);
        upd.prepare("UPDATE schema_version SET version=?");
        upd.addBindValue(version);
        upd.exec();
    }

    // FTS5 is optional in some distro SQLite builds; degrade instead of failing.
    if (q.exec("CREATE VIRTUAL TABLE IF NOT EXISTS search_idx USING fts5("
               "title, notes, transcript, content='', contentless_delete=1)")) {
        s->fts = FtsMode::Contentless;
    } else if (q.exec("CREATE VIRTUAL TABLE IF NOT EXISTS search_idx USING fts5("
                      "title, notes, transcript)")) {
        s->fts = FtsMode::Standard;
    } else {
        s->fts = FtsMode::None;
        qWarning("parfait: FTS5 unavailable (%s) — falling back to LIKE search",
                 qPrintable(q.lastError().text()));
    }

    s->opened = true;
    return true;
}

// --- FTS bookkeeping -------------------------------------------------------
// Sync strategy: the search index holds one row per meeting, rowid == meetings.id.
// createMeeting/updateMeeting reindex that meeting immediately (delete row, re-insert
// title + notes_md/enhanced_md + the concatenated transcript). appendSegment only
// marks the meeting dirty — reindexing on every final segment would rebuild the whole
// transcript row per segment — and the deferred rows are flushed before any search()
// and on the next updateMeeting, so query results are never stale.

void Library::Impl::reindexMeeting(qint64 id) {
    Impl* s = this;
    if (s->fts == FtsMode::None || id < 0) return;
    QSqlDatabase db = QSqlDatabase::database(s->connName, false);
    if (!db.isOpen()) return;

    QSqlQuery sel(db);
    sel.prepare("SELECT title, notes_md, enhanced_md FROM meetings WHERE id=?");
    sel.addBindValue(id);
    if (!sel.exec() || !sel.next()) return;
    const QString title = sel.value(0).toString();
    const QString notes = sel.value(1).toString() + "\n" + sel.value(2).toString();

    QString transcript;
    QSqlQuery seg(db);
    seg.prepare("SELECT text FROM segments WHERE meeting_id=? ORDER BY t0 ASC, id ASC");
    seg.addBindValue(id);
    if (seg.exec()) {
        while (seg.next()) {
            transcript += seg.value(0).toString();
            transcript += '\n';
        }
    }

    QSqlQuery del(db);
    del.prepare("DELETE FROM search_idx WHERE rowid=?");
    del.addBindValue(id);
    if (!del.exec())
        qWarning("parfait: search index delete failed: %s", qPrintable(del.lastError().text()));

    QSqlQuery ins(db);
    ins.prepare("INSERT INTO search_idx(rowid, title, notes, transcript) VALUES(?,?,?,?)");
    ins.addBindValue(id);
    ins.addBindValue(title);
    ins.addBindValue(notes);
    ins.addBindValue(transcript);
    if (!ins.exec())
        qWarning("parfait: search index insert failed: %s", qPrintable(ins.lastError().text()));

    s->dirty.remove(id);
}

void Library::Impl::flushDirty() {
    Impl* s = this;
    if (s->dirty.isEmpty()) return;
    const QList<qint64> ids = s->dirty.values();
    for (qint64 id : ids) s->reindexMeeting(id);
    s->dirty.clear();
}

qint64 Library::createMeeting(const Meeting& m) {
    Impl* s = d.get();
    if (!s->opened) {
        emit error(QStringLiteral("Library is not open"));
        return -1;
    }
    QSqlDatabase db = QSqlDatabase::database(s->connName, false);
    QSqlQuery q(db);
    q.prepare("INSERT INTO meetings (title, started_at, ended_at, calendar_uid, attendees_json,"
              " audio_path, notes_md, enhanced_md, template_id, state)"
              " VALUES (?,?,?,?,?,?,?,?,?,?)");
    q.addBindValue(m.title);
    q.addBindValue(isoOrEmpty(m.startedAt.isValid() ? m.startedAt : QDateTime::currentDateTime()));
    q.addBindValue(isoOrEmpty(m.endedAt));
    q.addBindValue(m.calendarUid);
    q.addBindValue(attendeesToJson(m.attendees));
    q.addBindValue(m.audioPath);
    q.addBindValue(m.notesMd);
    q.addBindValue(m.enhancedMd);
    q.addBindValue(m.templateId);
    q.addBindValue(m.state.isEmpty() ? QStringLiteral("recording") : m.state);
    if (!q.exec()) {
        const QString msg = QString("createMeeting failed: %1").arg(q.lastError().text());
        qWarning("parfait: %s", qPrintable(msg));
        emit error(msg);
        return -1;
    }
    const qint64 id = q.lastInsertId().toLongLong();
    s->reindexMeeting(id);
    emit meetingsChanged();
    return id;
}

void Library::updateMeeting(const Meeting& m) {
    Impl* s = d.get();
    if (!s || !s->opened) {
        emit error(QStringLiteral("Library is not open"));
        return;
    }
    if (m.id < 0) {
        emit error(QStringLiteral("updateMeeting: invalid meeting id"));
        return;
    }
    QSqlDatabase db = QSqlDatabase::database(s->connName, false);
    QSqlQuery q(db);
    q.prepare("UPDATE meetings SET title=?, started_at=?, ended_at=?, calendar_uid=?,"
              " attendees_json=?, audio_path=?, notes_md=?, enhanced_md=?, template_id=?,"
              " state=? WHERE id=?");
    q.addBindValue(m.title);
    q.addBindValue(isoOrEmpty(m.startedAt));
    q.addBindValue(isoOrEmpty(m.endedAt));
    q.addBindValue(m.calendarUid);
    q.addBindValue(attendeesToJson(m.attendees));
    q.addBindValue(m.audioPath);
    q.addBindValue(m.notesMd);
    q.addBindValue(m.enhancedMd);
    q.addBindValue(m.templateId);
    q.addBindValue(m.state);
    q.addBindValue(m.id);
    if (!q.exec()) {
        const QString msg = QString("updateMeeting failed: %1").arg(q.lastError().text());
        qWarning("parfait: %s", qPrintable(msg));
        emit error(msg);
        return;
    }
    s->reindexMeeting(m.id);
    emit meetingsChanged();
}

Meeting Library::meeting(qint64 id) const {
    Meeting m;
    Impl* s = d.get();
    if (!s || !s->opened) return m;
    QSqlDatabase db = QSqlDatabase::database(s->connName, false);
    QSqlQuery q(db);
    q.prepare("SELECT * FROM meetings WHERE id=?");
    q.addBindValue(id);
    if (!q.exec()) {
        qWarning("parfait: meeting(%lld) failed: %s", static_cast<long long>(id),
                 qPrintable(q.lastError().text()));
        emit const_cast<Library*>(this)->error(q.lastError().text());
        return m;
    }
    if (q.next()) m = meetingFromRecord(q.record());
    return m;
}

QList<Meeting> Library::allMeetings() const {
    QList<Meeting> out;
    Impl* s = d.get();
    if (!s || !s->opened) return out;
    QSqlDatabase db = QSqlDatabase::database(s->connName, false);
    QSqlQuery q(db);
    if (!q.exec("SELECT * FROM meetings ORDER BY started_at DESC, id DESC")) {
        qWarning("parfait: allMeetings failed: %s", qPrintable(q.lastError().text()));
        emit const_cast<Library*>(this)->error(q.lastError().text());
        return out;
    }
    while (q.next()) out.append(meetingFromRecord(q.record()));
    return out;
}

void Library::appendSegment(const TranscriptSegment& seg) {
    Impl* s = d.get();
    if (!s || !s->opened) return;
    if (seg.meetingId < 0) {
        emit error(QStringLiteral("appendSegment: invalid meeting id"));
        return;
    }
    QSqlDatabase db = QSqlDatabase::database(s->connName, false);
    QSqlQuery q(db);
    q.prepare("INSERT INTO segments (meeting_id, stream, t0, t1, text, speaker)"
              " VALUES (?,?,?,?,?,?)");
    q.addBindValue(seg.meetingId);
    q.addBindValue(seg.stream);
    q.addBindValue(seg.t0);
    q.addBindValue(seg.t1);
    q.addBindValue(seg.text);
    q.addBindValue(seg.speaker);
    if (!q.exec()) {
        const QString msg = QString("appendSegment failed: %1").arg(q.lastError().text());
        qWarning("parfait: %s", qPrintable(msg));
        emit error(msg);
        return;
    }
    s->dirty.insert(seg.meetingId);   // transcript reindex deferred, see note above
}

QList<TranscriptSegment> Library::segments(qint64 meetingId) const {
    QList<TranscriptSegment> out;
    Impl* s = d.get();
    if (!s || !s->opened) return out;
    QSqlDatabase db = QSqlDatabase::database(s->connName, false);
    QSqlQuery q(db);
    q.prepare("SELECT meeting_id, stream, t0, t1, text, speaker FROM segments"
              " WHERE meeting_id=? ORDER BY t0 ASC, id ASC");
    q.addBindValue(meetingId);
    if (!q.exec()) {
        qWarning("parfait: segments failed: %s", qPrintable(q.lastError().text()));
        emit const_cast<Library*>(this)->error(q.lastError().text());
        return out;
    }
    while (q.next()) {
        TranscriptSegment seg;
        seg.meetingId = q.value(0).toLongLong();
        seg.stream = q.value(1).toInt();
        seg.t0 = q.value(2).toDouble();
        seg.t1 = q.value(3).toDouble();
        seg.text = q.value(4).toString();
        // NULL (rows written before v2) reads back as -1 = unknown speaker.
        seg.speaker = q.value(5).isNull() ? -1 : q.value(5).toInt();
        seg.final = true;
        out.append(seg);
    }
    return out;
}

QString Library::transcriptText(qint64 meetingId) const {
    QStringList lines;
    const QList<TranscriptSegment> segs = segments(meetingId);
    for (const TranscriptSegment& s : segs) {
        const QString text = s.text.trimmed();
        if (text.isEmpty()) continue;
        // "Me:" for the mic; "Them:" for the system stream, or "Them#<n>:" when the
        // diarizer gave the turn a speaker index — the enhance prompt uses this to
        // attribute lines to distinct voices. <n> is the 1-based display index, so
        // it matches the "Them 1"/"Them 2" labels shown in the transcript pane.
        QString who;
        if (s.stream == int(Stream::Mic))
            who = QStringLiteral("Me");
        else if (s.speaker >= 0)
            who = QString("Them#%1").arg(s.speaker + 1);
        else
            who = QStringLiteral("Them");
        lines << QString("%1: %2").arg(who, text);
    }
    return lines.join('\n');
}

QList<Meeting> Library::search(const QString& query) const {
    QList<Meeting> out;
    Impl* s = d.get();
    if (!s || !s->opened) return out;
    const QString trimmed = query.trimmed();
    if (trimmed.isEmpty()) return allMeetings();

    s->flushDirty();
    QSqlDatabase db = QSqlDatabase::database(s->connName, false);
    QSqlQuery q(db);

    if (s->fts != FtsMode::None) {
        q.prepare("SELECT m.* FROM search_idx s JOIN meetings m ON m.id = s.rowid"
                  " WHERE search_idx MATCH ? ORDER BY m.started_at DESC, m.id DESC");
        q.addBindValue(ftsEscape(trimmed));
        if (q.exec()) {
            while (q.next()) out.append(meetingFromRecord(q.record()));
            return out;
        }
        qWarning("parfait: FTS search failed (%s) — using LIKE fallback",
                 qPrintable(q.lastError().text()));
    }

    // LIKE fallback: title/notes/enhanced only.
    q.prepare("SELECT * FROM meetings WHERE title LIKE ? OR notes_md LIKE ?"
              " OR enhanced_md LIKE ? ORDER BY started_at DESC, id DESC");
    const QString like = "%" + trimmed + "%";
    q.addBindValue(like);
    q.addBindValue(like);
    q.addBindValue(like);
    if (!q.exec()) {
        qWarning("parfait: search failed: %s", qPrintable(q.lastError().text()));
        emit const_cast<Library*>(this)->error(q.lastError().text());
        return out;
    }
    while (q.next()) out.append(meetingFromRecord(q.record()));
    return out;
}

QString Library::meetingDir(const Meeting& m) const {
    const QDateTime start = m.startedAt.isValid() ? m.startedAt : QDateTime::currentDateTime();
    const QString name = QString("%1 %2").arg(start.toString("yyyy-MM-dd"), sanitizeTitle(m.title));
    const QString path = QDir::homePath() + "/Meetings/" + name;
    if (!QDir().mkpath(path)) {
        const QString msg = QString("Cannot create meeting directory %1").arg(path);
        qWarning("parfait: %s", qPrintable(msg));
        emit const_cast<Library*>(this)->error(msg);
    }
    return path;
}

void Library::writeNoteFile(const Meeting& m) {
    const QString content = m.enhancedMd.isEmpty() ? m.notesMd : m.enhancedMd;
    const QString path = meetingDir(m) + "/note.md";
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        const QString msg = QString("Cannot write %1: %2").arg(path, f.errorString());
        qWarning("parfait: %s", qPrintable(msg));
        emit error(msg);
        return;
    }
    f.write(content.toUtf8());
    if (!content.endsWith('\n')) f.write("\n");
    if (!f.commit()) {
        const QString msg = QString("Cannot commit %1: %2").arg(path, f.errorString());
        qWarning("parfait: %s", qPrintable(msg));
        emit error(msg);
    }
}

} // namespace parfait
