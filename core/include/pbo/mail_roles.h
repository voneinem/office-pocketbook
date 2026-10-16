#pragma once
// Folder roles (FR-005): from the special-use attributes the server sends, otherwise from well-known names. A role that no
// folder has is left for the user to choose; the app never guesses beyond these rules.

#include <string>
#include <vector>

#include "pbo/imap_parse.h"
#include "pbo/mail_types.h"

namespace pbo {

struct RoleResult {
    std::string role = "other";   // inbox, sent, drafts, trash, junk, all, archive, other
    bool sync_enabled = true;     // Gmail's \All and \Junk folders are off by default (they only duplicate other folders)
};

// Attributes first (any case), then the name (decoded, case-insensitive, after removing a leading "INBOX." or "INBOX/").
RoleResult detect_role(const FolderListItem& item);

// Of sent, drafts and trash, the roles that none of `folders` has, in this order: the setup asks the user to choose.
std::vector<std::string> roles_needing_choice(const std::vector<Folder>& folders);

}  // namespace pbo
