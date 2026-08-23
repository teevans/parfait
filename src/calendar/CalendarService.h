#pragma once
#include <QList>
#include <QObject>
#include <memory>
#include "Types.h"

namespace gromarch {

// CalDAV/ICS polling: fetches upcoming events, detects "meetings" (video link
// or >=2 attendees), and signals when one is imminent so the app can offer to
// record. Accounts configured via Settings (caldav URL, username, password/app
// password, or a plain .ics URL).
class CalendarService : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool configured READ isConfigured NOTIFY eventsChanged)
public:
    explicit CalendarService(QObject* parent = nullptr);
    ~CalendarService() override;

    bool isConfigured() const;
    QList<CalendarEvent> todaysEvents() const;

public slots:
    void refresh();                  // also runs automatically every 5 minutes

signals:
    void eventsChanged();
    // Fired once per event, ~2 minutes before start.
    void meetingImminent(gromarch::CalendarEvent event);
    void error(const QString& message);

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace gromarch
