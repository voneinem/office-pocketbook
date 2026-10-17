#pragma once
// One event for the event page (contracts/bridge-qml.md EventBridge, view side). Editing arrives with the next story.
// Fields: summary, location, description, attendees (list), whenText, zoneText (only when the event's zone differs from the user's),
// recurrenceText, calendarName, readOnly with readOnlyReason ("feed" | "calendar"), pending, conflict, note.

#include <QObject>
#include <QString>
#include <QStringList>

#include "pbo/store.h"
#include "pbo/tz.h"

class EventBridge : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool loaded READ isOpen NOTIFY changed)
    Q_PROPERTY(QString summary READ summary NOTIFY changed)
    Q_PROPERTY(QString whenText READ whenText NOTIFY changed)
    Q_PROPERTY(QString zoneText READ zoneText NOTIFY changed)
    Q_PROPERTY(QString recurrenceText READ recurrenceText NOTIFY changed)
    Q_PROPERTY(QString location READ location NOTIFY changed)
    Q_PROPERTY(QString description READ description NOTIFY changed)
    Q_PROPERTY(QStringList attendees READ attendees NOTIFY changed)
    Q_PROPERTY(QString calendarName READ calendarName NOTIFY changed)
    Q_PROPERTY(bool readOnly READ readOnly NOTIFY changed)
    Q_PROPERTY(QString readOnlyReason READ readOnlyReason NOTIFY changed)
    Q_PROPERTY(bool pending READ pending NOTIFY changed)
    Q_PROPERTY(bool conflict READ conflict NOTIFY changed)
    Q_PROPERTY(QString note READ note NOTIFY changed)               // '' | 'rrule_unsupported' | 'tz_unknown' | 'no_end' | 'truncated'

public:
    struct Config {
        pbo::Store* store = nullptr;
        const pbo::TzDatabase* tz = nullptr;
        pbo::Clock* clock = nullptr;                      // which occurrence of a series to show when none is named: the next one
        pbo::ZoneEnvironment (*environment)() = nullptr;   // tests replace the real TZ and /etc/localtime
    };

    explicit EventBridge(Config config, QObject* parent = nullptr);

    bool isOpen() const { return open_; }
    QString summary() const { return summary_; }
    QString whenText() const { return when_; }
    QString zoneText() const { return zoneText_; }
    QString recurrenceText() const { return recurrence_; }
    QString location() const { return location_; }
    QString description() const { return description_; }
    QStringList attendees() const { return attendees_; }
    QString calendarName() const { return calendar_; }
    bool readOnly() const { return readOnly_; }
    QString readOnlyReason() const { return reason_; }
    bool pending() const { return pending_; }
    bool conflict() const { return conflict_; }
    QString note() const { return note_; }

    // The occurrence shown is the one at startUtc/endUtc (a series has many); 0/0 = the event's own first time.
    Q_INVOKABLE void open(qlonglong eventId, qlonglong startUtc = 0, qlonglong endUtc = 0, bool allDay = false);
    Q_INVOKABLE void close();

signals:
    void changed();

private:
    Config config_;
    bool open_ = false, readOnly_ = false, pending_ = false, conflict_ = false;
    QString summary_, when_, zoneText_, recurrence_, location_, description_, calendar_, reason_, note_;
    QStringList attendees_;
};
