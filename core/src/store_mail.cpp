// Store: the mail account, folders, message headers and the sync state (data-model.md). Every public method is one
// short transaction; a failing statement leaves the complete before-state.
#include <sqlite3.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>

#include "pbo/account_input.h"
#include "pbo/log.h"
#include "pbo/mime.h"
#include "pbo/mail_roles.h"
#include "pbo/store.h"
#include "store_sql.h"
#include "store_util.h"

namespace pbo {

using sql::Stmt;
using store_util::bind_opt;
using store_util::cap;
using store_util::opt_col;
using store_util::storage_error;
using store_util::Transaction;

namespace {

// Limits on text from the network (data-model.md: header text capped on insert).
constexpr size_t kMaxSubject = 998;
constexpr size_t kMaxAddressList = 8 * 1024;
constexpr int64_t kMinDate = 0;
constexpr int64_t kMaxDate = 4102444800;   // 2100-01-01

constexpr size_t kMaxBodyText = 2u << 20;

// ["a","b"] with the JSON escapes; only what this app writes and reads back
std::string links_to_json(const std::vector<std::string>& links) {
    std::string out = "[";
    for (size_t i = 0; i < links.size(); ++i) {
        out += i ? ",\"" : "\"";
        for (unsigned char c : links[i]) {
            if (c == '"' || c == '\\') { out += '\\'; out += static_cast<char>(c); }
            else if (c < 0x20) { char buf[8]; std::snprintf(buf, sizeof buf, "\\u%04x", c); out += buf; }
            else out += static_cast<char>(c);
        }
        out += '"';
    }
    return out + "]";
}

std::vector<std::string> links_from_json(const std::string& j) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < j.size()) {
        if (j[i] != '"') { ++i; continue; }
        std::string v;
        for (++i; i < j.size() && j[i] != '"'; ++i) {
            if (j[i] == '\\' && i + 1 < j.size()) {
                ++i;
                if (j[i] == 'u' && i + 4 < j.size()) { v += static_cast<char>(std::strtol(j.substr(i + 1, 4).c_str(), nullptr, 16)); i += 4; }
                else v += j[i];
            } else {
                v += j[i];
            }
        }
        out.push_back(std::move(v));
        ++i;
    }
    return out;
}

int64_t clamp_date(int64_t v) { return std::max(kMinDate, std::min(kMaxDate, v)); }

const char* security_text(Security s) { return s == Security::StartTls ? "starttls" : "tls"; }
Security security_from(const std::string& s) { return s == "starttls" ? Security::StartTls : Security::Tls; }

bool not_selectable(const FolderListItem& item) {
    for (const std::string& a : item.attributes) {
        std::string u;
        for (char c : a) u += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        if (u == "\\NOSELECT" || u == "\\NONEXISTENT") return true;
    }
    return false;
}

}  // namespace

// ---------------------------------------------------------------------------------------------- account

Error Store::save_mail_account(const MailAccount& a) {
    std::lock_guard<std::mutex> lock(mu_);
    const auto errors = validate_mail_account(a);
    if (!errors.empty()) return make_error(ErrorClass::Storage, "invalid_account", errors.front().field + ": " + errors.front().key);
    Stmt st(db_,
            "INSERT INTO account_mail(id, imap_host, imap_port, imap_security, smtp_host, smtp_port, smtp_security, username, smtp_username,"
            " address, display_name, sent_by_server, sync_days, attachment_limit, storage_limit, sent_folder_id, drafts_folder_id,"
            " trash_folder_id, capabilities, created) VALUES(1,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)"
            " ON CONFLICT(id) DO UPDATE SET imap_host=excluded.imap_host, imap_port=excluded.imap_port,"
            " imap_security=excluded.imap_security, smtp_host=excluded.smtp_host, smtp_port=excluded.smtp_port,"
            " smtp_security=excluded.smtp_security, username=excluded.username, smtp_username=excluded.smtp_username,"
            " address=excluded.address, display_name=excluded.display_name, sent_by_server=excluded.sent_by_server,"
            " sync_days=excluded.sync_days, attachment_limit=excluded.attachment_limit, storage_limit=excluded.storage_limit,"
            " sent_folder_id=excluded.sent_folder_id, drafts_folder_id=excluded.drafts_folder_id,"
            " trash_folder_id=excluded.trash_folder_id, capabilities=excluded.capabilities");
    if (!st.ok()) return storage_error(db_, "write_failed");
    int i = 1;
    st.bind_text(i++, a.imap_host);
    st.bind_int64(i++, a.imap_port);
    st.bind_text(i++, security_text(a.imap_security));
    st.bind_text(i++, a.smtp_host);
    st.bind_int64(i++, a.smtp_port);
    st.bind_text(i++, security_text(a.smtp_security));
    st.bind_text(i++, a.username);
    if (a.smtp_username.empty()) st.bind_null(i++);
    else st.bind_text(i++, a.smtp_username);
    st.bind_text(i++, a.address);
    st.bind_text(i++, a.display_name);
    st.bind_int64(i++, a.sent_by_server ? 1 : 0);
    st.bind_int64(i++, a.sync_days);
    st.bind_int64(i++, a.attachment_limit);
    st.bind_int64(i++, a.storage_limit);
    for (int64_t id : {a.sent_folder_id, a.drafts_folder_id, a.trash_folder_id}) {
        if (id) st.bind_int64(i++, id);
        else st.bind_null(i++);
    }
    st.bind_text(i++, a.capabilities);
    st.bind_int64(i++, a.created ? a.created : clock_->now());
    if (st.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
    return {};
}

std::optional<MailAccount> Store::mail_account() const {
    std::lock_guard<std::mutex> lock(mu_);
    Stmt st(db_,
            "SELECT imap_host, imap_port, imap_security, smtp_host, smtp_port, smtp_security, username, smtp_username, address,"
            " display_name, sent_by_server, sync_days, attachment_limit, storage_limit, sent_folder_id, drafts_folder_id,"
            " trash_folder_id, capabilities, created FROM account_mail WHERE id = 1");
    if (!st.ok() || st.step() != SQLITE_ROW) return std::nullopt;
    MailAccount a;
    int i = 0;
    a.imap_host = st.text(i++);
    a.imap_port = static_cast<int>(st.int64(i++));
    a.imap_security = security_from(st.text(i++));
    a.smtp_host = st.text(i++);
    a.smtp_port = static_cast<int>(st.int64(i++));
    a.smtp_security = security_from(st.text(i++));
    a.username = st.text(i++);
    a.smtp_username = st.is_null(i) ? std::string() : st.text(i);
    ++i;
    a.address = st.text(i++);
    a.display_name = st.text(i++);
    a.sent_by_server = st.int64(i++) != 0;
    a.sync_days = static_cast<int>(st.int64(i++));
    a.attachment_limit = st.int64(i++);
    a.storage_limit = st.int64(i++);
    a.sent_folder_id = st.is_null(i) ? 0 : st.int64(i);
    ++i;
    a.drafts_folder_id = st.is_null(i) ? 0 : st.int64(i);
    ++i;
    a.trash_folder_id = st.is_null(i) ? 0 : st.int64(i);
    ++i;
    a.capabilities = st.text(i++);
    a.created = st.int64(i++);
    return a;
}

// ---------------------------------------------------------------------------------------------- folders

std::vector<Folder> Store::folders() const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<Folder> out;
    Stmt st(db_,
            "SELECT id, name, raw_name, delimiter, role, sync_enabled, uidvalidity, uidnext, highestmodseq, window_low_uid,"
            " older_on_server, unread_count, last_sync_at FROM folder"
            " ORDER BY CASE role WHEN 'inbox' THEN 0 ELSE 1 END, name");
    if (!st.ok()) return out;
    while (st.step() == SQLITE_ROW) {
        Folder f;
        f.id = st.int64(0);
        f.name = st.text(1);
        f.raw_name = st.text(2);
        f.delimiter = st.text(3);
        f.role = st.text(4);
        f.sync_enabled = st.int64(5) != 0;
        f.uidvalidity = opt_col(st, 6);
        f.uidnext = opt_col(st, 7);
        f.highestmodseq = opt_col(st, 8);
        f.window_low_uid = opt_col(st, 9);
        f.older_on_server = st.int64(10) != 0;
        f.unread_count = static_cast<int>(st.int64(11));
        f.last_sync_at = opt_col(st, 12);
        out.push_back(std::move(f));
    }
    return out;
}

Error Store::upsert_folders(const std::vector<FolderListItem>& items) {
    std::lock_guard<std::mutex> lock(mu_);
    Transaction tx(db_);
    if (!tx.open) return storage_error(db_, "write_failed");
    for (const FolderListItem& item : items) {
        if (not_selectable(item) || item.raw_name.empty()) continue;
        const RoleResult detected = detect_role(item);
        const std::string& role = detected.role;
        // All Mail and Spam are off by default (spec clarification); the user can switch them on
        const int enabled = detected.sync_enabled ? 1 : 0;
        Stmt st(db_,
                "INSERT INTO folder(name, raw_name, delimiter, role, sync_enabled) VALUES(?,?,?,?,?)"
                " ON CONFLICT(name) DO UPDATE SET raw_name=excluded.raw_name, delimiter=excluded.delimiter, role=excluded.role");
        if (!st.ok()) return storage_error(db_, "write_failed");
        st.bind_text(1, cap(item.name, 1024));
        st.bind_text(2, cap(item.raw_name, 1024));
        st.bind_text(3, item.delimiter.empty() ? std::string("/") : cap(item.delimiter, 8));
        st.bind_text(4, role);
        st.bind_int64(5, enabled);
        if (st.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
    }
    if (!tx.commit()) return storage_error(db_, "write_failed");
    return {};
}

std::set<uint32_t> Store::known_uids(int64_t folder_id) const {
    std::lock_guard<std::mutex> lock(mu_);
    std::set<uint32_t> out;
    Stmt st(db_, "SELECT uid FROM message WHERE folder_id = ? AND stale = 0");
    if (!st.ok()) return out;
    st.bind_int64(1, folder_id);
    while (st.step() == SQLITE_ROW) out.insert(static_cast<uint32_t>(st.int64(0)));
    return out;
}

// ---------------------------------------------------------------------------------------------- messages

Error Store::apply_pull(int64_t folder_id, const PullBatch& batch) {
    std::lock_guard<std::mutex> lock(mu_);
    Transaction tx(db_);
    if (!tx.open) return storage_error(db_, "write_failed");
    bool has_stale = false;
    {
        Stmt q(db_, "SELECT EXISTS(SELECT 1 FROM message WHERE folder_id = ? AND stale = 1)");
        if (q.ok()) {
            q.bind_int64(1, folder_id);
            has_stale = q.step() == SQLITE_ROW && q.int64(0) != 0;
        }
    }
    for (const PulledMessage& m : batch.messages) {
        Stmt st(db_,
                "INSERT INTO message(folder_id, uid, message_id, in_reply_to, refs, from_name, from_addr, to_text, cc_text, subject,"
                " date_utc, internal_date, size, flags_base, flags_local) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)"
                " ON CONFLICT(folder_id, uid) DO UPDATE SET message_id=excluded.message_id, in_reply_to=excluded.in_reply_to,"
                " refs=excluded.refs, from_name=excluded.from_name, from_addr=excluded.from_addr, to_text=excluded.to_text,"
                " cc_text=excluded.cc_text, subject=excluded.subject, date_utc=excluded.date_utc,"
                " internal_date=excluded.internal_date, size=excluded.size, flags_base=excluded.flags_base,"
                " flags_local=CASE WHEN message.local_state='synced' THEN excluded.flags_base ELSE message.flags_local END,"
                " stale=0");
        if (!st.ok()) return storage_error(db_, "write_failed");
        int i = 1;
        st.bind_int64(i++, folder_id);
        st.bind_int64(i++, m.uid);
        if (m.message_id.empty()) st.bind_null(i++);
        else st.bind_text(i++, cap(m.message_id, kMaxSubject));
        st.bind_text(i++, cap(m.in_reply_to, kMaxSubject));
        st.bind_text(i++, cap(m.references, kMaxAddressList));
        st.bind_text(i++, cap(m.from_name, kMaxSubject));
        st.bind_text(i++, cap(m.from_addr, kMaxSubject));
        st.bind_text(i++, cap(m.to_text, kMaxAddressList));
        st.bind_text(i++, cap(m.cc_text, kMaxAddressList));
        st.bind_text(i++, cap(m.subject, kMaxSubject));
        const int64_t date = m.date_utc > 0 ? m.date_utc : (m.internal_date ? *m.internal_date : 0);
        st.bind_int64(i++, clamp_date(date));
        bind_opt(st, i++, m.internal_date);
        st.bind_int64(i++, m.size < 0 ? 0 : m.size);
        st.bind_int64(i++, m.flags);
        st.bind_int64(i++, m.flags);
        if (st.step() != SQLITE_DONE) return storage_error(db_, "write_failed");

        // the search index follows the row (rowid = message.id)
        Stmt idq(db_, "SELECT id FROM message WHERE folder_id = ? AND uid = ?");
        if (!idq.ok()) return storage_error(db_, "write_failed");
        idq.bind_int64(1, folder_id);
        idq.bind_int64(2, m.uid);
        if (idq.step() != SQLITE_ROW) return storage_error(db_, "write_failed");
        const long long id = idq.int64(0);
        Stmt del(db_, "DELETE FROM message_fts WHERE rowid = ?");
        if (!del.ok()) return storage_error(db_, "write_failed");
        del.bind_int64(1, id);
        if (del.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
        Stmt ins(db_, "INSERT INTO message_fts(rowid, subject, from_text, to_text, body_text) VALUES(?,?,?,?,'')");
        if (!ins.ok()) return storage_error(db_, "write_failed");
        ins.bind_int64(1, id);
        ins.bind_text(2, cap(m.subject, kMaxSubject));
        ins.bind_text(3, cap(m.from_name + " " + m.from_addr, kMaxSubject));
        ins.bind_text(4, cap(m.to_text, kMaxAddressList));
        if (ins.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
        store_util::note_address_sql(db_, clock_->now(), m.from_addr, m.from_name, 1);   // for the recipient fields

        // After a UIDVALIDITY reset: the same message among the stale rows (Message-ID, then date and size) hands its local work to
        // the new row and goes away; this is how pending changes survive a renumbering (research R-04)
        if (has_stale && !m.message_id.empty()) {
            Stmt find(db_,
                      "SELECT id, local_state, flags_local, move_target_folder_id FROM message WHERE folder_id = ? AND stale = 1 AND message_id = ?"
                      " ORDER BY (date_utc = ?) DESC, (size = ?) DESC, id LIMIT 1");
            if (!find.ok()) return storage_error(db_, "write_failed");
            find.bind_int64(1, folder_id);
            find.bind_text(2, cap(m.message_id, kMaxSubject));
            find.bind_int64(3, clamp_date(m.date_utc > 0 ? m.date_utc : (m.internal_date ? *m.internal_date : 0)));
            find.bind_int64(4, m.size < 0 ? 0 : m.size);
            if (find.step() == SQLITE_ROW) {
                const long long stale_id = find.int64(0);
                const std::string stale_state = find.text(1);
                const long long stale_flags = find.int64(2);
                const bool has_target = !find.is_null(3);
                const long long target = has_target ? find.int64(3) : 0;
                if (stale_state != "synced") {
                    Stmt carry(db_, "UPDATE message SET local_state = ?, flags_local = ?, move_target_folder_id = ?, local_error = '' WHERE id = ?");
                    if (!carry.ok()) return storage_error(db_, "write_failed");
                    carry.bind_text(1, stale_state);
                    carry.bind_int64(2, stale_flags);
                    if (has_target) carry.bind_int64(3, target);
                    else carry.bind_null(3);
                    carry.bind_int64(4, id);
                    if (carry.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
                }
                Stmt del_stale(db_, "DELETE FROM message WHERE id = ?");
                if (!del_stale.ok()) return storage_error(db_, "write_failed");
                del_stale.bind_int64(1, stale_id);
                if (del_stale.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
            }
        }
    }
    {
        Stmt st(db_,
                "UPDATE folder SET uidvalidity = COALESCE(?, uidvalidity), uidnext = COALESCE(?, uidnext),"
                " highestmodseq = COALESCE(?, highestmodseq), window_low_uid = COALESCE(?, window_low_uid),"
                " older_on_server = COALESCE(?, older_on_server), last_sync_at = ?,"
                " unread_count = (SELECT count(*) FROM message WHERE folder_id = ? AND (flags_local & 1) = 0 AND stale = 0 AND local_state NOT IN ('pending_delete','pending_move'))"
                " WHERE id = ?");
        if (!st.ok()) return storage_error(db_, "write_failed");
        bind_opt(st, 1, batch.uidvalidity);
        bind_opt(st, 2, batch.uidnext);
        bind_opt(st, 3, batch.highestmodseq);
        bind_opt(st, 4, batch.window_low_uid);
        if (batch.older_on_server) st.bind_int64(5, *batch.older_on_server ? 1 : 0);
        else st.bind_null(5);
        st.bind_int64(6, clock_->now());
        st.bind_int64(7, folder_id);
        st.bind_int64(8, folder_id);
        if (st.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
    }
    if (!batch.messages.empty()) store_util::prune_addresses_sql(db_);
    if (!tx.commit()) return storage_error(db_, "write_failed");
    return {};
}

std::optional<Folder> Store::folder(int64_t folder_id) const {
    for (const Folder& f : folders())
        if (f.id == folder_id) return f;
    return std::nullopt;
}

Error Store::set_folder_sync_enabled(int64_t folder_id, bool enabled) {
    std::lock_guard<std::mutex> lock(mu_);
    Stmt st(db_, "UPDATE folder SET sync_enabled = ? WHERE id = ?");
    if (!st.ok()) return storage_error(db_, "write_failed");
    st.bind_int64(1, enabled ? 1 : 0);
    st.bind_int64(2, folder_id);
    if (st.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
    if (sqlite3_changes(db_) == 0) return make_error(ErrorClass::Storage, "not_found", "no such folder");
    return {};
}

int Store::unsynced_count() const {
    std::lock_guard<std::mutex> lock(mu_);
    Stmt st(db_,
            "SELECT (SELECT count(*) FROM message WHERE local_state <> 'synced' AND stale = 0)"
            " + (SELECT count(*) FROM outbox)"   // a sent message leaves the outbox
            " + (SELECT count(*) FROM event WHERE sync_state IN ('pending_create','pending_update','pending_delete'))");
    if (!st.ok() || st.step() != SQLITE_ROW) return 0;
    return static_cast<int>(st.int64(0));
}

namespace {
void refresh_unread(sqlite3* db, int64_t folder_id) {
    Stmt st(db,
            "UPDATE folder SET unread_count = (SELECT count(*) FROM message WHERE folder_id = ?1 AND (flags_local & 1) = 0 AND stale = 0"
            " AND local_state <> 'pending_delete') WHERE id = ?1");
    if (st.ok()) {
        st.bind_int64(1, folder_id);
        st.step();
    }
}
}  // namespace

Error Store::apply_flags(int64_t folder_id, const std::vector<std::pair<uint32_t, uint32_t>>& updates) {
    std::lock_guard<std::mutex> lock(mu_);
    if (updates.empty()) return {};
    Transaction tx(db_);
    if (!tx.open) return storage_error(db_, "write_failed");
    for (const auto& u : updates) {
        Stmt st(db_,
                "UPDATE message SET flags_base = ?1, flags_local = CASE WHEN local_state = 'synced' THEN ?1 ELSE flags_local END"
                " WHERE folder_id = ?2 AND uid = ?3 AND stale = 0");
        if (!st.ok()) return storage_error(db_, "write_failed");
        st.bind_int64(1, u.second);
        st.bind_int64(2, folder_id);
        st.bind_int64(3, u.first);
        if (st.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
    }
    refresh_unread(db_, folder_id);
    if (!tx.commit()) return storage_error(db_, "write_failed");
    return {};
}

Error Store::remove_gone(int64_t folder_id, const std::vector<uint32_t>& uids, int* removed, int* kept) {
    std::lock_guard<std::mutex> lock(mu_);
    if (removed) *removed = 0;
    if (kept) *kept = 0;
    if (uids.empty()) return {};
    Transaction tx(db_);
    if (!tx.open) return storage_error(db_, "write_failed");
    for (uint32_t uid : uids) {
        Stmt del(db_, "DELETE FROM message WHERE folder_id = ? AND uid = ? AND stale = 0 AND local_state = 'synced'");
        if (!del.ok()) return storage_error(db_, "write_failed");
        del.bind_int64(1, folder_id);
        del.bind_int64(2, uid);
        if (del.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
        if (sqlite3_changes(db_) > 0) {
            if (removed) ++*removed;
            continue;
        }
        Stmt keep(db_, "UPDATE message SET local_error = 'gone_on_server' WHERE folder_id = ? AND uid = ? AND stale = 0 AND local_state <> 'synced'");
        if (!keep.ok()) return storage_error(db_, "write_failed");
        keep.bind_int64(1, folder_id);
        keep.bind_int64(2, uid);
        if (keep.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
        if (kept && sqlite3_changes(db_) > 0) ++*kept;
    }
    refresh_unread(db_, folder_id);
    if (!tx.commit()) return storage_error(db_, "write_failed");
    return {};
}

Error Store::begin_rebuild(int64_t folder_id, int64_t new_uidvalidity) {
    std::lock_guard<std::mutex> lock(mu_);
    Transaction tx(db_);
    if (!tx.open) return storage_error(db_, "write_failed");
    {   // the old numbering can never meet a new UID: stale rows get negative, unique numbers
        Stmt st(db_, "UPDATE message SET stale = 1, uid = -id WHERE folder_id = ? AND stale = 0");
        if (!st.ok()) return storage_error(db_, "write_failed");
        st.bind_int64(1, folder_id);
        if (st.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
    }
    {
        Stmt st(db_, "UPDATE folder SET uidvalidity = ?, uidnext = NULL, highestmodseq = NULL, window_low_uid = NULL, older_on_server = 0, unread_count = 0 WHERE id = ?");
        if (!st.ok()) return storage_error(db_, "write_failed");
        st.bind_int64(1, new_uidvalidity);
        st.bind_int64(2, folder_id);
        if (st.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
    }
    {
        Stmt st(db_, "INSERT INTO settings(key, value) VALUES(?, '1') ON CONFLICT(key) DO UPDATE SET value = '1'");
        if (!st.ok()) return storage_error(db_, "write_failed");
        st.bind_text(1, "rebuild." + std::to_string(folder_id));
        if (st.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
    }
    if (!tx.commit()) return storage_error(db_, "write_failed");
    log::event("store.rebuild_started", {});
    return {};
}

bool Store::rebuild_in_progress(int64_t folder_id) const {
    std::lock_guard<std::mutex> lock(mu_);
    Stmt st(db_, "SELECT value FROM settings WHERE key = ?");
    if (!st.ok()) return false;
    st.bind_text(1, "rebuild." + std::to_string(folder_id));
    return st.step() == SQLITE_ROW && st.text(0) == "1";
}

Error Store::finish_rebuild(int64_t folder_id, int* removed, int* kept) {
    std::lock_guard<std::mutex> lock(mu_);
    if (removed) *removed = 0;
    if (kept) *kept = 0;
    Transaction tx(db_);
    if (!tx.open) return storage_error(db_, "write_failed");
    {
        Stmt st(db_, "DELETE FROM message WHERE folder_id = ? AND stale = 1 AND local_state = 'synced'");
        if (!st.ok()) return storage_error(db_, "write_failed");
        st.bind_int64(1, folder_id);
        if (st.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
        if (removed) *removed = sqlite3_changes(db_);
    }
    {
        Stmt st(db_, "UPDATE message SET local_error = 'gone_on_server' WHERE folder_id = ? AND stale = 1 AND local_state <> 'synced'");
        if (!st.ok()) return storage_error(db_, "write_failed");
        st.bind_int64(1, folder_id);
        if (st.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
        if (kept) *kept = sqlite3_changes(db_);
    }
    {
        Stmt st(db_, "DELETE FROM settings WHERE key = ?");
        if (!st.ok()) return storage_error(db_, "write_failed");
        st.bind_text(1, "rebuild." + std::to_string(folder_id));
        if (st.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
    }
    refresh_unread(db_, folder_id);
    if (!tx.commit()) return storage_error(db_, "write_failed");
    return {};
}

std::vector<MissingBody> Store::messages_missing_body(int64_t folder_id, int limit) const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<MissingBody> out;
    Stmt st(db_,
            "SELECT id, uid FROM message WHERE folder_id = ? AND body_state = 'headers' AND stale = 0 AND local_state <> 'pending_delete'"
            " ORDER BY date_utc DESC, id DESC LIMIT ?");
    if (!st.ok()) return out;
    st.bind_int64(1, folder_id);
    st.bind_int64(2, limit < 1 ? 1 : limit);
    while (st.step() == SQLITE_ROW) out.push_back({st.int64(0), static_cast<uint32_t>(st.int64(1))});
    return out;
}

Error Store::apply_body(int64_t message_id, const BodyUpdate& u) {
    std::lock_guard<std::mutex> lock(mu_);
    if (u.state != "text" && u.state != "unreadable" && u.state != "too_large") return make_error(ErrorClass::Storage, "invalid_body_state", "not a body state");
    std::string text = cap(u.text, kMaxBodyText);
    std::string note = u.note;
    if (text.size() < u.text.size() && note.empty()) note = "truncated";

    Transaction tx(db_);
    if (!tx.open) return storage_error(db_, "write_failed");
    std::string subject, from_name, from_addr, to_text;
    {
        Stmt st(db_, "SELECT subject, from_name, from_addr, to_text FROM message WHERE id = ?");
        if (!st.ok()) return storage_error(db_, "write_failed");
        st.bind_int64(1, message_id);
        if (st.step() != SQLITE_ROW) return make_error(ErrorClass::Storage, "not_found", "no such message");
        subject = st.text(0);
        from_name = st.text(1);
        from_addr = st.text(2);
        to_text = st.text(3);
    }
    {
        Stmt up(db_, "UPDATE message SET body_state = ?, body_text = ?, body_note = ?, links_json = ?, has_attachments = ? WHERE id = ?");
        if (!up.ok()) return storage_error(db_, "write_failed");
        up.bind_text(1, u.state);
        up.bind_text(2, text);
        up.bind_text(3, note);
        up.bind_text(4, links_to_json(u.links));
        up.bind_int64(5, u.attachments.empty() ? 0 : 1);
        up.bind_int64(6, message_id);
        if (up.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
    }
    // attachments: update the listed ones (the user's download state stays), remove the others
    std::string keep;
    for (const StoredAttachment& a : u.attachments) {
        Stmt ins(db_,
                 "INSERT INTO attachment(message_id, section, name, mime_type, size, encoding) VALUES(?,?,?,?,?,?)"
                 " ON CONFLICT(message_id, section) DO UPDATE SET name=excluded.name, mime_type=excluded.mime_type,"
                 " size=excluded.size, encoding=excluded.encoding");
        if (!ins.ok()) return storage_error(db_, "write_failed");
        ins.bind_int64(1, message_id);
        ins.bind_text(2, cap(a.section, 64));
        ins.bind_text(3, cap(a.name, 255));
        ins.bind_text(4, a.mime_type.empty() ? std::string("application/octet-stream") : cap(a.mime_type, 255));
        ins.bind_int64(5, a.size < 0 ? 0 : a.size);
        ins.bind_text(6, cap(a.encoding, 64));
        if (ins.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
    }
    {
        std::string sql = "DELETE FROM attachment WHERE message_id = ?";
        for (size_t i = 0; i < u.attachments.size(); ++i) sql += i ? ",?" : " AND section NOT IN (?";
        if (!u.attachments.empty()) sql += ")";
        Stmt del(db_, sql.c_str());
        if (!del.ok()) return storage_error(db_, "write_failed");
        del.bind_int64(1, message_id);
        for (size_t i = 0; i < u.attachments.size(); ++i) del.bind_text(static_cast<int>(i) + 2, cap(u.attachments[i].section, 64));
        if (del.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
    }
    if (!store_util::replace_images_sql(db_, message_id, u.images)) return storage_error(db_, "write_failed");
    {   // the search index
        Stmt del(db_, "DELETE FROM message_fts WHERE rowid = ?");
        if (!del.ok()) return storage_error(db_, "write_failed");
        del.bind_int64(1, message_id);
        if (del.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
        Stmt ins(db_, "INSERT INTO message_fts(rowid, subject, from_text, to_text, body_text) VALUES(?,?,?,?,?)");
        if (!ins.ok()) return storage_error(db_, "write_failed");
        ins.bind_int64(1, message_id);
        ins.bind_text(2, subject);
        ins.bind_text(3, cap(from_name + " " + from_addr, kMaxSubject));
        ins.bind_text(4, to_text);
        ins.bind_text(5, store_util::without_picture_markers(text));
        if (ins.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
    }
    if (!tx.commit()) return storage_error(db_, "write_failed");
    return {};
}

std::optional<AttachmentRef> Store::attachment(int64_t attachment_id) const {
    std::lock_guard<std::mutex> lock(mu_);
    Stmt st(db_,
            "SELECT a.id, a.section, a.name, a.mime_type, a.size, a.encoding, a.state, a.error_key, m.id, m.uid, f.id, f.raw_name, a.saved_path"
            " FROM attachment a JOIN message m ON m.id = a.message_id JOIN folder f ON f.id = m.folder_id WHERE a.id = ?");
    if (!st.ok()) return std::nullopt;
    st.bind_int64(1, attachment_id);
    if (st.step() != SQLITE_ROW) return std::nullopt;
    AttachmentRef r;
    r.attachment.id = st.int64(0);
    r.attachment.section = st.text(1);
    r.attachment.name = st.text(2);
    r.attachment.mime_type = st.text(3);
    r.attachment.size = st.int64(4);
    r.attachment.encoding = st.text(5);
    r.attachment.state = st.text(6);
    r.attachment.error_key = st.text(7);
    r.message_id = st.int64(8);
    r.uid = static_cast<uint32_t>(st.int64(9));
    r.folder_id = st.int64(10);
    r.folder_raw_name = st.text(11);
    r.attachment.saved_path = st.text(12);
    return r;
}

Error Store::set_attachment_state(int64_t attachment_id, const std::string& state, const std::string& error_key, const std::string& saved_path) {
    std::lock_guard<std::mutex> lock(mu_);
    if ((state != "remote" && state != "downloaded") || (!error_key.empty() && error_key != "too_large" && error_key != "offline" && error_key != "refused"))
        return make_error(ErrorClass::Storage, "invalid_attachment_state", "not an attachment state");
    Stmt st(db_, "UPDATE attachment SET state = ?, error_key = ?, saved_path = ?, last_opened = CASE WHEN ? = 'downloaded' THEN ? ELSE last_opened END WHERE id = ?");
    if (!st.ok()) return storage_error(db_, "write_failed");
    st.bind_text(1, state);
    st.bind_text(2, error_key);
    st.bind_text(3, state == "downloaded" ? cap(saved_path, 4096) : std::string());
    st.bind_text(4, state);
    st.bind_int64(5, clock_->now());
    st.bind_int64(6, attachment_id);
    if (st.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
    if (sqlite3_changes(db_) == 0) return make_error(ErrorClass::Storage, "not_found", "no such attachment");
    return {};
}

std::optional<MessageView> Store::message(int64_t message_id) {
    std::lock_guard<std::mutex> lock(mu_);
    MessageView v;
    {
        Stmt st(db_,
                "SELECT id, folder_id, uid, COALESCE(message_id,''), from_name, from_addr, to_text, cc_text, subject, date_utc, size, flags_local,"
                " local_state, body_state, has_attachments, COALESCE(body_text,''), body_note, COALESCE(links_json,'[]') FROM message WHERE id = ?");
        if (!st.ok()) return std::nullopt;
        st.bind_int64(1, message_id);
        if (st.step() != SQLITE_ROW) return std::nullopt;
        MessageHeader& h = v.header;
        h.id = st.int64(0);
        h.folder_id = st.int64(1);
        h.uid = static_cast<uint32_t>(st.int64(2));
        h.message_id = st.text(3);
        h.from_name = st.text(4);
        h.from_addr = st.text(5);
        h.to_text = st.text(6);
        v.cc_text = st.text(7);
        h.subject = st.text(8);
        h.date_utc = st.int64(9);
        h.size = st.int64(10);
        h.flags = static_cast<uint32_t>(st.int64(11));
        h.local_state = st.text(12);
        h.body_state = st.text(13);
        h.has_attachments = st.int64(14) != 0;
        v.body_text = st.text(15);
        v.body_note = st.text(16);
        v.links = links_from_json(st.text(17));
    }
    {
        Stmt st(db_, "SELECT id, section, name, mime_type, size, encoding, state, error_key, saved_path FROM attachment WHERE message_id = ? ORDER BY id");
        if (st.ok()) {
            st.bind_int64(1, message_id);
            while (st.step() == SQLITE_ROW) {
                StoredAttachment a;
                a.id = st.int64(0);
                a.section = st.text(1);
                a.name = st.text(2);
                a.mime_type = st.text(3);
                a.size = st.int64(4);
                a.encoding = st.text(5);
                a.state = st.text(6);
                a.error_key = st.text(7);
                a.saved_path = st.text(8);
                v.attachments.push_back(std::move(a));
            }
        }
    }
    {
        Stmt up(db_, "UPDATE message SET last_opened = ? WHERE id = ?");
        if (up.ok()) {
            up.bind_int64(1, clock_->now());
            up.bind_int64(2, message_id);
            up.step();
        }
    }
    return v;
}

Page<MessageHeader> Store::list_messages(int64_t folder_id, SortKey sort, int page, int page_size) const {
    std::lock_guard<std::mutex> lock(mu_);
    Page<MessageHeader> out;
    if (page_size < 1) page_size = 1;
    {
        Stmt c(db_, "SELECT count(*) FROM message WHERE folder_id = ? AND (local_state NOT IN ('pending_delete','pending_move') OR local_error <> '') AND (flags_local & 8) = 0 AND (stale = 0 OR local_state <> 'synced')");
        if (c.ok()) {
            c.bind_int64(1, folder_id);
            if (c.step() == SQLITE_ROW) out.total = static_cast<int>(c.int64(0));
        }
    }
    out.page_count = std::max(1, (out.total + page_size - 1) / page_size);
    out.page = std::max(0, std::min(page, out.page_count - 1));
    const std::string order = sort == SortKey::DateAsc    ? " ORDER BY date_utc ASC, id ASC"
                              : sort == SortKey::SenderAsc ? " ORDER BY pbo_fold(CASE WHEN from_name <> '' THEN from_name ELSE from_addr END) ASC, date_utc DESC, id DESC"
                              : sort == SortKey::SubjectAsc ? " ORDER BY pbo_fold(subject) ASC, date_utc DESC, id DESC"
                                                            : " ORDER BY date_utc DESC, id DESC";
    const std::string sql = std::string(
            "SELECT id, folder_id, uid, COALESCE(message_id,''), from_name, from_addr, to_text, subject, date_utc, size, flags_local,"
            " local_state, body_state, has_attachments, local_error, stale FROM message WHERE folder_id = ? AND (local_state NOT IN ('pending_delete','pending_move') OR local_error <> '') AND (flags_local & 8) = 0 AND (stale = 0 OR local_state <> 'synced')")
            + order + " LIMIT ? OFFSET ?";
    Stmt st(db_, sql.c_str());
    if (!st.ok()) return out;
    st.bind_int64(1, folder_id);
    st.bind_int64(2, page_size);
    st.bind_int64(3, static_cast<long long>(out.page) * page_size);
    while (st.step() == SQLITE_ROW) {
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
        out.items.push_back(std::move(m));
    }
    return out;
}

Error Store::repair_encoded_headers(int* repaired) {
    std::lock_guard<std::mutex> lock(mu_);
    if (repaired) *repaired = 0;
    struct Row {
        long long id;
        std::string subject, from_name, from_addr, to_text, cc_text;
    };
    std::vector<Row> rows;
    {
        Stmt st(db_,
                "SELECT id, subject, from_name, from_addr, to_text, cc_text FROM message"
                " WHERE subject LIKE '%=?%' OR from_name LIKE '%=?%' OR to_text LIKE '%=?%' OR cc_text LIKE '%=?%'");
        if (!st.ok()) return storage_error(db_, "write_failed");
        while (st.step() == SQLITE_ROW) rows.push_back({st.int64(0), st.text(1), st.text(2), st.text(3), st.text(4), st.text(5)});
    }
    if (rows.empty()) return {};
    Transaction tx(db_);
    if (!tx.open) return storage_error(db_, "write_failed");
    int changed = 0;
    for (const Row& r : rows) {
        const std::string subject = cap(decode_header_words(r.subject), kMaxSubject);
        const std::string from_name = cap(decode_header_words(r.from_name), kMaxSubject);
        const std::string to_text = cap(r.to_text.find("=?") == std::string::npos ? r.to_text : format_address_header(r.to_text), kMaxAddressList);
        const std::string cc_text = cap(r.cc_text.find("=?") == std::string::npos ? r.cc_text : format_address_header(r.cc_text), kMaxAddressList);
        if (subject == r.subject && from_name == r.from_name && to_text == r.to_text && cc_text == r.cc_text) continue;
        Stmt up(db_, "UPDATE message SET subject = ?, from_name = ?, to_text = ?, cc_text = ? WHERE id = ?");
        if (!up.ok()) return storage_error(db_, "write_failed");
        up.bind_text(1, subject);
        up.bind_text(2, from_name);
        up.bind_text(3, to_text);
        up.bind_text(4, cc_text);
        up.bind_int64(5, r.id);
        if (up.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
        Stmt del(db_, "DELETE FROM message_fts WHERE rowid = ?");
        if (!del.ok()) return storage_error(db_, "write_failed");
        del.bind_int64(1, r.id);
        if (del.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
        Stmt ins(db_, "INSERT INTO message_fts(rowid, subject, from_text, to_text, body_text) VALUES(?,?,?,?,'')");
        if (!ins.ok()) return storage_error(db_, "write_failed");
        ins.bind_int64(1, r.id);
        ins.bind_text(2, subject);
        ins.bind_text(3, cap(from_name + " " + r.from_addr, kMaxSubject));
        ins.bind_text(4, to_text);
        if (ins.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
        ++changed;
    }
    if (!tx.commit()) return storage_error(db_, "write_failed");
    if (repaired) *repaired = changed;
    return {};
}

std::string Store::setting(const std::string& key) const {
    std::lock_guard<std::mutex> lock(mu_);
    Stmt st(db_, "SELECT value FROM settings WHERE key = ?");
    if (!st.ok()) return std::string();
    st.bind_text(1, key);
    return st.step() == SQLITE_ROW ? st.text(0) : std::string();
}

Error Store::set_setting(const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lock(mu_);
    Stmt st(db_, "INSERT INTO settings(key, value) VALUES(?,?) ON CONFLICT(key) DO UPDATE SET value = excluded.value");
    if (!st.ok()) return storage_error(db_, "write_failed");
    st.bind_text(1, cap(key, 128));
    st.bind_text(2, cap(value, 4096));
    if (st.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
    return {};
}

Error Store::mark_bodies_for_refetch(int* marked) {
    std::lock_guard<std::mutex> lock(mu_);
    Stmt st(db_, "UPDATE message SET body_state = 'headers' WHERE body_state <> 'headers' AND local_state <> 'pending_delete'");
    if (!st.ok()) return storage_error(db_, "write_failed");
    if (st.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
    if (marked) *marked = sqlite3_changes(db_);
    return {};
}

// ---------------------------------------------------------------------------------------------- sync state

Error Store::set_clock_skew(std::optional<int64_t> skew) {
    std::lock_guard<std::mutex> lock(mu_);
    Stmt st(db_,
            "INSERT INTO sync_state(id, clock_skew_s) VALUES(1, ?) ON CONFLICT(id) DO UPDATE SET clock_skew_s = excluded.clock_skew_s");
    if (!st.ok()) return storage_error(db_, "write_failed");
    bind_opt(st, 1, skew);
    if (st.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
    return {};
}

SyncState Store::sync_state() const {
    std::lock_guard<std::mutex> lock(mu_);
    SyncState s;
    Stmt st(db_,
            "SELECT last_started, last_finished, last_success, outcome, problem_key, problem_area, counts_json, clock_skew_s"
            " FROM sync_state WHERE id = 1");
    if (!st.ok() || st.step() != SQLITE_ROW) return s;
    s.last_started = opt_col(st, 0);
    s.last_finished = opt_col(st, 1);
    s.last_success = opt_col(st, 2);
    s.outcome = st.text(3);
    s.problem_key = st.text(4);
    s.problem_area = st.text(5);
    s.counts_json = st.text(6);
    s.clock_skew_s = opt_col(st, 7);
    return s;
}

Error Store::write_sync_state(const SyncState& s) {
    std::lock_guard<std::mutex> lock(mu_);
    Stmt st(db_,
            "INSERT INTO sync_state(id, last_started, last_finished, last_success, outcome, problem_key, problem_area, counts_json,"
            " clock_skew_s) VALUES(1,?,?,?,?,?,?,?,?)"
            " ON CONFLICT(id) DO UPDATE SET last_started=excluded.last_started, last_finished=excluded.last_finished,"
            " last_success=excluded.last_success, outcome=excluded.outcome, problem_key=excluded.problem_key,"
            " problem_area=excluded.problem_area, counts_json=excluded.counts_json, clock_skew_s=excluded.clock_skew_s");
    if (!st.ok()) return storage_error(db_, "write_failed");
    bind_opt(st, 1, s.last_started);
    bind_opt(st, 2, s.last_finished);
    bind_opt(st, 3, s.last_success);
    st.bind_text(4, s.outcome);
    st.bind_text(5, s.problem_key);
    st.bind_text(6, s.problem_area);
    st.bind_text(7, s.counts_json);
    bind_opt(st, 8, s.clock_skew_s);
    if (st.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
    return {};
}

}  // namespace pbo
