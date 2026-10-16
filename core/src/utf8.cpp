#include "pbo/utf8.h"

#include <cstdint>
#include <string>

namespace pbo {

namespace {

struct Decoded {
    bool valid = false;
    uint32_t cp = 0;
    size_t length = 1;
};

Decoded decode(std::string_view s, size_t pos) {
    Decoded d;
    if (pos >= s.size()) return d;
    const unsigned char b = static_cast<unsigned char>(s[pos]);
    size_t n;
    uint32_t cp;
    uint32_t min;
    if (b < 0x80) { d.valid = true; d.cp = b; return d; }
    if ((b & 0xE0) == 0xC0) { n = 2; cp = b & 0x1F; min = 0x80; }
    else if ((b & 0xF0) == 0xE0) { n = 3; cp = b & 0x0F; min = 0x800; }
    else if ((b & 0xF8) == 0xF0) { n = 4; cp = b & 0x07; min = 0x10000; }
    else return d;
    if (pos + n > s.size()) return d;
    for (size_t k = 1; k < n; ++k) {
        const unsigned char c = static_cast<unsigned char>(s[pos + k]);
        if ((c & 0xC0) != 0x80) return d;
        cp = (cp << 6) | (c & 0x3F);
    }
    if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return d;  // overlong, range, surrogate
    d.valid = true;
    d.cp = cp;
    d.length = n;
    return d;
}

bool is_regional_indicator(uint32_t cp) { return cp >= 0x1F1E6 && cp <= 0x1F1FF; }

// Code points that attach to the previous character.
bool is_extender(uint32_t cp) {
    return (cp >= 0x0300 && cp <= 0x036F) ||   // combining diacritical marks
           (cp >= 0x1AB0 && cp <= 0x1AFF) || (cp >= 0x1DC0 && cp <= 0x1DFF) ||
           (cp >= 0x20D0 && cp <= 0x20FF) ||   // combining marks for symbols, including the keycap U+20E3
           (cp >= 0xFE00 && cp <= 0xFE0F) ||   // variation selectors
           (cp >= 0xFE20 && cp <= 0xFE2F) ||   // combining half marks
           (cp >= 0x1F3FB && cp <= 0x1F3FF) || // emoji skin tone modifiers
           (cp >= 0xE0020 && cp <= 0xE007F) || // tag characters
           (cp >= 0xE0100 && cp <= 0xE01EF);   // variation selectors supplement
}

constexpr uint32_t kZwj = 0x200D;

}  // namespace

size_t utf8_cluster_end(std::string_view text, size_t pos) {
    if (pos >= text.size()) return text.size();
    const Decoded first = decode(text, pos);
    if (!first.valid) return pos + 1;
    size_t end = pos + first.length;

    if (is_regional_indicator(first.cp)) {  // a flag is a pair of regional indicators
        const Decoded second = decode(text, end);
        if (second.valid && is_regional_indicator(second.cp)) end += second.length;
    }
    for (;;) {
        const Decoded next = decode(text, end);
        if (!next.valid) break;
        if (is_extender(next.cp)) {
            end += next.length;
            continue;
        }
        if (next.cp == kZwj) {
            const Decoded joined = decode(text, end + next.length);
            if (joined.valid && joined.cp >= 0x80) {  // ZWJ + a character: one unit
                end += next.length + joined.length;
                continue;
            }
        }
        break;
    }
    return end;
}

namespace {
// U+00C0..U+00FF and U+0100..U+017F folded to lower-case ASCII ("" = keep the character as it is)
const char* const kLatin1Fold[64] = {"a","a","a","a","a","a","ae","c","e","e","e","e","i","i","i","i","d","n","o","o","o","o","o","","o","u","u","u","u","y","th","ss","a","a","a","a","a","a","ae","c","e","e","e","e","i","i","i","i","d","n","o","o","o","o","o","","o","u","u","u","u","y","th","y"};
const char* const kLatinExtAFold[128] = {"a","a","a","a","a","a","c","c","c","c","c","c","c","c","d","d","d","d","e","e","e","e","e","e","e","e","e","e","g","g","g","g","g","g","g","g","h","h","h","h","i","i","i","i","i","i","i","i","i","i","ij","ij","j","j","k","k","k","l","l","l","l","l","l","ŀ","ŀ","l","l","n","n","n","n","n","n","n","n","n","o","o","o","o","o","o","oe","oe","r","r","r","r","r","r","s","s","s","s","s","s","s","s","t","t","t","t","t","t","u","u","u","u","u","u","u","u","u","u","u","u","w","w","y","y","y","z","z","z","z","z","z","s"};
}  // namespace

std::string sort_key(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    size_t i = 0;
    while (i < text.size()) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        uint32_t cp = c;
        size_t len = 1;
        if (c >= 0xC2 && c <= 0xF4) {
            len = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : 2;
            if (i + len > text.size()) len = 1;
            else {
                cp = c & (len == 2 ? 0x1F : len == 3 ? 0x0F : 0x07);
                for (size_t k = 1; k < len; ++k) cp = (cp << 6) | (static_cast<unsigned char>(text[i + k]) & 0x3F);
            }
        }
        const std::string_view raw = text.substr(i, len);
        i += len;
        if (len == 1 && c < 0x80) {
            out += static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
        } else if (cp >= 0xC0 && cp <= 0xFF && kLatin1Fold[cp - 0xC0][0]) {
            out += kLatin1Fold[cp - 0xC0];
        } else if (cp >= 0x100 && cp <= 0x17F) {
            out += kLatinExtAFold[cp - 0x100];
        } else if (cp >= 0x410 && cp <= 0x42F) {   // Cyrillic capitals
            const uint32_t lower = cp + 0x20;
            out += static_cast<char>(0xC0 | (lower >> 6));
            out += static_cast<char>(0x80 | (lower & 0x3F));
        } else if (cp >= 0x391 && cp <= 0x3A9) {   // Greek capitals
            const uint32_t lower = cp + 0x20;
            out += static_cast<char>(0xC0 | (lower >> 6));
            out += static_cast<char>(0x80 | (lower & 0x3F));
        } else {
            out.append(raw);
        }
    }
    return out;
}

}  // namespace pbo
