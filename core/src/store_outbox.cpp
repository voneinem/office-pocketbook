// Store: drafts and the outbox (data-model.md, research R-08). Every public method is one short transaction; a failing statement leaves
// the complete before-state. The send state machine moves only through guarded updates ("WHERE state = ..."), so two callers can never both win.
#include <sqlite3.h>

#include <algorithm>
#include <cstdio>

#include "pbo/store.h"
#include "store_sql.h"
#include "store_util.h"

namespace pbo {

using sql::Stmt;
using store_util::cap;
using store_util::storage_error;
using store_util::Transaction;

namespace {

constexpr size_t kMaxBody = 1u << 20;
constexpr size_t kMaxRecipients = 16u << 10;
constexpr size_t kMaxSubject = 998;
constexpr size_t kMaxMessage = 32u << 20;

std::string content_hash(const Draft& d) {
    uint64_t h = 1469598103934665603ull;   // FNV-1a: change detection, not security
    auto feed = [&h](const std::string& s) {
        for (unsigned char c : s) {
            h ^= c;
            h *= 1099511628211ull;
        }
        h ^= 0xff;
        h *= 1099511628211ull;
    };
    feed(d.kind);
    feed(d.to_text);
    feed(d.cc_text);
    feed(d.bcc_text);
    feed(d.subject);
    feed(d.body);
    feed(d.in_reply_to);
    feed(d.refs);
    char b[24];
    snprintf(b, sizeof b, "%016llx", static_cast<unsigned long long>(h));
    return b;
}

bool valid_kind(const std::string& k) { return k == "new" || k == "reply" || k == "reply_all" || k == "forward"; }

const char* kDraftColumns =
    "d.id, d.kind, d.message_id, COALESCE(d.reply_to_message,0), d.in_reply_to, d.refs, d.to_text, d.cc_text, d.bcc_text, d.subject, d.body,"
    " d.created, d.updated, d.content_hash, d.server_state, COALESCE(d.server_uid,0), d.server_hash, COALESCE(d.conflict_of,0),"
    " EXISTS(SELECT 1 FROM outbox o WHERE o.draft_id = d.id)";

Draft read_draft(const Stmt& st) {
    Draft d;
    d.id = st.int64(0);
    d.kind = st.text(1);
    d.message_id = st.text(2);
    d.reply_to_message = st.int64(3);
    d.in_reply_to = st.text(4);
    d.refs = st.text(5);
    d.to_text = st.text(6);
    d.cc_text = st.text(7);
    d.bcc_text = st.text(8);
    d.subject = st.text(9);
    d.body = st.text(10);
    d.created = st.int64(11);
    d.updated = st.int64(12);
    d.content_hash = st.text(13);
    d.server_state = st.text(14);
    d.server_uid = st.int64(15);
    d.server_hash = st.text(16);
    d.conflict_of = st.int64(17);
    d.queued = st.int64(18) != 0;
    return d;
}

std::vector<std::string> split_lines(const std::string& s) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos <= s.size()) {
        const size_t e = s.find('\n', pos);
        std::string one = s.substr(pos, e == std::string::npos ? std::string::npos : e - pos);
        if (!one.empty()) out.push_back(std::move(one));
        if (e == std::string::npos) break;
        pos = e + 1;
    }
    return out;
}

const char* kOutboxColumns =
    "id, COALESCE(draft_id,0), message_id, envelope_from, envelope_to, subject_preview, raw_message, state, error_key, error_detail, attempts,"
    " queued_at, state_at, COALESCE(sent_at,0)";

OutboxItem read_outbox(const Stmt& st) {
    OutboxItem o;
    o.id = st.int64(0);
    o.draft_id = st.int64(1);
    o.message_id = st.text(2);
    o.envelope_from = st.text(3);
    o.recipients = split_lines(st.text(4));
    o.subject_preview = st.text(5);
    o.raw_message = st.text(6);
    o.state = st.text(7);
    o.error_key = st.text(8);
    o.error_detail = st.text(9);
    o.attempts = static_cast<int>(st.int64(10));
    o.queued_at = st.int64(11);
    o.state_at = st.int64(12);
    o.sent_at = st.int64(13);
    return o;
}

}  // namespace

// ---------------------------------------------------------------------------------------------- drafts

Result<int64_t> Store::save_draft(const Draft& in) {
    std::lock_guard<std::mutex> lock(mu_);
    Result<int64_t> out;
    auto fail = [&](const char* key, const char* detail) {
        out.error = make_error(ErrorClass::Storage, key, detail);
        return out;
    };
    if (!valid_kind(in.kind)) return fail("invalid_draft", "kind");
    if (in.body.size() > kMaxBody || in.to_text.size() > kMaxRecipients || in.cc_text.size() > kMaxRecipients || in.bcc_text.size() > kMaxRecipients ||
        in.subject.size() > 64u << 10 || in.refs.size() > 64u << 10)
        return fail("too_large", "a field of the draft is too large");
    Transaction tx(db_);
    if (!tx.open) {
        out.error = storage_error(db_, "write_failed");
        return out;
    }
    Draft d = in;
    d.content_hash = content_hash(d);
    const int64_t now = clock_->now();

    if (in.id == 0) {
        if (in.message_id.empty() || in.message_id.size() > 200) return fail("invalid_draft", "message_id");
        Stmt st(db_,
                "INSERT INTO draft(kind, message_id, reply_to_message, in_reply_to, refs, to_text, cc_text, bcc_text, subject, body, created, updated,"
                " content_hash, server_state, conflict_of) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,'none',?)");
        if (!st.ok()) {
            out.error = storage_error(db_, "write_failed");
            return out;
        }
        int i = 1;
        st.bind_text(i++, d.kind);
        st.bind_text(i++, d.message_id);
        if (d.reply_to_message) st.bind_int64(i++, d.reply_to_message);
        else st.bind_null(i++);
        st.bind_text(i++, d.in_reply_to);
        st.bind_text(i++, d.refs);
        st.bind_text(i++, d.to_text);
        st.bind_text(i++, d.cc_text);
        st.bind_text(i++, d.bcc_text);
        st.bind_text(i++, d.subject);
        st.bind_text(i++, d.body);
        st.bind_int64(i++, now);
        st.bind_int64(i++, now);
        st.bind_text(i++, d.content_hash);
        if (d.conflict_of) st.bind_int64(i++, d.conflict_of);
        else st.bind_null(i++);
        if (st.step() != SQLITE_DONE) {
            if (sqlite3_extended_errcode(db_) == SQLITE_CONSTRAINT_UNIQUE || sqlite3_errcode(db_) == SQLITE_CONSTRAINT) return fail("invalid_draft", "message_id");
            out.error = storage_error(db_, "write_failed");
            return out;
        }
        out.value = sqlite3_last_insert_rowid(db_);
    } else {
        Stmt cur(db_, "SELECT content_hash, server_state, EXISTS(SELECT 1 FROM outbox o WHERE o.draft_id = draft.id) FROM draft WHERE id = ?");
        cur.bind_int64(1, in.id);
        if (!cur.ok() || cur.step() != SQLITE_ROW) return fail("not_found", "no such draft");
        const std::string old_hash = cur.text(0), state = cur.text(1);
        if (state == "pending_delete") return fail("not_found", "no such draft");
        if (cur.int64(2) != 0) return fail("already_queued", "the draft is in the outbox");
        if (old_hash == d.content_hash) {
            out.value = in.id;
            return out;   // nothing changed: nothing written
        }
        Stmt st(db_,
                "UPDATE draft SET kind=?, in_reply_to=?, refs=?, to_text=?, cc_text=?, bcc_text=?, subject=?, body=?, updated=?, content_hash=?,"
                " server_state = CASE WHEN server_state = 'synced' THEN 'pending' ELSE server_state END WHERE id = ?");
        if (!st.ok()) {
            out.error = storage_error(db_, "write_failed");
            return out;
        }
        int i = 1;
        st.bind_text(i++, d.kind);
        st.bind_text(i++, d.in_reply_to);
        st.bind_text(i++, d.refs);
        st.bind_text(i++, d.to_text);
        st.bind_text(i++, d.cc_text);
        st.bind_text(i++, d.bcc_text);
        st.bind_text(i++, d.subject);
        st.bind_text(i++, d.body);
        st.bind_int64(i++, now);
        st.bind_text(i++, d.content_hash);
        st.bind_int64(i++, in.id);
        if (st.step() != SQLITE_DONE) {
            out.error = storage_error(db_, "write_failed");
            return out;
        }
        out.value = in.id;
    }
    if (!tx.commit()) {
        out = Result<int64_t>();
        out.error = storage_error(db_, "write_failed");
    }
    return out;
}

std::optional<Draft> Store::draft(int64_t draft_id) const {
    std::lock_guard<std::mutex> lock(mu_);
    const std::string sql = std::string("SELECT ") + kDraftColumns + " FROM draft d WHERE d.id = ?";
    Stmt st(db_, sql.c_str());
    if (!st.ok()) return std::nullopt;
    st.bind_int64(1, draft_id);
    if (st.step() != SQLITE_ROW) return std::nullopt;
    return read_draft(st);
}

std::vector<Draft> Store::drafts() const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<Draft> out;
    const std::string sql = std::string("SELECT ") + kDraftColumns +
                            " FROM draft d WHERE d.server_state <> 'pending_delete' AND NOT EXISTS (SELECT 1 FROM outbox o WHERE o.draft_id = d.id)"
                            " ORDER BY d.updated DESC, d.id DESC";
    Stmt st(db_, sql.c_str());
    if (!st.ok()) return out;
    while (st.step() == SQLITE_ROW) out.push_back(read_draft(st));
    return out;
}

std::vector<Draft> Store::drafts_to_mirror() const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<Draft> out;
    const std::string sql = std::string("SELECT ") + kDraftColumns +
                            " FROM draft d WHERE d.server_state IN ('none','pending','conflict') AND NOT EXISTS (SELECT 1 FROM outbox o WHERE o.draft_id = d.id)"
                            " ORDER BY d.id";
    Stmt st(db_, sql.c_str());
    if (!st.ok()) return out;
    while (st.step() == SQLITE_ROW) {
        Draft d = read_draft(st);
        if (d.server_state == "conflict" && d.server_uid != 0) continue;   // a conflict draft that is already mirrored
        out.push_back(std::move(d));
    }
    return out;
}

std::vector<Draft> Store::drafts_to_remove_on_server() const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<Draft> out;
    const std::string sql = std::string("SELECT ") + kDraftColumns + " FROM draft d WHERE d.server_state = 'pending_delete' ORDER BY d.id";
    Stmt st(db_, sql.c_str());
    if (!st.ok()) return out;
    while (st.step() == SQLITE_ROW) out.push_back(read_draft(st));
    return out;
}

Error Store::delete_draft(int64_t draft_id) {
    std::lock_guard<std::mutex> lock(mu_);
    Transaction tx(db_);
    if (!tx.open) return storage_error(db_, "write_failed");
    Stmt cur(db_, "SELECT COALESCE(server_uid,0), EXISTS(SELECT 1 FROM outbox o WHERE o.draft_id = draft.id) FROM draft WHERE id = ?");
    cur.bind_int64(1, draft_id);
    if (!cur.ok() || cur.step() != SQLITE_ROW) return make_error(ErrorClass::Storage, "not_found", "no such draft");
    if (cur.int64(1) != 0) return make_error(ErrorClass::Storage, "already_queued", "the draft is in the outbox");
    const bool server_copy = cur.int64(0) > 0;
    Stmt st(db_, server_copy ? "UPDATE draft SET server_state = 'pending_delete' WHERE id = ?" : "DELETE FROM draft WHERE id = ?");
    st.bind_int64(1, draft_id);
    if (!st.ok() || st.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
    return tx.commit() ? Error() : storage_error(db_, "write_failed");
}

Error Store::drop_draft_row(int64_t draft_id) {
    std::lock_guard<std::mutex> lock(mu_);
    Stmt cur(db_, "SELECT server_state FROM draft WHERE id = ?");
    cur.bind_int64(1, draft_id);
    if (!cur.ok() || cur.step() != SQLITE_ROW) return make_error(ErrorClass::Storage, "not_found", "no such draft");
    if (cur.text(0) == "pending") return make_error(ErrorClass::Storage, "draft_pending", "the server has not seen the latest text of this draft");
    Stmt st(db_, "DELETE FROM draft WHERE id = ?");
    st.bind_int64(1, draft_id);
    if (!st.ok() || st.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
    return {};
}

Result<int64_t> Store::split_draft_conflict(int64_t original_id, const Draft& v, int64_t uid, const std::string& raw_hash) {
    Result<int64_t> out;
    {
        std::lock_guard<std::mutex> lock(mu_);
        Transaction tx(db_);
        if (!tx.open) {
            out.error = storage_error(db_, "write_failed");
            return out;
        }
        Stmt exists(db_, "SELECT 1 FROM draft WHERE id = ? AND server_state <> 'pending_delete'");
        exists.bind_int64(1, original_id);
        if (!exists.ok() || exists.step() != SQLITE_ROW) {
            out.error = make_error(ErrorClass::Storage, "not_found", "no such draft");
            return out;
        }
        Draft d = v;
        if (!valid_kind(d.kind) || d.message_id.empty() || d.body.size() > kMaxBody) {
            out.error = make_error(ErrorClass::Storage, "invalid_draft", "the server's version cannot be kept");
            return out;
        }
        d.content_hash = content_hash(d);
        const int64_t now = clock_->now();
        Stmt ins(db_,
                 "INSERT INTO draft(kind, message_id, in_reply_to, refs, to_text, cc_text, bcc_text, subject, body, created, updated, content_hash,"
                 " server_state, server_uid, server_hash, conflict_of) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,'synced',?,?,?)");
        if (!ins.ok()) {
            out.error = storage_error(db_, "write_failed");
            return out;
        }
        int i = 1;
        ins.bind_text(i++, d.kind);
        ins.bind_text(i++, d.message_id);
        ins.bind_text(i++, d.in_reply_to);
        ins.bind_text(i++, d.refs);
        ins.bind_text(i++, d.to_text);
        ins.bind_text(i++, d.cc_text);
        ins.bind_text(i++, d.bcc_text);
        ins.bind_text(i++, d.subject);
        ins.bind_text(i++, d.body);
        ins.bind_int64(i++, now);
        ins.bind_int64(i++, now);
        ins.bind_text(i++, d.content_hash);
        ins.bind_int64(i++, uid);
        ins.bind_text(i++, raw_hash);
        ins.bind_int64(i++, original_id);
        if (ins.step() != SQLITE_DONE) {
            out.error = storage_error(db_, "write_failed");
            return out;
        }
        out.value = sqlite3_last_insert_rowid(db_);
        Stmt upd(db_, "UPDATE draft SET server_uid = NULL, server_hash = '', server_state = 'pending' WHERE id = ?");
        upd.bind_int64(1, original_id);
        if (!upd.ok() || upd.step() != SQLITE_DONE || !tx.commit()) {
            out = Result<int64_t>();
            out.error = storage_error(db_, "write_failed");
        }
    }
    return out;
}

Error Store::set_draft_mirrored(int64_t draft_id, int64_t uid, const std::string& hash, const std::string& raw_hash) {
    std::lock_guard<std::mutex> lock(mu_);
    Stmt st(db_,
            "UPDATE draft SET server_uid = ?1, server_hash = ?4, server_state = CASE WHEN content_hash = ?2 THEN 'synced' ELSE 'pending' END"
            " WHERE id = ?3 AND server_state <> 'pending_delete'");
    if (!st.ok()) return storage_error(db_, "write_failed");
    st.bind_int64(1, uid);
    st.bind_text(2, hash);
    st.bind_int64(3, draft_id);
    st.bind_text(4, raw_hash.empty() ? hash : raw_hash);
    if (st.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
    if (sqlite3_changes(db_) == 0) return make_error(ErrorClass::Storage, "not_found", "no such draft");
    return {};
}

// ---------------------------------------------------------------------------------------------- outbox

Result<int64_t> Store::queue_draft(int64_t draft_id, const OutboxRequest& r) {
    std::lock_guard<std::mutex> lock(mu_);
    Result<int64_t> out;
    auto fail = [&](const char* key, const char* detail) {
        out.error = make_error(ErrorClass::Storage, key, detail);
        return out;
    };
    if (r.recipients.empty() || r.raw_message.empty() || r.raw_message.size() > kMaxMessage || r.envelope_from.empty()) return fail("invalid_request", "recipients or message missing");
    Transaction tx(db_);
    if (!tx.open) {
        out.error = storage_error(db_, "write_failed");
        return out;
    }
    Stmt cur(db_, "SELECT message_id, content_hash, server_state, EXISTS(SELECT 1 FROM outbox o WHERE o.draft_id = draft.id) FROM draft WHERE id = ?");
    cur.bind_int64(1, draft_id);
    if (!cur.ok() || cur.step() != SQLITE_ROW) return fail("not_found", "no such draft");
    const std::string message_id = cur.text(0), hash = cur.text(1), state = cur.text(2);
    if (cur.int64(3) != 0) return fail("already_queued", "the draft is already in the outbox");
    if (state == "pending_delete") return fail("not_found", "no such draft");
    if (hash != r.expected_hash) return fail("draft_changed", "the draft changed after the message was built");
    std::string to;
    for (const std::string& a : r.recipients) to += (to.empty() ? "" : "\n") + a;
    const int64_t now = clock_->now();
    Stmt st(db_,
            "INSERT INTO outbox(draft_id, message_id, envelope_from, envelope_to, subject_preview, raw_message, state, queued_at, state_at)"
            " VALUES(?,?,?,?,?,?, 'queued', ?, ?)");
    if (!st.ok()) {
        out.error = storage_error(db_, "write_failed");
        return out;
    }
    st.bind_int64(1, draft_id);
    st.bind_text(2, message_id);
    st.bind_text(3, r.envelope_from);
    st.bind_text(4, to);
    st.bind_text(5, cap(r.subject_preview, 200));
    sqlite3_bind_blob(st.get(), 6, r.raw_message.data(), static_cast<int>(r.raw_message.size()), SQLITE_TRANSIENT);
    st.bind_int64(7, now);
    st.bind_int64(8, now);
    if (st.step() != SQLITE_DONE) {
        if (sqlite3_errcode(db_) == SQLITE_CONSTRAINT) return fail("already_queued", "the message is already in the outbox");
        out.error = storage_error(db_, "write_failed");
        return out;
    }
    out.value = sqlite3_last_insert_rowid(db_);
    if (!tx.commit()) {
        out = Result<int64_t>();
        out.error = storage_error(db_, "write_failed");
    }
    return out;
}

std::vector<OutboxItem> Store::outbox() const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<OutboxItem> out;
    const std::string sql = std::string("SELECT ") + kOutboxColumns + " FROM outbox ORDER BY queued_at, id";
    Stmt st(db_, sql.c_str());
    if (!st.ok()) return out;
    while (st.step() == SQLITE_ROW) out.push_back(read_outbox(st));
    return out;
}

std::optional<OutboxItem> Store::outbox_item(int64_t outbox_id) const {
    std::lock_guard<std::mutex> lock(mu_);
    const std::string sql = std::string("SELECT ") + kOutboxColumns + " FROM outbox WHERE id = ?";
    Stmt st(db_, sql.c_str());
    if (!st.ok()) return std::nullopt;
    st.bind_int64(1, outbox_id);
    if (st.step() != SQLITE_ROW) return std::nullopt;
    return read_outbox(st);
}

Result<bool> Store::begin_sending(int64_t outbox_id) {
    std::lock_guard<std::mutex> lock(mu_);
    Result<bool> out;
    Stmt st(db_, "UPDATE outbox SET state = 'sending', attempts = attempts + 1, state_at = ? WHERE id = ? AND state = 'queued'");
    if (!st.ok()) {
        out.error = storage_error(db_, "write_failed");
        return out;
    }
    st.bind_int64(1, clock_->now());
    st.bind_int64(2, outbox_id);
    if (st.step() != SQLITE_DONE) {
        out.error = storage_error(db_, "write_failed");
        return out;
    }
    out.value = sqlite3_changes(db_) == 1;
    return out;
}

namespace {

// the state of a row; "" if there is none
std::string state_of(sqlite3* db, int64_t id) {
    Stmt st(db, "SELECT state FROM outbox WHERE id = ?");
    st.bind_int64(1, id);
    if (!st.ok() || st.step() != SQLITE_ROW) return std::string();
    return st.text(0);
}

Error guard(sqlite3* db, int64_t id, std::initializer_list<const char*> allowed) {
    const std::string state = state_of(db, id);
    if (state.empty()) return make_error(ErrorClass::Storage, "not_found", "no such message in the outbox");
    for (const char* a : allowed)
        if (state == a) return {};
    return make_error(ErrorClass::Storage, "bad_state", "the message is not in a state that allows this");
}

// the draft of an outbox row leaves with it: a mirrored copy on the server is removed first, otherwise the row goes at once
bool release_draft(sqlite3* db, int64_t draft_id) {
    if (draft_id <= 0) return true;
    Stmt cur(db, "SELECT COALESCE(server_uid,0) FROM draft WHERE id = ?");
    cur.bind_int64(1, draft_id);
    if (!cur.ok() || cur.step() != SQLITE_ROW) return true;
    const bool server_copy = cur.int64(0) > 0;
    Stmt st(db, server_copy ? "UPDATE draft SET server_state = 'pending_delete' WHERE id = ?" : "DELETE FROM draft WHERE id = ?");
    st.bind_int64(1, draft_id);
    return st.ok() && st.step() == SQLITE_DONE;
}

int64_t draft_of(sqlite3* db, int64_t outbox_id) {
    Stmt st(db, "SELECT COALESCE(draft_id,0) FROM outbox WHERE id = ?");
    st.bind_int64(1, outbox_id);
    return st.ok() && st.step() == SQLITE_ROW ? st.int64(0) : 0;
}

}  // namespace

Error Store::finish_sending(int64_t outbox_id, const SendFinish& f) {
    std::lock_guard<std::mutex> lock(mu_);
    if (Error e = guard(db_, outbox_id, {"sending"}); !e.ok()) return e;
    const char* next = f.kind == SendFinish::Kind::Accepted ? "sent_copy_pending" : f.kind == SendFinish::Kind::Failed ? "failed"
                       : f.kind == SendFinish::Kind::Requeue ? "queued" : "uncertain";
    Stmt st(db_,
            "UPDATE outbox SET state = ?1, error_key = ?2, error_detail = ?3, state_at = ?4, sent_at = CASE WHEN ?1 = 'sent_copy_pending' THEN ?4 ELSE sent_at END"
            " WHERE id = ?5 AND state = 'sending'");
    if (!st.ok()) return storage_error(db_, "write_failed");
    st.bind_text(1, next);
    st.bind_text(2, f.kind == SendFinish::Kind::Accepted ? std::string() : cap(f.error_key, 80));
    st.bind_text(3, f.kind == SendFinish::Kind::Accepted ? std::string() : cap(f.error_detail, 200));
    st.bind_int64(4, clock_->now());
    st.bind_int64(5, outbox_id);
    if (st.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
    return sqlite3_changes(db_) == 1 ? Error() : make_error(ErrorClass::Storage, "bad_state", "the message is not being sent");
}

Error Store::set_outbox_error(int64_t outbox_id, const std::string& key, const std::string& detail) {
    std::lock_guard<std::mutex> lock(mu_);
    if (Error e = guard(db_, outbox_id, {"sent_copy_pending"}); !e.ok()) return e;
    Stmt st(db_, "UPDATE outbox SET error_key = ?, error_detail = ? WHERE id = ? AND state = 'sent_copy_pending'");
    if (!st.ok()) return storage_error(db_, "write_failed");
    st.bind_text(1, cap(key, 80));
    st.bind_text(2, cap(detail, 200));
    st.bind_int64(3, outbox_id);
    if (st.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
    return {};
}

Error Store::complete_outbox(int64_t outbox_id) {
    std::lock_guard<std::mutex> lock(mu_);
    Transaction tx(db_);
    if (!tx.open) return storage_error(db_, "write_failed");
    if (Error e = guard(db_, outbox_id, {"sent_copy_pending"}); !e.ok()) return e;
    const int64_t draft_id = draft_of(db_, outbox_id);
    Stmt del(db_, "DELETE FROM outbox WHERE id = ? AND state = 'sent_copy_pending'");
    del.bind_int64(1, outbox_id);
    if (!del.ok() || del.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
    if (!release_draft(db_, draft_id)) return storage_error(db_, "write_failed");
    return tx.commit() ? Error() : storage_error(db_, "write_failed");
}

namespace {

Error move_state(sqlite3* db, Clock& clock, int64_t id, const char* from, const char* to, bool clear_error, bool set_sent) {
    Stmt st(db,
            clear_error ? "UPDATE outbox SET state = ?1, error_key = '', error_detail = '', state_at = ?2, sent_at = CASE WHEN ?4 THEN ?2 ELSE sent_at END WHERE id = ?3 AND state = ?5"
                        : "UPDATE outbox SET state = ?1, state_at = ?2, sent_at = CASE WHEN ?4 THEN ?2 ELSE sent_at END WHERE id = ?3 AND state = ?5");
    if (!st.ok()) return storage_error(db, "write_failed");
    st.bind_text(1, to);
    st.bind_int64(2, clock.now());
    st.bind_int64(3, id);
    st.bind_int64(4, set_sent ? 1 : 0);
    st.bind_text(5, from);
    if (st.step() != SQLITE_DONE) return storage_error(db, "write_failed");
    return sqlite3_changes(db) == 1 ? Error() : make_error(ErrorClass::Storage, "bad_state", "the message is not in a state that allows this");
}

}  // namespace

Error Store::retry_outbox(int64_t outbox_id) {
    std::lock_guard<std::mutex> lock(mu_);
    if (Error e = guard(db_, outbox_id, {"failed"}); !e.ok()) return e;
    return move_state(db_, *clock_, outbox_id, "failed", "queued", true, false);
}

Error Store::send_again(int64_t outbox_id) {
    std::lock_guard<std::mutex> lock(mu_);
    if (Error e = guard(db_, outbox_id, {"uncertain"}); !e.ok()) return e;
    return move_state(db_, *clock_, outbox_id, "uncertain", "queued", true, false);
}

Error Store::mark_was_sent(int64_t outbox_id) {
    std::lock_guard<std::mutex> lock(mu_);
    if (Error e = guard(db_, outbox_id, {"uncertain"}); !e.ok()) return e;
    return move_state(db_, *clock_, outbox_id, "uncertain", "sent_copy_pending", true, true);
}

Error Store::edit_outbox(int64_t outbox_id) {
    std::lock_guard<std::mutex> lock(mu_);
    Transaction tx(db_);
    if (!tx.open) return storage_error(db_, "write_failed");
    if (Error e = guard(db_, outbox_id, {"failed"}); !e.ok()) return e;
    const int64_t draft_id = draft_of(db_, outbox_id);
    Stmt exists(db_, "SELECT 1 FROM draft WHERE id = ?");
    exists.bind_int64(1, draft_id);
    if (draft_id <= 0 || !exists.ok() || exists.step() != SQLITE_ROW) return make_error(ErrorClass::Storage, "draft_gone", "the draft no longer exists");
    Stmt del(db_, "DELETE FROM outbox WHERE id = ? AND state = 'failed'");
    del.bind_int64(1, outbox_id);
    if (!del.ok() || del.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
    return tx.commit() ? Error() : storage_error(db_, "write_failed");
}

Error Store::delete_outbox(int64_t outbox_id) {
    std::lock_guard<std::mutex> lock(mu_);
    Transaction tx(db_);
    if (!tx.open) return storage_error(db_, "write_failed");
    if (Error e = guard(db_, outbox_id, {"failed", "uncertain"}); !e.ok()) return e;
    const int64_t draft_id = draft_of(db_, outbox_id);
    Stmt del(db_, "DELETE FROM outbox WHERE id = ?");
    del.bind_int64(1, outbox_id);
    if (!del.ok() || del.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
    if (!release_draft(db_, draft_id)) return storage_error(db_, "write_failed");
    return tx.commit() ? Error() : storage_error(db_, "write_failed");
}

Error Store::recover_interrupted(int* recovered) {
    std::lock_guard<std::mutex> lock(mu_);
    Stmt st(db_,
            "UPDATE outbox SET state = 'uncertain', error_key = 'interrupted', error_detail = 'the app was closed while the message was being sent', state_at = ?"
            " WHERE state = 'sending'");
    if (!st.ok()) return storage_error(db_, "write_failed");
    st.bind_int64(1, clock_->now());
    if (st.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
    if (recovered) *recovered = sqlite3_changes(db_);
    return {};
}

}  // namespace pbo
