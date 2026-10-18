#pragma once
// The start page (US6; contracts/bridge-qml.md HomeBridge): how much unread mail the inbox has and what is on the calendar today and tomorrow. The
// numbers come from the store (offline); the sync state has its own bridge. Events: soonest first, all-day events first, at most six rows and the number
// of the others in moreCount ("+ 2 more" opens the agenda). Without a known time zone no times are listed (needsZone).
// Rows: {eventId, startUtc, endUtc, allDay, title, calendarName, timeText, dayText ("Today" | "Tomorrow"), pending, conflict}.

#include <QObject>
#include <QString>
#include <QVariantList>

#include "pbo/store.h"
#include "pbo/tz.h"
#include "pbo/types.h"

class HomeBridge : public QObject {
    Q_OBJECT
    Q_PROPERTY(int unreadCount READ unreadCount NOTIFY changed)
    Q_PROPERTY(bool mailConfigured READ mailConfigured NOTIFY changed)
    Q_PROPERTY(bool calendarConfigured READ calendarConfigured NOTIFY changed)
    // A calendar SERVER account exists, so the Contacts page can say what it found (a feed has no contacts)
    Q_PROPERTY(bool contactsAvailable READ contactsAvailable NOTIFY changed)
    // How many contact changes wait for the server (shown beside the Contacts button)
    Q_PROPERTY(int contactsPending READ contactsPending NOTIFY changed)
    Q_PROPERTY(QVariantList events READ events NOTIFY changed)
    Q_PROPERTY(int moreCount READ moreCount NOTIFY changed)
    Q_PROPERTY(bool needsZone READ needsZone NOTIFY changed)

public:
    struct Config {
        pbo::Store* store = nullptr;
        const pbo::TzDatabase* tz = nullptr;
        pbo::Clock* clock = nullptr;
        pbo::ZoneEnvironment (*environment)() = nullptr;   // tests replace the real TZ and /etc/localtime
    };

    explicit HomeBridge(Config config, QObject* parent = nullptr);

    int unreadCount() const { return unread_; }
    bool mailConfigured() const { return mail_; }
    bool calendarConfigured() const { return calendar_; }
    bool contactsAvailable() const { return contacts_; }
    int contactsPending() const { return contactsPending_; }
    QVariantList events() const { return events_; }
    int moreCount() const { return more_; }
    bool needsZone() const { return needsZone_; }

    Q_INVOKABLE void refresh();

signals:
    void changed();

private:
    Config config_;
    int unread_ = 0, more_ = 0;
    int contactsPending_ = 0;
    bool mail_ = false, calendar_ = false, contacts_ = false, needsZone_ = false;
    QVariantList events_;
};
