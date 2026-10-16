#pragma once
// Validation of the mail account form (data-model.md, table account_mail: "validated on entry"). Pure functions:
// a rejected value is reported with a stable key and never reaches the store or a transport.

#include <string>
#include <vector>

#include "pbo/account.h"

namespace pbo {

struct MailAccountInput {
    MailAccount account;
    std::string imap_password;
    std::string smtp_password;   // empty = the same as the mail password
};

struct FieldError {
    std::string field;   // "imap_host", "imap_port", "imap_security", "smtp_host", ..., "username", "address", ...
    std::string key;     // "host_invalid", "port_invalid", "starttls_port_not_allowed", "username_invalid",
                         // "address_invalid", "password_empty", "password_invalid", "value_out_of_range", ...
};

// Empty result = valid. Rules: hosts are plain DNS names or IPv4 addresses (no spaces, control characters, scheme,
// credentials, path or port); ports 1..65535; STARTTLS only on the ports where the upgrade is enforced and
// tested (IMAP 143, SMTP 587); user names and passwords without control characters; the address has one '@'.
std::vector<FieldError> validate_mail_account(const MailAccountInput& input);

// The same checks for the stored part only (no passwords), used by Store::save_mail_account.
std::vector<FieldError> validate_mail_account(const MailAccount& account);

// True for a syntactically acceptable host name or IPv4 address.
bool is_valid_host(const std::string& host);

// True for a plain "local@domain" address (no display name, no angle brackets): one '@', a local part without blanks,
// quotes, brackets or control characters, a domain that is a host name with at least one dot.
bool is_valid_address(const std::string& address);

}  // namespace pbo
