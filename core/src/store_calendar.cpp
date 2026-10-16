// Store: the calendar account, calendars, events and their occurrences (data-model.md). Every public method is one short transaction;
// a failing statement leaves the complete before-state.
#include <sqlite3.h>

#include <algorithm>
#include <cstdio>

#include "pbo/calendar_account_input.h"
#include "pbo/store.h"
#include "store_sql.h"
#include "store_util.h"

namespace pbo {

using sql::Stmt;
using store_util::bind_opt;
using store_util::bind_text_or_null;
using store_util::insert_occurrences;
using store_util::cap;
using store_util::opt_col;
using store_util::storage_error;
using store_util::Transaction;

namespace {

constexpr size_t kMaxSummary = 4096;
constexpr size_t kMaxLocation = 4096;
constexpr size_t kMaxLongText = 64u << 10;

CalendarAccountInput input_of(const CalendarAccount& a) {
    CalendarAccountInput in;
    in.caldav_url = a.caldav_url;
    in.username = a.username;
    in.feed_url = a.feed_url;
    in.feed_name = a.feed_name;
    return in;
}

AgendaItem read_item(const Stmt& st) {
    AgendaItem it;
    it.event_id = st.int64(0);
    it.recurrence_id = st.text(1);
    it.start_utc = st.int64(2);
    it.end_utc = st.int64(3);
    it.local_date = st.text(4);
    it.all_day = st.int64(5) != 0;
    it.summary = st.text(6);
    it.location = st.text(7);
    it.calendar_id = st.int64(8);
    it.calendar_name = st.text(9);
    it.writable = st.int64(10) != 0;
    it.sync_state = st.text(11);
    return it;
}

const char* kItemColumns =
    "o.event_id, o.recurrence_id, o.start_utc, o.end_utc, o.local_date, o.all_day, e.summary, e.location, c.id, c.name, c.writable, e.sync_state";
const char* kItemFrom = " FROM occurrence o JOIN event e ON e.id = o.event_id JOIN calendar c ON c.id = e.calendar_id WHERE c.shown = 1";
const char* kItemOrder = " ORDER BY o.start_utc, e.summary, o.event_id, o.recurrence_id";

}  // namespace

// ---------------------------------------------------------------------------------------------- account

Error Store::save_calendar_account(const CalendarAccount& account) {
    std::lock_guard<std::mutex> lock(mu_);
    const CalendarAccountInput in = normalize_calendar_account(input_of(account));
    const auto errors = validate_calendar_account(in, false);
    if (!errors.empty()) return make_error(ErrorClass::Storage, "invalid_account", errors.front().field + ": " + errors.front().key);
    // A server or feed that is no longer the account's: its calendars go with their events, except those with unsynced work (FR-032),
    // which stay (marked as not on the server) until the user decides.
    {
        Transaction tx(db_);
        if (!tx.open) return storage_error(db_, "write_failed");
        Stmt cur(db_, "SELECT COALESCE(caldav_url,''), COALESCE(feed_url,'') FROM account_calendar WHERE id = 1");
        std::string old_caldav, old_feed;
        if (cur.ok() && cur.step() == SQLITE_ROW) {
            old_caldav = cur.text(0);
            old_feed = cur.text(1);
        }
        auto drop = [&](const char* kind) -> bool {
            Stmt d(db_,
                   "DELETE FROM calendar WHERE kind = ? AND NOT EXISTS (SELECT 1 FROM event WHERE event.calendar_id = calendar.id AND event.sync_state <> 'synced')");
            d.bind_text(1, kind);
            if (!d.ok() || d.step() != SQLITE_DONE) return false;
            Stmt m(db_, "UPDATE calendar SET present_on_server = 0 WHERE kind = ?");
            m.bind_text(1, kind);
            return m.ok() && m.step() == SQLITE_DONE;
        };
        if (!old_caldav.empty() && old_caldav != in.caldav_url && !drop("caldav")) return storage_error(db_, "write_failed");
        if (!old_feed.empty() && old_feed != in.feed_url && !drop("feed")) return storage_error(db_, "write_failed");
        if (!tx.commit()) return storage_error(db_, "write_failed");
    }
    Stmt st(db_,
            "INSERT INTO account_calendar(id, caldav_url, username, feed_url, feed_name, created) VALUES(1,?,?,?,?,?)"
            " ON CONFLICT(id) DO UPDATE SET caldav_url=excluded.caldav_url, username=excluded.username, feed_url=excluded.feed_url,"
            " feed_name=excluded.feed_name");
    if (!st.ok()) return storage_error(db_, "write_failed");
    bind_text_or_null(st, 1, in.caldav_url);
    bind_text_or_null(st, 2, in.caldav_url.empty() ? std::string() : in.username);
    bind_text_or_null(st, 3, in.feed_url);
    st.bind_text(4, cap(in.feed_name, 200));
    st.bind_int64(5, account.created ? account.created : clock_->now());
    if (st.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
    return {};
}

std::optional<CalendarAccount> Store::calendar_account() const {
    std::lock_guard<std::mutex> lock(mu_);
    Stmt st(db_, "SELECT COALESCE(caldav_url,''), COALESCE(username,''), COALESCE(feed_url,''), feed_name, created FROM account_calendar WHERE id = 1");
    if (!st.ok() || st.step() != SQLITE_ROW) return std::nullopt;
    CalendarAccount a;
    a.caldav_url = st.text(0);
    a.username = st.text(1);
    a.feed_url = st.text(2);
    a.feed_name = st.text(3);
    a.created = st.int64(4);
    return a;
}

// ---------------------------------------------------------------------------------------------- calendars

std::vector<Calendar> Store::calendars() const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<Calendar> out;
    Stmt st(db_,
            "SELECT id, kind, href, name, writable, shown, supports_vevent, COALESCE(ctag,''), COALESCE(sync_token,''), COALESCE(feed_etag,''),"
            " COALESCE(feed_last_modified,''), present_on_server, COALESCE(last_sync_at,0) FROM calendar ORDER BY kind, name COLLATE NOCASE, id");
    if (!st.ok()) return out;
    while (st.step() == SQLITE_ROW) {
        Calendar c;
        c.id = st.int64(0);
        c.kind = st.text(1);
        c.href = st.text(2);
        c.name = st.text(3);
        c.writable = st.int64(4) != 0;
        c.shown = st.int64(5) != 0;
        c.supports_vevent = st.int64(6) != 0;
        c.ctag = st.text(7);
        c.sync_token = st.text(8);
        c.feed_etag = st.text(9);
        c.feed_last_modified = st.text(10);
        c.present_on_server = st.int64(11) != 0;
        c.last_sync_at = st.int64(12);
        out.push_back(std::move(c));
    }
    return out;
}

Result<int64_t> Store::upsert_calendar(const Calendar& c) {
    std::lock_guard<std::mutex> lock(mu_);
    Result<int64_t> out;
    if ((c.kind != "caldav" && c.kind != "feed") || c.href.empty()) {
        out.error = make_error(ErrorClass::Storage, "invalid_calendar", "kind or address missing");
        return out;
    }
    Stmt st(db_,
            "INSERT INTO calendar(kind, href, name, writable, supports_vevent, present_on_server) VALUES(?,?,?,?,?,1)"
            " ON CONFLICT(href) DO UPDATE SET name=excluded.name, writable=excluded.writable, supports_vevent=excluded.supports_vevent, present_on_server=1");
    if (!st.ok()) {
        out.error = storage_error(db_, "write_failed");
        return out;
    }
    st.bind_text(1, c.kind);
    st.bind_text(2, cap(c.href, 2048));
    st.bind_text(3, cap(c.name.empty() ? c.href : c.name, 200));
    st.bind_int64(4, c.writable ? 1 : 0);
    st.bind_int64(5, c.supports_vevent ? 1 : 0);
    if (st.step() != SQLITE_DONE) {
        out.error = storage_error(db_, "write_failed");
        return out;
    }
    Stmt id(db_, "SELECT id FROM calendar WHERE href = ?");
    id.bind_text(1, cap(c.href, 2048));
    if (!id.ok() || id.step() != SQLITE_ROW) {
        out.error = storage_error(db_, "write_failed");
        return out;
    }
    out.value = id.int64(0);
    return out;
}

Error Store::set_calendar_shown(int64_t calendar_id, bool shown) {
    std::lock_guard<std::mutex> lock(mu_);
    Stmt st(db_, "UPDATE calendar SET shown = ? WHERE id = ?");
    if (!st.ok()) return storage_error(db_, "write_failed");
    st.bind_int64(1, shown ? 1 : 0);
    st.bind_int64(2, calendar_id);
    if (st.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
    if (sqlite3_changes(db_) == 0) return make_error(ErrorClass::Storage, "not_found", "no such calendar");
    return {};
}

Error Store::mark_calendars_present(const std::string& kind, const std::set<std::string>& present_hrefs) {
    std::lock_guard<std::mutex> lock(mu_);
    Transaction tx(db_);
    if (!tx.open) return storage_error(db_, "write_failed");
    Stmt all(db_, "SELECT id, href FROM calendar WHERE kind = ?");
    if (!all.ok()) return storage_error(db_, "write_failed");
    all.bind_text(1, kind);
    std::vector<int64_t> gone;
    while (all.step() == SQLITE_ROW)
        if (!present_hrefs.count(all.text(1))) gone.push_back(all.int64(0));
    for (int64_t id : gone) {
        Stmt up(db_, "UPDATE calendar SET present_on_server = 0 WHERE id = ?");
        up.bind_int64(1, id);
        if (!up.ok() || up.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
    }
    return tx.commit() ? Error() : storage_error(db_, "write_failed");
}

// ---------------------------------------------------------------------------------------------- events

std::vector<StoredEventRef> Store::event_refs(int64_t calendar_id) const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<StoredEventRef> out;
    Stmt st(db_, "SELECT id, COALESCE(href,''), COALESCE(etag,''), uid, sync_state FROM event WHERE calendar_id = ? ORDER BY id");
    if (!st.ok()) return out;
    st.bind_int64(1, calendar_id);
    while (st.step() == SQLITE_ROW) out.push_back({st.int64(0), st.text(1), st.text(2), st.text(3), st.text(4)});
    return out;
}

Error Store::apply_calendar_pull(int64_t calendar_id, const CalendarPullBatch& b) {
    std::lock_guard<std::mutex> lock(mu_);
    Transaction tx(db_);
    if (!tx.open) return storage_error(db_, "write_failed");
    {
        Stmt c(db_, "SELECT 1 FROM calendar WHERE id = ?");
        c.bind_int64(1, calendar_id);
        if (!c.ok() || c.step() != SQLITE_ROW) return make_error(ErrorClass::Storage, "not_found", "no such calendar");
    }
    std::set<std::string> kept_hrefs = b.listed_hrefs;
    for (const EventUpsert& u : b.upserts) {
        if (u.uid.empty()) continue;
        kept_hrefs.insert(u.href);
        int64_t existing_id = 0;
        std::string existing_state;
        {
            Stmt f(db_, "SELECT id, sync_state FROM event WHERE calendar_id = ? AND uid = ?");
            if (!f.ok()) return storage_error(db_, "write_failed");
            f.bind_int64(1, calendar_id);
            f.bind_text(2, u.uid);
            if (f.step() == SQLITE_ROW) {
                existing_id = f.int64(0);
                existing_state = f.text(1);
            }
        }
        if (existing_id && existing_state != "synced") continue;   // local work is never overwritten (FR-032)
        if (u.occurrences.empty()) {                                // no instance in the window: not stored, an old copy goes
            if (existing_id) {
                Stmt d(db_, "DELETE FROM event WHERE id = ?");
                d.bind_int64(1, existing_id);
                if (!d.ok() || d.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
            }
            continue;
        }
        Stmt up(db_,
                "INSERT INTO event(calendar_id, href, etag, uid, ics, summary, location, description, attendees, all_day, tzid, start_utc, end_utc,"
                " start_local, end_local, rrule, note, sync_state) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,'synced')"
                " ON CONFLICT(calendar_id, uid) DO UPDATE SET href=excluded.href, etag=excluded.etag, ics=excluded.ics, summary=excluded.summary,"
                " location=excluded.location, description=excluded.description, attendees=excluded.attendees, all_day=excluded.all_day,"
                " tzid=excluded.tzid, start_utc=excluded.start_utc, end_utc=excluded.end_utc, start_local=excluded.start_local,"
                " end_local=excluded.end_local, rrule=excluded.rrule, note=excluded.note, sync_state='synced'");
        if (!up.ok()) return storage_error(db_, "write_failed");
        int i = 1;
        up.bind_int64(i++, calendar_id);
        bind_text_or_null(up, i++, u.href);
        bind_text_or_null(up, i++, u.etag);
        up.bind_text(i++, cap(u.uid, 1024));
        up.bind_text(i++, u.ics);
        up.bind_text(i++, cap(u.summary, kMaxSummary));
        up.bind_text(i++, cap(u.location, kMaxLocation));
        up.bind_text(i++, cap(u.description, kMaxLongText));
        up.bind_text(i++, cap(u.attendees, kMaxLongText));
        up.bind_int64(i++, u.all_day ? 1 : 0);
        bind_text_or_null(up, i++, u.tzid);
        bind_opt(up, i++, u.start_utc);
        bind_opt(up, i++, u.end_utc);
        up.bind_text(i++, u.start_local);
        if (u.end_local.empty()) up.bind_null(i++);
        else up.bind_text(i++, u.end_local);
        up.bind_text(i++, cap(u.rrule, 1024));
        up.bind_text(i++, u.note);
        if (up.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
        int64_t id = 0;
        {
            Stmt f(db_, "SELECT id FROM event WHERE calendar_id = ? AND uid = ?");
            f.bind_int64(1, calendar_id);
            f.bind_text(2, cap(u.uid, 1024));
            if (!f.ok() || f.step() != SQLITE_ROW) return storage_error(db_, "write_failed");
            id = f.int64(0);
        }
        Stmt del(db_, "DELETE FROM occurrence WHERE event_id = ?");
        del.bind_int64(1, id);
        if (!del.ok() || del.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
        if (!insert_occurrences(db_, id, u.occurrences)) return storage_error(db_, "write_failed");
    }
    for (const std::string& href : b.removed_hrefs) {
        Stmt d(db_, "DELETE FROM event WHERE calendar_id = ? AND href = ? AND sync_state = 'synced'");
        d.bind_int64(1, calendar_id);
        d.bind_text(2, href);
        if (!d.ok() || d.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
    }
    if (b.full) {
        std::vector<int64_t> drop;
        Stmt all(db_, "SELECT id, COALESCE(href,'') FROM event WHERE calendar_id = ? AND sync_state = 'synced'");
        if (!all.ok()) return storage_error(db_, "write_failed");
        all.bind_int64(1, calendar_id);
        while (all.step() == SQLITE_ROW)
            if (!kept_hrefs.count(all.text(1))) drop.push_back(all.int64(0));
        for (int64_t id : drop) {
            Stmt d(db_, "DELETE FROM event WHERE id = ?");
            d.bind_int64(1, id);
            if (!d.ok() || d.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
        }
    }
    {
        // the cursor: a value that is not given keeps the stored one
        Stmt cur(db_,
                 "UPDATE calendar SET ctag = COALESCE(NULLIF(?1,''), ctag), sync_token = COALESCE(NULLIF(?2,''), sync_token),"
                 " feed_etag = COALESCE(NULLIF(?3,''), feed_etag), feed_last_modified = COALESCE(NULLIF(?4,''), feed_last_modified),"
                 " last_sync_at = CASE WHEN ?5 > 0 THEN ?5 ELSE last_sync_at END WHERE id = ?6");
        if (!cur.ok()) return storage_error(db_, "write_failed");
        cur.bind_text(1, b.ctag);
        cur.bind_text(2, b.sync_token);
        cur.bind_text(3, b.feed_etag);
        cur.bind_text(4, b.feed_last_modified);
        cur.bind_int64(5, b.synced_at);
        cur.bind_int64(6, calendar_id);
        if (cur.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
    }
    return tx.commit() ? Error() : storage_error(db_, "write_failed");
}

Error Store::rebuild_occurrences(const std::function<std::vector<OccurrenceRow>(const std::string& ics)>& expander) {
    std::lock_guard<std::mutex> lock(mu_);
    struct Row {
        int64_t id;
        std::string ics, state;
    };
    std::vector<Row> rows;
    {
        Stmt st(db_, "SELECT id, ics, sync_state FROM event ORDER BY id");
        if (!st.ok()) return storage_error(db_, "write_failed");
        while (st.step() == SQLITE_ROW) rows.push_back({st.int64(0), st.text(1), st.text(2)});
    }
    Transaction tx(db_);
    if (!tx.open) return storage_error(db_, "write_failed");
    for (const Row& r : rows) {
        const std::vector<OccurrenceRow> occ = expander(r.ics);
        if (occ.empty() && r.state == "synced") {
            Stmt d(db_, "DELETE FROM event WHERE id = ?");
            d.bind_int64(1, r.id);
            if (!d.ok() || d.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
            continue;
        }
        Stmt del(db_, "DELETE FROM occurrence WHERE event_id = ?");
        del.bind_int64(1, r.id);
        if (!del.ok() || del.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
        if (r.state != "pending_delete" && !insert_occurrences(db_, r.id, occ)) return storage_error(db_, "write_failed");   // a deleted event stays out of every view
    }
    return tx.commit() ? Error() : storage_error(db_, "write_failed");
}

// ---------------------------------------------------------------------------------------------- queries

Page<AgendaItem> Store::agenda(int64_t from_utc, int page, int page_size) const {
    std::lock_guard<std::mutex> lock(mu_);
    Page<AgendaItem> out;
    if (page_size < 1) page_size = 1;
    {
        const std::string sql = std::string("SELECT count(*)") + kItemFrom + " AND (o.end_utc > ?1 OR o.start_utc >= ?1)";
        Stmt c(db_, sql.c_str());
        if (c.ok()) {
            c.bind_int64(1, from_utc);
            if (c.step() == SQLITE_ROW) out.total = static_cast<int>(c.int64(0));
        }
    }
    out.page_count = std::max(1, (out.total + page_size - 1) / page_size);
    out.page = std::max(0, std::min(page, out.page_count - 1));
    const std::string sql = std::string("SELECT ") + kItemColumns + kItemFrom + " AND (o.end_utc > ?1 OR o.start_utc >= ?1)" + kItemOrder + " LIMIT ?2 OFFSET ?3";
    Stmt st(db_, sql.c_str());
    if (!st.ok()) return out;
    st.bind_int64(1, from_utc);
    st.bind_int64(2, page_size);
    st.bind_int64(3, static_cast<long long>(out.page) * page_size);
    while (st.step() == SQLITE_ROW) out.items.push_back(read_item(st));
    return out;
}

std::vector<AgendaItem> Store::occurrences_between(int64_t from_utc, int64_t to_utc) const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<AgendaItem> out;
    const std::string sql = std::string("SELECT ") + kItemColumns + kItemFrom + " AND o.start_utc < ?2 AND (o.end_utc > ?1 OR o.start_utc >= ?1)" + kItemOrder;
    Stmt st(db_, sql.c_str());
    if (!st.ok()) return out;
    st.bind_int64(1, from_utc);
    st.bind_int64(2, to_utc);
    while (st.step() == SQLITE_ROW) out.push_back(read_item(st));
    return out;
}

std::vector<std::string> Store::marked_days(const std::string& from_date, const std::string& to_date) const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<std::string> out;
    Stmt st(db_,
            "SELECT DISTINCT o.local_date FROM occurrence o JOIN event e ON e.id = o.event_id JOIN calendar c ON c.id = e.calendar_id"
            " WHERE c.shown = 1 AND o.local_date >= ? AND o.local_date <= ? ORDER BY o.local_date");
    if (!st.ok()) return out;
    st.bind_text(1, from_date);
    st.bind_text(2, to_date);
    while (st.step() == SQLITE_ROW) out.push_back(st.text(0));
    return out;
}

std::optional<OccurrenceRow> Store::next_occurrence(int64_t event_id, int64_t from_utc) const {
    std::lock_guard<std::mutex> lock(mu_);
    for (const char* sql : {"SELECT start_utc, end_utc, local_date, all_day, recurrence_id FROM occurrence WHERE event_id = ?1 AND (end_utc > ?2 OR start_utc >= ?2) ORDER BY start_utc LIMIT 1",
                            "SELECT start_utc, end_utc, local_date, all_day, recurrence_id FROM occurrence WHERE event_id = ?1 ORDER BY start_utc DESC LIMIT 1"}) {
        Stmt st(db_, sql);
        if (!st.ok()) return std::nullopt;
        st.bind_int64(1, event_id);
        st.bind_int64(2, from_utc);
        if (st.step() == SQLITE_ROW) {
            OccurrenceRow o;
            o.start_utc = st.int64(0);
            o.end_utc = st.int64(1);
            o.local_date = st.text(2);
            o.all_day = st.int64(3) != 0;
            o.recurrence_id = st.text(4);
            return o;
        }
    }
    return std::nullopt;
}

std::optional<EventView> Store::event(int64_t event_id) const {
    std::lock_guard<std::mutex> lock(mu_);
    Stmt st(db_,
            "SELECT e.id, e.calendar_id, c.name, c.kind, c.writable, COALESCE(e.href,''), COALESCE(e.etag,''), e.uid, e.ics, e.summary, e.location,"
            " e.description, e.attendees, e.all_day, COALESCE(e.tzid,''), e.start_utc, e.end_utc, e.start_local, COALESCE(e.end_local,''), e.rrule,"
            " e.note, e.sync_state FROM event e JOIN calendar c ON c.id = e.calendar_id WHERE e.id = ?");
    if (!st.ok()) return std::nullopt;
    st.bind_int64(1, event_id);
    if (st.step() != SQLITE_ROW) return std::nullopt;
    EventView v;
    v.id = st.int64(0);
    v.calendar_id = st.int64(1);
    v.calendar_name = st.text(2);
    v.calendar_kind = st.text(3);
    v.calendar_writable = st.int64(4) != 0;
    v.href = st.text(5);
    v.etag = st.text(6);
    v.uid = st.text(7);
    v.ics = st.text(8);
    v.summary = st.text(9);
    v.location = st.text(10);
    v.description = st.text(11);
    v.attendees = st.text(12);
    v.all_day = st.int64(13) != 0;
    v.tzid = st.text(14);
    v.start_utc = opt_col(st, 15);
    v.end_utc = opt_col(st, 16);
    v.start_local = st.text(17);
    v.end_local = st.text(18);
    v.rrule = st.text(19);
    v.note = st.text(20);
    v.sync_state = st.text(21);
    return v;
}

}  // namespace pbo
