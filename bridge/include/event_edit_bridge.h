#pragma once
// Creating, changing and deleting one event (US4; contracts/bridge-qml.md EventBridge, edit side). The page edits plain strings - dates as "YYYY-MM-DD",
// times as "HH:MM" - and the bridge validates them, builds the event text through the core (only the properties that changed are rewritten) and
// writes it as local work; the sync sends it. Nothing here touches the network.
// All-day events show their LAST day in the form; the store keeps the exclusive end like iCalendar.
// blockReason: "" (editable) | "no_calendar" | "zone_unresolved" (new event) | "feed" | "read_only" | "series_read_only" | "in_conflict" | "delete_pending" | "not_found".
// errorKey / errorField after a failed save: title_missing (title), too_long (title), date_invalid (startDate / endDate), time_invalid (startTime / endTime),
// end_before_start (endDate or endTime), zone_unknown, read_only, ...

#include <QObject>
#include <QString>
#include <QVariantList>

#include "pbo/store.h"
#include "pbo/tz.h"
#include "pbo/types.h"

class EventEditBridge : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool loaded READ loaded NOTIFY changed)
    Q_PROPERTY(bool isNew READ isNew NOTIFY changed)
    Q_PROPERTY(QString title READ title WRITE setTitle NOTIFY changed)
    Q_PROPERTY(QString location READ location WRITE setLocation NOTIFY changed)
    Q_PROPERTY(QString description READ description WRITE setDescription NOTIFY changed)
    Q_PROPERTY(bool allDay READ allDay WRITE setAllDay NOTIFY changed)
    Q_PROPERTY(QString startDate READ startDate WRITE setStartDate NOTIFY changed)
    Q_PROPERTY(QString startTime READ startTime WRITE setStartTime NOTIFY changed)
    Q_PROPERTY(QString endDate READ endDate WRITE setEndDate NOTIFY changed)
    Q_PROPERTY(QString endTime READ endTime WRITE setEndTime NOTIFY changed)
    Q_PROPERTY(QString zoneName READ zoneName NOTIFY changed)
    Q_PROPERTY(qlonglong calendarId READ calendarId WRITE setCalendarId NOTIFY changed)
    Q_PROPERTY(QString calendarName READ calendarName NOTIFY changed)
    Q_PROPERTY(QVariantList calendars READ calendars NOTIFY changed)   // the calendars a new event can go to: [{id, name}]
    Q_PROPERTY(QString blockReason READ blockReason NOTIFY changed)
    Q_PROPERTY(bool canSave READ canSave NOTIFY changed)
    Q_PROPERTY(bool canDelete READ canDelete NOTIFY changed)
    Q_PROPERTY(QString errorKey READ errorKey NOTIFY changed)
    Q_PROPERTY(QString errorField READ errorField NOTIFY changed)

public:
    struct Config {
        pbo::Store* store = nullptr;
        const pbo::TzDatabase* tz = nullptr;
        pbo::Clock* clock = nullptr;
        pbo::Random* random = nullptr;
        pbo::ZoneEnvironment (*environment)() = nullptr;   // tests replace the real TZ and /etc/localtime
    };

    explicit EventEditBridge(Config config, QObject* parent = nullptr);

    bool loaded() const { return loaded_; }
    bool isNew() const { return new_; }
    QString title() const { return title_; }
    QString location() const { return location_; }
    QString description() const { return description_; }
    bool allDay() const { return allDay_; }
    QString startDate() const { return startDate_; }
    QString startTime() const { return startTime_; }
    QString endDate() const { return endDate_; }
    QString endTime() const { return endTime_; }
    QString zoneName() const { return zone_; }
    qlonglong calendarId() const { return calendarId_; }
    QString calendarName() const { return calendarName_; }
    QVariantList calendars() const { return calendars_; }
    QString blockReason() const { return block_; }
    bool canSave() const { return loaded_ && block_.isEmpty(); }
    bool canDelete() const { return loaded_ && !new_ && (block_.isEmpty() || block_ == QLatin1String("series_read_only")); }
    QString errorKey() const { return errorKey_; }
    QString errorField() const { return errorField_; }

    void setTitle(const QString& v);
    void setLocation(const QString& v);
    void setDescription(const QString& v);
    void setAllDay(bool v);
    void setStartDate(const QString& v);
    void setStartTime(const QString& v);
    void setEndDate(const QString& v);
    void setEndTime(const QString& v);
    void setCalendarId(qlonglong v);

    // A new event on `date` ("YYYY-MM-DD", "" = today in the user's zone) at the next full hour for one hour.
    Q_INVOKABLE void beginNew(const QString& date);
    Q_INVOKABLE void begin(qlonglong eventId);
    // Validates and writes; false with errorKey / errorField on a problem. On success `saved(eventId)` is sent.
    Q_INVOKABLE bool save();
    // Deletes the event (the page asked first); `removed()` on success.
    Q_INVOKABLE bool remove();
    Q_INVOKABLE void close();

signals:
    void changed();
    void saved(qlonglong eventId);
    void removed();

private:
    void touched();
    bool fail(const char* key, const char* field);
    void clear();

    Config config_;
    bool loaded_ = false, new_ = false, allDay_ = false;
    qlonglong eventId_ = 0, calendarId_ = 0;
    QString title_, location_, description_, startDate_, startTime_, endDate_, endTime_, zone_, calendarName_, block_, errorKey_, errorField_;
    QVariantList calendars_;
    std::string eventZone_;   // the zone the times are written in ("" for a floating event)
};
