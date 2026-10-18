// Store: what the user does with mail on the reader - flags, moves, deletes - search, and address suggestions (US5). Every public method is one
// short transaction. A change is local work until the server has confirmed it; nothing here removes a message the user has not seen disappear.
#include <sqlite3.h>

#include <algorithm>
#include <set>
#include <cctype>

#include "pbo/store.h"
#include "pbo/utf8.h"
#include "store_sql.h"
#include "store_util.h"

namespace pbo {

using sql::Stmt;
using store_util::cap;
using store_util::storage_error;
using store_util::Transaction;

namespace {

struct MsgRow {
    bool found = false;
    int64_t id = 0, folder_id = 0;
    uint32_t flags_base = 0, flags_local = 0;
    std::string state, error;
};

MsgRow load(sqlite3* db, int64_t id) {
    MsgRow r;
    Stmt st(db, "SELECT id, folder_id, flags_base, flags_local, local_state, local_error FROM message WHERE id = ?");
    if (!st.ok()) return r;
    st.bind_int64(1, id);
    if (st.step() != SQLITE_ROW) return r;
    r.found = true;
    r.id = st.int64(0);
    r.folder_id = st.int64(1);
    r.flags_base = static_cast<uint32_t>(st.int64(2));
    r.flags_local = static_cast<uint32_t>(st.int64(3));
    r.state = st.text(4);
    r.error = st.text(5);
    return r;
}

void refresh_unread(sqlite3* db, int64_t folder_id) {
    Stmt st(db,
            "UPDATE folder SET unread_count = (SELECT count(*) FROM message WHERE folder_id = ?1 AND (flags_local & 1) = 0 AND stale = 0"
            " AND (flags_local & 8) = 0 AND local_state NOT IN ('pending_delete','pending_move')) WHERE id = ?1");
    if (st.ok()) {
        st.bind_int64(1, folder_id);
        st.step();
    }
}

int64_t trash_folder(sqlite3* db) {
    Stmt a(db, "SELECT trash_folder_id FROM account_mail WHERE id = 1");
    if (a.ok() && a.step() == SQLITE_ROW && !a.is_null(0) && a.int64(0) != 0) {
        Stmt f(db, "SELECT 1 FROM folder WHERE id = ?");
        f.bind_int64(1, a.int64(0));
        if (f.ok() && f.step() == SQLITE_ROW) return a.int64(0);
    }
    Stmt st(db, "SELECT id FROM folder WHERE role = 'trash' ORDER BY id LIMIT 1");
    if (st.ok() && st.step() == SQLITE_ROW) return st.int64(0);
    return 0;
}

bool folder_role_is_trash(sqlite3* db, int64_t folder_id) {
    Stmt st(db, "SELECT role FROM folder WHERE id = ?");
    st.bind_int64(1, folder_id);
    return st.ok() && st.step() == SQLITE_ROW && st.text(0) == "trash";
}

MessageHeader read_header(const Stmt& st) {
    MessageHeader m;
    m.id = st.int64(0);
    m.folder_id = st.int64(1);
    m.uid = static_cast<uint32_t>(st.int64(2));
    m.message_id = st.text(3);
    m.from_name = st.text(4);
    m.from_addr = st.text(5);
    m.to_text = st.text(6);
    m.subject = st.text(7);
    m.date_utc = st.int64(8);
    m.size = st.int64(9);
    m.flags = static_cast<uint32_t>(st.int64(10));
    m.local_state = st.text(11);
    m.body_state = st.text(12);
    m.has_attachments = st.int64(13) != 0;
    m.local_error = st.text(14);
    m.stale = st.int64(15) != 0;
    return m;
}

constexpr const char* kHeaderColumns =
    "id, folder_id, uid, COALESCE(message_id,''), from_name, from_addr, to_text, subject, date_utc, size, flags_local, local_state, body_state,"
    " has_attachments, local_error, stale";

}  // namespace

Error Store::set_flags_local(int64_t message_id, uint32_t add, uint32_t remove) {
    std::lock_guard<std::mutex> lock(mu_);
    Transaction tx(db_);
    if (!tx.open) return storage_error(db_, "write_failed");
    const MsgRow m = load(db_, message_id);
    if (!m.found) return make_error(ErrorClass::Storage, "not_found", "no such message");
    if (m.state == "pending_move" || m.state == "pending_delete") return make_error(ErrorClass::Storage, "bad_state", "a move or a delete is waiting");
    const uint32_t now = (m.flags_local | add) & ~remove;
    Stmt up(db_, "UPDATE message SET flags_local = ?, local_state = ? WHERE id = ?");
    up.bind_int64(1, now);
    up.bind_text(2, now != m.flags_base ? "pending_flags" : "synced");
    up.bind_int64(3, message_id);
    if (!up.ok() || up.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
    refresh_unread(db_, m.folder_id);
    return tx.commit() ? Error() : storage_error(db_, "write_failed");
}

Error Store::request_move(int64_t message_id, int64_t target_folder_id) {
    std::lock_guard<std::mutex> lock(mu_);
    Transaction tx(db_);
    if (!tx.open) return storage_error(db_, "write_failed");
    const MsgRow m = load(db_, message_id);
    if (!m.found) return make_error(ErrorClass::Storage, "not_found", "no such message");
    if (m.state == "pending_delete") return make_error(ErrorClass::Storage, "bad_state", "a delete is waiting");
    if (target_folder_id == m.folder_id) return make_error(ErrorClass::Storage, "same_folder", "the message is in that folder already");
    Stmt t(db_, "SELECT 1 FROM folder WHERE id = ?");
    t.bind_int64(1, target_folder_id);
    if (!t.ok() || t.step() != SQLITE_ROW) return make_error(ErrorClass::Storage, "bad_target", "no such folder");
    Stmt up(db_, "UPDATE message SET local_state = 'pending_move', move_target_folder_id = ?, local_error = '' WHERE id = ?");
    up.bind_int64(1, target_folder_id);
    up.bind_int64(2, message_id);
    if (!up.ok() || up.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
    refresh_unread(db_, m.folder_id);
    return tx.commit() ? Error() : storage_error(db_, "write_failed");
}

Error Store::request_delete(int64_t message_id, bool permanent_confirmed) {
    std::lock_guard<std::mutex> lock(mu_);
    Transaction tx(db_);
    if (!tx.open) return storage_error(db_, "write_failed");
    const MsgRow m = load(db_, message_id);
    if (!m.found) return make_error(ErrorClass::Storage, "not_found", "no such message");
    if (m.state == "pending_delete") return {};
    const int64_t trash = trash_folder(db_);
    const bool in_trash = folder_role_is_trash(db_, m.folder_id);
    if (trash != 0 && !in_trash) {   // the normal delete: into the Trash, nothing is lost
        Stmt up(db_, "UPDATE message SET local_state = 'pending_move', move_target_folder_id = ?, local_error = '' WHERE id = ?");
        up.bind_int64(1, trash);
        up.bind_int64(2, message_id);
        if (!up.ok() || up.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
    } else {
        if (!permanent_confirmed) return make_error(ErrorClass::Storage, in_trash ? "confirm_required" : "no_trash", in_trash ? "deleting from the Trash is permanent" : "there is no Trash folder");
        Stmt up(db_, "UPDATE message SET local_state = 'pending_delete', move_target_folder_id = NULL, local_error = '' WHERE id = ?");
        up.bind_int64(1, message_id);
        if (!up.ok() || up.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
    }
    refresh_unread(db_, m.folder_id);
    return tx.commit() ? Error() : storage_error(db_, "write_failed");
}

std::vector<PendingMessage> Store::pending_messages() const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<PendingMessage> out;
    Stmt st(db_,
            "SELECT id, folder_id, uid, flags_base, flags_local, local_state, move_target_folder_id, COALESCE(message_id,'') FROM message"
            " WHERE local_state <> 'synced' AND local_error = '' AND stale = 0 ORDER BY folder_id, id");
    if (!st.ok()) return out;
    while (st.step() == SQLITE_ROW) {
        PendingMessage p;
        p.id = st.int64(0);
        p.folder_id = st.int64(1);
        p.uid = static_cast<uint32_t>(st.int64(2));
        p.flags_base = static_cast<uint32_t>(st.int64(3));
        p.flags_local = static_cast<uint32_t>(st.int64(4));
        p.local_state = st.text(5);
        if (!st.is_null(6)) p.move_target_folder_id = st.int64(6);
        p.message_id = st.text(7);
        out.push_back(std::move(p));
    }
    return out;
}

Error Store::message_flags_pushed(int64_t message_id, uint32_t server_flags) {
    std::lock_guard<std::mutex> lock(mu_);
    Transaction tx(db_);
    if (!tx.open) return storage_error(db_, "write_failed");
    const MsgRow m = load(db_, message_id);
    if (!m.found) return make_error(ErrorClass::Storage, "not_found", "no such message");
    const uint32_t changed = m.flags_local ^ m.flags_base;   // the bits the user wanted different
    const uint32_t merged = (server_flags & ~changed) | (m.flags_local & changed);
    // a bit the user changed that the server does not show: the server did not take it
    const bool refused = ((server_flags ^ m.flags_local) & changed) != 0;
    Stmt up(db_, "UPDATE message SET flags_base = ?1, flags_local = ?2, local_state = ?3, local_error = ?4 WHERE id = ?5");
    up.bind_int64(1, server_flags);
    up.bind_int64(2, refused ? m.flags_local : merged);
    up.bind_text(3, refused ? "pending_flags" : (merged == server_flags ? "synced" : "pending_flags"));
    up.bind_text(4, refused ? "refused" : "");
    up.bind_int64(5, message_id);
    if (!up.ok() || up.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
    refresh_unread(db_, m.folder_id);
    return tx.commit() ? Error() : storage_error(db_, "write_failed");
}

Error Store::message_left_folder(int64_t message_id) {
    std::lock_guard<std::mutex> lock(mu_);
    Transaction tx(db_);
    if (!tx.open) return storage_error(db_, "write_failed");
    const MsgRow m = load(db_, message_id);
    if (!m.found) return {};
    Stmt d(db_, "DELETE FROM message WHERE id = ?");
    d.bind_int64(1, message_id);
    if (!d.ok() || d.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
    refresh_unread(db_, m.folder_id);
    return tx.commit() ? Error() : storage_error(db_, "write_failed");
}

Error Store::set_message_error(int64_t message_id, const std::string& key) {
    if (key != "gone_on_server" && key != "moved_on_server" && key != "refused" && key != "no_trash") return make_error(ErrorClass::Storage, "invalid_error", "unknown error key");
    std::lock_guard<std::mutex> lock(mu_);
    Stmt up(db_, "UPDATE message SET local_error = ? WHERE id = ?");
    up.bind_text(1, key);
    up.bind_int64(2, message_id);
    if (!up.ok() || up.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
    if (sqlite3_changes(db_) == 0) return make_error(ErrorClass::Storage, "not_found", "no such message");
    return {};
}

Error Store::acknowledge_message_error(int64_t message_id) {
    std::lock_guard<std::mutex> lock(mu_);
    Transaction tx(db_);
    if (!tx.open) return storage_error(db_, "write_failed");
    const MsgRow m = load(db_, message_id);
    if (!m.found) return make_error(ErrorClass::Storage, "not_found", "no such message");
    if (m.error.empty()) return {};
    if (m.error == "gone_on_server" || m.error == "moved_on_server") {
        Stmt d(db_, "DELETE FROM message WHERE id = ?");
        d.bind_int64(1, message_id);
        if (!d.ok() || d.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
    } else {   // refused / no_trash: the change that could not be applied is dropped, the message is as the server has it
        Stmt up(db_, "UPDATE message SET flags_local = flags_base, local_state = 'synced', move_target_folder_id = NULL, local_error = '' WHERE id = ?");
        up.bind_int64(1, message_id);
        if (!up.ok() || up.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
    }
    refresh_unread(db_, m.folder_id);
    return tx.commit() ? Error() : storage_error(db_, "write_failed");
}

std::vector<MessageHeader> Store::messages_with_errors() const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<MessageHeader> out;
    const std::string sql = std::string("SELECT ") + kHeaderColumns + " FROM message WHERE local_error <> '' ORDER BY id";
    Stmt st(db_, sql.c_str());
    if (!st.ok()) return out;
    while (st.step() == SQLITE_ROW) out.push_back(read_header(st));
    return out;
}

// ---------------------------------------------------------------------------------------------- search

namespace {

// Every word of the input as a quoted FTS5 prefix term; characters that FTS5 would read as syntax are only ever inside quotes.
std::string fts_query(const std::string& input) {
    std::string out;
    size_t i = 0;
    while (i < input.size()) {
        while (i < input.size() && std::isspace(static_cast<unsigned char>(input[i]))) ++i;
        const size_t b = i;
        while (i < input.size() && !std::isspace(static_cast<unsigned char>(input[i]))) ++i;
        if (i == b) break;
        std::string word = input.substr(b, i - b);
        // a word without any letter or digit has nothing to match (the tokenizer would drop it and the query would be empty)
        bool has_content = false;
        for (unsigned char c : word)
            if (std::isalnum(c) || c >= 0x80) has_content = true;
        if (!has_content) continue;
        std::string quoted = "\"";
        for (char c : word) {
            if (c == '"') quoted += '"';
            quoted += c;
        }
        quoted += "\"*";
        if (!out.empty()) out += ' ';
        out += quoted;
    }
    return out;
}

}  // namespace

std::vector<MessageHeader> Store::search(const std::string& query, int limit) const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<MessageHeader> out;
    const std::string match = fts_query(cap(query, 512));
    if (match.empty() || limit < 1) return out;
    const std::string sql = std::string("SELECT ") + kHeaderColumns +
                            " FROM message WHERE id IN (SELECT rowid FROM message_fts WHERE message_fts MATCH ?1) AND local_state <> 'pending_delete'"
                            " AND (local_state <> 'pending_move' OR local_error <> '') AND (flags_local & 8) = 0 AND (stale = 0 OR local_state <> 'synced') ORDER BY date_utc DESC, id DESC LIMIT ?2";
    Stmt st(db_, sql.c_str());
    if (!st.ok()) return out;
    st.bind_text(1, match);
    st.bind_int64(2, limit);
    while (st.step() == SQLITE_ROW) out.push_back(read_header(st));
    return out;
}

// ---------------------------------------------------------------------------------------------- addresses

Error Store::note_address(const std::string& addr, const std::string& name, int weight) {
    std::lock_guard<std::mutex> lock(mu_);
    Transaction tx(db_);
    if (!tx.open) return storage_error(db_, "write_failed");
    if (!store_util::note_address_sql(db_, clock_->now(), addr, name, weight)) return make_error(ErrorClass::Storage, "invalid_address", "not an address");
    if (!store_util::prune_addresses_sql(db_)) return storage_error(db_, "write_failed");
    return tx.commit() ? Error() : storage_error(db_, "write_failed");
}

std::vector<Store::AddressSuggestion> Store::address_suggestions(const std::string& prefix_in, int limit) const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<AddressSuggestion> out;
    std::string prefix = cap(prefix_in, 200);
    while (!prefix.empty() && std::isspace(static_cast<unsigned char>(prefix.front()))) prefix.erase(prefix.begin());
    if (prefix.empty() || limit < 1) return out;
    // LIKE with the wildcards of the input escaped; the name is compared word by word as well ("anna b" finds "Anna Berg")
    auto escaped = [](const std::string& text) {
        std::string like;
        for (char c : text) {
            if (c == '%' || c == '_' || c == '\\') like += '\\';
            like += c;
        }
        return like;
    };
    std::set<std::string> seen;   // lower-cased addresses already offered

    // the contacts first
    {
        std::string lowered = prefix;
        for (char& c : lowered) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        const std::string folded = sort_key(prefix);
        Stmt st(db_,
                "SELECT e.addr, c.display_name, e.label FROM contact_email e JOIN contact c ON c.id = e.contact_id JOIN addressbook b ON b.id = c.addressbook_id"
                " WHERE b.shown = 1 AND c.sync_state <> 'pending_delete' AND (e.addr_key LIKE ?1 ESCAPE '\\' OR (?2 <> '' AND (' ' || c.search_text) LIKE ?3 ESCAPE '\\'))"
                " ORDER BY c.sort_key, c.id, e.idx LIMIT ?4");
        if (st.ok()) {
            st.bind_text(1, escaped(lowered) + "%");
            st.bind_text(2, folded);
            st.bind_text(3, "% " + escaped(folded) + "%");
            st.bind_int64(4, limit);
            while (st.step() == SQLITE_ROW) {
                AddressSuggestion s;
                s.addr = st.text(0);
                s.name = st.text(1);
                s.label = st.text(2);
                s.from_contact = true;
                std::string key = s.addr;
                for (char& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (!seen.insert(key).second) continue;   // the same address twice in one contact or in two
                out.push_back(std::move(s));
            }
        }
    }
    if (static_cast<int>(out.size()) >= limit) {
        out.resize(static_cast<size_t>(limit));
        return out;
    }

    // then the cached and sent mail
    Stmt st(db_,
            "SELECT addr, name FROM address_suggestion WHERE addr LIKE ?1 ESCAPE '\\' OR name LIKE ?1 ESCAPE '\\' ORDER BY uses DESC, last_seen DESC, addr LIMIT ?2");
    if (!st.ok()) return out;
    st.bind_text(1, escaped(prefix) + "%");
    st.bind_int64(2, limit + static_cast<int64_t>(out.size()));   // (room for the ones a contact already gave)
    while (st.step() == SQLITE_ROW && static_cast<int>(out.size()) < limit) {
        std::string key = st.text(0);
        for (char& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (!seen.insert(key).second) continue;
        AddressSuggestion s;
        s.addr = st.text(0);
        s.name = st.text(1);
        out.push_back(std::move(s));
    }
    return out;
}

// ---------------------------------------------------------------------------------------------- storage

namespace {
int64_t used_bytes(sqlite3* db) {
    Stmt a(db, "PRAGMA page_count");
    Stmt b(db, "PRAGMA freelist_count");
    Stmt c(db, "PRAGMA page_size");
    if (!a.ok() || !b.ok() || !c.ok() || a.step() != SQLITE_ROW || b.step() != SQLITE_ROW || c.step() != SQLITE_ROW) return 0;
    return (a.int64(0) - b.int64(0)) * c.int64(0);
}
}  // namespace

Store::Usage Store::usage() const {
    std::lock_guard<std::mutex> lock(mu_);
    Usage u;
    u.database_bytes = used_bytes(db_);
    return u;
}

Store::Eviction Store::evict_to_limit(int64_t limit_bytes, int64_t window_start_utc, int64_t keep_message_id) {
    std::lock_guard<std::mutex> lock(mu_);
    Eviction r;
    r.used_after = used_bytes(db_);
    if (limit_bytes <= 0 || r.used_after <= limit_bytes) return r;
    const int64_t target = limit_bytes / 10 * 9;
    // the picture data of the oldest synced messages outside the window go first: they can be loaded again, a message cannot
    {
        std::vector<int64_t> with_pictures;
        {
            Stmt st(db_,
                    "SELECT DISTINCT m.id FROM message m JOIN message_image i ON i.message_id = m.id WHERE i.data IS NOT NULL AND m.local_state = 'synced' AND m.local_error = ''"
                    " AND m.stale = 0 AND m.date_utc < ?1 AND m.id <> ?2 ORDER BY m.date_utc ASC, m.id ASC");
            if (st.ok()) {
                st.bind_int64(1, window_start_utc);
                st.bind_int64(2, keep_message_id);
                while (st.step() == SQLITE_ROW) with_pictures.push_back(st.int64(0));
            }
        }
        for (int64_t id : with_pictures) {
            if (r.used_after <= target) break;
            Transaction tx(db_);
            if (!tx.open) break;
            Stmt d(db_, "UPDATE message_image SET data = NULL, state = 'pending' WHERE message_id = ? AND data IS NOT NULL");
            d.bind_int64(1, id);
            if (!d.ok() || d.step() != SQLITE_DONE) break;
            if (!tx.commit()) break;
            r.used_after = used_bytes(db_);
        }
        if (r.used_after <= target) {
            r.over_limit = r.used_after > limit_bytes;
            return r;
        }
    }
    // the candidates, oldest first; deleted in small steps, each its own transaction, so that an interruption leaves a consistent store
    std::vector<int64_t> candidates;
    {
        Stmt st(db_,
                "SELECT id FROM message WHERE local_state = 'synced' AND local_error = '' AND stale = 0 AND date_utc < ?1 AND id <> ?2"
                " ORDER BY date_utc ASC, id ASC");
        if (st.ok()) {
            st.bind_int64(1, window_start_utc);
            st.bind_int64(2, keep_message_id);
            while (st.step() == SQLITE_ROW) candidates.push_back(st.int64(0));
        }
    }
    size_t next = 0;
    while (r.used_after > target && next < candidates.size()) {
        Transaction tx(db_);
        if (!tx.open) break;
        for (int n = 0; n < 25 && next < candidates.size(); ++n, ++next) {
            Stmt d(db_, "DELETE FROM message WHERE id = ?");   // the search index and the attachment rows follow (trigger, cascade)
            d.bind_int64(1, candidates[next]);
            if (!d.ok() || d.step() != SQLITE_DONE) break;
            ++r.deleted;
            r.used_after = used_bytes(db_);   // the pages freed so far (the count is current inside the transaction)
            if (r.used_after <= target) {
                ++next;
                break;
            }
        }
        if (!tx.commit()) break;
        // folder unread counts follow
        Stmt u(db_, "UPDATE folder SET unread_count = (SELECT count(*) FROM message WHERE folder_id = folder.id AND (flags_local & 1) = 0 AND stale = 0 AND (flags_local & 8) = 0 AND local_state NOT IN ('pending_delete','pending_move'))");
        if (u.ok()) u.step();
        r.used_after = used_bytes(db_);
    }
    r.over_limit = r.used_after > limit_bytes;
    return r;
}

Error Store::trim_to_window(int64_t window_start_utc, int64_t keep_message_id, int* deleted) {
    std::lock_guard<std::mutex> lock(mu_);
    if (deleted) *deleted = 0;
    std::vector<int64_t> candidates;
    {
        Stmt st(db_, "SELECT id FROM message WHERE local_state = 'synced' AND local_error = '' AND stale = 0 AND date_utc < ?1 AND id <> ?2 ORDER BY date_utc ASC, id ASC");
        if (!st.ok()) return storage_error(db_, "read_failed");
        st.bind_int64(1, window_start_utc);
        st.bind_int64(2, keep_message_id);
        while (st.step() == SQLITE_ROW) candidates.push_back(st.int64(0));
    }
    // small steps, each its own transaction: an interruption leaves a consistent store and the rest follows at the next sync
    size_t next = 0;
    while (next < candidates.size()) {
        Transaction tx(db_);
        if (!tx.open) return storage_error(db_, "write_failed");
        int in_step = 0;
        for (; in_step < 50 && next < candidates.size(); ++in_step, ++next) {
            Stmt d(db_, "DELETE FROM message WHERE id = ?");   // the search index and the attachment rows follow (trigger, cascade)
            d.bind_int64(1, candidates[next]);
            if (!d.ok() || d.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
        }
        if (!tx.commit()) return storage_error(db_, "write_failed");
        if (deleted) *deleted += in_step;
    }
    if (!candidates.empty()) {
        Stmt u(db_,
               "UPDATE folder SET older_on_server = 1, unread_count = (SELECT count(*) FROM message WHERE folder_id = folder.id AND (flags_local & 1) = 0 AND stale = 0"
               " AND (flags_local & 8) = 0 AND local_state NOT IN ('pending_delete','pending_move'))");
        if (u.ok()) u.step();
    }
    return Error();
}

}  // namespace pbo
