#include "month_model.h"

#include <QVariantMap>
#include <algorithm>
#include <set>

#include "calendar_text.h"
#include "pbo/calendar_sync.h"

MonthModel::MonthModel(Config config, QObject* parent) : QObject(parent), config_(std::move(config)) {}

QStringList MonthModel::weekdayNames() const { return {QStringLiteral("Mon"), QStringLiteral("Tue"), QStringLiteral("Wed"), QStringLiteral("Thu"), QStringLiteral("Fri"), QStringLiteral("Sat"), QStringLiteral("Sun")}; }

void MonthModel::refresh() { load(); }

void MonthModel::setMode(const QString& mode) {
    if (mode != QLatin1String("month") && mode != QLatin1String("week")) return;
    mode_ = mode;
    load();
}

void MonthModel::goToday() {
    date_.clear();
    load();
}

void MonthModel::selectDate(const QString& date) {
    int y, m, d;
    if (!caltext::parse_date(date, &y, &m, &d)) return;
    date_ = date;
    load();
}

void MonthModel::next() {
    if (date_.isEmpty()) load();
    int y, m, d;
    if (!caltext::parse_date(date_, &y, &m, &d)) return;
    if (mode_ == QLatin1String("week")) {
        date_ = caltext::date_of_days(pbo::civil::days_from_civil(y, m, d) + 7);
    } else {
        m += 1;
        if (m > 12) { m = 1; ++y; }
        d = qMin(d, pbo::civil::days_in_month(y, m));
        date_ = QStringLiteral("%1-%2-%3").arg(y, 4, 10, QLatin1Char('0')).arg(m, 2, 10, QLatin1Char('0')).arg(d, 2, 10, QLatin1Char('0'));
    }
    load();
}

void MonthModel::previous() {
    if (date_.isEmpty()) load();
    int y, m, d;
    if (!caltext::parse_date(date_, &y, &m, &d)) return;
    if (mode_ == QLatin1String("week")) {
        date_ = caltext::date_of_days(pbo::civil::days_from_civil(y, m, d) - 7);
    } else {
        m -= 1;
        if (m < 1) { m = 12; --y; }
        d = qMin(d, pbo::civil::days_in_month(y, m));
        date_ = QStringLiteral("%1-%2-%3").arg(y, 4, 10, QLatin1Char('0')).arg(m, 2, 10, QLatin1Char('0')).arg(d, 2, 10, QLatin1Char('0'));
    }
    load();
}

void MonthModel::load() {
    weeks_.clear();
    title_.clear();
    const std::string zone = caltext::zone_of(config_.store, config_.tz, config_.environment);
    zone_ = QString::fromStdString(zone);
    if (zone.empty() || !config_.clock) {
        emit changed();
        return;
    }
    const pbo::LocalDateTime now_local = config_.tz->utc_to_local(zone, config_.clock->now());
    const QString today = caltext::date_key(now_local);
    if (date_.isEmpty()) date_ = today;
    int y, m, d;
    caltext::parse_date(date_, &y, &m, &d);
    const int64_t anchor = pbo::civil::days_from_civil(y, m, d);

    // the days shown: Monday of the first week .. Sunday of the last
    auto monday_on_or_before = [](int64_t days) {
        const int wd = pbo::civil::weekday_from_days(days);   // 0 = Sunday
        return days - ((wd + 6) % 7);
    };
    int64_t first, last;
    if (mode_ == QLatin1String("week")) {
        first = monday_on_or_before(anchor);
        last = first + 6;
        int y1, m1, d1, y2, m2, d2;
        pbo::civil::civil_from_days(first, &y1, &m1, &d1);
        pbo::civil::civil_from_days(last, &y2, &m2, &d2);
        const QString dot = caltext::german_flag() ? QStringLiteral(".") : QString();
        if (m1 == m2) title_ = QStringLiteral("%1%2 - %3%2 %4 %5").arg(d1).arg(dot).arg(d2).arg(QString::fromUtf8(caltext::month_name(m2))).arg(y2);
        else title_ = QStringLiteral("%1%2 %3 - %4%2 %5 %6").arg(d1).arg(dot).arg(caltext::short_month(m1)).arg(d2).arg(caltext::short_month(m2)).arg(y2);
    } else {
        first = monday_on_or_before(pbo::civil::days_from_civil(y, m, 1));
        const int64_t month_last = pbo::civil::days_from_civil(y, m, pbo::civil::days_in_month(y, m));
        last = monday_on_or_before(month_last) + 6;
        title_ = caltext::month_title(y, m);
    }

    // marks: every day an occurrence touches (a multi-day event marks all its days)
    std::set<int64_t> marked;
    {
        int fy, fm, fd, ly, lm, ld;
        pbo::civil::civil_from_days(first, &fy, &fm, &fd);
        pbo::civil::civil_from_days(last + 1, &ly, &lm, &ld);
        const int64_t from = config_.tz->local_to_utc(zone, {fy, fm, fd, 0, 0, 0});
        const int64_t to = config_.tz->local_to_utc(zone, {ly, lm, ld, 0, 0, 0});
        for (const pbo::AgendaItem& it : config_.store->occurrences_between(from, to)) {
            const pbo::LocalDateTime s = config_.tz->utc_to_local(zone, it.start_utc);
            pbo::LocalDateTime e = config_.tz->utc_to_local(zone, it.end_utc > it.start_utc ? it.end_utc - 1 : it.end_utc);
            int64_t a = pbo::civil::days_from_civil(s.year, s.month, s.day);
            int64_t b = pbo::civil::days_from_civil(e.year, e.month, e.day);
            if (it.all_day) {   // the start is the user's midnight; the exclusive end falls on the next midnight
                a = pbo::civil::days_from_civil(s.year, s.month, s.day);
            }
            for (int64_t day = std::max(a, first); day <= std::min(b, last) && day - a < 400; ++day) marked.insert(day);
        }
    }
    for (int64_t week = first; week <= last; week += 7) {
        QVariantList cells;
        for (int i = 0; i < 7; ++i) {
            const int64_t day = week + i;
            int cy, cm, cd;
            pbo::civil::civil_from_days(day, &cy, &cm, &cd);
            QVariantMap cell;
            const QString key = caltext::date_of_days(day);
            cell["date"] = key;
            cell["day"] = cd;
            cell["inMonth"] = mode_ == QLatin1String("week") || cm == m;
            cell["marked"] = marked.count(day) > 0;
            cell["today"] = key == today;
            cell["selected"] = key == date_;
            cells.push_back(cell);
        }
        weeks_.push_back(cells);
    }
    emit changed();
}
