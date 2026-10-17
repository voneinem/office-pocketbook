#include "pbo/mail_push.h"

#include <algorithm>
#include <map>

#include "pbo/imap_parse.h"

namespace pbo {

namespace {

std::string quote(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out + "\"";
}

bool connection_class(const Error& e) {
    return e.cls == ErrorClass::Network || e.cls == ErrorClass::Tls || e.cls == ErrorClass::Auth || e.cls == ErrorClass::Cancelled;
}

// the IMAP flags of the bits the app lets the user change
std::string flag_names(uint32_t bits) {
    bits &= kFlagSeen | kFlagFlagged | kFlagAnswered | kFlagForwarded;   // \Deleted and \Draft are never set by a flag change
    return bits_to_flag_list(bits);
}

class Pusher {
public:
    Pusher(MessagePushDeps& d, MessagePushCounts* c) : d_(d), counts_(c) {}

    // Its flags on the server: found / gone / the connection failed
    enum class Look { Found, Gone, Failed };
    Look look(const Folder& f, uint32_t uid, uint32_t* flags, Error* error) {
        auto r = d_.imap.command(f.raw_name, "UID FETCH " + std::to_string(uid) + " (FLAGS)");
        if (!r.ok()) {
            *error = r.error;
            return Look::Failed;
        }
        const auto parsed = parse_fetch(r.value);
        if (!parsed.ok) {
            *error = make_error(ErrorClass::Protocol, "protocol_error", "the flags could not be read");
            return Look::Failed;
        }
        for (const FetchItem& it : parsed.value)
            if (it.uid == uid && it.flags_bits) {
                *flags = *it.flags_bits;
                return Look::Found;
            }
        return Look::Gone;
    }

    // false: stop (the connection is gone)
    bool message(const Folder& f, const PendingMessage& pm) {
        const std::string uid = std::to_string(pm.uid);
        Error err;
        uint32_t server_flags = 0;
        const uint32_t add = pm.flags_local & ~pm.flags_base, remove = pm.flags_base & ~pm.flags_local;
        const bool flags_differ = (add | remove) != 0;

        // 1. flags: the difference only
        if (flags_differ && pm.local_state != "pending_delete") {
            bool refused = false;
            if (flag_names(add) != "()") {
                auto r = d_.imap.command(f.raw_name, "UID STORE " + uid + " +FLAGS.SILENT " + flag_names(add));
                if (!r.ok()) {
                    if (connection_class(r.error)) return false;
                    refused = true;
                }
            }
            if (!refused && flag_names(remove) != "()") {
                auto r = d_.imap.command(f.raw_name, "UID STORE " + uid + " -FLAGS.SILENT " + flag_names(remove));
                if (!r.ok()) {
                    if (connection_class(r.error)) return false;
                    refused = true;
                }
            }
            if (refused) {
                d_.store.set_message_error(pm.id, "refused");
                ++counts_->errors;
                return true;
            }
            switch (look(f, pm.uid, &server_flags, &err)) {
                case Look::Failed:
                    return !connection_class(err);
                case Look::Gone:
                    d_.store.set_message_error(pm.id, "gone_on_server");
                    ++counts_->errors;
                    return true;
                case Look::Found:
                    break;
            }
            ++counts_->flags;
            if (pm.local_state == "pending_flags") {
                d_.store.message_flags_pushed(pm.id, server_flags);
                return true;
            }
            // a move follows: the flags travel with the message (COPY and MOVE keep them)
        } else if (pm.local_state != "pending_flags") {
            // before a move or a delete: is the message still there?
            switch (look(f, pm.uid, &server_flags, &err)) {
                case Look::Failed:
                    return !connection_class(err);
                case Look::Gone:
                    d_.store.set_message_error(pm.id, "gone_on_server");
                    ++counts_->errors;
                    return true;
                case Look::Found:
                    break;
            }
        } else {
            return true;
        }

        if (pm.local_state == "pending_move") return move(f, pm);
        if (pm.local_state == "pending_delete") return purge(f, pm);
        return true;
    }

private:
    bool has(const char* cap) const { return d_.imap.capabilities().count(cap) != 0; }

    // \Deleted on the original and, with UIDPLUS, UID EXPUNGE of that one UID. False: the connection is gone.
    bool delete_original(const Folder& f, uint32_t uid, bool* ok) {
        const std::string u = std::to_string(uid);
        *ok = false;
        auto s = d_.imap.command(f.raw_name, "UID STORE " + u + " +FLAGS.SILENT (\\Deleted)");
        if (!s.ok()) return !connection_class(s.error);
        if (has("uidplus")) {
            auto e = d_.imap.command(f.raw_name, "UID EXPUNGE " + u);
            if (!e.ok()) return !connection_class(e.error);
        }
        *ok = true;
        return true;
    }

    bool move(const Folder& f, const PendingMessage& pm) {
        const auto target = pm.move_target_folder_id ? d_.store.folder(*pm.move_target_folder_id) : std::nullopt;
        if (!target) {
            d_.store.set_message_error(pm.id, "refused");
            ++counts_->errors;
            return true;
        }
        const std::string u = std::to_string(pm.uid);
        if (has("move")) {
            auto r = d_.imap.command(f.raw_name, "UID MOVE " + u + " " + quote(target->raw_name));
            if (!r.ok()) {
                if (connection_class(r.error)) return false;
                d_.store.set_message_error(pm.id, "refused");
                ++counts_->errors;
                return true;
            }
        } else {
            auto c = d_.imap.command(f.raw_name, "UID COPY " + u + " " + quote(target->raw_name));
            if (!c.ok()) {
                if (connection_class(c.error)) return false;
                d_.store.set_message_error(pm.id, "refused");   // nothing was deleted: the original is untouched
                ++counts_->errors;
                return true;
            }
            bool ok = false;
            if (!delete_original(f, pm.uid, &ok)) return false;
            if (!ok) {   // the copy exists, the original could not be marked: the message is in both folders; reported, nothing is lost
                d_.store.set_message_error(pm.id, "refused");
                ++counts_->errors;
                return true;
            }
        }
        d_.store.message_left_folder(pm.id);
        ++counts_->moved;
        return true;
    }

    bool purge(const Folder& f, const PendingMessage& pm) {
        bool ok = false;
        if (!delete_original(f, pm.uid, &ok)) return false;
        if (!ok) {
            d_.store.set_message_error(pm.id, "refused");
            ++counts_->errors;
            return true;
        }
        d_.store.message_left_folder(pm.id);
        ++counts_->deleted;
        return true;
    }

    MessagePushDeps& d_;
    MessagePushCounts* counts_;
};

}  // namespace

Error push_message_changes(MessagePushDeps& d, const Credentials& creds, MessagePushCounts* counts) {
    MessagePushCounts local;
    if (!counts) counts = &local;
    const std::vector<PendingMessage> pending = d.store.pending_messages();
    if (pending.empty()) return {};
    const auto account = d.store.mail_account();
    if (!account) return {};
    ImapEndpoint ie;
    ie.host = account->imap_host;
    ie.port = account->imap_port;
    ie.security = account->imap_security;
    ie.username = account->username;
    if (const Error e = d.imap.connect(ie, creds.mail_password); !e.ok()) return e;

    std::map<int64_t, std::vector<PendingMessage>> by_folder;
    for (const PendingMessage& pm : pending) by_folder[pm.folder_id].push_back(pm);
    Pusher pusher(d, counts);
    Error result;
    for (const auto& kv : by_folder) {
        if (d.cancel.requested()) {
            result = make_error(ErrorClass::Cancelled, "cancelled", "the sync was cancelled");
            break;
        }
        const auto folder = d.store.folder(kv.first);
        if (!folder) continue;
        // I8/K1: before the first UID-addressed command of the folder
        if (d.guard.check(d.imap, *folder) != GuardResult::Ok) {
            ++counts->skipped;
            continue;
        }
        bool alive = true;
        const std::vector<PendingMessage> latest = d.store.pending_messages();
        for (const PendingMessage& pm : kv.second) {
            if (d.cancel.requested()) {
                result = make_error(ErrorClass::Cancelled, "cancelled", "the sync was cancelled");
                alive = false;
                break;
            }
            // the user may have changed it meanwhile: the latest state of this message
            const PendingMessage* now = nullptr;
            for (const PendingMessage& p : latest)
                if (p.id == pm.id) now = &p;
            if (!now) continue;
            if (!pusher.message(*folder, *now)) {
                alive = false;
                break;
            }
        }
        if (!alive) {
            if (result.ok()) result = make_error(ErrorClass::Network, "connection_lost", "the connection was lost while changes were sent");
            break;
        }
    }
    d.imap.disconnect();
    return result;
}

}  // namespace pbo
