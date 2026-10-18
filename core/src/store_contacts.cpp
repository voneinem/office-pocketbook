// Store: address books and contacts (specs/002-server-contacts/data-model.md: addressbook, contact, contact_email, contact_conflict). Every public
// method is one short transaction; a failing statement leaves the complete before-state.
#include <sqlite3.h>

#include <algorithm>
#include <cctype>
#include <set>
#include <tuple>
#include <vector>

#include "pbo/store.h"
#include "pbo/utf8.h"
#include "pbo/vcard.h"
#include "store_sql.h"
#include "store_util.h"

namespace pbo {

using sql::Stmt;
using store_util::cap;
using store_util::storage_error;
using store_util::Transaction;

namespace {

AddressBook read_book(const Stmt& st) {
    AddressBook b;
    b.id = st.int64(0);
    b.href = st.text(1);
    b.name = st.text(2);
    b.writable = st.int64(3) != 0;
    b.shown = st.int64(4) != 0;
    b.ctag = st.text(5);
    b.sync_token = st.text(6);
    b.present_on_server = st.int64(7) != 0;
    b.last_sync_at = st.int64(8);
    return b;
}

const char* kBookColumns = "id, href, name, writable, shown, COALESCE(ctag,''), COALESCE(sync_token,''), present_on_server, COALESCE(last_sync_at,0)";

// "%", "_" and "\" are ordinary characters in a query
std::string like_escape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '%' || c == '_' || c == '\\') out += '\\';
        out += c;
    }
    return out;
}

// A word made of the characters of a phone number (digits, + - ( ) . / and spaces) that holds at least one digit: its digits and "+" (the card keeps
// the numbers that way, whatever spaces and dashes they were written with)
std::string phone_digits(const std::string& word) {
    bool digit = false;
    std::string out;
    for (char c : word) {
        if (std::isdigit(static_cast<unsigned char>(c))) {
            digit = true;
            out += c;
        } else if (c == '+') {
            out += c;
        } else if (c != '-' && c != '(' && c != ')' && c != '.' && c != '/') {
            return std::string();
        }
    }
    return digit ? out : std::string();
}

struct Filter {
    std::string where;                 // starts with " WHERE ..."
    std::vector<std::string> binds;    // the LIKE patterns, in order
};

Filter filter_of(const std::string& query) {
    Filter f;
    f.where = " WHERE b.shown = 1 AND c.sync_state <> 'pending_delete'";
    size_t i = 0;
    while (i < query.size()) {
        while (i < query.size() && std::isspace(static_cast<unsigned char>(query[i]))) ++i;
        size_t j = i;
        while (j < query.size() && !std::isspace(static_cast<unsigned char>(query[j]))) ++j;
        if (j > i) {
            const std::string word = query.substr(i, j - i);
            const std::string folded = sort_key(word);
            const std::string digits = phone_digits(word);
            if (!folded.empty() || !digits.empty()) {
                if (!digits.empty()) {
                    f.where += " AND (c.search_text LIKE ? ESCAPE '\\' OR c.search_text LIKE ? ESCAPE '\\')";
                    f.binds.push_back("%" + like_escape(folded) + "%");
                    f.binds.push_back("%" + like_escape(digits) + "%");
                } else {
                    f.where += " AND c.search_text LIKE ? ESCAPE '\\'";
                    f.binds.push_back("%" + like_escape(folded) + "%");
                }
            }
        }
        i = j;
    }
    return f;
}

// the contact_email rows of a contact, rewritten with every write of the contact
bool write_emails(sqlite3* db, int64_t contact_id, const std::vector<ContactEmailRow>& emails) {
    Stmt del(db, "DELETE FROM contact_email WHERE contact_id = ?");
    if (!del.ok()) return false;
    del.bind_int64(1, contact_id);
    if (del.step() != SQLITE_DONE) return false;
    int idx = 0;
    for (const ContactEmailRow& e : emails) {
        Stmt ins(db, "INSERT INTO contact_email(contact_id, idx, addr, addr_key, label) VALUES(?,?,?,?,?)");
        if (!ins.ok()) return false;
        ins.bind_int64(1, contact_id);
        ins.bind_int64(2, idx++);
        ins.bind_text(3, cap(e.addr, 512));
        ins.bind_text(4, cap(e.addr_key, 512));
        ins.bind_text(5, cap(e.label, 200));
        if (ins.step() != SQLITE_DONE) return false;
    }
    return true;
}

// The derived columns, the card text and the email rows of a contact, rewritten together
bool write_columns(sqlite3* db, int64_t id, const std::string& vcf, const ContactFields& f, bool partial, bool unreadable) {
    Stmt up(db,
            "UPDATE contact SET vcf=?, partial=?, unreadable=?, display_name=?, family=?, given=?, org=?, subtitle=?, sort_key=?, search_text=? WHERE id = ?");
    if (!up.ok()) return false;
    up.bind_text(1, vcf);
    up.bind_int64(2, partial ? 1 : 0);
    up.bind_int64(3, unreadable ? 1 : 0);
    up.bind_text(4, cap(f.display_name, 4096));
    up.bind_text(5, cap(f.family, 1024));
    up.bind_text(6, cap(f.given, 1024));
    up.bind_text(7, cap(f.org, 1024));
    up.bind_text(8, cap(f.subtitle, 1024));
    up.bind_text(9, cap(f.sort_key, 2048));
    up.bind_text(10, cap(f.search_text, 16384));
    up.bind_int64(11, id);
    if (up.step() != SQLITE_DONE) return false;
    return write_emails(db, id, f.emails);
}

bool drop_conflict(sqlite3* db, int64_t id) {
    Stmt del(db, "DELETE FROM contact_conflict WHERE contact_id = ?");
    if (!del.ok()) return false;
    del.bind_int64(1, id);
    return del.step() == SQLITE_DONE;
}

bool put_conflict(sqlite3* db, int64_t id, const std::string& server_vcf, const std::string& server_etag, bool deleting, int64_t now) {
    Stmt c(db, "INSERT OR REPLACE INTO contact_conflict(contact_id, server_vcf, server_etag, fetched_at, deleting) VALUES(?,?,?,?,?)");
    if (!c.ok()) return false;
    c.bind_int64(1, id);
    c.bind_text(2, strip_heavy_properties(server_vcf));
    c.bind_text(3, server_etag);
    c.bind_int64(4, now);
    c.bind_int64(5, deleting ? 1 : 0);
    return c.step() == SQLITE_DONE;
}

bool set_state(sqlite3* db, int64_t id, const char* state) {
    Stmt up(db, "UPDATE contact SET sync_state = ?, error_key = '' WHERE id = ?");
    if (!up.ok()) return false;
    up.bind_text(1, state);
    up.bind_int64(2, id);
    return up.step() == SQLITE_DONE;
}

// the contact with local work whose card the server no longer has: the user's version stays, marked; a pending deletion has reached its goal
bool mark_server_deleted(sqlite3* db, int64_t id, const std::string& state) {
    if (state == "pending_delete") {
        Stmt del(db, "DELETE FROM contact WHERE id = ?");
        if (!del.ok()) return false;
        del.bind_int64(1, id);
        return del.step() == SQLITE_DONE;
    }
    if (state == "pending_update" || state == "conflict") return drop_conflict(db, id) && set_state(db, id, "server_deleted");
    return true;   // pending_create (never confirmed), server_deleted (already marked), synced (handled by the caller)
}

// Is the uid used by another contact of the book?
bool uid_taken(sqlite3* db, int64_t book_id, const std::string& uid, int64_t except_id) {
    Stmt st(db, "SELECT 1 FROM contact WHERE addressbook_id = ? AND uid = ? AND id <> ?");
    if (!st.ok()) return false;
    st.bind_int64(1, book_id);
    st.bind_text(2, uid);
    st.bind_int64(3, except_id);
    return st.step() == SQLITE_ROW;
}

}  // namespace

// ---------------------------------------------------------------------------------------------- address books

std::vector<AddressBook> Store::addressbooks() const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<AddressBook> out;
    Stmt st(db_, (std::string("SELECT ") + kBookColumns + " FROM addressbook ORDER BY name, id").c_str());
    if (!st.ok()) return out;
    while (st.step() == SQLITE_ROW) out.push_back(read_book(st));
    return out;
}

Result<int64_t> Store::upsert_addressbook(const AddressBook& book) {
    std::lock_guard<std::mutex> lock(mu_);
    Result<int64_t> out;
    if (book.href.empty()) {
        out.error = make_error(ErrorClass::Storage, "invalid_addressbook", "address missing");
        return out;
    }
    Stmt st(db_,
            "INSERT INTO addressbook(href, name, writable, present_on_server) VALUES(?,?,?,1)"
            " ON CONFLICT(href) DO UPDATE SET name=excluded.name, writable=excluded.writable, present_on_server=1");
    if (!st.ok()) {
        out.error = storage_error(db_, "write_failed");
        return out;
    }
    st.bind_text(1, cap(book.href, 2048));
    st.bind_text(2, cap(book.name.empty() ? book.href : book.name, 200));
    st.bind_int64(3, book.writable ? 1 : 0);
    if (st.step() != SQLITE_DONE) {
        out.error = storage_error(db_, "write_failed");
        return out;
    }
    Stmt id(db_, "SELECT id FROM addressbook WHERE href = ?");
    id.bind_text(1, cap(book.href, 2048));
    if (!id.ok() || id.step() != SQLITE_ROW) {
        out.error = storage_error(db_, "write_failed");
        return out;
    }
    out.value = id.int64(0);
    return out;
}

Error Store::set_addressbook_shown(int64_t addressbook_id, bool shown) {
    std::lock_guard<std::mutex> lock(mu_);
    Stmt st(db_, "UPDATE addressbook SET shown = ? WHERE id = ?");
    if (!st.ok()) return storage_error(db_, "write_failed");
    st.bind_int64(1, shown ? 1 : 0);
    st.bind_int64(2, addressbook_id);
    if (st.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
    if (sqlite3_changes(db_) == 0) return make_error(ErrorClass::Storage, "not_found", "no such address book");
    return {};
}

Error Store::mark_addressbooks_present(const std::set<std::string>& present_hrefs) {
    std::lock_guard<std::mutex> lock(mu_);
    Transaction tx(db_);
    if (!tx.open) return storage_error(db_, "write_failed");
    std::vector<std::pair<int64_t, std::string>> books;
    {
        Stmt st(db_, "SELECT id, href FROM addressbook");
        if (!st.ok()) return storage_error(db_, "write_failed");
        while (st.step() == SQLITE_ROW) books.emplace_back(st.int64(0), st.text(1));
    }
    for (const auto& b : books) {
        if (present_hrefs.count(b.second)) {
            Stmt up(db_, "UPDATE addressbook SET present_on_server = 1 WHERE id = ?");
            if (!up.ok()) return storage_error(db_, "write_failed");
            up.bind_int64(1, b.first);
            if (up.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
            continue;
        }
        // gone on the server: the contacts the server confirmed go; a change waiting for the server makes the contact "deleted on the server" and the
        // book stays (marked) as long as such a contact is left, else the book goes with its contacts
        {
            Stmt rows(db_, "SELECT id, sync_state FROM contact WHERE addressbook_id = ?");
            if (!rows.ok()) return storage_error(db_, "write_failed");
            rows.bind_int64(1, b.first);
            std::vector<std::pair<int64_t, std::string>> list;
            while (rows.step() == SQLITE_ROW) list.emplace_back(rows.int64(0), rows.text(1));
            for (const auto& r : list) {
                bool ok = true;
                if (r.second == "synced") {
                    Stmt del(db_, "DELETE FROM contact WHERE id = ?");
                    ok = del.ok();
                    if (ok) {
                        del.bind_int64(1, r.first);
                        ok = del.step() == SQLITE_DONE;
                    }
                } else {
                    ok = mark_server_deleted(db_, r.first, r.second);
                }
                if (!ok) return storage_error(db_, "write_failed");
            }
        }
        Stmt work(db_, "SELECT count(*) FROM contact WHERE addressbook_id = ? AND sync_state <> 'synced'");
        if (!work.ok()) return storage_error(db_, "write_failed");
        work.bind_int64(1, b.first);
        const bool has_work = work.step() == SQLITE_ROW && work.int64(0) > 0;
        Stmt act(db_, has_work ? "UPDATE addressbook SET present_on_server = 0 WHERE id = ?" : "DELETE FROM addressbook WHERE id = ?");
        if (!act.ok()) return storage_error(db_, "write_failed");
        act.bind_int64(1, b.first);
        if (act.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
    }
    if (!tx.commit()) return storage_error(db_, "write_failed");
    return {};
}

// ---------------------------------------------------------------------------------------------- the pull

std::vector<StoredContactRef> Store::contact_refs(int64_t addressbook_id) const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<StoredContactRef> out;
    Stmt st(db_, "SELECT id, href, COALESCE(etag,''), uid, sync_state, refetch FROM contact WHERE addressbook_id = ?");
    if (!st.ok()) return out;
    st.bind_int64(1, addressbook_id);
    while (st.step() == SQLITE_ROW) {
        StoredContactRef r;
        r.id = st.int64(0);
        r.href = st.text(1);
        r.etag = st.text(2);
        r.uid = st.text(3);
        r.sync_state = st.text(4);
        r.refetch = st.int64(5) != 0;
        out.push_back(std::move(r));
    }
    return out;
}

Error Store::apply_contact_pull(int64_t addressbook_id, const ContactPullBatch& batch) {
    std::lock_guard<std::mutex> lock(mu_);
    Transaction tx(db_);
    if (!tx.open) return storage_error(db_, "write_failed");

    // a removal takes a contact that the server confirmed; a contact with local work stays, marked "deleted on the server" (rule 2)
    auto remove_row = [&](int64_t id, const std::string& state) -> bool {
        if (state == "synced") {
            Stmt del(db_, "DELETE FROM contact WHERE id = ?");
            if (!del.ok()) return false;
            del.bind_int64(1, id);
            return del.step() == SQLITE_DONE;
        }
        return mark_server_deleted(db_, id, state);
    };

    for (const std::string& href : batch.removed_hrefs) {
        Stmt find(db_, "SELECT id, sync_state FROM contact WHERE addressbook_id = ? AND href = ?");
        if (!find.ok()) return storage_error(db_, "write_failed");
        find.bind_int64(1, addressbook_id);
        find.bind_text(2, href);
        if (find.step() == SQLITE_ROW && !remove_row(find.int64(0), find.text(1))) return storage_error(db_, "write_failed");
    }
    if (batch.full) {
        const std::set<std::string> listed(batch.listed_hrefs.begin(), batch.listed_hrefs.end());
        std::vector<std::tuple<int64_t, std::string, std::string>> rows;
        {
            Stmt all(db_, "SELECT id, href, sync_state FROM contact WHERE addressbook_id = ?");
            if (!all.ok()) return storage_error(db_, "write_failed");
            all.bind_int64(1, addressbook_id);
            while (all.step() == SQLITE_ROW) rows.emplace_back(all.int64(0), all.text(1), all.text(2));
        }
        for (const auto& r : rows)
            if (!listed.count(std::get<1>(r)) && !remove_row(std::get<0>(r), std::get<2>(r))) return storage_error(db_, "write_failed");
    }

    for (const ContactUpsert& u : batch.upserts) {
        Stmt find(db_, "SELECT id, sync_state FROM contact WHERE addressbook_id = ? AND href = ?");
        if (!find.ok()) return storage_error(db_, "write_failed");
        find.bind_int64(1, addressbook_id);
        find.bind_text(2, u.href);
        int64_t id = 0;
        if (find.step() == SQLITE_ROW) {
            id = find.int64(0);
            const std::string state = find.text(1);
            if (state == "pending_create") {
                // The card at the path of a contact the server never confirmed is the app's own (its UID was made here): an earlier request was
                // accepted and its answer lost. Adopt it: the same content is simply synced, other content stays waiting as a change.
                Stmt own(db_, "SELECT uid, vcf FROM contact WHERE id = ?");
                if (!own.ok()) return storage_error(db_, "write_failed");
                own.bind_int64(1, id);
                if (own.step() != SQLITE_ROW || own.text(0) != u.uid) continue;   // another card at that path: left alone
                const bool same = same_contact_content(own.text(1), u.vcf);
                Stmt adopt(db_, same ? "UPDATE contact SET etag=?, vcf=?, partial=1, sync_state='synced', error_key='', refetch=0 WHERE id = ?"
                                     : "UPDATE contact SET etag=?, sync_state='pending_update', error_key='' WHERE id = ?");
                if (!adopt.ok()) return storage_error(db_, "write_failed");
                adopt.bind_text(1, u.etag);
                if (same) {
                    adopt.bind_text(2, u.vcf);
                    adopt.bind_int64(3, id);
                } else {
                    adopt.bind_int64(2, id);
                }
                if (adopt.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
                continue;
            }
            if (state == "pending_update" || state == "pending_delete" || state == "conflict") {
                // local work is never overwritten by the pull: a changed server card is kept beside it as a conflict (the same text is no conflict)
                Stmt own(db_, "SELECT COALESCE(etag,''), vcf FROM contact WHERE id = ?");
                if (!own.ok()) return storage_error(db_, "write_failed");
                own.bind_int64(1, id);
                if (own.step() != SQLITE_ROW) continue;
                const std::string base_etag = own.text(0), local_vcf = own.text(1);
                if (state == "conflict") {
                    Stmt kept(db_, "SELECT COALESCE(server_etag,''), deleting FROM contact_conflict WHERE contact_id = ?");
                    if (!kept.ok()) return storage_error(db_, "write_failed");
                    kept.bind_int64(1, id);
                    if (kept.step() == SQLITE_ROW && kept.text(0) != u.etag && !put_conflict(db_, id, u.vcf, u.etag, kept.int64(1) != 0, clock_->now()))
                        return storage_error(db_, "write_failed");   // the server moved on again: the newest server version is the one kept
                    continue;
                }
                if (u.etag == base_etag || u.unreadable) continue;
                if (state == "pending_update" && same_contact_content(local_vcf, u.vcf)) {
                    const ContactFields& sf = u.fields;
                    if (!write_columns(db_, id, u.vcf, sf, true, false)) return storage_error(db_, "write_failed");
                    Stmt ok(db_, "UPDATE contact SET etag = ?, sync_state = 'synced', error_key = '', refetch = 0 WHERE id = ?");
                    if (!ok.ok()) return storage_error(db_, "write_failed");
                    ok.bind_text(1, u.etag);
                    ok.bind_int64(2, id);
                    if (ok.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
                    continue;
                }
                if (!put_conflict(db_, id, u.vcf, u.etag, state == "pending_delete", clock_->now()) || !set_state(db_, id, "conflict"))
                    return storage_error(db_, "write_failed");
                continue;
            }
            if (state != "synced") continue;
        }
        std::string uid = u.uid;
        bool unreadable = u.unreadable;
        if (!unreadable && uid_taken(db_, addressbook_id, uid, id)) {   // a second card with the UID of another one: kept, as unreadable
            uid = "href:" + u.href;
            unreadable = true;
        }
        const ContactFields& f = u.fields;
        if (id == 0) {
            Stmt ins(db_,
                     "INSERT INTO contact(addressbook_id, href, etag, uid, vcf, partial, unreadable, display_name, family, given, org, subtitle, sort_key, search_text)"
                     " VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
            if (!ins.ok()) return storage_error(db_, "write_failed");
            ins.bind_int64(1, addressbook_id);
            ins.bind_text(2, cap(u.href, 2048));
            ins.bind_text(3, u.etag);
            ins.bind_text(4, cap(uid, 2048));
            ins.bind_text(5, u.vcf);
            ins.bind_int64(6, u.partial ? 1 : 0);
            ins.bind_int64(7, unreadable ? 1 : 0);
            ins.bind_text(8, cap(f.display_name, 4096));
            ins.bind_text(9, cap(f.family, 1024));
            ins.bind_text(10, cap(f.given, 1024));
            ins.bind_text(11, cap(f.org, 1024));
            ins.bind_text(12, cap(f.subtitle, 1024));
            ins.bind_text(13, cap(f.sort_key, 2048));
            ins.bind_text(14, cap(f.search_text, 16384));
            if (ins.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
            id = sqlite3_last_insert_rowid(db_);
        } else {
            Stmt up(db_,
                    "UPDATE contact SET etag=?, uid=?, vcf=?, partial=?, unreadable=?, display_name=?, family=?, given=?, org=?, subtitle=?, sort_key=?, search_text=?,"
                    " refetch=0, error_key='' WHERE id = ?");
            if (!up.ok()) return storage_error(db_, "write_failed");
            up.bind_text(1, u.etag);
            up.bind_text(2, cap(uid, 2048));
            up.bind_text(3, u.vcf);
            up.bind_int64(4, u.partial ? 1 : 0);
            up.bind_int64(5, unreadable ? 1 : 0);
            up.bind_text(6, cap(f.display_name, 4096));
            up.bind_text(7, cap(f.family, 1024));
            up.bind_text(8, cap(f.given, 1024));
            up.bind_text(9, cap(f.org, 1024));
            up.bind_text(10, cap(f.subtitle, 1024));
            up.bind_text(11, cap(f.sort_key, 2048));
            up.bind_text(12, cap(f.search_text, 16384));
            up.bind_int64(13, id);
            if (up.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
        }
        if (!write_emails(db_, id, f.emails)) return storage_error(db_, "write_failed");
    }

    // the cursor, in the same transaction; a field that the batch leaves empty keeps its value
    Stmt cur(db_,
             "UPDATE addressbook SET ctag = CASE WHEN ?1 <> '' THEN ?1 ELSE ctag END, sync_token = CASE WHEN ?2 <> '' THEN ?2 ELSE sync_token END,"
             " last_sync_at = CASE WHEN ?3 > 0 THEN ?3 ELSE last_sync_at END WHERE id = ?4");
    if (!cur.ok()) return storage_error(db_, "write_failed");
    cur.bind_text(1, batch.ctag);
    cur.bind_text(2, batch.sync_token);
    cur.bind_int64(3, batch.synced_at);
    cur.bind_int64(4, addressbook_id);
    if (cur.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
    if (!tx.commit()) return storage_error(db_, "write_failed");
    return {};
}

// ---------------------------------------------------------------------------------------------- reading

std::vector<ContactListItem> Store::contacts_page(const std::string& query, int offset, int limit) const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<ContactListItem> out;
    limit = std::min(limit, 100);
    if (limit <= 0) return out;
    if (offset < 0) offset = 0;
    const Filter f = filter_of(query);
    const std::string sql =
        "SELECT c.id, c.display_name, c.subtitle, c.sync_state, c.error_key, c.addressbook_id, b.name, c.unreadable FROM contact c JOIN addressbook b ON b.id = c.addressbook_id" +
        f.where + " ORDER BY c.sort_key, c.id LIMIT ? OFFSET ?";
    Stmt st(db_, sql.c_str());
    if (!st.ok()) return out;
    int i = 1;
    for (const std::string& bind : f.binds) st.bind_text(i++, bind);
    st.bind_int64(i++, limit);
    st.bind_int64(i, offset);
    while (st.step() == SQLITE_ROW) {
        ContactListItem it;
        it.id = st.int64(0);
        it.display_name = st.text(1);
        it.subtitle = st.text(2);
        it.sync_state = st.text(3);
        it.error_key = st.text(4);
        it.addressbook_id = st.int64(5);
        it.addressbook_name = st.text(6);
        it.unreadable = st.int64(7) != 0;
        out.push_back(std::move(it));
    }
    return out;
}

int Store::contacts_count(const std::string& query) const {
    std::lock_guard<std::mutex> lock(mu_);
    const Filter f = filter_of(query);
    const std::string sql = "SELECT count(*) FROM contact c JOIN addressbook b ON b.id = c.addressbook_id" + f.where;
    Stmt st(db_, sql.c_str());
    if (!st.ok()) return 0;
    int i = 1;
    for (const std::string& bind : f.binds) st.bind_text(i++, bind);
    return st.step() == SQLITE_ROW ? static_cast<int>(st.int64(0)) : 0;
}

std::optional<ContactView> Store::contact(int64_t contact_id) const {
    std::lock_guard<std::mutex> lock(mu_);
    Stmt st(db_,
            "SELECT c.id, c.addressbook_id, b.name, b.writable, b.present_on_server, b.shown, c.href, COALESCE(c.etag,''), c.uid, c.vcf, c.partial, c.unreadable,"
            " c.display_name, c.family, c.given, c.org, c.sync_state, c.undo_state, c.error_key FROM contact c JOIN addressbook b ON b.id = c.addressbook_id WHERE c.id = ?");
    if (!st.ok()) return std::nullopt;
    st.bind_int64(1, contact_id);
    if (st.step() != SQLITE_ROW) return std::nullopt;
    ContactView v;
    v.id = st.int64(0);
    v.addressbook_id = st.int64(1);
    v.addressbook_name = st.text(2);
    v.addressbook_writable = st.int64(3) != 0;
    v.addressbook_present = st.int64(4) != 0;
    v.addressbook_shown = st.int64(5) != 0;
    v.href = st.text(6);
    v.etag = st.text(7);
    v.uid = st.text(8);
    v.vcf = st.text(9);
    v.partial = st.int64(10) != 0;
    v.unreadable = st.int64(11) != 0;
    v.display_name = st.text(12);
    v.family = st.text(13);
    v.given = st.text(14);
    v.org = st.text(15);
    v.sync_state = st.text(16);
    v.undo_state = st.text(17);
    v.error_key = st.text(18);
    if (!v.unreadable && !v.vcf.empty()) v.card = parse_vcard(v.vcf);
    return v;
}

// ---------------------------------------------------------------------------------------------- contacts made on the reader

Result<int64_t> Store::save_contact_local(int64_t addressbook_id, int64_t contact_id, const ContactUpsert& record) {
    std::lock_guard<std::mutex> lock(mu_);
    Result<int64_t> out;
    Transaction tx(db_);
    if (!tx.open) {
        out.error = storage_error(db_, "write_failed");
        return out;
    }
    if (contact_id != 0) {
        Stmt row(db_, "SELECT c.sync_state, c.unreadable, b.writable FROM contact c JOIN addressbook b ON b.id = c.addressbook_id WHERE c.id = ?");
        if (!row.ok()) {
            out.error = storage_error(db_, "write_failed");
            return out;
        }
        row.bind_int64(1, contact_id);
        if (row.step() != SQLITE_ROW) {
            out.error = make_error(ErrorClass::Storage, "not_found", "no such contact");
            return out;
        }
        const std::string state = row.text(0);
        if (row.int64(1) != 0) out.error = make_error(ErrorClass::Storage, "unreadable", "the card cannot be read");
        else if (row.int64(2) == 0) out.error = make_error(ErrorClass::Storage, "read_only", "the address book does not accept changes");
        else if (state == "conflict") out.error = make_error(ErrorClass::Storage, "in_conflict", "decide the conflict first");
        else if (state == "pending_delete") out.error = make_error(ErrorClass::Storage, "delete_pending", "the contact is about to be deleted");
        if (!out.ok()) return out;
        // (partial stays as stored: the text is still the stripped card and the push merges it into the server's full card)
        Stmt cur(db_, "SELECT partial FROM contact WHERE id = ?");
        cur.bind_int64(1, contact_id);
        const bool partial = cur.ok() && cur.step() == SQLITE_ROW && cur.int64(0) != 0;
        if (!write_columns(db_, contact_id, record.vcf, record.fields, partial, false) ||
            !set_state(db_, contact_id, state == "synced" ? "pending_update" : state.c_str()) || !tx.commit()) {
            out.error = storage_error(db_, "write_failed");
            return out;
        }
        out.value = contact_id;
        return out;
    }
    Stmt book(db_, "SELECT writable, present_on_server FROM addressbook WHERE id = ?");
    if (!book.ok()) {
        out.error = storage_error(db_, "write_failed");
        return out;
    }
    book.bind_int64(1, addressbook_id);
    if (book.step() != SQLITE_ROW || book.int64(1) == 0) {
        out.error = make_error(ErrorClass::Storage, "not_found", "no such address book");
        return out;
    }
    if (book.int64(0) == 0) {
        out.error = make_error(ErrorClass::Storage, "read_only", "the address book does not accept changes");
        return out;
    }
    if (uid_taken(db_, addressbook_id, record.uid, 0)) {
        out.error = make_error(ErrorClass::Storage, "duplicate_uid", "a contact with this identity exists");
        return out;
    }
    const ContactFields& f = record.fields;
    Stmt ins(db_,
             "INSERT INTO contact(addressbook_id, href, etag, uid, vcf, partial, unreadable, display_name, family, given, org, subtitle, sort_key, search_text, sync_state)"
             " VALUES(?,?,NULL,?,?,?,0,?,?,?,?,?,?,?,'pending_create')");
    if (!ins.ok()) {
        out.error = storage_error(db_, "write_failed");
        return out;
    }
    ins.bind_int64(1, addressbook_id);
    ins.bind_text(2, cap(record.href, 2048));
    ins.bind_text(3, cap(record.uid, 2048));
    ins.bind_text(4, record.vcf);
    ins.bind_int64(5, record.partial ? 1 : 0);
    ins.bind_text(6, cap(f.display_name, 4096));
    ins.bind_text(7, cap(f.family, 1024));
    ins.bind_text(8, cap(f.given, 1024));
    ins.bind_text(9, cap(f.org, 1024));
    ins.bind_text(10, cap(f.subtitle, 1024));
    ins.bind_text(11, cap(f.sort_key, 2048));
    ins.bind_text(12, cap(f.search_text, 16384));
    if (ins.step() != SQLITE_DONE) {
        out.error = storage_error(db_, "write_failed");
        return out;
    }
    const int64_t id = sqlite3_last_insert_rowid(db_);
    if (!write_emails(db_, id, f.emails) || !tx.commit()) {
        out.error = storage_error(db_, "write_failed");
        return out;
    }
    out.value = id;
    return out;
}

std::vector<PendingContact> Store::pending_contacts() const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<PendingContact> out;
    Stmt st(db_,
            "SELECT c.id, c.addressbook_id, b.href, b.writable, b.present_on_server, c.href, COALESCE(c.etag,''), c.uid, c.vcf, c.display_name, c.partial, c.sync_state, c.error_key"
            " FROM contact c JOIN addressbook b ON b.id = c.addressbook_id WHERE c.sync_state IN ('pending_create','pending_update','pending_delete') ORDER BY c.id");
    if (!st.ok()) return out;
    while (st.step() == SQLITE_ROW) {
        PendingContact p;
        p.id = st.int64(0);
        p.addressbook_id = st.int64(1);
        p.addressbook_href = st.text(2);
        p.addressbook_writable = st.int64(3) != 0;
        p.addressbook_present = st.int64(4) != 0;
        p.href = st.text(5);
        p.etag = st.text(6);
        p.uid = st.text(7);
        p.vcf = st.text(8);
        p.display_name = st.text(9);
        p.partial = st.int64(10) != 0;
        p.sync_state = st.text(11);
        p.error_key = st.text(12);
        out.push_back(std::move(p));
    }
    return out;
}

int Store::pending_contact_count() const {
    std::lock_guard<std::mutex> lock(mu_);
    Stmt st(db_, "SELECT count(*) FROM contact WHERE sync_state IN ('pending_create','pending_update','pending_delete')");
    return st.ok() && st.step() == SQLITE_ROW ? static_cast<int>(st.int64(0)) : 0;
}

Error Store::contact_pushed(int64_t contact_id, const std::string& pushed_vcf, const std::string& href, const std::string& etag) {
    std::lock_guard<std::mutex> lock(mu_);
    Stmt cur(db_, "SELECT vcf, sync_state FROM contact WHERE id = ?");
    if (!cur.ok()) return storage_error(db_, "write_failed");
    cur.bind_int64(1, contact_id);
    if (cur.step() != SQLITE_ROW) return make_error(ErrorClass::Storage, "not_found", "no such contact");
    const std::string state = cur.text(1);
    if (state != "pending_create" && state != "pending_update") return {};   // nothing waits (or a deletion does): not this call's business
    const std::string next = cur.text(0) == pushed_vcf ? "synced" : "pending_update";   // edited since the request went out: stays waiting
    Stmt up(db_, "UPDATE contact SET etag = ?, href = ?, sync_state = ?, error_key = '', refetch = 0 WHERE id = ?");
    if (!up.ok()) return storage_error(db_, "write_failed");
    store_util::bind_text_or_null(up, 1, etag);
    up.bind_text(2, cap(href, 2048));
    up.bind_text(3, next);
    up.bind_int64(4, contact_id);
    if (up.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
    return {};
}

Error Store::contact_refused(int64_t contact_id, const std::string& error_key) {
    std::lock_guard<std::mutex> lock(mu_);
    if (error_key != "read_only" && error_key != "too_large" && error_key != "refused" && error_key != "storage_full_server")
        return make_error(ErrorClass::Storage, "invalid_input", "unknown refusal");
    Stmt up(db_, "UPDATE contact SET error_key = ? WHERE id = ? AND sync_state IN ('pending_create','pending_update','pending_delete')");
    if (!up.ok()) return storage_error(db_, "write_failed");
    up.bind_text(1, error_key);
    up.bind_int64(2, contact_id);
    if (up.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
    if (sqlite3_changes(db_) == 0) return make_error(ErrorClass::Storage, "not_found", "no such waiting contact");
    return {};
}

Error Store::clear_contact_error(int64_t contact_id) {
    std::lock_guard<std::mutex> lock(mu_);
    Stmt up(db_, "UPDATE contact SET error_key = '' WHERE id = ?");
    if (!up.ok()) return storage_error(db_, "write_failed");
    up.bind_int64(1, contact_id);
    if (up.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
    if (sqlite3_changes(db_) == 0) return make_error(ErrorClass::Storage, "not_found", "no such contact");
    return {};
}

Error Store::discard_contact_change(int64_t contact_id) {
    std::lock_guard<std::mutex> lock(mu_);
    Stmt cur(db_, "SELECT sync_state FROM contact WHERE id = ?");
    if (!cur.ok()) return storage_error(db_, "write_failed");
    cur.bind_int64(1, contact_id);
    if (cur.step() != SQLITE_ROW) return make_error(ErrorClass::Storage, "not_found", "no such contact");
    const std::string state = cur.text(0);
    if (state == "pending_create") {
        Stmt del(db_, "DELETE FROM contact WHERE id = ?");
        if (!del.ok()) return storage_error(db_, "write_failed");
        del.bind_int64(1, contact_id);
        if (del.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
        return {};
    }
    if (state == "pending_update") {   // back to the server's version: the next pull downloads it
        Stmt up(db_, "UPDATE contact SET sync_state = 'synced', refetch = 1, error_key = '' WHERE id = ?");
        if (!up.ok()) return storage_error(db_, "write_failed");
        up.bind_int64(1, contact_id);
        if (up.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
        return {};
    }
    return make_error(ErrorClass::Storage, "not_failed", "this contact has no waiting change");
}

// ---------------------------------------------------------------------------------------------- deleting

Error Store::request_contact_delete(int64_t contact_id) {
    std::lock_guard<std::mutex> lock(mu_);
    Transaction tx(db_);
    if (!tx.open) return storage_error(db_, "write_failed");
    Stmt row(db_, "SELECT c.sync_state, b.writable FROM contact c JOIN addressbook b ON b.id = c.addressbook_id WHERE c.id = ?");
    if (!row.ok()) return storage_error(db_, "write_failed");
    row.bind_int64(1, contact_id);
    if (row.step() != SQLITE_ROW) return make_error(ErrorClass::Storage, "not_found", "no such contact");
    const std::string state = row.text(0);
    const bool writable = row.int64(1) != 0;
    if (state == "conflict") return make_error(ErrorClass::Storage, "in_conflict", "decide the conflict first");
    if (state == "pending_delete") return {};
    if (state == "pending_create" || state == "server_deleted") {   // the server has no card (any more): nothing to tell it
        Stmt del(db_, "DELETE FROM contact WHERE id = ?");
        if (!del.ok()) return storage_error(db_, "write_failed");
        del.bind_int64(1, contact_id);
        if (del.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
        return tx.commit() ? Error() : storage_error(db_, "write_failed");
    }
    if (!writable) return make_error(ErrorClass::Storage, "read_only", "the address book does not accept changes");
    Stmt up(db_, "UPDATE contact SET sync_state = 'pending_delete', undo_state = ?, error_key = '' WHERE id = ?");
    if (!up.ok()) return storage_error(db_, "write_failed");
    up.bind_text(1, state);
    up.bind_int64(2, contact_id);
    if (up.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
    return tx.commit() ? Error() : storage_error(db_, "write_failed");
}

Error Store::undo_contact_delete(int64_t contact_id) {
    std::lock_guard<std::mutex> lock(mu_);
    Stmt row(db_, "SELECT sync_state, undo_state FROM contact WHERE id = ?");
    if (!row.ok()) return storage_error(db_, "write_failed");
    row.bind_int64(1, contact_id);
    if (row.step() != SQLITE_ROW) return make_error(ErrorClass::Storage, "not_found", "no such contact");
    const std::string undo = row.text(1);
    if (row.text(0) != "pending_delete" || (undo != "synced" && undo != "pending_update")) return make_error(ErrorClass::Storage, "nothing_to_undo", "no deletion waits");
    Stmt up(db_, "UPDATE contact SET sync_state = ?, undo_state = '', error_key = '' WHERE id = ?");
    if (!up.ok()) return storage_error(db_, "write_failed");
    up.bind_text(1, undo);
    up.bind_int64(2, contact_id);
    if (up.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
    return {};
}

std::vector<ContactListItem> Store::deleted_contacts(int limit) const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<ContactListItem> out;
    if (limit <= 0) return out;
    Stmt st(db_,
            "SELECT c.id, c.display_name, c.subtitle, c.sync_state, c.error_key, c.addressbook_id, b.name, c.unreadable FROM contact c JOIN addressbook b ON b.id = c.addressbook_id"
            " WHERE c.sync_state = 'pending_delete' ORDER BY c.id DESC LIMIT ?");
    if (!st.ok()) return out;
    st.bind_int64(1, limit);
    while (st.step() == SQLITE_ROW) {
        ContactListItem it;
        it.id = st.int64(0);
        it.display_name = st.text(1);
        it.subtitle = st.text(2);
        it.sync_state = st.text(3);
        it.error_key = st.text(4);
        it.addressbook_id = st.int64(5);
        it.addressbook_name = st.text(6);
        it.unreadable = st.int64(7) != 0;
        out.push_back(std::move(it));
    }
    return out;
}

// ---------------------------------------------------------------------------------------------- conflicts and removals

Error Store::contact_conflicted(int64_t contact_id, const std::string& server_vcf, const std::string& server_etag, const std::string& href) {
    std::lock_guard<std::mutex> lock(mu_);
    Transaction tx(db_);
    if (!tx.open) return storage_error(db_, "write_failed");
    Stmt row(db_, "SELECT sync_state FROM contact WHERE id = ?");
    if (!row.ok()) return storage_error(db_, "write_failed");
    row.bind_int64(1, contact_id);
    if (row.step() != SQLITE_ROW) return make_error(ErrorClass::Storage, "not_found", "no such contact");
    const std::string state = row.text(0);
    if (state != "pending_update" && state != "pending_delete") return make_error(ErrorClass::Storage, "bad_state", "the contact has no change waiting");
    (void)href;   // an existing contact keeps its path
    if (!put_conflict(db_, contact_id, server_vcf, server_etag, state == "pending_delete", clock_->now()) || !set_state(db_, contact_id, "conflict"))
        return storage_error(db_, "write_failed");
    return tx.commit() ? Error() : storage_error(db_, "write_failed");
}

Error Store::contact_gone_on_server(int64_t contact_id) {
    std::lock_guard<std::mutex> lock(mu_);
    Transaction tx(db_);
    if (!tx.open) return storage_error(db_, "write_failed");
    Stmt row(db_, "SELECT sync_state FROM contact WHERE id = ?");
    if (!row.ok()) return storage_error(db_, "write_failed");
    row.bind_int64(1, contact_id);
    if (row.step() != SQLITE_ROW) return tx.commit() ? Error() : storage_error(db_, "write_failed");
    if (!mark_server_deleted(db_, contact_id, row.text(0))) return storage_error(db_, "write_failed");
    return tx.commit() ? Error() : storage_error(db_, "write_failed");
}

Error Store::contact_removed_on_server(int64_t contact_id) {
    std::lock_guard<std::mutex> lock(mu_);
    Stmt del(db_, "DELETE FROM contact WHERE id = ? AND sync_state = 'pending_delete'");
    if (!del.ok()) return storage_error(db_, "write_failed");
    del.bind_int64(1, contact_id);
    if (del.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
    return {};
}

std::vector<ContactProblem> Store::contact_problems() const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<ContactProblem> out;
    Stmt st(db_,
            "SELECT c.id, c.sync_state, c.display_name, b.name, COALESCE(k.deleting,0), COALESCE(k.server_vcf,''), COALESCE(k.server_etag,'')"
            " FROM contact c JOIN addressbook b ON b.id = c.addressbook_id LEFT JOIN contact_conflict k ON k.contact_id = c.id"
            " WHERE c.sync_state IN ('conflict','server_deleted') ORDER BY c.id");
    if (!st.ok()) return out;
    while (st.step() == SQLITE_ROW) {
        ContactProblem p;
        p.contact_id = st.int64(0);
        p.state = st.text(1);
        p.display_name = st.text(2);
        p.addressbook_name = st.text(3);
        p.deleting = p.state == "conflict" && st.int64(4) != 0;
        p.server_vcf = st.text(5);
        p.server_etag = st.text(6);
        out.push_back(std::move(p));
    }
    return out;
}

namespace {

struct ConflictRow {
    bool found = false, in_conflict = false, deleting = false;
    int64_t book_id = 0;
    std::string server_etag;
};

ConflictRow load_conflict(sqlite3* db, int64_t id) {
    ConflictRow r;
    Stmt st(db, "SELECT c.sync_state, c.addressbook_id, COALESCE(k.server_etag,''), COALESCE(k.deleting,0), k.contact_id IS NOT NULL FROM contact c LEFT JOIN contact_conflict k ON k.contact_id = c.id WHERE c.id = ?");
    if (!st.ok()) return r;
    st.bind_int64(1, id);
    if (st.step() != SQLITE_ROW) return r;
    r.found = true;
    r.in_conflict = st.text(0) == "conflict" && st.int64(4) != 0;
    r.book_id = st.int64(1);
    r.server_etag = st.text(2);
    r.deleting = st.int64(3) != 0;
    return r;
}

// the row takes the server's version: card, derived columns, ETag; synced
bool take_server_version(sqlite3* db, int64_t id, const ContactUpsert& server) {
    if (!write_columns(db, id, server.vcf, server.fields, server.partial, server.unreadable)) return false;
    Stmt up(db, "UPDATE contact SET etag = ?, sync_state = 'synced', error_key = '', refetch = 0 WHERE id = ?");
    if (!up.ok()) return false;
    up.bind_text(1, server.etag);
    up.bind_int64(2, id);
    return up.step() == SQLITE_DONE && drop_conflict(db, id);
}

}  // namespace

Error Store::keep_my_contact(int64_t contact_id) {
    std::lock_guard<std::mutex> lock(mu_);
    Transaction tx(db_);
    if (!tx.open) return storage_error(db_, "write_failed");
    const ConflictRow row = load_conflict(db_, contact_id);
    if (!row.found) return make_error(ErrorClass::Storage, "not_found", "no such contact");
    if (!row.in_conflict) return make_error(ErrorClass::Storage, "not_in_conflict", "the contact is not in conflict");
    Stmt up(db_, "UPDATE contact SET etag = ?, sync_state = ?, error_key = '' WHERE id = ?");
    if (!up.ok()) return storage_error(db_, "write_failed");
    store_util::bind_text_or_null(up, 1, row.server_etag);
    up.bind_text(2, row.deleting ? "pending_delete" : "pending_update");
    up.bind_int64(3, contact_id);
    if (up.step() != SQLITE_DONE || !drop_conflict(db_, contact_id)) return storage_error(db_, "write_failed");
    return tx.commit() ? Error() : storage_error(db_, "write_failed");
}

Error Store::keep_server_contact(int64_t contact_id, const ContactUpsert& server) {
    std::lock_guard<std::mutex> lock(mu_);
    Transaction tx(db_);
    if (!tx.open) return storage_error(db_, "write_failed");
    const ConflictRow row = load_conflict(db_, contact_id);
    if (!row.found) return make_error(ErrorClass::Storage, "not_found", "no such contact");
    if (!row.in_conflict) return make_error(ErrorClass::Storage, "not_in_conflict", "the contact is not in conflict");
    if (!take_server_version(db_, contact_id, server)) return storage_error(db_, "write_failed");
    return tx.commit() ? Error() : storage_error(db_, "write_failed");
}

Error Store::keep_both_contacts(int64_t contact_id, const ContactUpsert& server, const ContactUpsert& mine_copy) {
    std::lock_guard<std::mutex> lock(mu_);
    Transaction tx(db_);
    if (!tx.open) return storage_error(db_, "write_failed");
    const ConflictRow row = load_conflict(db_, contact_id);
    if (!row.found) return make_error(ErrorClass::Storage, "not_found", "no such contact");
    if (!row.in_conflict) return make_error(ErrorClass::Storage, "not_in_conflict", "the contact is not in conflict");
    if (!row.deleting) {   // with a deletion on the user's side there is no version of theirs to keep
        if (mine_copy.uid.empty() || mine_copy.href.empty() || uid_taken(db_, row.book_id, mine_copy.uid, 0))
            return make_error(ErrorClass::Storage, "duplicate_uid", "the copy needs a new identity");
        Stmt ins(db_,
                 "INSERT INTO contact(addressbook_id, href, etag, uid, vcf, partial, unreadable, sync_state) VALUES(?,?,NULL,?,?,?,0,'pending_create')");
        if (!ins.ok()) return storage_error(db_, "write_failed");
        ins.bind_int64(1, row.book_id);
        ins.bind_text(2, cap(mine_copy.href, 2048));
        ins.bind_text(3, cap(mine_copy.uid, 2048));
        ins.bind_text(4, mine_copy.vcf);
        ins.bind_int64(5, mine_copy.partial ? 1 : 0);
        if (ins.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
        const int64_t copy = sqlite3_last_insert_rowid(db_);
        if (!write_columns(db_, copy, mine_copy.vcf, mine_copy.fields, mine_copy.partial, false)) return storage_error(db_, "write_failed");
    }
    if (!take_server_version(db_, contact_id, server)) return storage_error(db_, "write_failed");
    return tx.commit() ? Error() : storage_error(db_, "write_failed");
}

Error Store::recreate_contact(int64_t contact_id, const std::string& new_href) {
    std::lock_guard<std::mutex> lock(mu_);
    Transaction tx(db_);
    if (!tx.open) return storage_error(db_, "write_failed");
    Stmt row(db_, "SELECT c.sync_state, b.writable, b.present_on_server FROM contact c JOIN addressbook b ON b.id = c.addressbook_id WHERE c.id = ?");
    if (!row.ok()) return storage_error(db_, "write_failed");
    row.bind_int64(1, contact_id);
    if (row.step() != SQLITE_ROW) return make_error(ErrorClass::Storage, "not_found", "no such contact");
    if (row.text(0) != "server_deleted") return make_error(ErrorClass::Storage, "not_server_deleted", "the server still has the contact");
    if (row.int64(2) == 0) return make_error(ErrorClass::Storage, "book_gone", "the address book is no longer on the server");
    if (row.int64(1) == 0) return make_error(ErrorClass::Storage, "read_only", "the address book does not accept changes");
    if (new_href.empty()) return make_error(ErrorClass::Storage, "invalid_input", "no path");
    Stmt up(db_, "UPDATE contact SET sync_state = 'pending_create', href = ?, etag = NULL, error_key = '' WHERE id = ?");
    if (!up.ok()) return storage_error(db_, "write_failed");
    up.bind_text(1, cap(new_href, 2048));
    up.bind_int64(2, contact_id);
    if (up.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
    return tx.commit() ? Error() : storage_error(db_, "write_failed");
}

Error Store::discard_contact(int64_t contact_id) {
    std::lock_guard<std::mutex> lock(mu_);
    Transaction tx(db_);
    if (!tx.open) return storage_error(db_, "write_failed");
    Stmt row(db_, "SELECT sync_state FROM contact WHERE id = ?");
    if (!row.ok()) return storage_error(db_, "write_failed");
    row.bind_int64(1, contact_id);
    if (row.step() != SQLITE_ROW) return make_error(ErrorClass::Storage, "not_found", "no such contact");
    if (row.text(0) != "server_deleted") return make_error(ErrorClass::Storage, "not_server_deleted", "the server still has the contact");
    Stmt del(db_, "DELETE FROM contact WHERE id = ?");
    if (!del.ok()) return storage_error(db_, "write_failed");
    del.bind_int64(1, contact_id);
    if (del.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
    return tx.commit() ? Error() : storage_error(db_, "write_failed");
}

}  // namespace pbo
