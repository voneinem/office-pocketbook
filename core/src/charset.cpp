#include "pbo/charset.h"

#include <iconv.h>

#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstring>

#include "charset_tables.h"

namespace pbo {

namespace {

constexpr uint32_t kReplacement = 0xFFFD;

void put(std::string* out, uint32_t cp) {
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = kReplacement;
    if (cp < 0x80) {
        *out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        *out += static_cast<char>(0xC0 | (cp >> 6));
        *out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        *out += static_cast<char>(0xE0 | (cp >> 12));
        *out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        *out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        *out += static_cast<char>(0xF0 | (cp >> 18));
        *out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        *out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        *out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

// Length of the well-formed UTF-8 sequence at s[i], or 0 (RFC 3629: no overlong forms, no surrogates, nothing above U+10FFFF).
size_t utf8_sequence(std::string_view s, size_t i) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    auto cont = [&](size_t k) { return i + k < s.size() && (static_cast<unsigned char>(s[i + k]) & 0xC0) == 0x80; };
    if (c < 0x80) return 1;
    if (c >= 0xC2 && c <= 0xDF) return cont(1) ? 2 : 0;
    if (c >= 0xE0 && c <= 0xEF) {
        if (!cont(1) || !cont(2)) return 0;
        const unsigned char c1 = static_cast<unsigned char>(s[i + 1]);
        if (c == 0xE0 && c1 < 0xA0) return 0;   // overlong
        if (c == 0xED && c1 >= 0xA0) return 0;  // surrogates
        return 3;
    }
    if (c >= 0xF0 && c <= 0xF4) {
        if (!cont(1) || !cont(2) || !cont(3)) return 0;
        const unsigned char c1 = static_cast<unsigned char>(s[i + 1]);
        if (c == 0xF0 && c1 < 0x90) return 0;   // overlong
        if (c == 0xF4 && c1 >= 0x90) return 0;  // above U+10FFFF
        return 4;
    }
    return 0;
}

enum class Kind { Utf8, Ascii, Latin1, Latin2, Latin9, Win1250, Win1252, Utf16, Utf16LE, Utf16BE, Utf32, Utf32LE, Utf32BE, Unknown };

Kind kind_of(std::string_view label) {
    std::string l;
    for (char c : label) {
        if (c == '"' || c == '\'' || std::isspace(static_cast<unsigned char>(c))) continue;
        l += c == '_' ? '-' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    struct Alias { const char* name; Kind kind; };
    static const Alias kAliases[] = {
        {"utf-8", Kind::Utf8}, {"utf8", Kind::Utf8},
        {"us-ascii", Kind::Ascii}, {"ascii", Kind::Ascii}, {"ansi-x3.4-1968", Kind::Ascii}, {"iso646-us", Kind::Ascii}, {"us", Kind::Ascii},
        {"iso-8859-1", Kind::Latin1}, {"iso8859-1", Kind::Latin1}, {"latin1", Kind::Latin1}, {"latin-1", Kind::Latin1}, {"l1", Kind::Latin1},
        {"iso-ir-100", Kind::Latin1}, {"cp819", Kind::Latin1}, {"ibm819", Kind::Latin1}, {"8859-1", Kind::Latin1},
        {"iso-8859-2", Kind::Latin2}, {"iso8859-2", Kind::Latin2}, {"latin2", Kind::Latin2}, {"latin-2", Kind::Latin2}, {"l2", Kind::Latin2},
        {"iso-8859-15", Kind::Latin9}, {"iso8859-15", Kind::Latin9}, {"latin9", Kind::Latin9}, {"latin-9", Kind::Latin9}, {"l9", Kind::Latin9},
        {"windows-1250", Kind::Win1250}, {"cp1250", Kind::Win1250}, {"x-cp1250", Kind::Win1250},
        {"windows-1252", Kind::Win1252}, {"cp1252", Kind::Win1252}, {"x-cp1252", Kind::Win1252},
        {"utf-16", Kind::Utf16}, {"utf-16le", Kind::Utf16LE}, {"utf-16be", Kind::Utf16BE},
        {"utf-32", Kind::Utf32}, {"utf-32le", Kind::Utf32LE}, {"utf-32be", Kind::Utf32BE}};
    for (const Alias& a : kAliases)
        if (l == a.name) return a.kind;
    return Kind::Unknown;
}

std::string decode_utf8(std::string_view s) { return sanitize_utf8(s); }

std::string decode_single_byte(std::string_view s, Kind kind) {
    std::string out;
    out.reserve(s.size() + s.size() / 4);
    for (unsigned char c : s) {
        uint32_t cp;
        if (c < 0x80) {
            cp = c;
        } else if (kind == Kind::Ascii) {
            cp = kReplacement;
        } else if (kind == Kind::Win1252 || ((kind == Kind::Latin1 || kind == Kind::Latin9 || kind == Kind::Latin2) && c < 0xA0)) {
            // mail labelled ISO-8859-x very often really is Windows-1252 in the 0x80..0x9F range (smart quotes): read it that way
            cp = c < 0xA0 ? charset_tables::kWindows1252[c - 0x80] : c;
        } else if (kind == Kind::Latin1) {
            cp = c;
        } else if (kind == Kind::Latin2) {
            cp = charset_tables::kIso8859_2[c - 0xA0];
        } else if (kind == Kind::Latin9) {
            cp = charset_tables::kIso8859_15[c - 0xA0];
        } else {   // Win1250
            cp = charset_tables::kWindows1250[c - 0x80];
        }
        put(&out, cp);
    }
    return out;
}

std::string decode_utf16(std::string_view s, Kind kind) {
    bool big = kind != Kind::Utf16LE;
    size_t i = 0;
    if (s.size() >= 2) {
        const unsigned char a = static_cast<unsigned char>(s[0]), b = static_cast<unsigned char>(s[1]);
        if (a == 0xFE && b == 0xFF) { big = true; i = 2; }
        else if (a == 0xFF && b == 0xFE) { big = false; i = 2; }
    }
    std::string out;
    while (i + 1 < s.size()) {
        auto unit = [&](size_t at) {
            const unsigned char x = static_cast<unsigned char>(s[at]), y = static_cast<unsigned char>(s[at + 1]);
            return static_cast<uint32_t>(big ? (x << 8) | y : (y << 8) | x);
        };
        uint32_t u = unit(i);
        i += 2;
        if (u >= 0xD800 && u <= 0xDBFF) {
            if (i + 1 < s.size()) {
                const uint32_t low = unit(i);
                if (low >= 0xDC00 && low <= 0xDFFF) {
                    put(&out, 0x10000 + ((u - 0xD800) << 10) + (low - 0xDC00));
                    i += 2;
                    continue;
                }
            }
            put(&out, kReplacement);
        } else if (u >= 0xDC00 && u <= 0xDFFF) {
            put(&out, kReplacement);
        } else {
            put(&out, u);
        }
    }
    if (i < s.size()) put(&out, kReplacement);   // a half unit at the end
    return out;
}

std::string decode_utf32(std::string_view s, Kind kind) {
    bool big = kind != Kind::Utf32LE;
    size_t i = 0;
    if (s.size() >= 4) {
        auto b = [&](size_t k) { return static_cast<unsigned char>(s[k]); };
        if (b(0) == 0 && b(1) == 0 && b(2) == 0xFE && b(3) == 0xFF) { big = true; i = 4; }
        else if (b(0) == 0xFF && b(1) == 0xFE && b(2) == 0 && b(3) == 0) { big = false; i = 4; }
    }
    std::string out;
    while (i + 3 < s.size()) {
        auto b = [&](size_t k) { return static_cast<uint32_t>(static_cast<unsigned char>(s[i + k])); };
        const uint32_t cp = big ? (b(0) << 24) | (b(1) << 16) | (b(2) << 8) | b(3) : (b(3) << 24) | (b(2) << 16) | (b(1) << 8) | b(0);
        put(&out, cp);
        i += 4;
    }
    if (i < s.size()) put(&out, kReplacement);
    return out;
}

// Other sets through the system's iconv; false if it does not know the set.
bool decode_with_iconv(std::string_view s, const std::string& label, std::string* out) {
    iconv_t cd = iconv_open("UTF-8", label.c_str());
    if (cd == reinterpret_cast<iconv_t>(-1)) return false;
    std::string result;
    std::string in(s);
    char* ip = in.empty() ? nullptr : &in[0];
    size_t left = in.size();
    char buf[4096];
    while (left > 0) {
        char* op = buf;
        size_t room = sizeof buf;
        const size_t rc = iconv(cd, &ip, &left, &op, &room);
        result.append(buf, static_cast<size_t>(op - buf));
        if (rc == static_cast<size_t>(-1)) {
            if (errno == E2BIG) continue;
            // an invalid or incomplete sequence: one replacement character, skip one byte
            put(&result, kReplacement);
            if (left > 0) { ++ip; --left; }
            iconv(cd, nullptr, nullptr, nullptr, nullptr);   // reset the shift state
        }
    }
    iconv_close(cd);
    *out = sanitize_utf8(result);
    return true;
}

}  // namespace

bool is_valid_utf8(std::string_view s) {
    for (size_t i = 0; i < s.size();) {
        const size_t n = utf8_sequence(s, i);
        if (n == 0) return false;
        i += n;
    }
    return true;
}

std::string sanitize_utf8(std::string_view s) {
    if (is_valid_utf8(s)) return std::string(s);
    std::string out;
    out.reserve(s.size() + 8);
    for (size_t i = 0; i < s.size();) {
        const size_t n = utf8_sequence(s, i);
        if (n == 0) {
            put(&out, kReplacement);
            ++i;
        } else {
            out.append(s.substr(i, n));
            i += n;
        }
    }
    return out;
}

std::string to_utf8(std::string_view bytes, std::string_view charset, bool* supported) {
    if (supported) *supported = true;
    bool blank = true;
    for (char c : charset)
        if (!std::isspace(static_cast<unsigned char>(c))) blank = false;
    if (blank) return is_valid_utf8(bytes) ? std::string(bytes) : decode_single_byte(bytes, Kind::Win1252);

    switch (kind_of(charset)) {
        case Kind::Utf8: return decode_utf8(bytes);
        case Kind::Ascii:
        case Kind::Latin1:
        case Kind::Latin2:
        case Kind::Latin9:
        case Kind::Win1250:
        case Kind::Win1252: return decode_single_byte(bytes, kind_of(charset));
        case Kind::Utf16:
        case Kind::Utf16LE:
        case Kind::Utf16BE: return decode_utf16(bytes, kind_of(charset));
        case Kind::Utf32:
        case Kind::Utf32LE:
        case Kind::Utf32BE: return decode_utf32(bytes, kind_of(charset));
        case Kind::Unknown: break;
    }
    std::string label;
    for (char c : charset)
        if (!std::isspace(static_cast<unsigned char>(c)) && c != '"' && c != '\'') label += c;
    std::string out;
    if (label.size() <= 64 && decode_with_iconv(bytes, label, &out)) return out;
    if (supported) *supported = false;
    return decode_single_byte(bytes, Kind::Win1252);
}

}  // namespace pbo
