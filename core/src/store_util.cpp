#include "store_util.h"

#include <cctype>

#include "pbo/log.h"

namespace pbo {
namespace store_util {

Error storage_error(sqlite3* db, const char* fallback_key) {
    const int rc = sqlite3_errcode(db);
    const bool full = rc == SQLITE_FULL || sqlite3_extended_errcode(db) == SQLITE_IOERR_WRITE;
    log::event("store.write_failed", {log::count("sqlite", rc)});
    return make_error(ErrorClass::Storage, full ? "storage_full" : fallback_key, "the database could not be written");
}

bool insert_occurrences(sqlite3* db, int64_t event_id, const std::vector<OccurrenceRow>& rows) {
    sql::Stmt st(db, "INSERT INTO occurrence(event_id, start_utc, end_utc, local_date, all_day, recurrence_id) VALUES(?,?,?,?,?,?)");
    if (!st.ok()) return false;
    for (const OccurrenceRow& o : rows) {
        sqlite3_reset(st.get());
        st.bind_int64(1, event_id);
        st.bind_int64(2, o.start_utc);
        st.bind_int64(3, o.end_utc < o.start_utc ? o.start_utc : o.end_utc);
        st.bind_text(4, o.local_date);
        st.bind_int64(5, o.all_day ? 1 : 0);
        st.bind_text(6, o.recurrence_id);
        if (st.step() != SQLITE_DONE) return false;
    }
    return true;
}

bool note_address_sql(sqlite3* db, int64_t now, const std::string& addr_in, const std::string& name, int weight) {
    std::string addr = addr_in;
    const size_t at = addr.find('@');
    bool ok = at != std::string::npos && at > 0 && at + 1 < addr.size() && addr.find('@', at + 1) == std::string::npos && addr.size() <= 320;
    for (unsigned char c : addr)
        if (c <= 0x20 || c == 0x7F || c == '<' || c == '>' || c == ',' || c == ';' || c == '"') ok = false;
    if (!ok) return false;
    for (char& c : addr) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    sql::Stmt up(db,
                 "INSERT INTO address_suggestion(addr, name, last_seen, uses) VALUES(?1, ?2, ?3, ?4)"
                 " ON CONFLICT(addr) DO UPDATE SET uses = uses + ?4, last_seen = max(last_seen, ?3), name = CASE WHEN ?2 <> '' THEN ?2 ELSE name END");
    if (!up.ok()) return false;
    up.bind_text(1, addr);
    up.bind_text(2, cap(name, 200));
    up.bind_int64(3, now);
    up.bind_int64(4, weight < 1 ? 1 : weight);
    return up.step() == SQLITE_DONE;
}

bool prune_addresses_sql(sqlite3* db) {
    sql::Stmt prune(db, "DELETE FROM address_suggestion WHERE addr NOT IN (SELECT addr FROM address_suggestion ORDER BY uses DESC, last_seen DESC, addr LIMIT 2000)");
    return prune.ok() && prune.step() == SQLITE_DONE;
}

}  // namespace store_util
}  // namespace pbo
