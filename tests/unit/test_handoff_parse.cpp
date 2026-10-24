// T003 (spec 003): the rules for a text handed to PB Office by another app (research R-03, R-07). The text, the subject and the recipient are untrusted:
// bounded, cleaned, never interpreted. German letters, the euro sign and emoji are test data for non-English input (constitution X).
#include <chrono>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "pbo/handoff.h"
#include "pbo/log.h"
#include "test_support.h"

namespace {

using Args = std::vector<std::string>;
const std::string kFffd = "\xef\xbf\xbd";   // U+FFFD, the visible replacement

std::string fixture(const std::string& name) {
    std::ifstream f(std::string(PBO_FIXTURE_DIR) + "/handoff/" + name, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// the arguments of a well-formed hand-over carrying `text` (and optionally subject / recipient)
Args with_text(const std::string& text, const std::string& subject = "", const std::string& to = "") {
    Args a = {"pbo-mail-v1", "text=" + text, "textlen=" + std::to_string(text.size())};
    if (!subject.empty()) a.push_back("subject=" + subject);
    if (!to.empty()) a.push_back("to=" + to);
    return a;
}

}  // namespace

// (a) the marker

PB_TEST(the_first_argument_equal_to_the_marker_starts_a_handoff) {
    const auto r = pbo::parse_handoff(with_text("hello"));
    PB_CHECK(r.is_handoff);
    PB_CHECK(r.refusal == pbo::HandoffRefusal::None);
    PB_CHECK_EQ(r.handoff.text, std::string("hello"));
    PB_CHECK_EQ(r.handoff.subject, std::string(""));
    PB_CHECK_EQ(r.handoff.to, std::string(""));
    PB_CHECK(r.handoff.notices.empty());
}

PB_TEST(arguments_before_the_marker_such_as_the_app_path_are_skipped) {
    Args a = {"/mnt/ext1/applications/pboffice.app"};
    for (const auto& s : with_text("hi")) a.push_back(s);
    const auto r = pbo::parse_handoff(a);
    PB_CHECK(r.is_handoff);
    PB_CHECK_EQ(r.handoff.text, std::string("hi"));
}

PB_TEST(without_the_marker_the_start_is_an_ordinary_start) {
    PB_CHECK(!pbo::parse_handoff({}).is_handoff);
    PB_CHECK(!pbo::parse_handoff({"text=hello", "textlen=5"}).is_handoff);
    PB_CHECK(!pbo::parse_handoff({"pbo-mail-v2", "text=hello", "textlen=5"}).is_handoff);   // another version is not understood
    PB_CHECK(!pbo::parse_handoff({"PBO-MAIL-V1"}).is_handoff);                              // exact match only
}

PB_TEST(a_second_marker_is_just_an_argument_without_an_equals_sign_and_is_ignored) {
    Args a = with_text("hi");
    a.push_back("pbo-mail-v1");
    const auto r = pbo::parse_handoff(a);
    PB_CHECK(r.is_handoff);
    PB_CHECK(r.refusal == pbo::HandoffRefusal::None);
    PB_CHECK_EQ(r.handoff.text, std::string("hi"));
}

// (b) the text

PB_TEST(texts_up_to_the_limit_are_accepted_whole) {
    for (size_t n : {size_t(1), size_t(100), size_t(1500), size_t(6000), size_t(8192)}) {
        const std::string text(n, 'x');
        const auto r = pbo::parse_handoff(with_text(text));
        PB_CHECK(r.is_handoff);
        PB_CHECK(r.refusal == pbo::HandoffRefusal::None);
        PB_CHECK_EQ(r.handoff.text.size(), n);
    }
    PB_CHECK_EQ(pbo::kHandoffMaxTextBytes, size_t(8192));
}

PB_TEST(a_text_over_the_limit_is_refused_whole_and_never_cut) {
    const std::string text(8193, 'x');
    const auto r = pbo::parse_handoff(with_text(text));
    PB_CHECK(r.is_handoff);
    PB_CHECK(r.refusal == pbo::HandoffRefusal::TooLong);
}

PB_TEST(fifteen_hundred_characters_of_four_byte_emoji_are_accepted_FR_012) {
    std::string text;
    for (int i = 0; i < 1500; ++i) text += "\xf0\x9f\x98\x80";   // 6000 bytes, 1500 characters
    const auto r = pbo::parse_handoff(with_text(text));
    PB_CHECK(r.refusal == pbo::HandoffRefusal::None);
    PB_CHECK_EQ(r.handoff.text, text);
}

PB_TEST(text_without_textlen_or_with_a_wrong_textlen_is_incomplete) {
    PB_CHECK(pbo::parse_handoff({"pbo-mail-v1", "text=hello"}).refusal == pbo::HandoffRefusal::Incomplete);
    PB_CHECK(pbo::parse_handoff({"pbo-mail-v1", "text=hello", "textlen=4"}).refusal == pbo::HandoffRefusal::Incomplete);   // longer than announced
    PB_CHECK(pbo::parse_handoff({"pbo-mail-v1", "text=hello", "textlen=6"}).refusal == pbo::HandoffRefusal::Incomplete);   // cut on the way
    for (const char* bad : {"textlen=abc", "textlen=-5", "textlen=+5", "textlen= 5", "textlen=5 ", "textlen=", "textlen=5x", "textlen=0x5"})
        PB_CHECK(pbo::parse_handoff({"pbo-mail-v1", "text=hello", bad}).refusal == pbo::HandoffRefusal::Incomplete);
    PB_CHECK(pbo::parse_handoff({"pbo-mail-v1", "text=", "textlen=1"}).refusal == pbo::HandoffRefusal::Incomplete);
    PB_CHECK(pbo::parse_handoff({"pbo-mail-v1", "text="}).refusal == pbo::HandoffRefusal::Incomplete);
    PB_CHECK(pbo::parse_handoff({"pbo-mail-v1", "text=hello", "textlen=99999999999999999999"}).refusal == pbo::HandoffRefusal::Incomplete);   // overflow
}

PB_TEST(the_length_counts_the_bytes_of_the_text_as_received_not_its_characters) {
    const std::string text = "Gr\xc3\xbc\xc3\x9f" "e";   // 7 bytes, 5 characters
    PB_CHECK(pbo::parse_handoff({"pbo-mail-v1", "text=" + text, "textlen=7"}).refusal == pbo::HandoffRefusal::None);
    PB_CHECK(pbo::parse_handoff({"pbo-mail-v1", "text=" + text, "textlen=5"}).refusal == pbo::HandoffRefusal::Incomplete);
}

PB_TEST(a_value_is_everything_after_the_first_equals_sign) {
    for (const std::string& text : {std::string("a=b=c"), std::string("-x --y"), std::string("text=inner"), std::string("=lead"), std::string("subject=not a key")}) {
        const auto r = pbo::parse_handoff(with_text(text));
        PB_CHECK(r.refusal == pbo::HandoffRefusal::None);
        PB_CHECK_EQ(r.handoff.text, text);
        PB_CHECK_EQ(r.handoff.subject, std::string(""));
    }
}

PB_TEST(an_empty_text_with_textlen_zero_is_an_empty_value) {
    const auto r = pbo::parse_handoff({"pbo-mail-v1", "text=", "textlen=0", "subject=Hello"});
    PB_CHECK(r.refusal == pbo::HandoffRefusal::None);
    PB_CHECK_EQ(r.handoff.text, std::string(""));
    PB_CHECK_EQ(r.handoff.subject, std::string("Hello"));
    PB_CHECK(!r.handoff.empty());
}

PB_TEST(a_link_with_a_password_on_the_next_line_arrives_unchanged) {
    const std::string text = fixture("link_with_password.txt");
    PB_CHECK(!text.empty());
    const auto r = pbo::parse_handoff(with_text(text));
    PB_CHECK_EQ(r.handoff.text, text);   // both lines, not split, not hidden, not reformatted
}

// (c) cleaning

PB_TEST(valid_text_in_any_script_and_emoji_is_unchanged_by_cleaning) {
    const std::string text = fixture("umlaut_euro_emoji.txt");   // umlauts, sharp s, euro sign, an emoji, quotes
    PB_CHECK(!text.empty());
    PB_CHECK_EQ(pbo::clean_handoff_text(text), text);
    const std::string rtl = "\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d \xd8\xb3\xd9\x84\xd8\xa7\xd9\x85";   // Hebrew and Arabic, right to left
    PB_CHECK_EQ(pbo::clean_handoff_text(rtl), rtl);
    const std::string zwj = "\xf0\x9f\x91\xa8\xe2\x80\x8d\xf0\x9f\x91\xa9";   // emoji joined by a zero width joiner
    PB_CHECK_EQ(pbo::clean_handoff_text(zwj), zwj);
    PB_CHECK_EQ(pbo::clean_handoff_text("a\nb\tc"), std::string("a\nb\tc"));   // line feed and tab are kept
}

PB_TEST(line_ends_become_a_line_feed) {
    PB_CHECK_EQ(pbo::clean_handoff_text("a\r\nb\rc\nd"), std::string("a\nb\nc\nd"));
    PB_CHECK_EQ(pbo::clean_handoff_text("\r\n\r\n"), std::string("\n\n"));
}

PB_TEST(control_characters_and_the_c1_range_become_the_visible_replacement) {
    PB_CHECK_EQ(pbo::clean_handoff_text("a\x01" "b"), "a" + kFffd + "b");
    PB_CHECK_EQ(pbo::clean_handoff_text("a\x1b[31mb"), "a" + kFffd + "[31mb");   // an escape sequence cannot reach the screen
    PB_CHECK_EQ(pbo::clean_handoff_text("a\x7f" "b"), "a" + kFffd + "b");        // DEL
    PB_CHECK_EQ(pbo::clean_handoff_text(std::string("a\0b", 3)), "a" + kFffd + "b");
    PB_CHECK_EQ(pbo::clean_handoff_text("a\xc2\x85" "b"), "a" + kFffd + "b");    // U+0085 (next line)
    PB_CHECK_EQ(pbo::clean_handoff_text("a\xc2\x9f" "b"), "a" + kFffd + "b");    // U+009F, the end of the C1 range
    PB_CHECK_EQ(pbo::clean_handoff_text("a\xc2\xa0" "b"), "a\xc2\xa0" "b");      // U+00A0 (no-break space) is text
}

PB_TEST(the_fixture_with_all_kinds_of_line_ends_and_controls_is_cleaned_as_a_whole) {
    const std::string raw = fixture("crlf_and_controls.txt");   // a CRLF b CR c TAB d 0x01 e 0x7f f U+0085 g
    PB_CHECK_EQ(pbo::clean_handoff_text(raw), "a\nb\nc\td" + kFffd + "e" + kFffd + "f" + kFffd + "g");
}

PB_TEST(invalid_utf8_becomes_one_replacement_per_invalid_byte_and_valid_text_around_it_stays) {
    PB_CHECK_EQ(pbo::clean_handoff_text("ok \xff mid"), "ok " + kFffd + " mid");
    PB_CHECK_EQ(pbo::clean_handoff_text("x\xe2\x82"), "x" + kFffd + kFffd);                       // truncated euro sign
    PB_CHECK_EQ(pbo::clean_handoff_text("\x80y"), kFffd + "y");                                    // a stray continuation byte
    PB_CHECK_EQ(pbo::clean_handoff_text("\xc0\xaf"), kFffd + kFffd);                               // overlong form of "/"
    PB_CHECK_EQ(pbo::clean_handoff_text("\xed\xa0\x80"), kFffd + kFffd + kFffd);                   // a UTF-16 surrogate
    PB_CHECK_EQ(pbo::clean_handoff_text("\xf4\x90\x80\x80"), kFffd + kFffd + kFffd + kFffd);       // above U+10FFFF
    PB_CHECK_EQ(pbo::clean_handoff_text(fixture("invalid_utf8.bin")), "ok " + kFffd + " mid " + kFffd + kFffd);
}

PB_TEST(cleaning_runs_after_the_limit_check_so_the_limit_is_on_the_bytes_received) {
    // 8192 invalid bytes are 8192 bytes received: accepted although cleaning makes them three times longer
    const std::string bad(8192, '\xff');
    const auto r = pbo::parse_handoff(with_text(bad));
    PB_CHECK(r.refusal == pbo::HandoffRefusal::None);
    PB_CHECK_EQ(r.handoff.text.size(), size_t(8192 * 3));
}

// (d) the subject

PB_TEST(the_subject_is_one_line_and_a_line_break_or_tab_becomes_one_space) {
    PB_CHECK_EQ(pbo::clean_handoff_subject("a\nb\tc\r\nd\re"), std::string("a b c d e"));
    PB_CHECK_EQ(pbo::clean_handoff_subject("x\x01y"), "x" + kFffd + "y");
    const auto r = pbo::parse_handoff(with_text("t", "Hello\nWorld"));
    PB_CHECK_EQ(r.handoff.subject, std::string("Hello World"));
}

PB_TEST(a_subject_of_300_characters_is_accepted_and_301_is_left_out_with_a_notice_while_the_text_arrives) {
    const std::string ok(300, 'a');
    const auto r1 = pbo::parse_handoff(with_text("text", ok));
    PB_CHECK_EQ(r1.handoff.subject, ok);
    PB_CHECK(r1.handoff.notices.empty());
    PB_CHECK_EQ(pbo::kHandoffMaxSubjectChars, size_t(300));

    const std::string too_long(301, 'a');
    const auto r2 = pbo::parse_handoff(with_text("text", too_long));
    PB_CHECK(r2.refusal == pbo::HandoffRefusal::None);
    PB_CHECK_EQ(r2.handoff.subject, std::string(""));
    PB_CHECK_EQ(r2.handoff.text, std::string("text"));
    PB_CHECK_EQ(r2.handoff.notices.size(), size_t(1));
    PB_CHECK(r2.handoff.notices[0] == pbo::HandoffNotice::SubjectTooLong);
}

PB_TEST(the_subject_limit_counts_characters_not_bytes) {
    std::string euro;
    for (int i = 0; i < 300; ++i) euro += "\xe2\x82\xac";   // 900 bytes, 300 characters
    PB_CHECK_EQ(pbo::parse_handoff(with_text("t", euro)).handoff.subject, euro);
    PB_CHECK(!pbo::parse_handoff(with_text("t", euro + "\xe2\x82\xac")).handoff.notices.empty());
}

PB_TEST(a_subject_with_a_second_header_is_one_line_without_a_header_FR_013) {
    const auto r = pbo::parse_handoff(with_text("t", fixture("injection_subject.txt")));
    PB_CHECK_EQ(r.handoff.subject, std::string("Hello Bcc: attacker@example.org"));
    PB_CHECK(r.handoff.subject.find('\n') == std::string::npos);
    PB_CHECK(r.handoff.subject.find('\r') == std::string::npos);
}

// (e) the recipient

PB_TEST(one_plain_address_is_accepted) {
    const auto r = pbo::parse_handoff(with_text("t", "", "bob@example.org"));
    PB_CHECK_EQ(r.handoff.to, std::string("bob@example.org"));
    PB_CHECK(r.handoff.notices.empty());
    PB_CHECK_EQ(pbo::kHandoffMaxAddressBytes, size_t(254));
}

PB_TEST(anything_that_is_not_one_plain_address_is_not_prefilled_and_the_user_is_told_but_text_and_subject_stay) {
    const std::vector<std::string> bad = {
        "a@example.org, b@example.org",   // a list
        "a@example.org;b@example.org",
        "Bob <bob@example.org>",          // a display name
        "<bob@example.org>",              // angle brackets
        "bob @example.org",               // a blank inside
        "\"bob\"@example.org",            // quotes
        "bob@example.org\x01",            // a control character
        "bob",                            // no @
        "bob@example",                    // no dot in the domain
        "bob@@example.org",
        std::string(246, 'a') + "@example.org",   // 258 bytes
    };
    for (const auto& to : bad) {
        const auto r = pbo::parse_handoff(with_text("the text", "the subject", to));
        PB_CHECK(r.refusal == pbo::HandoffRefusal::None);
        PB_CHECK_EQ(r.handoff.to, std::string(""));
        PB_CHECK_EQ(r.handoff.text, std::string("the text"));
        PB_CHECK_EQ(r.handoff.subject, std::string("the subject"));
        PB_CHECK_EQ(r.handoff.notices.size(), size_t(1));
        PB_CHECK(r.handoff.notices[0] == pbo::HandoffNotice::RecipientIgnored);
    }
}

PB_TEST(an_address_of_254_bytes_is_the_longest_that_is_accepted_and_255_is_not) {
    const std::string label(60, 'l');
    const std::string longest = std::string(64, 'a') + "@" + label + "." + label + "." + label + ".abc.de";   // 64 + 1 + 189 = 254 bytes
    PB_CHECK_EQ(longest.size(), size_t(254));
    const auto ok = pbo::parse_handoff(with_text("t", "", longest));
    PB_CHECK_EQ(ok.handoff.to, longest);
    PB_CHECK(ok.handoff.notices.empty());
    const std::string over = longest + "x";   // 255 bytes
    const auto no = pbo::parse_handoff(with_text("t", "", over));
    PB_CHECK(no.handoff.to.empty());
    PB_CHECK(!no.handoff.notices.empty());
}

PB_TEST(a_recipient_with_a_hidden_second_header_is_refused_FR_013) {
    const auto r = pbo::parse_handoff(with_text("t", "", fixture("injection_recipient.txt")));
    PB_CHECK_EQ(r.handoff.to, std::string(""));
    PB_CHECK(!r.handoff.notices.empty());
}

PB_TEST(blanks_around_a_plain_address_are_dropped_and_an_empty_recipient_has_no_notice) {
    const auto r = pbo::parse_handoff({"pbo-mail-v1", "text=t", "textlen=1", "to=  bob@example.org\t"});
    PB_CHECK_EQ(r.handoff.to, std::string("bob@example.org"));
    const auto e = pbo::parse_handoff({"pbo-mail-v1", "text=t", "textlen=1", "to="});
    PB_CHECK_EQ(e.handoff.to, std::string(""));
    PB_CHECK(e.handoff.notices.empty());
}

// (f) keys

PB_TEST(unknown_keys_and_arguments_without_an_equals_sign_are_ignored) {
    Args a = with_text("hi");
    a.push_back("color=blue");
    a.push_back("color=red");   // an unknown key given twice is still ignored
    a.push_back("loose");
    const auto r = pbo::parse_handoff(a);
    PB_CHECK(r.refusal == pbo::HandoffRefusal::None);
    PB_CHECK_EQ(r.handoff.text, std::string("hi"));
}

PB_TEST(a_known_key_given_twice_makes_the_whole_handoff_malformed) {
    PB_CHECK(pbo::parse_handoff({"pbo-mail-v1", "text=a", "textlen=1", "text=b"}).refusal == pbo::HandoffRefusal::Malformed);
    PB_CHECK(pbo::parse_handoff({"pbo-mail-v1", "text=a", "textlen=1", "textlen=1"}).refusal == pbo::HandoffRefusal::Malformed);
    PB_CHECK(pbo::parse_handoff({"pbo-mail-v1", "to=a@example.org", "to=b@example.org"}).refusal == pbo::HandoffRefusal::Malformed);
    PB_CHECK(pbo::parse_handoff({"pbo-mail-v1", "subject=a", "subject=b"}).refusal == pbo::HandoffRefusal::Malformed);
}

PB_TEST(textlen_without_a_text_is_ignored) {
    const auto r = pbo::parse_handoff({"pbo-mail-v1", "textlen=5", "subject=Only a subject"});
    PB_CHECK(r.refusal == pbo::HandoffRefusal::None);
    PB_CHECK_EQ(r.handoff.subject, std::string("Only a subject"));
}

// (g) empty

PB_TEST(a_handoff_without_content_is_empty_and_one_with_only_a_subject_or_a_recipient_is_not) {
    PB_CHECK(pbo::parse_handoff({"pbo-mail-v1"}).handoff.empty());
    PB_CHECK(pbo::parse_handoff({"pbo-mail-v1"}).is_handoff);
    PB_CHECK(pbo::parse_handoff(with_text("   \n\t  ")).handoff.empty());   // only white space
    PB_CHECK(pbo::parse_handoff({"pbo-mail-v1", "subject= \t", "to=  "}).handoff.empty());
    PB_CHECK(!pbo::parse_handoff({"pbo-mail-v1", "subject=Hi"}).handoff.empty());
    PB_CHECK(!pbo::parse_handoff({"pbo-mail-v1", "to=bob@example.org"}).handoff.empty());
    PB_CHECK(!pbo::parse_handoff(with_text("x")).handoff.empty());
}

// (h) hostile size

PB_TEST(an_argument_of_a_megabyte_and_ten_thousand_arguments_are_answered_at_once) {
    const auto start = std::chrono::steady_clock::now();
    const std::string big(1024 * 1024, 'x');
    PB_CHECK(pbo::parse_handoff({"pbo-mail-v1", "text=" + big, "textlen=" + std::to_string(big.size())}).refusal == pbo::HandoffRefusal::TooLong);
    PB_CHECK(pbo::parse_handoff({"pbo-mail-v1", "subject=" + big}).handoff.subject.empty());
    PB_CHECK(!pbo::parse_handoff({"pbo-mail-v1", "subject=" + big}).handoff.notices.empty());
    PB_CHECK(pbo::parse_handoff({"pbo-mail-v1", "to=" + big}).handoff.to.empty());
    Args many = {"pbo-mail-v1"};
    for (int i = 0; i < 10000; ++i) many.push_back("k" + std::to_string(i) + "=v");
    PB_CHECK(pbo::parse_handoff(many).refusal == pbo::HandoffRefusal::None);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    PB_CHECK(ms < 2000);
}

// (i) no side effects

PB_TEST(parsing_and_cleaning_never_write_to_a_log) {
    std::vector<std::string> lines;
    pbo::log::set_sink([&](const std::string& line) { lines.push_back(line); });
    pbo::parse_handoff(with_text("PBO-MARKER-7f3a", "PBO-MARKER-7f3a", "PBO-MARKER-7f3a@example.org"));
    pbo::parse_handoff({"pbo-mail-v1", "text=PBO-MARKER-7f3a"});
    pbo::clean_handoff_text("PBO-MARKER-7f3a\xff");
    pbo::clean_handoff_subject("PBO-MARKER-7f3a");
    pbo::log::reset_sink();
    PB_CHECK(lines.empty());
}

PB_TEST(the_marker_constant_is_the_one_of_the_contract) {
    PB_CHECK_EQ(std::string(pbo::kHandoffMarker), std::string("pbo-mail-v1"));
}

PB_TEST_MAIN()
