#pragma once
// Character sets for mail text (research R-05). Built-in tables cover the sets that matter most (UTF-8, US-ASCII,
// ISO-8859-1/-2/-15, Windows-1250/-1252, UTF-16/32); any other set goes through iconv when the system has it. The
// result is ALWAYS valid UTF-8: bytes that cannot be converted become U+FFFD.

#include <string>
#include <string_view>

namespace pbo {

// Converts `bytes` in `charset` (a MIME label, any case, aliases such as latin1) to UTF-8.
//  - an empty label means "not stated": valid UTF-8 is kept, anything else is read as Windows-1252;
//  - an unknown label falls back to Windows-1252 and sets *supported = false (the reader then shows a note).
std::string to_utf8(std::string_view bytes, std::string_view charset, bool* supported = nullptr);

// True if `s` is well-formed UTF-8 (no overlong forms, surrogates or code points above U+10FFFF).
bool is_valid_utf8(std::string_view s);

// `s` with every ill-formed sequence replaced by U+FFFD; valid input is returned unchanged.
std::string sanitize_utf8(std::string_view s);

}  // namespace pbo
