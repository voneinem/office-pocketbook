#include "home_bridge.h"

#include <QVariantMap>
#include <algorithm>

#include "calendar_text.h"

HomeBridge::HomeBridge(Config config, QObject* parent) : QObject(parent), config_(std::move(config)) {}

void HomeBridge::refresh() {
    unread_ = 0;
    more_ = 0;
    events_.clear();
    needsZone_ = false;
    mail_ = config_.store->mail_account().has_value();
    calendar_ = config_.store->calendar_account().has_value();
    for (const pbo::Folder& f : config_.store->folders())
        if (f.role == "inbox") unread_ = f.unread_count;

    const std::string zone = caltext::zone_of(config_.store, config_.tz, config_.environment);
    if (calendar_ && zone.empty()) {
        needsZone_ = true;
    } else if (!zone.empty() && config_.clock && config_.tz) {
        const pbo::LocalDateTime now = config_.tz->utc_to_local(zone, config_.clock->now());
        const int64_t today = pbo::civil::days_from_civil(now.year, now.month, now.day);
        auto day_start = [&](int64_t days) {
            int y, m, d;
            pbo::civil::civil_from_days(days, &y, &m, &d);
            return config_.tz->local_to_utc(zone, {y, m, d, 0, 0, 0});
        };
        const int64_t t0 = day_start(today), t1 = day_start(today + 1), t2 = day_start(today + 2);
        auto items = config_.store->occurrences_between(t0, t2);
        std::stable_sort(items.begin(), items.end(), [&](const pbo::AgendaItem& a, const pbo::AgendaItem& b) {
            const bool a_tomorrow = a.start_utc >= t1, b_tomorrow = b.start_utc >= t1;
            if (a_tomorrow != b_tomorrow) return !a_tomorrow;
            if (a.all_day != b.all_day) return a.all_day;
            return a.start_utc < b.start_utc;
        });
        for (const pbo::AgendaItem& it : items) {
            if (events_.size() >= 6) {
                ++more_;
                continue;
            }
            QVariantMap row;
            row["eventId"] = static_cast<qlonglong>(it.event_id);
            row["startUtc"] = static_cast<qlonglong>(it.start_utc);
            row["endUtc"] = static_cast<qlonglong>(it.end_utc);
            row["allDay"] = it.all_day;
            row["title"] = QString::fromStdString(it.summary);
            row["calendarName"] = QString::fromStdString(it.calendar_name);
            row["timeText"] = caltext::time_text(*config_.tz, zone, it.start_utc, it.end_utc, it.all_day);
            row["dayText"] = it.start_utc >= t1 ? QStringLiteral("Tomorrow") : QStringLiteral("Today");
            row["pending"] = caltext::pending_state(it.sync_state);
            row["conflict"] = caltext::conflict_state(it.sync_state);
            events_.append(row);
        }
    }
    emit changed();
}
