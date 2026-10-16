#pragma once
// Minimal RFC 5322 header helpers for the header list. RFC 2047 encoded words, charsets and address-list syntax
// arrive with the MIME module; until then the raw (unfolded) text is kept.

#include <cstdint>
#include <string>
#include <string_view>

namespace pbo {

// Value of the first header `name` (case-insensitive) in a header block, with folded lines joined by one space and
// surrounding blanks removed. "" if absent.
std::string header_value(std::string_view block, std::string_view name);

// RFC 5322 date ("Mon, 05 Oct 2026 10:00:00 +0200", day of week and seconds optional, zone offset or GMT/UT/Z)
// to epoch seconds. Returns false for anything else.
bool parse_rfc5322_date(std::string_view text, int64_t* epoch);

// "Anna <anna@example.org>", "\"Beispiel, Anna\" <anna@example.org>", "anna@example.org" -> display name, address.
void split_first_address(std::string_view text, std::string* name, std::string* address);

// Seconds since 1970-01-01 00:00:00 UTC of a proleptic Gregorian date (no time zone, no leap seconds).
// Month 1..12; day 1..31 (not checked against the month length).
int64_t epoch_from_civil(int year, int month, int day, int hour, int minute, int second);

// 1..31 + month name + year, as IMAP wants it in SEARCH SINCE: "05-Oct-2026".
std::string imap_date(int64_t epoch);

}  // namespace pbo
