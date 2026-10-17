#pragma once
// The UIDVALIDITY guard (research R-04, constitution Principle III, task I8). UIDs only mean something inside one UIDVALIDITY
// value of a folder. Before the FIRST write of a folder in a sync, and before every command that names a UID (STORE, COPY,
// MOVE, EXPUNGE, deleting a mirrored draft, searching a Sent copy), the app asks the server for the folder's UIDVALIDITY and
// compares it with the stored one. A changed value, or an answer that cannot be read, means: send nothing for that folder.
//
// One guard object lives for ONE sync. A result is valid for one folder only and only inside that sync.

#include <cstdint>
#include <map>
#include <optional>

#include "pbo/imap_transport.h"
#include "pbo/mail_types.h"

namespace pbo {

enum class GuardResult {
    Ok,         // equal to the stored value, or nothing was stored yet (first sync of the folder)
    Mismatch,   // the folder was reset on the server: its UIDs are not the stored ones
    Failed      // the server could not be asked, or the answer was unusable: treated like Mismatch (fail closed)
};

// One STATUS "<folder>" (UIDVALIDITY) round trip. `seen` (optional) gets the server's value when it was read.
GuardResult check_uidvalidity(ImapTransport& imap, const Folder& folder, uint32_t* seen = nullptr);

class UidvalidityGuard {
public:
    // Asks the server now (also when this folder was checked before in this sync) and remembers the answer.
    GuardResult check(ImapTransport& imap, const Folder& folder);
    // The same decision from a STATUS answer the caller already has (the folder pull asks for UIDVALIDITY together with the other
    // items, immediately before it writes anything). `server_value` 0 = unreadable.
    GuardResult accept(const Folder& folder, uint32_t server_value);
    // The answer of the latest check of this folder; Failed if it was never checked.
    GuardResult result_for(int64_t folder_id) const;
    // True only after a check that said Ok. Every UID-addressed write is guarded by this.
    bool allows_writes(int64_t folder_id) const { return result_for(folder_id) == GuardResult::Ok; }
    // The value the server reported at the latest check (for the rebuild), if it was read.
    std::optional<uint32_t> server_value(int64_t folder_id) const;

private:
    struct Entry {
        GuardResult result = GuardResult::Failed;
        std::optional<uint32_t> seen;
    };
    std::map<int64_t, Entry> entries_;
};

}  // namespace pbo
