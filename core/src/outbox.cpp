#include "pbo/outbox.h"

#include "pbo/clock_check.h"

#include <algorithm>

#include "pbo/imap_parse.h"
#include "pbo/log.h"

namespace pbo {

namespace {

// a failure that happened before any connection existed: nothing can have been sent
bool no_connection_key(const std::string& key) { return key == "no_network" || key == "server_unreachable"; }

std::string quote_for_imap(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out + "\"";
}

std::optional<Folder> sent_folder(const Store& store, const MailAccount& account) {
    if (account.sent_folder_id != 0)
        if (auto f = store.folder(account.sent_folder_id)) return f;
    for (const Folder& f : store.folders())
        if (f.role == "sent") return f;
    return std::nullopt;
}

// UIDs of the messages with this Message-ID in the selected folder
Result<std::vector<uint32_t>> find_copy(ImapTransport& imap, const Folder& folder, const std::string& message_id) {
    Result<std::vector<uint32_t>> out;
    auto r = imap.command(folder.raw_name, "UID SEARCH HEADER Message-ID " + quote_for_imap(message_id));
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

bool connection_class(const Error& e) {
    return e.cls == ErrorClass::Network || e.cls == ErrorClass::Tls || e.cls == ErrorClass::Auth || e.cls == ErrorClass::Cancelled;
}

}  // namespace

Error run_outbox(OutboxDeps& d, const Credentials& creds, OutboxCounts* counts) {
    OutboxCounts local;
    if (!counts) counts = &local;
    const auto account = d.store.mail_account();
    if (!account) return {};

    // ---- 1. send what is queued, oldest first, each message once ----
    SmtpEndpoint se;
    se.host = account->smtp_host;
    se.port = account->smtp_port;
    se.security = account->smtp_security;
    se.username = account->smtp_username.empty() ? account->username : account->smtp_username;
    const std::string smtp_password = creds.smtp_password.empty() ? creds.mail_password : creds.smtp_password;

    for (const OutboxItem& item : d.store.outbox()) {
        if (item.state != "queued") continue;
        if (d.cancel.requested()) break;
        auto began = d.store.begin_sending(item.id);   // committed BEFORE the first byte goes out
        if (!began.ok()) return began.error;
        if (!began.value) continue;                    // somebody else has it
        const SendResult r = d.smtp.send(se, smtp_password, item.envelope_from, item.recipients, item.raw_message);
        SendFinish f;
        bool stop = false;
        switch (r.outcome) {
            case SendOutcome::Accepted:
                f.kind = SendFinish::Kind::Accepted;
                ++counts->sent;
                break;
            case SendOutcome::Unknown:
                f.kind = SendFinish::Kind::Uncertain;
                f.error_key = r.error.key;
                f.error_detail = r.error.detail;
                ++counts->uncertain;
                stop = true;   // the connection broke at the worst moment: no more tries in this sync
                break;
            case SendOutcome::CertainFailure:
                f.error_key = r.error.key;
                f.error_detail = r.error.detail;
                if (no_connection_key(r.error.key)) {
                    f.kind = SendFinish::Kind::Requeue;
                    ++counts->requeued;
                    stop = true;   // the next message would meet the same closed door
                } else {
                    f.kind = SendFinish::Kind::Failed;
                    ++counts->failed;
                    stop = r.error.cls == ErrorClass::Tls;   // a certificate problem is the same for every message
                }
                break;
        }
        if (Error e = d.store.finish_sending(item.id, f); !e.ok()) return e;
        if (stop) break;
    }

    // ---- 2. the Sent copy of accepted messages ----
    std::vector<OutboxItem> pending;
    for (const OutboxItem& item : d.store.outbox())
        if (item.state == "sent_copy_pending") pending.push_back(item);
    if (pending.empty() || d.cancel.requested()) return {};

    if (account->sent_by_server) {   // the provider files it: nothing to upload
        for (const OutboxItem& item : pending) {
            if (Error e = d.store.complete_outbox(item.id); !e.ok()) return e;
            ++counts->copies;
        }
        return {};
    }
    const auto folder = sent_folder(d.store, *account);
    if (!folder) {
        for (const OutboxItem& item : pending) d.store.set_outbox_error(item.id, "sent_folder_missing", "no Sent folder is known yet");
        counts->copies_failed += static_cast<int>(pending.size());
        return {};
    }
    ImapEndpoint ie;
    ie.host = account->imap_host;
    ie.port = account->imap_port;
    ie.security = account->imap_security;
    ie.username = account->username;
    if (Error e = d.imap.connect(ie, creds.mail_password); !e.ok()) {
        for (const OutboxItem& item : pending) d.store.set_outbox_error(item.id, e.key, e.detail);
        counts->copies_failed += static_cast<int>(pending.size());
        return {};   // the mail step reports the connection problem; the copies wait for the next sync
    }
    for (const OutboxItem& item : pending) {
        if (d.cancel.requested()) break;
        // an APPEND whose answer was lost is not repeated: look first
        auto have = find_copy(d.imap, *folder, item.message_id);
        if (!have.ok()) {
            d.store.set_outbox_error(item.id, have.error.key, have.error.detail);
            ++counts->copies_failed;
            if (connection_class(have.error)) break;
            continue;
        }
        if (!have.value.empty()) {
            if (Error e = d.store.complete_outbox(item.id); !e.ok()) return e;
            ++counts->copies;
            continue;
        }
        const Error appended = d.imap.append(folder->raw_name, item.raw_message);
        if (!appended.ok()) {
            d.store.set_outbox_error(item.id, appended.key.empty() ? "append_failed" : appended.key, appended.detail);
            ++counts->copies_failed;
            if (connection_class(appended)) break;
            continue;
        }
        // libcurl's APPEND carries no flags: the copy is marked as read afterwards (a cosmetic step: no failure here keeps the row)
        auto found = find_copy(d.imap, *folder, item.message_id);
        if (found.ok() && !found.value.empty()) note_server_stamp(d.imap, d.store, d.clock, folder->raw_name, *std::max_element(found.value.begin(), found.value.end()));
        if (found.ok() && !found.value.empty() && d.guard.check(d.imap, *folder) == GuardResult::Ok) {
            d.imap.command(folder->raw_name, "UID STORE " + std::to_string(*std::max_element(found.value.begin(), found.value.end())) + " +FLAGS.SILENT (\\Seen)");
        }
        if (Error e = d.store.complete_outbox(item.id); !e.ok()) return e;
        ++counts->copies;
    }
    d.imap.disconnect();
    return {};
}

}  // namespace pbo
