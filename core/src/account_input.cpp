#include "pbo/account_input.h"

#include <cctype>

namespace pbo {

namespace {

bool has_control(const std::string& s) {
    for (unsigned char c : s)
        if (c < 0x20 || c == 0x7F) return true;
    return false;
}

bool is_label_char(unsigned char c) { return std::isalnum(c) || c == '-' || c == '_'; }

bool valid_name_field(const std::string& s, size_t max) { return !s.empty() && s.size() <= max && !has_control(s); }

}  // namespace

bool is_valid_address(const std::string& a) {
    if (a.empty() || a.size() > 254 || has_control(a)) return false;
    const size_t at = a.find('@');
    if (at == std::string::npos || at != a.rfind('@') || at == 0 || at > 64) return false;
    for (size_t i = 0; i < at; ++i) {
        const unsigned char c = static_cast<unsigned char>(a[i]);
        if (c == ' ' || c == '<' || c == '>' || c == '"' || c == '(' || c == ')' || c == ',' || c == ';' || c == ':' || c == '\\' || c == '[' || c == ']' || c >= 0x80) return false;
    }
    const std::string domain = a.substr(at + 1);
    return domain.find('.') != std::string::npos && is_valid_host(domain);
}

namespace {

void check_stored_part(const MailAccount& a, std::vector<FieldError>* errors) {
    auto add = [&](const char* field, const char* key) { errors->push_back({field, key}); };
    if (!is_valid_host(a.imap_host)) add("imap_host", "host_invalid");
    if (a.imap_port < 1 || a.imap_port > 65535) add("imap_port", "port_invalid");
    if (a.imap_security == Security::StartTls && a.imap_port != 143) add("imap_security", "starttls_port_not_allowed");
    if (!is_valid_host(a.smtp_host)) add("smtp_host", "host_invalid");
    if (a.smtp_port < 1 || a.smtp_port > 65535) add("smtp_port", "port_invalid");
    if (a.smtp_security == Security::StartTls && a.smtp_port != 587) add("smtp_security", "starttls_port_not_allowed");
    if (!valid_name_field(a.username, 255)) add("username", "username_invalid");
    if (!a.smtp_username.empty() && !valid_name_field(a.smtp_username, 255)) add("smtp_username", "username_invalid");
    if (!is_valid_address(a.address)) add("address", "address_invalid");
    if (a.display_name.size() > 200 || has_control(a.display_name)) add("display_name", "display_name_invalid");
    if (a.sync_days < 1 || a.sync_days > 365) add("sync_days", "value_out_of_range");
    if (a.attachment_limit < 1048576 || a.attachment_limit > 104857600) add("attachment_limit", "value_out_of_range");
    if (a.storage_limit < 20971520) add("storage_limit", "value_out_of_range");
}

bool bad_password(const std::string& p) {
    for (unsigned char c : p)
        if (c < 0x20 || c == 0x7F) return true;
    return p.size() > 1024;
}

}  // namespace

bool is_valid_host(const std::string& host) {
    if (host.empty() || host.size() > 253) return false;
    size_t label_len = 0;
    char prev = '.';
    for (unsigned char c : host) {
        if (c == '.') {
            if (label_len == 0) return false;   // empty label
            if (prev == '-') return false;
            label_len = 0;
        } else if (is_label_char(c)) {
            if (label_len == 0 && c == '-') return false;
            if (++label_len > 63) return false;
        } else {
            return false;   // blanks, control characters, '/', '@', ':', '?', '#', non-ASCII ...
        }
        prev = static_cast<char>(c);
    }
    return label_len > 0 && prev != '-';
}

std::vector<FieldError> validate_mail_account(const MailAccount& account) {
    std::vector<FieldError> errors;
    check_stored_part(account, &errors);
    return errors;
}

std::vector<FieldError> validate_mail_account(const MailAccountInput& input) {
    std::vector<FieldError> errors;
    check_stored_part(input.account, &errors);
    if (input.imap_password.empty()) errors.push_back({"imap_password", "password_empty"});
    else if (bad_password(input.imap_password)) errors.push_back({"imap_password", "password_invalid"});
    if (!input.smtp_password.empty() && bad_password(input.smtp_password)) errors.push_back({"smtp_password", "password_invalid"});
    return errors;
}

}  // namespace pbo
