#pragma once
// The folder of pending hand-overs (spec 003, research R-04, data-model "Pending entry"). The launcher writes one folder per hand-over, with one raw file per
// argument ("0", "1", ...), as <id>.tmp and renames it to <id> when it is complete. The app reads it here; it removes an entry only after the draft is saved
// (a crash in between makes a second draft, never a lost text). Entries older than 10 minutes are removed unread. No Qt, no InkView, no logging.

#include <cstddef>
#include <string>
#include <vector>

#include "pbo/types.h"

namespace pbo {

struct PendingHandoff {
    std::string id;                    // the folder name
    std::vector<std::string> args;     // one entry per file, raw bytes, in numeric file order; empty when refused
    bool refused = false;              // too many or too large files, or something that is not what the launcher writes
};

class HandoffInbox {
public:
    static constexpr int kMaxFiles = 8;
    static constexpr size_t kMaxFileBytes = 16384;
    static constexpr int kExpirySeconds = 600;

    HandoffInbox(std::string dir, Clock& clock);

    // The entries that are complete (not *.tmp), oldest name first. An entry (also an unfinished one) whose folder is older than kExpirySeconds is removed
    // unread and counted in `expired`; a time in the future counts as fresh. Reading does not remove what it returns. A missing directory is an empty result.
    std::vector<PendingHandoff> take_ready(int* expired = nullptr);

    // Removes one entry with its files. False when there is no such entry, or when the id could leave the folder (empty, ".", "..", a slash).
    bool remove(const std::string& id);

private:
    std::string dir_;
    Clock& clock_;
};

}  // namespace pbo
