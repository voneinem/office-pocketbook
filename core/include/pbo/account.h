#pragma once
// The mail account as stored (data-model.md, table account_mail). Passwords are NOT part of it: they live in the
// credentials file (credentials_file.h) and are passed to the transports per connection.

#include <cstdint>
#include <string>

#include "pbo/curl_session.h"

namespace pbo {

struct MailAccount {
    std::string imap_host;
    int imap_port = 993;
    Security imap_security = Security::Tls;
    std::string smtp_host;
    int smtp_port = 465;
    Security smtp_security = Security::Tls;
    std::string username;
    std::string smtp_username;     // empty = same as username
    std::string address;           // From address
    std::string display_name;
    bool sent_by_server = false;   // "the server files sent mail itself"
    int sync_days = 30;            // 1..365
    int64_t attachment_limit = 10485760;   // 1 MiB .. 100 MiB
    int64_t storage_limit = 209715200;     // >= 20 MiB
    int64_t sent_folder_id = 0;    // 0 = not chosen yet
    int64_t drafts_folder_id = 0;
    int64_t trash_folder_id = 0;
    std::string capabilities;      // last CAPABILITY tokens, informational
    int64_t created = 0;           // set by the store on the first save
};

}  // namespace pbo
