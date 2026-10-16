#include "event_bridge.h"

#include "calendar_text.h"
#include "pbo/calendar_sync.h"
#include "pbo/rrule.h"

EventBridge::EventBridge(Config config, QObject* parent) : QObject(parent), config_(std::move(config)) {}

void EventBridge::close() {
    open_ = false;
    summary_ = when_ = zoneText_ = recurrence_ = location_ = description_ = calendar_ = reason_ = note_ = QString();
    attendees_.clear();
    readOnly_ = pending_ = conflict_ = false;
    emit changed();
}

void EventBridge::open(qlonglong eventId, qlonglong startUtc, qlonglong endUtc, bool allDay) {
    close();
    if (!config_.store || !config_.tz) return;
    const auto ev = config_.store->event(eventId);
    if (!ev) return;
    const std::string zone = caltext::zone_of(config_.store, config_.tz, config_.environment);
    summary_ = QString::fromStdString(ev->summary);
    location_ = QString::fromStdString(ev->location);
    description_ = QString::fromStdString(ev->description);
    for (const QString& a : QString::fromStdString(ev->attendees).split(QLatin1Char('\n'), Qt::SkipEmptyParts)) attendees_ << a;
    calendar_ = QString::fromStdString(ev->calendar_name);
    note_ = QString::fromStdString(ev->note);
    pending_ = caltext::pending_state(ev->sync_state);
    conflict_ = caltext::conflict_state(ev->sync_state);
    recurrence_ = QString::fromStdString(pbo::describe_rrule(ev->rrule, caltext::german_flag() ? pbo::Language::German : pbo::Language::English));
    if (ev->calendar_kind == "feed") {
        readOnly_ = true;
        reason_ = QStringLiteral("feed");
    } else if (!ev->calendar_writable) {
        readOnly_ = true;
        reason_ = QStringLiteral("calendar");
    }

    if (zone.empty()) {   // the page asks for the zone first; no time is shown with an assumed one
        when_ = QString();
        open_ = true;
        emit changed();
        return;
    }
    // which occurrence
    int64_t s = startUtc, e = endUtc;
    bool all_day = ev->all_day;
    if (s == 0) {
        if (const auto occ = config_.store->next_occurrence(eventId, config_.clock ? config_.clock->now() : 0)) {
            s = occ->start_utc;
            e = occ->end_utc;
            all_day = occ->all_day;
        } else if (ev->start_utc) {
            s = *ev->start_utc;
            e = ev->end_utc ? *ev->end_utc : s;
        }
    } else {
        all_day = allDay;
    }
    if (all_day) {
        const pbo::LocalDateTime a = config_.tz->utc_to_local(zone, s);
        const pbo::LocalDateTime b = config_.tz->utc_to_local(zone, e > s ? e - 1 : e);
        const int64_t da = pbo::civil::days_from_civil(a.year, a.month, a.day), db = pbo::civil::days_from_civil(b.year, b.month, b.day);
        when_ = caltext::long_date(a.year, a.month, a.day);
        if (db > da) when_ += QStringLiteral(" – ") + caltext::long_date(b.year, b.month, b.day);
        when_ += QStringLiteral(" (all day)");
    } else if (s != 0) {
        const pbo::LocalDateTime a = config_.tz->utc_to_local(zone, s);
        const pbo::LocalDateTime b = config_.tz->utc_to_local(zone, e);
        when_ = caltext::long_date(a.year, a.month, a.day) + QStringLiteral(", ") + caltext::time_text(*config_.tz, zone, s, e, false);
        (void)b;
        // the zone of the event when it differs from the user's
        if (!ev->tzid.empty() && config_.tz->canonical_name(ev->tzid) != config_.tz->canonical_name(zone)) {
            const pbo::LocalDateTime la = config_.tz->utc_to_local(ev->tzid, s);
            const pbo::LocalDateTime lb = config_.tz->utc_to_local(ev->tzid, e);
            zoneText_ = QStringLiteral("%1 %2 – %3 (%4)").arg(caltext::short_date(la.year, la.month, la.day), caltext::clock_text(la), caltext::clock_text(lb), QString::fromStdString(ev->tzid));
        }
    } else {
        // a floating event has no instant of its own: the wall clock as written
        when_ = QString::fromStdString(ev->start_local).replace(QLatin1Char('T'), QLatin1Char(' '));
    }
    open_ = true;
    emit changed();
}
