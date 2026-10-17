#include "day_model.h"

#include <QVariantMap>
#include <algorithm>

#include "calendar_text.h"
#include "pbo/calendar_sync.h"

DayModel::DayModel(Config config, QObject* parent) : QObject(parent), config_(std::move(config)) {}

void DayModel::setPageSize(int n) {
    n = qBound(1, n, 100);
    if (n == pageSize_) return;
    pageSize_ = n;
    load();
}

void DayModel::refresh() { load(); }

void DayModel::goToPage(int n) {
    page_ = qBound(0, n, pageCount_ - 1);
    rows_.clear();
    for (int i = page_ * pageSize_; i < qMin(static_cast<int>(all_.size()), (page_ + 1) * pageSize_); ++i) rows_.push_back(all_[i]);
    emit changed();
}

void DayModel::goToday() {
    date_.clear();
    page_ = 0;
    load();
}

void DayModel::selectDate(const QString& date) {
    int y, m, d;
    if (!caltext::parse_date(date, &y, &m, &d)) return;
    date_ = date;
    page_ = 0;
    load();
}

void DayModel::shift(int days) {
    int y, m, d;
    if (date_.isEmpty()) {
        load();   // fixes date_ to today
    }
    if (!caltext::parse_date(date_, &y, &m, &d)) return;
    date_ = caltext::date_of_days(pbo::civil::days_from_civil(y, m, d) + days);
    page_ = 0;
    load();
}

void DayModel::nextDay() { shift(1); }
void DayModel::previousDay() { shift(-1); }

void DayModel::load() {
    all_.clear();
    rows_.clear();
    const std::string zone = caltext::zone_of(config_.store, config_.tz, config_.environment);
    zone_ = QString::fromStdString(zone);
    if (zone.empty() || !config_.clock) {
        pageCount_ = 1;
        page_ = 0;
        emit changed();
        return;
    }
    const pbo::LocalDateTime now_local = config_.tz->utc_to_local(zone, config_.clock->now());
    const QString today = caltext::date_key(now_local);
    if (date_.isEmpty()) date_ = today;
    isToday_ = date_ == today;
    int y, m, d;
    caltext::parse_date(date_, &y, &m, &d);
    dateText_ = caltext::long_date(y, m, d);

    const int64_t day_start = config_.tz->local_to_utc(zone, {y, m, d, 0, 0, 0});
    const int64_t next_days = pbo::civil::days_from_civil(y, m, d) + 1;
    int ny, nm, nd;
    pbo::civil::civil_from_days(next_days, &ny, &nm, &nd);
    const int64_t day_end = config_.tz->local_to_utc(zone, {ny, nm, nd, 0, 0, 0});
    auto items = config_.store->occurrences_between(day_start, day_end);
    // all-day events first, then by start
    std::stable_sort(items.begin(), items.end(), [](const pbo::AgendaItem& a, const pbo::AgendaItem& b) { return a.all_day != b.all_day ? a.all_day : false; });
    for (const pbo::AgendaItem& it : items) {
        QVariantMap row;
        row["eventId"] = static_cast<qlonglong>(it.event_id);
        row["startUtc"] = static_cast<qlonglong>(it.start_utc);
        row["endUtc"] = static_cast<qlonglong>(it.end_utc);
        row["allDay"] = it.all_day;
        QString time = caltext::time_text(*config_.tz, zone, it.start_utc, it.end_utc, it.all_day);
        if (!it.all_day) {   // an event that crosses midnight is clipped to the day
            const pbo::LocalDateTime s = config_.tz->utc_to_local(zone, it.start_utc);
            const pbo::LocalDateTime e = config_.tz->utc_to_local(zone, it.end_utc);
            const bool starts_before = it.start_utc < day_start;
            const bool ends_after = it.end_utc > day_end;
            if (starts_before && ends_after) time = QStringLiteral("All day");
            else if (starts_before) time = QStringLiteral("until ") + caltext::clock_text(e);
            else if (ends_after) time = QStringLiteral("from ") + caltext::clock_text(s);
        }
        row["timeText"] = time;
        row["title"] = QString::fromStdString(it.summary);
        row["calendarName"] = QString::fromStdString(it.calendar_name);
        row["readOnly"] = !it.writable;
        row["pending"] = caltext::pending_state(it.sync_state);
        row["conflict"] = caltext::conflict_state(it.sync_state);
        all_.push_back(row);
    }
    pageCount_ = qMax(1, (static_cast<int>(all_.size()) + pageSize_ - 1) / pageSize_);
    page_ = qBound(0, page_, pageCount_ - 1);
    for (int i = page_ * pageSize_; i < qMin(static_cast<int>(all_.size()), (page_ + 1) * pageSize_); ++i) rows_.push_back(all_[i]);
    emit changed();
}
