// Store: edited events and their way to the server (data-model.md: state "event sync"). Every public method is one short transaction;
// a failing statement leaves the complete before-state. Nothing here ever drops the user's version of an event without a decision.
#include <sqlite3.h>

#include <algorithm>

#include "pbo/store.h"
#include "store_sql.h"
#include "store_util.h"

namespace pbo {

using sql::Stmt;
using store_util::bind_opt;
using store_util::bind_text_or_null;
using store_util::cap;
using store_util::insert_occurrences;
using store_util::storage_error;
using store_util::Transaction;

namespace {

constexpr size_t kMaxSummary = 4096;
constexpr size_t kMaxLocation = 4096;
constexpr size_t kMaxLongText = 64u << 10;

struct Row {
    bool found = false;
    int64_t id = 0, calendar_id = 0;
    std::string href, etag, uid, state, note, summary;
    bool has_href = false;
};

Row load(sqlite3* db, int64_t id) {
    Row r;
    Stmt st(db, "SELECT id, calendar_id, href, COALESCE(etag,''), uid, sync_state, note, summary FROM event WHERE id = ?");
    if (!st.ok()) return r;
    st.bind_int64(1, id);
    if (st.step() != SQLITE_ROW) return r;
    r.found = true;
    r.id = st.int64(0);
    r.calendar_id = st.int64(1);
    r.has_href = !st.is_null(2);
    r.href = st.text(2);
    r.etag = st.text(3);
    r.uid = st.text(4);
    r.state = st.text(5);
    r.note = st.text(6);
    r.summary = st.text(7);
    return r;
}

struct CalRow {
    bool found = false;
    bool writable = false, present = true;
    std::string kind, href, name;
};

CalRow load_calendar(sqlite3* db, int64_t id) {
    CalRow c;
    Stmt st(db, "SELECT kind, href, name, writable, present_on_server FROM calendar WHERE id = ?");
    if (!st.ok()) return c;
    st.bind_int64(1, id);
    if (st.step() != SQLITE_ROW) return c;
    c.found = true;
    c.kind = st.text(0);
    c.href = st.text(1);
    c.name = st.text(2);
    c.writable = st.int64(3) != 0;
    c.present = st.int64(4) != 0;
    return c;
}

bool can_write(const CalRow& c) { return c.kind == "caldav" && c.writable; }

// The content columns of an event row from a record (not state, href, etag).
bool bind_content(Stmt& st, int& i, const EventUpsert& u) {
    st.bind_text(i++, u.ics);
    st.bind_text(i++, cap(u.summary, kMaxSummary));
    st.bind_text(i++, cap(u.location, kMaxLocation));
    st.bind_text(i++, cap(u.description, kMaxLongText));
    st.bind_text(i++, cap(u.attendees, kMaxLongText));
    st.bind_int64(i++, u.all_day ? 1 : 0);
    bind_text_or_null(st, i++, u.tzid);
    bind_opt(st, i++, u.start_utc);
    bind_opt(st, i++, u.end_utc);
    st.bind_text(i++, u.start_local);
    if (u.end_local.empty()) st.bind_null(i++);
    else st.bind_text(i++, u.end_local);
    st.bind_text(i++, cap(u.rrule, 1024));
    st.bind_text(i++, u.note);
    return true;
}

bool replace_occurrences(sqlite3* db, int64_t id, const std::vector<OccurrenceRow>& rows) {
    Stmt del(db, "DELETE FROM occurrence WHERE event_id = ?");
    del.bind_int64(1, id);
    if (!del.ok() || del.step() != SQLITE_DONE) return false;
    return insert_occurrences(db, id, rows);
}

bool delete_event_row(sqlite3* db, int64_t id) {
    Stmt d(db, "DELETE FROM event WHERE id = ?");
    d.bind_int64(1, id);
    return d.ok() && d.step() == SQLITE_DONE;
}

bool set_state(sqlite3* db, int64_t id, const char* state) {
    Stmt u(db, "UPDATE event SET sync_state = ? WHERE id = ?");
    u.bind_text(1, state);
    u.bind_int64(2, id);
    return u.ok() && u.step() == SQLITE_DONE;
}

bool uid_taken(sqlite3* db, int64_t calendar_id, const std::string& uid) {
    Stmt f(db, "SELECT 1 FROM event WHERE calendar_id = ? AND uid = ?");
    f.bind_int64(1, calendar_id);
    f.bind_text(2, cap(uid, 1024));
    return f.ok() && f.step() == SQLITE_ROW;
}

// A new row from a record
Result<int64_t> insert_event(sqlite3* db, int64_t calendar_id, const EventUpsert& u, const char* state, bool with_server_ids) {
    Result<int64_t> out;
    Stmt ins(db,
             "INSERT INTO event(calendar_id, href, etag, uid, ics, summary, location, description, attendees, all_day, tzid, start_utc, end_utc,"
             " start_local, end_local, rrule, note, sync_state) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
    if (!ins.ok()) {
        out.error = storage_error(db, "write_failed");
        return out;
    }
    int i = 1;
    ins.bind_int64(i++, calendar_id);
    bind_text_or_null(ins, i++, with_server_ids ? u.href : std::string());
    bind_text_or_null(ins, i++, with_server_ids ? u.etag : std::string());
    ins.bind_text(i++, cap(u.uid, 1024));
    bind_content(ins, i, u);
    ins.bind_text(i++, state);
    if (ins.step() != SQLITE_DONE) {
        out.error = storage_error(db, "write_failed");
        return out;
    }
    out.value = sqlite3_last_insert_rowid(db);
    if (!replace_occurrences(db, out.value, u.occurrences)) out.error = storage_error(db, "write_failed");
    return out;
}

// Overwrites the content (and optionally href and etag) of an existing row
bool update_event(sqlite3* db, int64_t id, const EventUpsert& u, const char* state, bool with_server_ids) {
    Stmt up(db,
            with_server_ids ? "UPDATE event SET ics=?, summary=?, location=?, description=?, attendees=?, all_day=?, tzid=?, start_utc=?, end_utc=?,"
                              " start_local=?, end_local=?, rrule=?, note=?, sync_state=?, href=?, etag=? WHERE id=?"
                            : "UPDATE event SET ics=?, summary=?, location=?, description=?, attendees=?, all_day=?, tzid=?, start_utc=?, end_utc=?,"
                              " start_local=?, end_local=?, rrule=?, note=?, sync_state=? WHERE id=?");
    if (!up.ok()) return false;
    int i = 1;
    bind_content(up, i, u);
    up.bind_text(i++, state);
    if (with_server_ids) {
        bind_text_or_null(up, i++, u.href);
        bind_text_or_null(up, i++, u.etag);
    }
    up.bind_int64(i++, id);
    if (up.step() != SQLITE_DONE) return false;
    return replace_occurrences(db, id, u.occurrences);
}

bool drop_conflict(sqlite3* db, int64_t id) {
    Stmt d(db, "DELETE FROM event_conflict WHERE event_id = ?");
    d.bind_int64(1, id);
    return d.ok() && d.step() == SQLITE_DONE;
}

}  // namespace

Result<int64_t> Store::save_event_local(int64_t calendar_id, int64_t event_id, const EventUpsert& record) {
    std::lock_guard<std::mutex> lock(mu_);
    Result<int64_t> out;
    auto fail = [&](Error e) {
        out.error = std::move(e);
        return out;
    };
    if (record.uid.empty() || record.ics.empty()) return fail(make_error(ErrorClass::Storage, "invalid_event", "the event has no UID or text"));
    Transaction tx(db_);
    if (!tx.open) return fail(storage_error(db_, "write_failed"));
    const CalRow cal = load_calendar(db_, calendar_id);
    if (!cal.found) return fail(make_error(ErrorClass::Storage, "not_found", "no such calendar"));
    if (!can_write(cal)) return fail(make_error(ErrorClass::Storage, "read_only", "the calendar cannot be changed"));

    if (event_id == 0) {
        if (!cal.present) return fail(make_error(ErrorClass::Storage, "read_only", "the calendar is no longer on the server"));
        if (uid_taken(db_, calendar_id, record.uid)) return fail(make_error(ErrorClass::Storage, "duplicate_uid", "an event with this UID exists"));
        Result<int64_t> ins = insert_event(db_, calendar_id, record, "pending_create", false);
        if (!ins.ok()) return fail(ins.error);
        if (!tx.commit()) return fail(storage_error(db_, "write_failed"));
        return ins;
    }
    const Row row = load(db_, event_id);
    if (!row.found || row.calendar_id != calendar_id) return fail(make_error(ErrorClass::Storage, "not_found", "no such event"));
    if (row.state == "conflict") return fail(make_error(ErrorClass::Storage, "in_conflict", "the event has to be resolved first"));
    if (row.state == "pending_delete") return fail(make_error(ErrorClass::Storage, "delete_pending", "the event is being deleted"));
    const char* next = row.state == "synced" ? "pending_update" : row.state.c_str();
    EventUpsert u = record;
    u.uid = row.uid;   // the identity of an event never changes
    if (!update_event(db_, event_id, u, next, false)) return fail(storage_error(db_, "write_failed"));
    if (!tx.commit()) return fail(storage_error(db_, "write_failed"));
    out.value = event_id;
    return out;
}

Error Store::request_event_delete(int64_t event_id) {
    std::lock_guard<std::mutex> lock(mu_);
    Transaction tx(db_);
    if (!tx.open) return storage_error(db_, "write_failed");
    const Row row = load(db_, event_id);
    if (!row.found) return make_error(ErrorClass::Storage, "not_found", "no such event");
    const CalRow cal = load_calendar(db_, row.calendar_id);
    if (!can_write(cal)) return make_error(ErrorClass::Storage, "read_only", "the calendar cannot be changed");
    if (row.state == "conflict") return make_error(ErrorClass::Storage, "in_conflict", "the event has to be resolved first");
    if (row.state == "pending_create" || row.state == "server_deleted") {
        if (!delete_event_row(db_, event_id)) return storage_error(db_, "write_failed");   // the server never had it, or no longer has it
    } else if (row.state != "pending_delete") {
        Stmt del(db_, "DELETE FROM occurrence WHERE event_id = ?");
        del.bind_int64(1, event_id);
        if (!del.ok() || del.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
        if (!set_state(db_, event_id, "pending_delete")) return storage_error(db_, "write_failed");
    }
    return tx.commit() ? Error() : storage_error(db_, "write_failed");
}

std::vector<PendingEvent> Store::pending_events() const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<PendingEvent> out;
    Stmt st(db_,
            "SELECT e.id, e.calendar_id, c.href, c.kind, c.writable, c.present_on_server, COALESCE(e.href,''), COALESCE(e.etag,''), e.uid, e.ics,"
            " e.summary, e.sync_state FROM event e JOIN calendar c ON c.id = e.calendar_id"
            " WHERE e.sync_state IN ('pending_create','pending_update','pending_delete') ORDER BY e.id");
    if (!st.ok()) return out;
    while (st.step() == SQLITE_ROW) {
        PendingEvent p;
        p.id = st.int64(0);
        p.calendar_id = st.int64(1);
        p.calendar_href = st.text(2);
        p.calendar_kind = st.text(3);
        p.calendar_writable = st.int64(4) != 0;
        p.calendar_present = st.int64(5) != 0;
        p.href = st.text(6);
        p.etag = st.text(7);
        p.uid = st.text(8);
        p.ics = st.text(9);
        p.summary = st.text(10);
        p.sync_state = st.text(11);
        out.push_back(std::move(p));
    }
    return out;
}

Error Store::event_pushed(int64_t event_id, const std::string& pushed_ics, const std::string& href, const std::string& etag) {
    std::lock_guard<std::mutex> lock(mu_);
    Transaction tx(db_);
    if (!tx.open) return storage_error(db_, "write_failed");
    Stmt cur(db_, "SELECT ics, sync_state FROM event WHERE id = ?");
    cur.bind_int64(1, event_id);
    if (!cur.ok() || cur.step() != SQLITE_ROW) return make_error(ErrorClass::Storage, "not_found", "no such event");
    const std::string current = cur.text(0), state = cur.text(1);
    if (state != "pending_create" && state != "pending_update" && state != "pending_delete") return {};   // nothing waits for this answer
    // an edit made while the push was in flight (or a deletion) is still to be sent, now against the new etag
    const char* next = state == "pending_delete" ? "pending_delete" : (current == pushed_ics ? "synced" : "pending_update");
    Stmt up(db_, "UPDATE event SET href = ?, etag = ?, sync_state = ? WHERE id = ?");
    bind_text_or_null(up, 1, href);
    bind_text_or_null(up, 2, etag);
    up.bind_text(3, next);
    up.bind_int64(4, event_id);
    if (!up.ok() || up.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
    return tx.commit() ? Error() : storage_error(db_, "write_failed");
}

Error Store::event_removed_on_server(int64_t event_id) {
    std::lock_guard<std::mutex> lock(mu_);
    Transaction tx(db_);
    if (!tx.open) return storage_error(db_, "write_failed");
    const Row row = load(db_, event_id);
    if (row.found && row.state == "pending_delete" && !delete_event_row(db_, event_id)) return storage_error(db_, "write_failed");
    return tx.commit() ? Error() : storage_error(db_, "write_failed");
}

Error Store::event_conflicted(int64_t event_id, const std::string& server_ics, const std::string& server_etag, const std::string& href) {
    std::lock_guard<std::mutex> lock(mu_);
    Transaction tx(db_);
    if (!tx.open) return storage_error(db_, "write_failed");
    const Row row = load(db_, event_id);
    if (!row.found) return make_error(ErrorClass::Storage, "not_found", "no such event");
    if (row.state != "pending_create" && row.state != "pending_update" && row.state != "pending_delete")
        return make_error(ErrorClass::Storage, "bad_state", "the event has no change waiting");
    Stmt c(db_, "INSERT OR REPLACE INTO event_conflict(event_id, server_ics, server_etag, fetched_at) VALUES(?,?,?,?)");
    c.bind_int64(1, event_id);
    c.bind_text(2, server_ics);
    c.bind_text(3, server_etag);
    c.bind_int64(4, clock_->now());
    if (!c.ok() || c.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
    Stmt up(db_, "UPDATE event SET sync_state = 'conflict', note = CASE WHEN ?1 = 'pending_delete' THEN 'deleting' ELSE note END,"
                 " href = COALESCE(href, NULLIF(?2,'')) WHERE id = ?3");
    up.bind_text(1, row.state);
    up.bind_text(2, href);
    up.bind_int64(3, event_id);
    if (!up.ok() || up.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
    return tx.commit() ? Error() : storage_error(db_, "write_failed");
}

Error Store::event_gone_on_server(int64_t event_id) {
    std::lock_guard<std::mutex> lock(mu_);
    Transaction tx(db_);
    if (!tx.open) return storage_error(db_, "write_failed");
    const Row row = load(db_, event_id);
    if (!row.found) return tx.commit() ? Error() : storage_error(db_, "write_failed");
    if (row.state == "pending_delete") {
        if (!delete_event_row(db_, event_id)) return storage_error(db_, "write_failed");   // it is gone, as the user wanted
    } else if (row.state == "pending_create" || row.state == "pending_update") {
        if (!set_state(db_, event_id, "server_deleted")) return storage_error(db_, "write_failed");
    }
    return tx.commit() ? Error() : storage_error(db_, "write_failed");
}

std::vector<EventProblem> Store::event_problems() const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<EventProblem> out;
    Stmt st(db_,
            "SELECT e.id, e.sync_state, e.summary, c.name, e.note, COALESCE(k.server_ics,''), COALESCE(k.server_etag,'')"
            " FROM event e JOIN calendar c ON c.id = e.calendar_id LEFT JOIN event_conflict k ON k.event_id = e.id"
            " WHERE e.sync_state IN ('conflict','server_deleted') ORDER BY e.id");
    if (!st.ok()) return out;
    while (st.step() == SQLITE_ROW) {
        EventProblem p;
        p.event_id = st.int64(0);
        p.state = st.text(1);
        p.summary = st.text(2);
        p.calendar_name = st.text(3);
        p.deleting = p.state == "conflict" && st.text(4) == "deleting";
        p.server_ics = st.text(5);
        p.server_etag = st.text(6);
        out.push_back(std::move(p));
    }
    return out;
}

Error Store::keep_my_event(int64_t event_id) {
    std::lock_guard<std::mutex> lock(mu_);
    Transaction tx(db_);
    if (!tx.open) return storage_error(db_, "write_failed");
    const Row row = load(db_, event_id);
    if (!row.found || row.state != "conflict") return make_error(ErrorClass::Storage, "bad_state", "the event is not in conflict");
    Stmt k(db_, "SELECT COALESCE(server_etag,'') FROM event_conflict WHERE event_id = ?");
    k.bind_int64(1, event_id);
    if (!k.ok() || k.step() != SQLITE_ROW) return make_error(ErrorClass::Storage, "bad_state", "the server's version is missing");
    const std::string server_etag = k.text(0);
    const bool deleting = row.note == "deleting";
    Stmt up(db_, "UPDATE event SET etag = ?, sync_state = ?, note = CASE WHEN note = 'deleting' THEN '' ELSE note END WHERE id = ?");
    bind_text_or_null(up, 1, server_etag);
    up.bind_text(2, deleting ? "pending_delete" : "pending_update");
    up.bind_int64(3, event_id);
    if (!up.ok() || up.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
    if (!drop_conflict(db_, event_id)) return storage_error(db_, "write_failed");
    return tx.commit() ? Error() : storage_error(db_, "write_failed");
}

Error Store::keep_server_event(int64_t event_id, const EventUpsert& server) {
    std::lock_guard<std::mutex> lock(mu_);
    Transaction tx(db_);
    if (!tx.open) return storage_error(db_, "write_failed");
    const Row row = load(db_, event_id);
    if (!row.found || row.state != "conflict") return make_error(ErrorClass::Storage, "bad_state", "the event is not in conflict");
    EventUpsert u = server;
    if (!update_event(db_, event_id, u, "synced", true)) return storage_error(db_, "write_failed");
    Stmt n(db_, "UPDATE event SET note = ? WHERE id = ?");
    n.bind_text(1, server.note);
    n.bind_int64(2, event_id);
    if (!n.ok() || n.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
    if (!drop_conflict(db_, event_id)) return storage_error(db_, "write_failed");
    return tx.commit() ? Error() : storage_error(db_, "write_failed");
}

Error Store::keep_both_events(int64_t event_id, const EventUpsert& server, const EventUpsert& mine_copy) {
    std::lock_guard<std::mutex> lock(mu_);
    Transaction tx(db_);
    if (!tx.open) return storage_error(db_, "write_failed");
    const Row row = load(db_, event_id);
    if (!row.found || row.state != "conflict") return make_error(ErrorClass::Storage, "bad_state", "the event is not in conflict");
    const bool deleting = row.note == "deleting";   // the user's side is a deletion: there is no version of theirs to keep
    if (!deleting) {
        if (mine_copy.uid.empty() || uid_taken(db_, row.calendar_id, mine_copy.uid)) return make_error(ErrorClass::Storage, "duplicate_uid", "the copy needs a new UID");
        Result<int64_t> ins = insert_event(db_, row.calendar_id, mine_copy, "pending_create", false);
        if (!ins.ok()) return ins.error;
    }
    if (!update_event(db_, event_id, server, "synced", true)) return storage_error(db_, "write_failed");
    Stmt n(db_, "UPDATE event SET note = ? WHERE id = ?");
    n.bind_text(1, server.note);
    n.bind_int64(2, event_id);
    if (!n.ok() || n.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
    if (!drop_conflict(db_, event_id)) return storage_error(db_, "write_failed");
    return tx.commit() ? Error() : storage_error(db_, "write_failed");
}

Error Store::recreate_event(int64_t event_id) {
    std::lock_guard<std::mutex> lock(mu_);
    Transaction tx(db_);
    if (!tx.open) return storage_error(db_, "write_failed");
    const Row row = load(db_, event_id);
    if (!row.found) return make_error(ErrorClass::Storage, "not_found", "no such event");
    if (row.state != "server_deleted") return make_error(ErrorClass::Storage, "bad_state", "the event is not marked as deleted on the server");
    const CalRow cal = load_calendar(db_, row.calendar_id);
    if (!cal.found || !can_write(cal) || !cal.present) return make_error(ErrorClass::Storage, "calendar_gone", "the calendar is no longer on the server");
    Stmt up(db_, "UPDATE event SET sync_state = 'pending_create', href = NULL, etag = NULL WHERE id = ?");
    up.bind_int64(1, event_id);
    if (!up.ok() || up.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
    return tx.commit() ? Error() : storage_error(db_, "write_failed");
}

Error Store::discard_event(int64_t event_id) {
    std::lock_guard<std::mutex> lock(mu_);
    Transaction tx(db_);
    if (!tx.open) return storage_error(db_, "write_failed");
    const Row row = load(db_, event_id);
    if (!row.found) return make_error(ErrorClass::Storage, "not_found", "no such event");
    if (row.state != "server_deleted") return make_error(ErrorClass::Storage, "bad_state", "the event is not marked as deleted on the server");
    if (!delete_event_row(db_, event_id)) return storage_error(db_, "write_failed");
    return tx.commit() ? Error() : storage_error(db_, "write_failed");
}

}  // namespace pbo
