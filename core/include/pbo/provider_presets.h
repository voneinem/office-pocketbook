#pragma once
// Presets by mail provider (research R-09, E1, FR-016): whether the provider files a copy of sent mail itself. Only EXACT host names count
// (case-insensitive); "imap.gmail.com.evil.example" or "myimap.gmail.com" are not Gmail. The preset is applied when the account is saved for
// the first time; after that the user's choice (Settings) is kept.

#include <optional>
#include <string>
#include <string_view>

#include "pbo/account.h"

namespace pbo {

struct SentCopyDefault {
    bool sent_by_server = true;   // the provider files sent mail itself: the app does not APPEND a copy
    int imap_port = 993;
    int smtp_port = 465;
};

// nullopt for every host that is not in the table: such an account gets sent_by_server = false
std::optional<SentCopyDefault> preset_for_host(std::string_view imap_host);

// The account as it should be stored. First save (`existing` empty): sent_by_server comes from the preset. Later saves: everything that is not
// in the account form (sent_by_server, the days of mail kept, the limits, the folder roles, the creation time) is taken from `existing`.
MailAccount merge_for_save(MailAccount from_form, const std::optional<MailAccount>& existing);

}  // namespace pbo
