// T056 (header part): RFC 2047 encoded words, RFC 2231 parameters, address headers and typed recipient lists.
#include <string>

#include "pbo/charset.h"
#include "pbo/mime.h"
#include "test_support.h"

using pbo::decode_header_words;

PB_TEST(encoded_words_in_b_and_q_form) {
    PB_CHECK_EQ(decode_header_words("=?UTF-8?B?w6TDtsO8?="), std::string("\xC3\xA4\xC3\xB6\xC3\xBC"));
    PB_CHECK_EQ(decode_header_words("=?ISO-8859-1?Q?Gr=FC=DFe_aus_K=F6ln?="), std::string("Gr\xC3\xBC\xC3\x9F" "e aus K\xC3\xB6ln"));
    PB_CHECK_EQ(decode_header_words("=?windows-1252?Q?=93quoted=94?="), std::string("\xE2\x80\x9Cquoted\xE2\x80\x9D"));
    PB_CHECK_EQ(decode_header_words("=?utf-8?q?lower_case?="), std::string("lower case"));
    PB_CHECK_EQ(decode_header_words("=?UTF-8*de?Q?Gr=C3=BC=C3=9F" "e?="), std::string("Gr\xC3\xBC\xC3\x9F" "e"));   // language tag
    PB_CHECK_EQ(decode_header_words("=?UTF-16?B?/v8AQQDk?="), std::string("A\xC3\xA4"));
}

PB_TEST(plain_text_around_and_between_encoded_words) {
    PB_CHECK_EQ(decode_header_words("Re: =?UTF-8?Q?Gr=C3=BC=C3=9F" "e?= an alle"), std::string("Re: Gr\xC3\xBC\xC3\x9F" "e an alle"));
    PB_CHECK_EQ(decode_header_words("=?UTF-8?Q?a?= =?UTF-8?Q?b?="), std::string("ab"));                 // blanks between two words vanish
    PB_CHECK_EQ(decode_header_words("=?UTF-8?Q?a?=\r\n =?UTF-8?Q?b?="), std::string("ab"));
    PB_CHECK_EQ(decode_header_words("=?UTF-8?Q?a?= text =?UTF-8?Q?b?="), std::string("a text b"));
    PB_CHECK_EQ(decode_header_words("no encoding at all"), std::string("no encoding at all"));
}

PB_TEST(a_character_split_between_two_words_is_decoded_together) {
    PB_CHECK_EQ(decode_header_words("=?UTF-8?Q?=C3?= =?UTF-8?Q?=A4?="), std::string("\xC3\xA4"));
    PB_CHECK_EQ(decode_header_words("=?UTF-8?B?w6TDtg==?= =?UTF-8?B?w7w=?="), std::string("\xC3\xA4\xC3\xB6\xC3\xBC"));
}

PB_TEST(invalid_words_stay_as_written_and_nothing_crashes) {
    PB_CHECK_EQ(decode_header_words("=?UTF-8?X?abc?="), std::string("=?UTF-8?X?abc?="));            // unknown encoding
    PB_CHECK_EQ(decode_header_words("=?UTF-8?B?abc"), std::string("=?UTF-8?B?abc"));                 // unterminated
    PB_CHECK_EQ(decode_header_words("=?UTF-8?B?!!!!?="), std::string("=?UTF-8?B?!!!!?="));            // not base64
    PB_CHECK_EQ(decode_header_words("=??Q??="), std::string("=??Q??="));
    PB_CHECK_EQ(decode_header_words(""), std::string(""));
    const std::string bomb(1 << 20, '=');
    PB_CHECK(pbo::is_valid_utf8(decode_header_words(bomb)));
    std::string many;
    for (int i = 0; i < 20000; ++i) many += "=?UTF-8?Q?x?= ";
    PB_CHECK_EQ(decode_header_words(many).size(), static_cast<size_t>(20001));   // 20000 letters and the last blank
}

PB_TEST(an_unknown_charset_in_a_word_is_read_as_windows_1252) {
    PB_CHECK_EQ(decode_header_words("=?x-unknown?Q?Gr=FC=DF" "e?="), std::string("Gr\xC3\xBC\xC3\x9F" "e"));
}

PB_TEST(unencoded_8bit_text_is_kept_when_valid_and_repaired_when_not) {
    PB_CHECK_EQ(decode_header_words("Gr\xC3\xBC\xC3\x9F" "e"), std::string("Gr\xC3\xBC\xC3\x9F" "e"));
    PB_CHECK_EQ(decode_header_words("Gr\xFC\xDF" "e"), std::string("Gr\xC3\xBC\xC3\x9F" "e"));      // old Latin-1 mail without any label
    PB_CHECK_EQ(decode_header_words("a\tb\x01" "c"), std::string("a b c"));                           // control characters become blanks
}

PB_TEST(header_params_simple_quoted_and_case) {
    auto p = pbo::parse_header_params("Text/Plain; Charset=\"utf-8\"; format=flowed");
    PB_CHECK_EQ(p.value, std::string("text/plain"));
    PB_CHECK_EQ(p.params["charset"], std::string("utf-8"));
    PB_CHECK_EQ(p.params["format"], std::string("flowed"));
    p = pbo::parse_header_params("attachment; filename=\"a \\\"quoted\\\" name.txt\"");
    PB_CHECK_EQ(p.value, std::string("attachment"));
    PB_CHECK_EQ(p.params["filename"], std::string("a \"quoted\" name.txt"));
    p = pbo::parse_header_params("multipart/mixed;\r\n boundary=\"----=_Part_1\"");
    PB_CHECK_EQ(p.params["boundary"], std::string("----=_Part_1"));
    p = pbo::parse_header_params("text/plain; charset=utf-8; ; broken; =x; name=");
    PB_CHECK_EQ(p.params["charset"], std::string("utf-8"));
    PB_CHECK(pbo::parse_header_params("").params.empty());
}

PB_TEST(header_params_rfc2231_extended_and_continued) {
    auto p = pbo::parse_header_params("attachment; filename*=UTF-8''gr%C3%BC%C3%9Fe.pdf");
    PB_CHECK_EQ(p.params["filename"], std::string("gr\xC3\xBC\xC3\x9F" "e.pdf"));
    p = pbo::parse_header_params("attachment; filename*=ISO-8859-1''Gr%FC%DFe.txt");
    PB_CHECK_EQ(p.params["filename"], std::string("Gr\xC3\xBC\xC3\x9F" "e.txt"));
    p = pbo::parse_header_params("attachment; filename*0*=UTF-8''gr%C3%BC; filename*1*=%C3%9Fe; filename*2=\".pdf\"");
    PB_CHECK_EQ(p.params["filename"], std::string("gr\xC3\xBC\xC3\x9F" "e.pdf"));
    p = pbo::parse_header_params("attachment; filename*1=\"part two\"; filename*0=\"part one \"");   // out of order
    PB_CHECK_EQ(p.params["filename"], std::string("part one part two"));
    p = pbo::parse_header_params("attachment; filename=\"=?UTF-8?B?w6TDtg==?=.txt\"");               // not allowed by RFC, common in practice
    PB_CHECK_EQ(p.params["filename"], std::string("\xC3\xA4\xC3\xB6.txt"));
    p = pbo::parse_header_params("attachment; filename*0=a; filename*5=late");                         // a gap: what is there is kept
    PB_CHECK(p.params["filename"].find('a') == 0);
}

PB_TEST(header_params_are_bounded) {
    std::string many = "x/y";
    for (int i = 0; i < 1000; ++i) many += "; p" + std::to_string(i) + "=v";
    PB_CHECK(pbo::parse_header_params(many).params.size() <= 64);
    std::string cont = "x/y";
    for (int i = 0; i < 5000; ++i) cont += "; f*" + std::to_string(i) + "=aaaa";
    (void)pbo::parse_header_params(cont);
}

PB_TEST(mailboxes_with_names_quotes_comments_and_encoded_words) {
    auto v = pbo::parse_mailboxes("Anna Beispiel <anna@example.org>, \"Muster, Ben\" <ben@example.org>, carla@example.org");
    PB_CHECK_EQ(v.size(), static_cast<size_t>(3));
    PB_CHECK_EQ(v[0].name, std::string("Anna Beispiel"));
    PB_CHECK_EQ(v[0].address, std::string("anna@example.org"));
    PB_CHECK_EQ(v[1].name, std::string("Muster, Ben"));
    PB_CHECK_EQ(v[2].name, std::string(""));
    PB_CHECK_EQ(v[2].address, std::string("carla@example.org"));
    v = pbo::parse_mailboxes("=?UTF-8?Q?J=C3=BCrgen_M=C3=BCller?= <j@example.org>");
    PB_CHECK_EQ(v.at(0).name, std::string("J\xC3\xBCrgen M\xC3\xBCller"));
    v = pbo::parse_mailboxes("\"=?UTF-8?Q?J=C3=BCrgen?=\" <j@example.org>");
    PB_CHECK_EQ(v.at(0).name, std::string("J\xC3\xBCrgen"));
    v = pbo::parse_mailboxes("dieter@example.org (Dieter Test)");
    PB_CHECK_EQ(v.at(0).address, std::string("dieter@example.org"));
    PB_CHECK_EQ(v.at(0).name, std::string("Dieter Test"));
    v = pbo::parse_mailboxes("<erika@example.org>");
    PB_CHECK_EQ(v.at(0).address, std::string("erika@example.org"));
}

PB_TEST(mailbox_groups_and_garbage) {
    auto v = pbo::parse_mailboxes("Team: a@example.org, B <b@example.org>;, c@example.org");
    PB_CHECK_EQ(v.size(), static_cast<size_t>(3));
    PB_CHECK_EQ(v[1].address, std::string("b@example.org"));
    v = pbo::parse_mailboxes("undisclosed-recipients:;");
    PB_CHECK(v.empty());
    v = pbo::parse_mailboxes("garbage, a@example.org, @@, \"unterminated <b@example.org>");
    PB_CHECK(v.size() >= 1);
    PB_CHECK_EQ(v[0].address, std::string("a@example.org"));
    PB_CHECK(pbo::parse_mailboxes("").empty());
    std::string many;
    for (int i = 0; i < 1000; ++i) many += "u" + std::to_string(i) + "@example.org, ";
    PB_CHECK(pbo::parse_mailboxes(many).size() <= 200);
}

PB_TEST(typed_recipient_lists_are_validated_strictly) {
    auto v = pbo::parse_address_list("a@example.org, Ben <b@example.org>; A@EXAMPLE.ORG");
    PB_CHECK_EQ(v.size(), static_cast<size_t>(2));                            // the duplicate (ignoring case) is dropped
    PB_CHECK_EQ(v[0], std::string("a@example.org"));
    PB_CHECK_EQ(v[1], std::string("b@example.org"));
    PB_CHECK_EQ(pbo::parse_address_list("  single@example.org  ").size(), static_cast<size_t>(1));
    for (const char* bad : {"a@@example.org", "no-at-sign", "<a@example.org", "a@example", "a@example.org, oops", "a b@example.org",
                            "a@example.org\r\nBcc: x@example.org", "a@example.org,, b@example.org", ""})
        if (!pbo::parse_address_list(bad).empty()) PB_FAIL_AT(std::string("accepted: ") + bad);
    std::string many;
    for (int i = 0; i < 101; ++i) many += "u" + std::to_string(i) + "@example.org,";
    PB_CHECK(pbo::parse_address_list(many).empty());
}

PB_TEST_MAIN()
