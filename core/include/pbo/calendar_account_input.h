#pragma once
// Validation of the calendar account form (data-model.md, table account_calendar; E2, Principle IV). Pure functions:
// a rejected value is reported with a stable key and never reaches the store or a transport.

#include <string>
#include <vector>

#include "pbo/account_input.h"

namespace pbo {

struct CalendarAccountInput {
    std::string caldav_url;    // "" = no server account
    std::string username;
    std::string password;
    std::string feed_url;      // "" = no feed
    std::string feed_name;
};

// Empty result = valid (the input is normalised first, so "webcal://" is accepted). Field names: "caldav_url", "username",
// "password", "feed_url". Keys: "calendar_source_missing" (neither given), "url_invalid", "url_not_https", "url_has_credentials",
// "username_invalid", "password_empty", "password_invalid".
// Rules: https only, a plain DNS name or IPv4 host, optional port 1..65535, no user:password@, no spaces or control characters,
// at most 2048 characters; a server address has no query or fragment; a feed address may have a query but no fragment.
std::vector<FieldError> validate_calendar_account(const CalendarAccountInput& input, bool check_password = true);   // false: the stored part only

// Trims blanks, lower-cases the scheme, rewrites webcal:// and webcals:// to https://, trims the user name; an empty (blank)
// server address also clears the user name and password. The password is otherwise left as it is.
CalendarAccountInput normalize_calendar_account(const CalendarAccountInput& input);

}  // namespace pbo
