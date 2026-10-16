#include "pbo/provider_presets.h"

#include <algorithm>
#include <cctype>

namespace pbo {

std::optional<SentCopyDefault> preset_for_host(std::string_view imap_host) {
    std::string h(imap_host);
    std::transform(h.begin(), h.end(), h.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    // a trailing dot is the same name in DNS; anything else around the name is not
    if (!h.empty() && h.back() == '.') h.pop_back();
    static const char* kHosts[] = {"imap.gmail.com", "outlook.office365.com", "imap-mail.outlook.com"};
    for (const char* k : kHosts)
        if (h == k) return SentCopyDefault{};
    return std::nullopt;
}

MailAccount merge_for_save(MailAccount a, const std::optional<MailAccount>& existing) {
    if (!existing) {
        a.sent_by_server = preset_for_host(a.imap_host).has_value();
        return a;
    }
    a.sent_by_server = existing->sent_by_server;
    a.sync_days = existing->sync_days;
    a.attachment_limit = existing->attachment_limit;
    a.storage_limit = existing->storage_limit;
    a.sent_folder_id = existing->sent_folder_id;
    a.drafts_folder_id = existing->drafts_folder_id;
    a.trash_folder_id = existing->trash_folder_id;
    a.capabilities = existing->capabilities;
    a.created = existing->created;
    return a;
}

}  // namespace pbo
