#pragma once
#include <QAbstractListModel>
#include <QList>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include "Types.h"

namespace gromarch {

class Library;

// Read-only list of meetings for the sidebar (newest first), refreshed on
// Library::meetingsChanged. Exposed to QML as `Meetings`.
class MeetingListModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(QString filter READ filter WRITE setFilter NOTIFY filterChanged)
public:
    enum Roles {
        IdRole = Qt::UserRole + 1,
        TitleRole,
        StartedAtRole,
        StateRole,
        DayGroupRole,
    };

    explicit MeetingListModel(Library* library, QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int count() const;
    QString filter() const;
    void setFilter(const QString& text);

public slots:
    void refresh();
    // Full record for one meeting as a plain JS object (empty when unknown).
    QVariantMap meetingById(qint64 id) const;
    // Persisted final segments: { stream, t0, t1, text }.
    QVariantList segmentsFor(qint64 id) const;
    int indexOfId(qint64 id) const;

signals:
    void countChanged();
    void filterChanged();

private:
    Library* m_library = nullptr;
    QList<Meeting> m_rows;
    QString m_filter;
};

} // namespace gromarch
