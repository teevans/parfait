#include "calendar/CalendarService.h"

#include <QByteArray>
#include <QDateTime>
#include <QHash>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSet>
#include <QSettings>
#include <QTimeZone>
#include <QTimer>
#include <QUrl>
#include <QXmlStreamReader>
#include <QtGlobal>

#include <algorithm>

// Everything here runs on the object's own thread (the main thread in practice):
// QNetworkAccessManager, the QTimers and the cached event list are not guarded.

namespace gromarch {
namespace {

constexpr int kRefreshMs = 5 * 60 * 1000;
constexpr int kImminentCheckMs = 30 * 1000;
constexpr int kWindowDays = 7;

// CalendarService.h is a frozen interface with no private data; per-instance state
// lives in this side table keyed by the object pointer.
struct CalState {
    QNetworkAccessManager* nam = nullptr;
    QList<CalendarEvent> events;
    QList<CalendarEvent> incoming;   // accumulated across the replies of one refresh
    QSet<QString> firedUids;
    int pending = 0;
};

QHash<const CalendarService*, CalState*> g_state;

CalState* state(const CalendarService* self) { return g_state.value(self, nullptr); }

QSettings makeSettings() { return QSettings("gromarch", "gromarch"); }

QString setting(const char* key) {
    QSettings s = makeSettings();
    return s.value(QString::fromLatin1(key)).toString().trimmed();
}

// --- ICS parsing -----------------------------------------------------------

QString unfold(const QString& raw) {
    QString s = raw;
    s.replace("\r\n", "\n");
    s.replace('\r', '\n');
    s.replace("\n ", "");    // RFC 5545 line folding
    s.replace("\n\t", "");
    return s;
}

QString unescapeText(const QString& v) {
    QString out;
    out.reserve(v.size());
    for (int i = 0; i < v.size(); ++i) {
        if (v[i] == '\\' && i + 1 < v.size()) {
            const QChar n = v[++i];
            if (n == 'n' || n == 'N') out.append('\n');
            else out.append(n);
        } else {
            out.append(v[i]);
        }
    }
    return out;
}

// Split "NAME;P1=a;P2="b;c"" into name + params, honouring quoted param values.
void splitProperty(const QString& line, QString* name, QHash<QString, QString>* params,
                   QString* value) {
    bool quoted = false;
    int colon = -1;
    for (int i = 0; i < line.size(); ++i) {
        const QChar c = line[i];
        if (c == '"') quoted = !quoted;
        else if (c == ':' && !quoted) { colon = i; break; }
    }
    if (colon < 0) {
        *name = line.toUpper();
        *value = QString();
        return;
    }
    *value = line.mid(colon + 1);
    const QString head = line.left(colon);

    QStringList parts;
    QString cur;
    quoted = false;
    for (const QChar c : head) {
        if (c == '"') { quoted = !quoted; continue; }
        if (c == ';' && !quoted) { parts << cur; cur.clear(); continue; }
        cur.append(c);
    }
    parts << cur;

    *name = parts.takeFirst().toUpper();
    for (const QString& p : std::as_const(parts)) {
        const int eq = p.indexOf('=');
        if (eq > 0) params->insert(p.left(eq).toUpper(), p.mid(eq + 1));
    }
}

QDateTime parseIcsDateTime(const QString& value, const QHash<QString, QString>& params) {
    const QString v = value.trimmed();
    if (v.isEmpty()) return QDateTime();

    // All-day: VALUE=DATE or a bare yyyyMMdd.
    if (params.value("VALUE").compare("DATE", Qt::CaseInsensitive) == 0 || v.size() == 8) {
        const QDate d = QDate::fromString(v.left(8), "yyyyMMdd");
        return d.isValid() ? QDateTime(d, QTime(0, 0)) : QDateTime();
    }
    if (v.endsWith('Z')) {
        QDateTime dt = QDateTime::fromString(v.left(v.size() - 1), "yyyyMMddTHHmmss");
        if (dt.isValid()) dt.setTimeZone(QTimeZone::utc());
        return dt;
    }
    QDateTime dt = QDateTime::fromString(v, "yyyyMMddTHHmmss");
    if (!dt.isValid()) return dt;
    const QString tzid = params.value("TZID");
    if (!tzid.isEmpty()) {
        const QTimeZone tz(tzid.toUtf8());
        if (tz.isValid()) dt.setTimeZone(tz);
    }
    return dt;   // floating time -> local
}

QString detectMeetingUrl(const QString& haystack) {
    static const QRegularExpression re(
        R"((https?://[^\s<>"'\]\)]*(?:zoom\.us|meet\.google\.com|teams\.microsoft\.com|whereby\.com|around\.co|meet\.jit\.si)[^\s<>"'\]\)]*))",
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch m = re.match(haystack);
    return m.hasMatch() ? m.captured(1) : QString();
}

// Minimal VEVENT reader. RRULE is not expanded in v1: a recurring event is kept only
// if its DTSTART itself falls inside the window, so later occurrences are missed.
QList<CalendarEvent> parseIcs(const QString& raw, const QDateTime& from, const QDateTime& to) {
    QList<CalendarEvent> out;
    const QStringList lines = unfold(raw).split('\n');

    bool inEvent = false;
    CalendarEvent ev;
    QString location, description, url;

    for (const QString& line : lines) {
        const QString t = line.trimmed();
        if (t.isEmpty()) continue;
        if (t.compare("BEGIN:VEVENT", Qt::CaseInsensitive) == 0) {
            inEvent = true;
            ev = CalendarEvent();
            location.clear();
            description.clear();
            url.clear();
            continue;
        }
        if (t.compare("END:VEVENT", Qt::CaseInsensitive) == 0) {
            if (!inEvent) continue;
            inEvent = false;
            if (!ev.start.isValid()) continue;
            if (!ev.end.isValid()) ev.end = ev.start.addSecs(30 * 60);
            // Recurring events are kept only when DTSTART itself lands in the window.
            if (ev.start < from || ev.start > to) continue;
            if (ev.uid.isEmpty())
                ev.uid = ev.title + "@" + ev.start.toString(Qt::ISODate);
            ev.meetingUrl = detectMeetingUrl(location + "\n" + description + "\n" + url);
            out.append(ev);
            continue;
        }
        if (!inEvent) continue;

        QString name, value;
        QHash<QString, QString> params;
        splitProperty(t, &name, &params, &value);

        if (name == "UID") ev.uid = unescapeText(value);
        else if (name == "SUMMARY") ev.title = unescapeText(value);
        else if (name == "DTSTART") ev.start = parseIcsDateTime(value, params);
        else if (name == "DTEND") ev.end = parseIcsDateTime(value, params);
        else if (name == "LOCATION") location = unescapeText(value);
        else if (name == "DESCRIPTION") description = unescapeText(value);
        else if (name == "URL") url = unescapeText(value);
        else if (name == "ATTENDEE") {
            QString who = params.value("CN");
            if (who.isEmpty()) {
                who = value.trimmed();
                if (who.startsWith("mailto:", Qt::CaseInsensitive)) who = who.mid(7);
            }
            who = who.trimmed();
            if (!who.isEmpty() && !ev.attendees.contains(who)) ev.attendees << who;
        }
    }
    return out;
}

// Pull every <C:calendar-data> payload out of a CalDAV multistatus response.
QStringList calendarDataFromMultistatus(const QByteArray& xml) {
    QStringList out;
    QXmlStreamReader r(xml);
    while (!r.atEnd()) {
        if (r.readNext() == QXmlStreamReader::StartElement &&
            r.name().compare(QLatin1String("calendar-data"), Qt::CaseInsensitive) == 0) {
            const QString text = r.readElementText(QXmlStreamReader::IncludeChildElements);
            if (!text.trimmed().isEmpty()) out << text;
        }
    }
    return out;
}

bool isMeetingEvent(const CalendarEvent& e) {
    return !e.meetingUrl.isEmpty() || e.attendees.size() >= 2;
}

} // namespace

CalendarService::CalendarService(QObject* parent) : QObject(parent) {
    auto* s = new CalState;
    s->nam = new QNetworkAccessManager(this);
    g_state.insert(this, s);

    auto* poll = new QTimer(this);
    poll->setInterval(kRefreshMs);
    connect(poll, &QTimer::timeout, this, &CalendarService::refresh);
    poll->start();

    auto* imminent = new QTimer(this);
    imminent->setInterval(kImminentCheckMs);
    connect(imminent, &QTimer::timeout, this, [this] {
        CalState* st = state(this);
        if (!st) return;
        const QDateTime now = QDateTime::currentDateTime();
        for (const CalendarEvent& e : std::as_const(st->events)) {
            if (!e.start.isValid() || st->firedUids.contains(e.uid)) continue;
            if (!isMeetingEvent(e)) continue;
            if (now >= e.start.addSecs(-120) && now <= e.start.addSecs(60)) {
                st->firedUids.insert(e.uid);
                emit meetingImminent(e);
            }
        }
    });
    imminent->start();

    if (isConfigured()) QTimer::singleShot(0, this, &CalendarService::refresh);
}

CalendarService::~CalendarService() {
    delete g_state.take(this);
}

bool CalendarService::isConfigured() const {
    return !setting("calendar/icsUrl").isEmpty() || !setting("calendar/caldavUrl").isEmpty();
}

QList<CalendarEvent> CalendarService::todaysEvents() const {
    QList<CalendarEvent> out;
    CalState* s = state(this);
    if (!s) return out;
    const QDate today = QDate::currentDate();
    for (const CalendarEvent& e : std::as_const(s->events))
        if (e.start.isValid() && e.start.toLocalTime().date() == today) out.append(e);
    std::sort(out.begin(), out.end(),
              [](const CalendarEvent& a, const CalendarEvent& b) { return a.start < b.start; });
    return out;
}

void CalendarService::refresh() {
    CalState* s = state(this);
    if (!s) return;
    if (s->pending > 0) return;   // a refresh is already in flight

    const QString icsUrl = setting("calendar/icsUrl");
    const QString caldavUrl = setting("calendar/caldavUrl");
    if (icsUrl.isEmpty() && caldavUrl.isEmpty()) return;

    const QDateTime from = QDateTime(QDate::currentDate(), QTime(0, 0));
    const QDateTime to = QDateTime::currentDateTime().addDays(kWindowDays);
    s->incoming.clear();

    // Called when a reply is done; publishes once every request of this refresh landed.
    auto finish = [this, s] {
        if (--s->pending > 0) return;
        s->events = s->incoming;
        s->incoming.clear();
        std::sort(s->events.begin(), s->events.end(),
                  [](const CalendarEvent& a, const CalendarEvent& b) { return a.start < b.start; });
        emit eventsChanged();
    };

    if (!icsUrl.isEmpty()) {
        QUrl u(icsUrl);
        if (u.scheme().compare("webcal", Qt::CaseInsensitive) == 0) u.setScheme("https");
        QNetworkRequest req(u);
        req.setRawHeader("Accept", "text/calendar");
        req.setHeader(QNetworkRequest::UserAgentHeader, "gromarch/0.1");
        ++s->pending;
        QNetworkReply* reply = s->nam->get(req);
        connect(reply, &QNetworkReply::finished, this, [this, s, reply, from, to, finish] {
            reply->deleteLater();
            if (reply->error() != QNetworkReply::NoError) {
                const QString msg = QString("Calendar (ICS) fetch failed: %1").arg(reply->errorString());
                qWarning("gromarch: %s", qPrintable(msg));
                emit error(msg);
            } else {
                s->incoming += parseIcs(QString::fromUtf8(reply->readAll()), from, to);
            }
            finish();
        });
    }

    if (!caldavUrl.isEmpty()) {
        // CalDAV calendar-query REPORT (Depth: 1) for VEVENTs in the window.
        const QString body =
            QStringLiteral("<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                           "<C:calendar-query xmlns:D=\"DAV:\" xmlns:C=\"urn:ietf:params:xml:ns:caldav\">"
                           "<D:prop><D:getetag/><C:calendar-data/></D:prop>"
                           "<C:filter><C:comp-filter name=\"VCALENDAR\">"
                           "<C:comp-filter name=\"VEVENT\">"
                           "<C:time-range start=\"%1\" end=\"%2\"/>"
                           "</C:comp-filter></C:comp-filter></C:filter>"
                           "</C:calendar-query>")
                .arg(from.toUTC().toString("yyyyMMdd'T'HHmmss'Z'"),
                     to.toUTC().toString("yyyyMMdd'T'HHmmss'Z'"));

        QNetworkRequest req{QUrl(caldavUrl)};
        req.setHeader(QNetworkRequest::ContentTypeHeader, "application/xml; charset=utf-8");
        req.setHeader(QNetworkRequest::UserAgentHeader, "gromarch/0.1");
        req.setRawHeader("Depth", "1");
        const QString user = setting("calendar/username");
        const QString pass = setting("calendar/password");
        if (!user.isEmpty()) {
            const QByteArray token = (user + ":" + pass).toUtf8().toBase64();
            req.setRawHeader("Authorization", "Basic " + token);
        }
        ++s->pending;
        QNetworkReply* reply = s->nam->sendCustomRequest(req, "REPORT", body.toUtf8());
        connect(reply, &QNetworkReply::finished, this, [this, s, reply, from, to, finish] {
            reply->deleteLater();
            if (reply->error() != QNetworkReply::NoError) {
                const QString msg = QString("Calendar (CalDAV) request failed: %1").arg(reply->errorString());
                qWarning("gromarch: %s", qPrintable(msg));
                emit error(msg);
            } else {
                const QStringList blobs = calendarDataFromMultistatus(reply->readAll());
                for (const QString& blob : blobs) s->incoming += parseIcs(blob, from, to);
            }
            finish();
        });
    }
}

} // namespace gromarch
