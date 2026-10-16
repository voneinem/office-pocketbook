#include "pbo/calendar_account_input.h"

#include <cctype>

namespace pbo {

namespace {

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool has_control_or_blank(const std::string& s) {
    for (unsigned char c : s)
        if (c <= 0x20 || c == 0x7f) return true;
    return false;
}

std::string rewrite_scheme(const std::string& url, bool feed) {
    const size_t sep = url.find("://");
    if (sep == std::string::npos) return url;
    std::string scheme = lower(url.substr(0, sep));
    if (feed && (scheme == "webcal" || scheme == "webcals")) scheme = "https";
    return scheme + url.substr(sep);
}

// "" = fine, else the key
std::string check_url(const std::string& url, bool feed) {
    if (url.size() > 2048 || has_control_or_blank(url)) return "url_invalid";
    const size_t sep = url.find("://");
    if (sep == std::string::npos) return "url_invalid";
    const std::string scheme = url.substr(0, sep);
    const std::string rest = url.substr(sep + 3);
    if (scheme == "http") return "url_not_https";
    if (scheme != "https") return "url_invalid";
    const size_t auth_end = rest.find_first_of("/?#");
    const std::string authority = rest.substr(0, auth_end);
    const std::string tail = auth_end == std::string::npos ? std::string() : rest.substr(auth_end);
    if (authority.empty()) return "url_invalid";
    if (authority.find('@') != std::string::npos) return "url_has_credentials";
    std::string host = authority;
    const size_t colon = authority.find(':');
    if (colon != std::string::npos) {
        host = authority.substr(0, colon);
        const std::string port = authority.substr(colon + 1);
        if (port.empty() || port.size() > 5) return "url_invalid";
        int p = 0;
        for (char c : port) {
            if (c < '0' || c > '9') return "url_invalid";
            p = p * 10 + (c - '0');
        }
        if (p < 1 || p > 65535) return "url_invalid";
    }
    if (!is_valid_host(host)) return "url_invalid";
    if (tail.find('#') != std::string::npos) return "url_invalid";
    if (!feed && tail.find('?') != std::string::npos) return "url_invalid";
    return std::string();
}

bool has_control(const std::string& s) {
    for (unsigned char c : s)
        if (c < 0x20 || c == 0x7f) return true;
    return false;
}

}  // namespace

CalendarAccountInput normalize_calendar_account(const CalendarAccountInput& input) {
    CalendarAccountInput out = input;
    out.caldav_url = rewrite_scheme(trim(input.caldav_url), false);
    out.feed_url = rewrite_scheme(trim(input.feed_url), true);
    out.username = trim(input.username);
    out.feed_name = trim(input.feed_name);
    if (out.caldav_url.empty()) {
        out.username.clear();
        out.password.clear();
    }
    return out;
}

std::vector<FieldError> validate_calendar_account(const CalendarAccountInput& raw, bool check_password) {
    const CalendarAccountInput in = normalize_calendar_account(raw);
    std::vector<FieldError> errors;
    if (in.caldav_url.empty() && in.feed_url.empty()) {
        errors.push_back({"caldav_url", "calendar_source_missing"});
        return errors;
    }
    if (!in.caldav_url.empty()) {
        const std::string k = check_url(in.caldav_url, false);
        if (!k.empty()) errors.push_back({"caldav_url", k});
        if (in.username.empty() || has_control(in.username)) errors.push_back({"username", "username_invalid"});
        if (check_password) {
            if (in.password.empty()) errors.push_back({"password", "password_empty"});
            else if (has_control(in.password)) errors.push_back({"password", "password_invalid"});
        }
    }
    if (!in.feed_url.empty()) {
        const std::string k = check_url(in.feed_url, true);
        if (!k.empty()) errors.push_back({"feed_url", k});
    }
    return errors;
}

}  // namespace pbo
