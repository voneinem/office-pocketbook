#include "pbo/draft_mirror.h"

#include "pbo/clock_check.h"

#include <algorithm>
#include <cstdio>

#include "pbo/compose.h"
#include "pbo/imap_parse.h"
#include "pbo/mime.h"

namespace pbo {

namespace {

std::string raw_fingerprint(const std::string& raw) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : raw) {
        h ^= c;
        h *= 1099511628211ull;
    }
    char b[24];
    snprintf(b, sizeof b, "%016llx", static_cast<unsigned long long>(h));
    return b;
}

std::string quote_for_imap(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out + "\"";
}

std::optional<Folder> drafts_folder(const Store& store, const MailAccount& account) {
    if (account.drafts_folder_id != 0)
        if (auto f = store.folder(account.drafts_folder_id)) return f;
    for (const Folder& f : store.folders())
        if (f.role == "drafts") return f;
    return std::nullopt;
}

struct StringSink : ByteSink {
    std::string data;
    bool write(const char* d, size_t n) override {
        data.append(d, n);
        return data.size() <= (4u << 20);
    }
};

bool connection_class(const Error& e) {
    return e.cls == ErrorClass::Network || e.cls == ErrorClass::Tls || e.cls == ErrorClass::Auth || e.cls == ErrorClass::Cancelled;
}

class Mirror {
public:
    Mirror(DraftMirrorDeps& d, const MailAccount& a, const Folder& f, DraftMirrorCounts* c) : d_(d), account_(a), folder_(f), counts_(c) {}

    Result<std::vector<uint32_t>> search(const std::string& message_id) {
        Result<std::vector<uint32_t>> out;
        auto r = d_.imap.command(folder_.raw_name, "UID SEARCH HEADER Message-ID " + quote_for_imap(message_id));
        if (!r.ok()) {
            out.error = r.error;
            return out;
        }
        const auto parsed = parse_search(r.value);
        if (!parsed.ok) {
            out.error = make_error(ErrorClass::Protocol, "protocol_error", "the answer to the search could not be read");
            return out;
        }
        out.value = parsed.value;
        return out;
    }

    // \Deleted + UID EXPUNGE of ONE uid (never a bare EXPUNGE); false if the guard or the server refused
    bool remove_copy(uint32_t uid) {
        if (d_.guard.check(d_.imap, folder_) != GuardResult::Ok) return false;
        const std::string u = std::to_string(uid);
        if (!d_.imap.command(folder_.raw_name, "UID STORE " + u + " +FLAGS.SILENT (\\Deleted)").ok()) return false;
        if (d_.imap.capabilities().count("uidplus")) {
            if (!d_.imap.command(folder_.raw_name, "UID EXPUNGE " + u).ok()) return false;
        }
        // without UIDPLUS the copy stays flagged \Deleted: a bare EXPUNGE could remove other people's messages
        return true;
    }

    bool mark_draft(uint32_t uid) {
        if (d_.guard.check(d_.imap, folder_) != GuardResult::Ok) return false;
        return d_.imap.command(folder_.raw_name, "UID STORE " + std::to_string(uid) + " +FLAGS.SILENT (\\Draft \\Seen)").ok();
    }

    std::string fetch_raw(uint32_t uid, bool* ok) {
        StringSink sink;
        const Error e = d_.imap.fetch_to(folder_.raw_name, uid, "", 4u << 20, sink);
        *ok = e.ok();
        return e.ok() ? sink.data : std::string();
    }

    ComposeDraft compose_of(const Draft& dr) const {
        ComposeDraft c;
        c.from_name = account_.display_name;
        c.from_addr = account_.address;
        c.to = dr.to_text;
        c.cc = dr.cc_text;
        c.bcc = dr.bcc_text;
        c.subject = dr.subject;
        c.body = dr.body;
        c.message_id = dr.message_id;
        c.in_reply_to = dr.in_reply_to;
        c.references = dr.refs;
        c.date_utc = dr.updated > 0 ? dr.updated : d_.clock.now();
        return c;
    }

    // Uploads one draft; returns false on a problem of the connection (the caller stops)
    bool mirror(const Draft& dr) {
        auto built = build_draft_message(compose_of(dr));
        if (!built.ok()) {
            ++counts_->failed;
            return true;
        }
        const std::string& raw = built.value.raw;
        const std::string fingerprint = raw_fingerprint(raw);
        const uint32_t old_uid = static_cast<uint32_t>(dr.server_uid);

        // the copy we know: changed elsewhere since we put it there?
        Draft current = dr;
        if (old_uid != 0) {
            auto copies = search(dr.message_id);
            if (!copies.ok()) {
                ++counts_->failed;
                return !connection_class(copies.error);
            }
            const bool still_there = std::find(copies.value.begin(), copies.value.end(), old_uid) != copies.value.end();
            // The version somebody else saved: our copy itself with other content, or - because other clients save by replacing the message -
            // a newer copy under the same Message-ID that we did not upload.
            uint32_t other_uid = 0;
            std::string other_raw;
            if (still_there) {
                bool got = false;
                const std::string server_raw = fetch_raw(old_uid, &got);
                if (got && !dr.server_hash.empty() && raw_fingerprint(server_raw) != dr.server_hash) {
                    other_uid = old_uid;
                    other_raw = server_raw;
                }
            } else if (!copies.value.empty()) {
                const uint32_t newest = *std::max_element(copies.value.begin(), copies.value.end());
                bool got = false;
                const std::string server_raw = fetch_raw(newest, &got);
                const std::string fp = raw_fingerprint(server_raw);
                if (got && fp != dr.server_hash && fp != fingerprint) {
                    other_uid = newest;
                    other_raw = server_raw;
                }
            }
            if (other_uid != 0) {
                // changed on both sides: the server's version becomes a second draft, the user's text stays as it is
                const ParsedMessage p = parse_message(other_raw);
                Draft other;
                other.kind = "new";
                other.message_id = make_message_id(d_.random, account_.address);
                other.to_text = format_address_header(p.to);
                other.cc_text = format_address_header(p.cc);
                other.subject = p.subject;
                other.body = p.text;
                auto split = d_.store.split_draft_conflict(dr.id, other, other_uid, raw_fingerprint(other_raw));
                if (!split.ok()) {
                    ++counts_->failed;
                    return true;
                }
                ++counts_->conflicts;
                return mirror_fresh(*d_.store.draft(dr.id), raw, fingerprint);
            }
        }
        return mirror_fresh(current, raw, fingerprint);
    }

    bool mirror_fresh(const Draft& dr, const std::string& raw, const std::string& fingerprint) {
        const uint32_t old_uid = static_cast<uint32_t>(dr.server_uid);
        // an APPEND whose answer was lost: a copy with our text may be in the folder already
        auto before = search(dr.message_id);
        if (!before.ok()) {
            ++counts_->failed;
            return !connection_class(before.error);
        }
        uint32_t new_uid = 0;
        for (uint32_t u : before.value) {
            if (u == old_uid || u < old_uid) continue;
            bool got = false;
            if (fetch_raw(u, &got) == raw && got) {
                new_uid = u;
                break;
            }
        }
        if (new_uid == 0) {
            if (d_.guard.check(d_.imap, folder_) != GuardResult::Ok) {   // a reset folder gets nothing: neither a copy nor a deletion
                ++counts_->failed;
                return true;
            }
            const Error appended = d_.imap.append(folder_.raw_name, raw);
            if (!appended.ok()) {
                ++counts_->failed;
                return !connection_class(appended);
            }
            auto after = search(dr.message_id);
            if (!after.ok() || after.value.empty()) {
                ++counts_->failed;
                return after.ok() || !connection_class(after.error);
            }
            new_uid = *std::max_element(after.value.begin(), after.value.end());
            if (new_uid == old_uid) {   // the server did not give the copy a new place we can tell apart: try again at the next sync
                ++counts_->failed;
                return true;
            }
            note_server_stamp(d_.imap, d_.store, d_.clock, folder_.raw_name, new_uid);   // the time the server put on it: the reader's clock is checked against it
        }
        mark_draft(new_uid);   // cosmetic: the copy shows as a draft elsewhere
        if (!d_.store.set_draft_mirrored(dr.id, new_uid, dr.content_hash, fingerprint).ok()) return true;
        ++counts_->mirrored;
        // only now, with the new copy confirmed, the superseded one goes - exactly that UID
        if (old_uid != 0 && old_uid != new_uid && remove_copy(old_uid)) ++counts_->removed;
        return true;
    }

    // a draft the user deleted (or sent): its server copy goes, then the row
    bool remove(const Draft& dr) {
        const uint32_t uid = static_cast<uint32_t>(dr.server_uid);
        auto copies = search(dr.message_id);
        if (!copies.ok()) {
            ++counts_->failed;
            return !connection_class(copies.error);
        }
        const bool there = std::find(copies.value.begin(), copies.value.end(), uid) != copies.value.end();
        if (there) {
            if (!remove_copy(uid)) {
                ++counts_->failed;
                return true;
            }
            ++counts_->removed;
        }
        d_.store.drop_draft_row(dr.id);
        return true;
    }

private:
    DraftMirrorDeps& d_;
    const MailAccount& account_;
    const Folder& folder_;
    DraftMirrorCounts* counts_;
};

}  // namespace

Error run_draft_mirror(DraftMirrorDeps& d, const Credentials& creds, DraftMirrorCounts* counts) {
    DraftMirrorCounts local;
    if (!counts) counts = &local;
    const auto account = d.store.mail_account();
    if (!account) return {};
    const std::vector<Draft> to_mirror = d.store.drafts_to_mirror();
    const std::vector<Draft> to_remove = d.store.drafts_to_remove_on_server();
    if (to_mirror.empty() && to_remove.empty()) return {};
    const auto folder = drafts_folder(d.store, *account);
    if (!folder) return {};   // no Drafts folder known yet: the drafts stay local, nothing is lost

    ImapEndpoint ie;
    ie.host = account->imap_host;
    ie.port = account->imap_port;
    ie.security = account->imap_security;
    ie.username = account->username;
    if (!d.imap.connect(ie, creds.mail_password).ok()) return {};   // the mail step reports the connection; the drafts wait
    Mirror m(d, *account, *folder, counts);
    // removals first: they cannot lose anything (the draft is gone for the user), then the uploads
    for (const Draft& dr : to_remove) {
        if (d.cancel.requested()) break;
        if (dr.server_uid <= 0) {
            d.store.drop_draft_row(dr.id);
            continue;
        }
        if (!m.remove(dr)) break;
    }
    for (const Draft& dr : to_mirror) {
        if (d.cancel.requested()) break;
        // the draft may have changed while we were working: always the latest text
        const auto latest = d.store.draft(dr.id);
        if (!latest || latest->queued) continue;
        if (!m.mirror(*latest)) break;
    }
    d.imap.disconnect();
    return {};
}

}  // namespace pbo
