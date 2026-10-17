// T056 (charset part): to_utf8 converts the character sets mail uses and ALWAYS returns valid UTF-8.
#include <string>

#include "pbo/charset.h"
#include "test_support.h"

using pbo::to_utf8;

namespace {
std::string s(const char* c, size_t n) { return std::string(c, n); }
const std::string kFffd = "\xEF\xBF\xBD";
}  // namespace

PB_TEST(utf8_passes_through_and_ill_formed_bytes_become_replacement_characters) {
    bool ok = false;
    PB_CHECK_EQ(to_utf8("Gr\xC3\xBC\xC3\x9F" "e \xE2\x82\xAC \xF0\x9F\x93\xA7", "UTF-8", &ok), std::string("Gr\xC3\xBC\xC3\x9F" "e \xE2\x82\xAC \xF0\x9F\x93\xA7"));
    PB_CHECK(ok);
    PB_CHECK_EQ(to_utf8("a\xFF" "b", "utf-8"), "a" + kFffd + "b");
    PB_CHECK_EQ(to_utf8("a\xC3", "utf-8"), "a" + kFffd);                      // cut in the middle of a character
    PB_CHECK(pbo::is_valid_utf8(to_utf8("\xC0\xAF\xED\xA0\x80\xF4\x90\x80\x80", "utf-8")));   // overlong, surrogate, > U+10FFFF
    PB_CHECK(!pbo::is_valid_utf8("\xC0\xAF"));
    PB_CHECK(!pbo::is_valid_utf8("\xED\xA0\x80"));
    PB_CHECK(pbo::is_valid_utf8("\xF0\x9F\x93\xA7"));
    PB_CHECK_EQ(pbo::sanitize_utf8("ok"), std::string("ok"));
    PB_CHECK_EQ(pbo::sanitize_utf8("a\x80"), "a" + kFffd);
}

PB_TEST(ascii_keeps_ascii_and_replaces_high_bytes) {
    PB_CHECK_EQ(to_utf8("plain text", "US-ASCII"), std::string("plain text"));
    PB_CHECK_EQ(to_utf8("a\xE4" "b", "ascii"), "a" + kFffd + "b");
}

PB_TEST(latin1_and_its_aliases) {
    const std::string want = "\xC3\xA4\xC3\xB6\xC3\xBC\xC3\x9F";   // äöüß
    for (const char* label : {"ISO-8859-1", "iso-8859-1", "latin1", "Latin-1", "l1", "ISO_8859-1", "cp819"}) {
        bool ok = false;
        PB_CHECK_EQ(to_utf8("\xE4\xF6\xFC\xDF", label, &ok), want);
        PB_CHECK(ok);
    }
}

PB_TEST(latin9_has_the_euro_sign_and_the_other_letters) {
    PB_CHECK_EQ(to_utf8("\xA4\xE4", "ISO-8859-15"), std::string("\xE2\x82\xAC\xC3\xA4"));
    PB_CHECK_EQ(to_utf8("\xA6\xA8\xBC\xBD\xBE", "latin9"), std::string("\xC5\xA0\xC5\xA1\xC5\x92\xC5\x93\xC5\xB8"));   // Š š Œ œ Ÿ
}

PB_TEST(latin2_covers_central_european_letters) {
    PB_CHECK_EQ(to_utf8("\xB1\xE8\xA3\xF8\xA9", "ISO-8859-2"), std::string("\xC4\x85\xC4\x8D\xC5\x81\xC5\x99\xC5\xA0"));   // ą č Ł ř Š
    PB_CHECK_EQ(to_utf8("\xE4", "latin2"), std::string("\xC3\xA4"));
}

PB_TEST(windows_1252_has_the_typographic_characters) {
    PB_CHECK_EQ(to_utf8("\x80\x93quoted\x94 \x96 \x9F", "windows-1252"), std::string("\xE2\x82\xAC\xE2\x80\x9Cquoted\xE2\x80\x9D \xE2\x80\x93 \xC5\xB8"));
    PB_CHECK_EQ(to_utf8("\x81\x8D\x8F\x90\x9D", "cp1252"), kFffd + kFffd + kFffd + kFffd + kFffd);   // undefined bytes
    PB_CHECK_EQ(to_utf8("\xE4\xFC", "CP1252"), std::string("\xC3\xA4\xC3\xBC"));
}

PB_TEST(windows_1250_covers_czech_polish_and_hungarian_letters) {
    PB_CHECK_EQ(to_utf8("\x8A\x9A\xE8\xF8\xB9\xEA", "windows-1250"), std::string("\xC5\xA0\xC5\xA1\xC4\x8D\xC5\x99\xC4\x85\xC4\x99"));   // Š š č ř ą ę
}

PB_TEST(utf16_and_utf32_with_and_without_a_byte_order_mark) {
    PB_CHECK_EQ(to_utf8(s("\xFF\xFE" "A\0\xE4\0", 6), "utf-16"), std::string("A\xC3\xA4"));
    PB_CHECK_EQ(to_utf8(s("\xFE\xFF\0A\0\xE4", 6), "utf-16"), std::string("A\xC3\xA4"));
    PB_CHECK_EQ(to_utf8(s("\0A\0\xE4", 4), "utf-16"), std::string("A\xC3\xA4"));             // no mark: big endian (RFC 2781)
    PB_CHECK_EQ(to_utf8(s("A\0\xE4\0", 4), "utf-16le"), std::string("A\xC3\xA4"));
    PB_CHECK_EQ(to_utf8(s("\0A\0\xE4", 4), "UTF-16BE"), std::string("A\xC3\xA4"));
    PB_CHECK_EQ(to_utf8(s("\x3D\xD8\xE7\xDC", 4), "utf-16le"), std::string("\xF0\x9F\x93\xA7"));   // surrogate pair U+1F4E7
    PB_CHECK_EQ(to_utf8(s("\x3D\xD8", 2), "utf-16le"), kFffd);                                   // lone surrogate
    PB_CHECK_EQ(to_utf8(s("A\0", 1), "utf-16le"), kFffd);                                        // odd length
    PB_CHECK_EQ(to_utf8(s("\xFF\xFE\0\0" "A\0\0\0", 8), "utf-32"), std::string("A"));
    PB_CHECK_EQ(to_utf8(s("A\0\0\0\xE4\0\0\0", 8), "utf-32le"), std::string("A\xC3\xA4"));
    PB_CHECK_EQ(to_utf8(s("\0\x11\0\0", 4), "utf-32be"), kFffd);                                 // above U+10FFFF
}

PB_TEST(no_label_means_utf8_when_valid_else_windows_1252) {
    bool ok = false;
    PB_CHECK_EQ(to_utf8("Gr\xC3\xBC\xC3\x9F" "e", "", &ok), std::string("Gr\xC3\xBC\xC3\x9F" "e"));
    PB_CHECK(ok);
    PB_CHECK_EQ(to_utf8("Gr\xFC\xDF" "e", "", &ok), std::string("Gr\xC3\xBC\xC3\x9F" "e"));
    PB_CHECK(ok);
}

PB_TEST(an_unknown_set_falls_back_to_windows_1252_and_says_so) {
    bool ok = true;
    const std::string out = to_utf8("Gr\xFC\xDF" "e \x93x\x94", "x-no-such-charset", &ok);
    PB_CHECK(!ok);
    PB_CHECK_EQ(out, std::string("Gr\xC3\xBC\xC3\x9F" "e \xE2\x80\x9Cx\xE2\x80\x9D"));
}

PB_TEST(other_sets_use_iconv_when_the_system_has_it) {
    bool ok = false;
    const std::string out = to_utf8("\xC1\xC2", "koi8-r", &ok);
    if (!ok) PB_SKIP("iconv has no KOI8-R here");
    PB_CHECK_EQ(out, std::string("\xD0\xB0\xD0\xB1"));   // а б
}

PB_TEST(every_set_returns_valid_utf8_for_any_bytes) {
    std::string noise;
    for (int i = 0; i < 4096; ++i) noise += static_cast<char>((i * 37 + (i >> 3)) & 0xFF);
    for (const char* label : {"utf-8", "us-ascii", "iso-8859-1", "iso-8859-2", "iso-8859-15", "windows-1250", "windows-1252", "utf-16", "utf-16le",
                              "utf-16be", "utf-32", "utf-32le", "utf-32be", "koi8-r", "shift_jis", "gb2312", "", "nonsense"}) {
        PB_CHECK(pbo::is_valid_utf8(to_utf8(noise, label)));
        PB_CHECK(pbo::is_valid_utf8(to_utf8("", label)));
    }
}

PB_TEST_MAIN()
