// The header helpers behind the message list: unfolding, RFC 5322 dates, the first address, IMAP dates.
#include <string>

#include "pbo/mail_headers.h"
#include "test_support.h"

PB_TEST(header_value_finds_a_field_case_insensitively_and_unfolds) {
    const std::string block = "From: Anna <anna@example.org>\r\nsubject: A long\r\n  folded\tsubject\r\nX-Other: 1\r\n\r\n";
    PB_CHECK_EQ(pbo::header_value(block, "Subject"), std::string("A long folded subject"));
    PB_CHECK_EQ(pbo::header_value(block, "FROM"), std::string("Anna <anna@example.org>"));
    PB_CHECK_EQ(pbo::header_value(block, "Date"), std::string(""));
    PB_CHECK_EQ(pbo::header_value(block, "X-Oth"), std::string(""));   // whole field names only
}

PB_TEST(header_value_ignores_the_body_and_lf_only_line_ends_work) {
    const std::string block = "Subject: one\n\nSubject: two\n";
    PB_CHECK_EQ(pbo::header_value(block, "Subject"), std::string("one"));
}

PB_TEST(rfc5322_dates_become_epoch_seconds) {
    int64_t t = 0;
    PB_CHECK(pbo::parse_rfc5322_date("Mon, 05 Oct 2026 10:00:00 +0000", &t));
    PB_CHECK_EQ(t, static_cast<int64_t>(1791194400));
    PB_CHECK(pbo::parse_rfc5322_date("Tue, 6 Oct 2026 08:30:00 +0200", &t));
    PB_CHECK_EQ(t, static_cast<int64_t>(1791268200));   // 06:30 UTC
    PB_CHECK(pbo::parse_rfc5322_date("5 Oct 2026 10:00 GMT", &t));
    PB_CHECK_EQ(t, static_cast<int64_t>(1791194400));
    PB_CHECK(pbo::parse_rfc5322_date("Mon, 05 Oct 2026 10:00:00 -0330 (NST)", &t));
    PB_CHECK_EQ(t, static_cast<int64_t>(1791194400 + 12600));
    PB_CHECK(!pbo::parse_rfc5322_date("yesterday", &t));
    PB_CHECK(!pbo::parse_rfc5322_date("Mon, 32 Oct 2026 10:00:00 +0000", &t));
    PB_CHECK(!pbo::parse_rfc5322_date("Mon, 05 Foo 2026 10:00:00 +0000", &t));
    PB_CHECK(!pbo::parse_rfc5322_date("", &t));
}

PB_TEST(the_first_address_is_split_into_name_and_address) {
    std::string name, addr;
    pbo::split_first_address("Anna Beispiel <anna@example.org>", &name, &addr);
    PB_CHECK_EQ(name, std::string("Anna Beispiel"));
    PB_CHECK_EQ(addr, std::string("anna@example.org"));
    pbo::split_first_address("\"Beispiel, Anna\" <anna@example.org>, ben@example.org", &name, &addr);
    PB_CHECK_EQ(name, std::string("Beispiel, Anna"));
    PB_CHECK_EQ(addr, std::string("anna@example.org"));
    pbo::split_first_address("ben@example.org", &name, &addr);
    PB_CHECK_EQ(name, std::string(""));
    PB_CHECK_EQ(addr, std::string("ben@example.org"));
    pbo::split_first_address("", &name, &addr);
    PB_CHECK_EQ(addr, std::string(""));
}

PB_TEST(imap_dates_use_english_month_names) {
    PB_CHECK_EQ(pbo::imap_date(1791194400), std::string("5-Oct-2026"));
    PB_CHECK_EQ(pbo::imap_date(0), std::string("1-Jan-1970"));
}

PB_TEST_MAIN()
