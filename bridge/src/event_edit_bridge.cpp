#include "event_edit_bridge.h"

#include <QVariantMap>

#include "calendar_text.h"
#include "pbo/event_edit.h"

namespace {

bool parse_time(const QString& s, int* h, int* m) {
    const QStringList p = s.trimmed().split(QLatin1Char(':'));
    if (p.size() != 2) return false;
    bool a, b;
    *h = p[0].toInt(&a);
    *m = p[1].toInt(&b);
    return a && b && p[0].size() <= 2 && p[1].size() == 2 && *h >= 0 && *h <= 23 && *m >= 0 && *m <= 59;
}

QString date_text(const pbo::LocalDateTime& t) { return caltext::date_key(t); }

}  // namespace

EventEditBridge::EventEditBridge(Config config, QObject* parent) : QObject(parent), config_(std::move(config)) {}

void EventEditBridge::clear() {
    loaded_ = new_ = allDay_ = false;
    eventId_ = calendarId_ = 0;
    title_ = location_ = description_ = startDate_ = startTime_ = endDate_ = endTime_ = zone_ = calendarName_ = block_ = errorKey_ = errorField_ = QString();
    calendars_.clear();
    eventZone_.clear();
}

void EventEditBridge::close() {
    clear();
    emit changed();
}

void EventEditBridge::touched() {
    errorKey_.clear();
    errorField_.clear();
    emit changed();
}

bool EventEditBridge::fail(const char* key, const char* field) {
    errorKey_ = QString::fromLatin1(key);
    errorField_ = QString::fromLatin1(field);
    emit changed();
    return false;
}

void EventEditBridge::setTitle(const QString& v) { if (v != title_) { title_ = v; touched(); } }
void EventEditBridge::setLocation(const QString& v) { if (v != location_) { location_ = v; touched(); } }
void EventEditBridge::setDescription(const QString& v) { if (v != description_) { description_ = v; touched(); } }
void EventEditBridge::setAllDay(bool v) { if (v != allDay_) { allDay_ = v; touched(); } }
void EventEditBridge::setStartDate(const QString& v) { if (v != startDate_) { startDate_ = v; touched(); } }
void EventEditBridge::setStartTime(const QString& v) { if (v != startTime_) { startTime_ = v; touched(); } }
void EventEditBridge::setEndDate(const QString& v) { if (v != endDate_) { endDate_ = v; touched(); } }
void EventEditBridge::setEndTime(const QString& v) { if (v != endTime_) { endTime_ = v; touched(); } }

void EventEditBridge::setCalendarId(qlonglong v) {
    if (v == calendarId_) return;
    for (const QVariant& c : calendars_)
        if (c.toMap()["id"].toLongLong() == v) {
            calendarId_ = v;
            calendarName_ = c.toMap()["name"].toString();
            touched();
            return;
        }
}

void EventEditBridge::beginNew(const QString& date) {
    clear();
    new_ = true;
    loaded_ = true;
    if (config_.store && config_.tz) {
        for (const pbo::Calendar& c : config_.store->calendars()) {
            if (c.kind != "caldav" || !c.writable || !c.present_on_server || !c.shown) continue;
            QVariantMap m;
            m["id"] = static_cast<qlonglong>(c.id);
            m["name"] = QString::fromStdString(c.name);
            calendars_ << m;
        }
        const std::string zone = caltext::zone_of(config_.store, config_.tz, config_.environment);
        if (zone.empty()) {
            block_ = QStringLiteral("zone_unresolved");
        } else if (calendars_.isEmpty()) {
            block_ = QStringLiteral("no_calendar");
        } else {
            zone_ = QString::fromStdString(zone);
            eventZone_ = zone;
            calendarId_ = calendars_.first().toMap()["id"].toLongLong();
            calendarName_ = calendars_.first().toMap()["name"].toString();
            const int64_t now = config_.clock ? config_.clock->now() : 0;
            pbo::LocalDateTime t = config_.tz->utc_to_local(zone, now);
            int y, m, d;
            if (caltext::parse_date(date, &y, &m, &d)) {
                t.year = y;
                t.month = m;
                t.day = d;
            }
            // the next full hour; past 23:00 the event starts at 09:00 (a day picked from the calendar) or 23:00 (today)
            int hour = t.hour + 1;
            if (hour > 22) hour = caltext::parse_date(date, &y, &m, &d) ? 9 : 22;
            startDate_ = endDate_ = QStringLiteral("%1-%2-%3").arg(t.year, 4, 10, QLatin1Char('0')).arg(t.month, 2, 10, QLatin1Char('0')).arg(t.day, 2, 10, QLatin1Char('0'));
            startTime_ = caltext::two(hour) + QStringLiteral(":00");
            endTime_ = caltext::two(hour + 1) + QStringLiteral(":00");
        }
    } else {
        block_ = QStringLiteral("not_found");
    }
    emit changed();
}

void EventEditBridge::begin(qlonglong eventId) {
    clear();
    if (!config_.store || !config_.tz) {
        emit changed();
        return;
    }
    const auto view = config_.store->event(eventId);
    if (!view) {
        block_ = QStringLiteral("not_found");
        emit changed();
        return;
    }
    const auto edit = pbo::edit_of_event(*config_.store, *config_.tz, eventId);
    if (!edit.ok()) {
        block_ = QStringLiteral("not_found");
        emit changed();
        return;
    }
    loaded_ = true;
    eventId_ = eventId;
    calendarId_ = view->calendar_id;
    calendarName_ = QString::fromStdString(view->calendar_name);
    const pbo::EventEdit& e = edit.value;
    title_ = QString::fromStdString(e.title);
    location_ = QString::fromStdString(e.location);
    description_ = QString::fromStdString(e.description);
    allDay_ = e.all_day;
    eventZone_ = e.tzid;
    zone_ = QString::fromStdString(e.tzid);
    startDate_ = date_text(e.start);
    startTime_ = caltext::clock_text(e.start);
    if (e.all_day) {
        // the form shows the last day, the event stores the day after it
        endDate_ = caltext::date_of_days(pbo::civil::days_from_civil(e.end.year, e.end.month, e.end.day) - 1);
        endTime_ = QStringLiteral("10:00");
        startTime_ = QStringLiteral("09:00");
    } else {
        endDate_ = date_text(e.end);
        endTime_ = caltext::clock_text(e.end);
    }
    block_ = QString::fromStdString(pbo::event_edit_block_reason(*view, *config_.tz));
    if (block_ == QLatin1String("feed")) block_ = QStringLiteral("feed");
    emit changed();
}

bool EventEditBridge::save() {
    errorKey_.clear();
    errorField_.clear();
    if (!canSave() || !config_.store || !config_.tz || !config_.clock || !config_.random) return fail(block_.isEmpty() ? "not_found" : block_.toLatin1().constData(), "title");
    pbo::EventEdit e;
    e.title = title_.trimmed().toStdString();
    e.location = location_.toStdString();
    e.description = description_.toStdString();
    e.all_day = allDay_;
    e.tzid = allDay_ ? std::string() : eventZone_;
    int sy, sm, sd, ey, em, ed, sh = 0, smin = 0, eh = 0, emin = 0;
    if (!caltext::parse_date(startDate_.trimmed(), &sy, &sm, &sd)) return fail("date_invalid", "startDate");
    if (!caltext::parse_date(endDate_.trimmed(), &ey, &em, &ed)) return fail("date_invalid", "endDate");
    if (!allDay_) {
        if (!parse_time(startTime_, &sh, &smin)) return fail("time_invalid", "startTime");
        if (!parse_time(endTime_, &eh, &emin)) return fail("time_invalid", "endTime");
    }
    e.start = {sy, sm, sd, sh, smin, 0};
    e.end = {ey, em, ed, eh, emin, 0};
    if (allDay_) {
        // the last day is part of the event: the stored end is the day after it
        int y, m, d;
        pbo::civil::civil_from_days(pbo::civil::days_from_civil(ey, em, ed) + 1, &y, &m, &d);
        e.end = {y, m, d, 0, 0, 0};
        if (pbo::civil::days_from_civil(ey, em, ed) < pbo::civil::days_from_civil(sy, sm, sd)) return fail("end_before_start", "endDate");
    }
    const std::string bad = pbo::validate_event_edit(e, *config_.tz);
    if (!bad.empty()) {
        const char* field = bad == "title_missing" || bad == "too_long" ? "title" : bad == "end_before_start" ? (startDate_ == endDate_ ? "endTime" : "endDate") : bad == "zone_unknown" ? "title" : "startDate";
        errorKey_ = QString::fromStdString(bad);
        errorField_ = QString::fromLatin1(field);
        emit changed();
        return false;
    }
    pbo::EventEditDeps deps{*config_.store, *config_.tz, *config_.clock, *config_.random};
    const pbo::Result<int64_t> r = new_ ? pbo::create_event(deps, calendarId_, e) : pbo::change_event(deps, eventId_, e);
    if (!r.ok()) {
        errorKey_ = QString::fromStdString(r.error.key);
        errorField_ = QStringLiteral("title");
        emit changed();
        return false;
    }
    eventId_ = r.value;
    new_ = false;
    emit changed();
    emit saved(static_cast<qlonglong>(r.value));
    return true;
}

bool EventEditBridge::remove() {
    if (!canDelete() || !config_.store) return false;
    const pbo::Error err = pbo::delete_event(*config_.store, eventId_);
    if (!err.ok()) {
        errorKey_ = QString::fromStdString(err.key);
        errorField_.clear();
        emit changed();
        return false;
    }
    emit removed();
    return true;
}
