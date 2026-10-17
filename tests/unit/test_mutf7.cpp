// T060 (part): modified UTF-7 for mailbox names (RFC 3501 5.1.3).
#include <string>

#include "pbo/imap_parse.h"
#include "test_support.h"

PB_TEST(plain_ascii_and_the_ampersand_round_trip) {
    PB_CHECK_EQ(pbo::decode_mutf7("INBOX"), std::string("INBOX"));
    PB_CHECK_EQ(pbo::decode_mutf7("A&-B"), std::string("A&B"));
    PB_CHECK_EQ(pbo::encode_mutf7("A&B"), std::string("A&-B"));
    PB_CHECK_EQ(pbo::encode_mutf7(""), std::string(""));
    PB_CHECK_EQ(pbo::decode_mutf7(""), std::string(""));
}

PB_TEST(umlauts_and_other_bmp_characters_round_trip) {
    for (const std::string& utf8 : {std::string("Entw\xC3\xBCrfe"), std::string("Gel\xC3\xB6schte Elemente"), std::string("\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E"),
                                    std::string("a\xC3\xA4" "b\xC3\xB6" "c&d")}) {
        const std::string enc = pbo::encode_mutf7(utf8);
        for (char c : enc) PB_CHECK(static_cast<unsigned char>(c) >= 0x20 && static_cast<unsigned char>(c) < 0x7f);   // only printable ASCII
        PB_CHECK_EQ(pbo::decode_mutf7(enc), utf8);
    }
    PB_CHECK_EQ(pbo::decode_mutf7("Entw&APw-rfe"), std::string("Entw\xC3\xBCrfe"));
    PB_CHECK_EQ(pbo::decode_mutf7("&AOQ-&APY-"), std::string("\xC3\xA4\xC3\xB6"));   // adjacent shifted runs
}

PB_TEST(characters_outside_the_bmp_use_surrogate_pairs) {
    const std::string emoji = "\xF0\x9F\x93\xA7 Mail";   // U+1F4E7
    const std::string enc = pbo::encode_mutf7(emoji);
    PB_CHECK_EQ(pbo::decode_mutf7(enc), emoji);
    PB_CHECK_EQ(pbo::decode_mutf7("&2D3c5w-"), std::string("\xF0\x9F\x93\xA7"));   // D83D DCE7 = U+1F4E7
}

PB_TEST(invalid_input_never_crashes_and_is_kept_as_it_is) {
    for (const char* bad : {"&", "&A", "&!!!-", "&AOQ", "&-&-&", "a&b", "&AOQ-&", "\xFF\xFE", "&2D0-", "&3NA-", "&ADx-"}) {
        const std::string out = pbo::decode_mutf7(bad);   // result unspecified, must return
        (void)out;
    }
    PB_CHECK_EQ(pbo::decode_mutf7("&"), std::string("&"));
    PB_CHECK_EQ(pbo::decode_mutf7("a&b"), std::string("a&b"));
    std::string long_input = "&";
    for (int i = 0; i < 100000; ++i) long_input += "AOQ";
    (void)pbo::decode_mutf7(long_input);
}

PB_TEST_MAIN()
