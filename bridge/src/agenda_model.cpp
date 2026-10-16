#include "agenda_model.h"

#include <QVariantMap>

#include "calendar_text.h"
#include "pbo/calendar_sync.h"

namespace {

constexpr int kMaxEvents = 1500;

QVariantMap header_row(const QString& local_date) {
    int y = 0, m = 0, d = 0;
    caltext::parse_date(local_date, &y, &m, &d);
    QVariantMap h;
    h["kind"] = QStringLiteral("header");
    h["localDate"] = local_date;
    h["dateText"] = caltext::long_date(y, m, d);
    return h;
}

}  // namespace

AgendaModel::AgendaModel(Config config, QObject* parent) : QObject(parent), config_(std::move(config)) {}

void AgendaModel::setPageSize(int n) {
    n = qBound(2, n, 100);
    if (n == pageSize_) return;
    pageSize_ = n;
    load();
}

void AgendaModel::refresh() { load(); }

void AgendaModel::goToPage(int n) {
    page_ = qBound(0, n, pageCount_ - 1);
    rows_ = pages_.empty() ? QVariantList() : pages_[static_cast<size_t>(page_)];
    emit changed();
}

void AgendaModel::load() {
    pages_.clear();
    rows_.clear();
    total_ = 0;
    const std::string zone = caltext::zone_of(config_.store, config_.tz, config_.environment);
    zone_ = QString::fromStdString(zone);
    if (zone.empty() || !config_.clock) {
        pageCount_ = 1;
        page_ = 0;
        emit changed();
        return;
    }
    // from the start of today in the user's zone: what happened earlier today is still part of the day
    pbo::LocalDateTime today = config_.tz->utc_to_local(zone, config_.clock->now());
    today.hour = today.minute = today.second = 0;
    const int64_t from = config_.tz->local_to_utc(zone, today);
    const pbo::Page<pbo::AgendaItem> all = config_.store->agenda(from, 0, kMaxEvents);
    total_ = all.total;

    // flatten into rows, then cut into pages of at most pageSize_ rows; a page that continues a day starts with that day's header again
    QVariantList page;
    QString current_date;
    auto flush = [&] {
        if (!page.isEmpty()) pages_.push_back(page);
        page = QVariantList();
    };
    for (const pbo::AgendaItem& it : all.items) {
        QString date = QString::fromStdString(it.local_date);
        const bool new_day = date != current_date;
        const int need = new_day ? 2 : 1;
        if (page.size() + need > pageSize_) {
            flush();
            if (!new_day) page.push_back(header_row(date));   // continuation: the day's header again
        }
        if (new_day) page.push_back(header_row(date));
        current_date = date;
        QVariantMap row;
        row["kind"] = QStringLiteral("event");
        row["eventId"] = static_cast<qlonglong>(it.event_id);
        row["startUtc"] = static_cast<qlonglong>(it.start_utc);
        row["endUtc"] = static_cast<qlonglong>(it.end_utc);
        row["allDay"] = it.all_day;
        row["localDate"] = date;
        row["timeText"] = caltext::time_text(*config_.tz, zone, it.start_utc, it.end_utc, it.all_day);
        row["title"] = QString::fromStdString(it.summary);
        row["calendarName"] = QString::fromStdString(it.calendar_name);
        row["readOnly"] = !it.writable;
        row["pending"] = caltext::pending_state(it.sync_state);
        row["conflict"] = caltext::conflict_state(it.sync_state);
        row["recurrenceId"] = QString::fromStdString(it.recurrence_id);
        page.push_back(row);
    }
    flush();
    pageCount_ = qMax(1, static_cast<int>(pages_.size()));
    page_ = qBound(0, page_, pageCount_ - 1);
    rows_ = pages_.empty() ? QVariantList() : pages_[static_cast<size_t>(page_)];
    emit changed();
}
