#include "pbo/uidvalidity_guard.h"

#include "pbo/imap_parse.h"

namespace pbo {

namespace {

bool quote_mailbox(const std::string& raw, std::string* out) {
    std::string q = "\"";
    for (char c : raw) {
        if (c == '\r' || c == '\n' || c == '\0') return false;   // a name with a line break is never sent
        if (c == '"' || c == '\\') q += '\\';
        q += c;
    }
    *out = q + "\"";
    return true;
}

}  // namespace

GuardResult check_uidvalidity(ImapTransport& imap, const Folder& folder, uint32_t* seen) {
    std::string quoted;
    if (!quote_mailbox(folder.raw_name, &quoted)) return GuardResult::Failed;
    Result<ImapRaw> r = imap.command("", "STATUS " + quoted + " (UIDVALIDITY)");
    if (!r.ok()) return GuardResult::Failed;
    const auto status = parse_status(r.value);
    // UIDVALIDITY is a 32-bit number greater than zero; anything else is not an answer to trust
    if (!status.ok || !status.value.has_uidvalidity || status.value.uidvalidity == 0) return GuardResult::Failed;
    if (seen) *seen = status.value.uidvalidity;
    if (!folder.uidvalidity) return GuardResult::Ok;   // the first sync of the folder: nothing stored to disagree with
    return static_cast<int64_t>(status.value.uidvalidity) == *folder.uidvalidity ? GuardResult::Ok : GuardResult::Mismatch;
}

GuardResult UidvalidityGuard::check(ImapTransport& imap, const Folder& folder) {
    Entry e;
    uint32_t seen = 0;
    e.result = check_uidvalidity(imap, folder, &seen);
    if (e.result != GuardResult::Failed) e.seen = seen;
    entries_[folder.id] = e;
    return e.result;
}

GuardResult UidvalidityGuard::accept(const Folder& folder, uint32_t server_value) {
    Entry e;
    if (server_value == 0) {
        e.result = GuardResult::Failed;
    } else {
        e.seen = server_value;
        e.result = (!folder.uidvalidity || static_cast<int64_t>(server_value) == *folder.uidvalidity) ? GuardResult::Ok : GuardResult::Mismatch;
    }
    entries_[folder.id] = e;
    return e.result;
}

GuardResult UidvalidityGuard::result_for(int64_t folder_id) const {
    const auto it = entries_.find(folder_id);
    return it == entries_.end() ? GuardResult::Failed : it->second.result;
}

std::optional<uint32_t> UidvalidityGuard::server_value(int64_t folder_id) const {
    const auto it = entries_.find(folder_id);
    return it == entries_.end() ? std::nullopt : it->second.seen;
}

}  // namespace pbo
