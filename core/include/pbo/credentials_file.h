#pragma once
// The `credentials` file (data-model.md, research R-13): passwords as plain text key=value lines, version line
// first, written atomically (temp file + rename), mode 0600 where the file system honors it. This is a
// documented, honest trade-off: the app tells the user whether the protection holds on this storage.

#include <string>

#include "pbo/types.h"

namespace pbo {

struct Credentials {
    std::string mail_password;
    std::string smtp_password;       // empty = same as the mail password
    std::string calendar_password;
};

enum class ProtectionStatus { Enforced, NotEnforced, Unknown };

// Creates `dir` (0700) if needed. The temp file is created with mode 0600 from the start. Errors: "storage_full",
// "write_failed". `status` (optional) reports what a stat after the write shows.
Error save_credentials(const std::string& dir, const Credentials& credentials, ProtectionStatus* status = nullptr);

// A missing file is not an error: all passwords are empty. A damaged or newer file: "credentials_unreadable".
Result<Credentials> load_credentials(const std::string& dir);

// Overwrites the content, then unlinks. Missing file = success.
Error forget_credentials(const std::string& dir);

// Mode check of the existing file: Enforced = only the owner can access it; NotEnforced = group or others can
// (typical on FAT); Unknown = no file or no answer.
ProtectionStatus credentials_protection(const std::string& dir);

std::string credentials_path(const std::string& dir);

}  // namespace pbo
