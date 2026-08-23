#include "MeetingListModel.h"

#include <QDateTime>
#include "store/Library.h"

namespace gromarch {
namespace {

QString dayGroup(const QDateTime& when) {
    if (!when.isValid()) return QStringLiteral("Earlier");
    const QDate today = QDate::currentDate();
    const QDate d = when.date();
    if (d == today) return QStringLiteral("Today");
    if (d == today.addDays(-1)) return QStringLiteral("Yesterday");
    if (d > today.addDays(-7)) return QStringLiteral("This week");
    return d.toString(QStringLiteral("MMMM yyyy"));
}

} // namespace

MeetingListModel::MeetingListModel(Library* library, QObject* parent)
    : QAbstractListModel(parent), m_library(library) {
    if (m_library) {
        connect(m_library, &Library::meetingsChanged, this, &MeetingListModel::refresh);
    }
    refresh();
}

int MeetingListModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : int(m_rows.size());
}

QVariant MeetingListModel::data(const QModelIndex& index, int role) const {
    if (index.row() < 0 || index.row() >= m_rows.size()) return QVariant();
    const Meeting& m = m_rows.at(index.row());
    switch (role) {
    case IdRole: return QVariant::fromValue(m.id);
    case TitleRole: return m.title;
    case StartedAtRole: return m.startedAt;
    case StateRole: return m.state;
    case DayGroupRole: return dayGroup(m.startedAt);
    default: return QVariant();
    }
}

QHash<int, QByteArray> MeetingListModel::roleNames() const {
    return {
        {IdRole, "meetingId"},
        {TitleRole, "title"},
        {StartedAtRole, "startedAt"},
        {StateRole, "state"},
        {DayGroupRole, "dayGroup"},
    };
}

int MeetingListModel::count() const {
    return int(m_rows.size());
}

QString MeetingListModel::filter() const {
    return m_filter;
}

void MeetingListModel::setFilter(const QString& text) {
    if (m_filter == text) return;
    m_filter = text;
    emit filterChanged();
    refresh();
}

void MeetingListModel::refresh() {
    beginResetModel();
    m_rows.clear();
    if (m_library) {
        const QList<Meeting> all = m_library->allMeetings();
        const QString needle = m_filter.trimmed();
        for (const Meeting& m : all) {
            if (needle.isEmpty() || m.title.contains(needle, Qt::CaseInsensitive))
                m_rows.append(m);
        }
    }
    endResetModel();
    emit countChanged();
}

QVariantMap MeetingListModel::meetingById(qint64 id) const {
    QVariantMap out;
    if (!m_library || id < 0) return out;
    const Meeting m = m_library->meeting(id);
    if (m.id < 0) return out;
    out.insert(QStringLiteral("id"), QVariant::fromValue(m.id));
    out.insert(QStringLiteral("title"), m.title);
    out.insert(QStringLiteral("startedAt"), m.startedAt);
    out.insert(QStringLiteral("endedAt"), m.endedAt);
    out.insert(QStringLiteral("attendees"), m.attendees);
    out.insert(QStringLiteral("audioPath"), m.audioPath);
    out.insert(QStringLiteral("notesMd"), m.notesMd);
    out.insert(QStringLiteral("enhancedMd"), m.enhancedMd);
    out.insert(QStringLiteral("templateId"), m.templateId);
    out.insert(QStringLiteral("state"), m.state);
    return out;
}

QVariantList MeetingListModel::segmentsFor(qint64 id) const {
    QVariantList out;
    if (!m_library || id < 0) return out;
    const QList<TranscriptSegment> segs = m_library->segments(id);
    for (const TranscriptSegment& s : segs) {
        QVariantMap m;
        m.insert(QStringLiteral("stream"), s.stream);
        m.insert(QStringLiteral("t0"), s.t0);
        m.insert(QStringLiteral("t1"), s.t1);
        m.insert(QStringLiteral("text"), s.text);
        m.insert(QStringLiteral("final"), true);
        out.append(m);
    }
    return out;
}

int MeetingListModel::indexOfId(qint64 id) const {
    for (int i = 0; i < m_rows.size(); ++i) {
        if (m_rows.at(i).id == id) return i;
    }
    return -1;
}

} // namespace gromarch
