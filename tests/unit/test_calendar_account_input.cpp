// T091 (E2, Principle IV): validation of the calendar account form. A rejected value never reaches the store or a transport.
#include <algorithm>
#include <string>

#include "pbo/calendar_account_input.h"
#include "test_support.h"

namespace {

pbo::CalendarAccountInput server() {
    pbo::CalendarAccountInput in;
    in.caldav_url = "https://cloud.example.org/remote.php/dav";
    in.username = "alice";
    in.password = "pw-for-tests";
    return in;
}

pbo::CalendarAccountInput feed() {
    pbo::CalendarAccountInput in;
    in.feed_url = "https://calendars.example.org/holidays.ics";
    return in;
}

bool has_error(const std::vector<pbo::FieldError>& errors, const std::string& field, const std::string& key) {
    return std::any_of(errors.begin(), errors.end(), [&](const pbo::FieldError& e) { return e.field == field && e.key == key; });
}

}  // namespace

PB_TEST(a_server_account_and_a_feed_alone_are_valid_and_so_are_both) {
    PB_CHECK(pbo::validate_calendar_account(server()).empty());
    PB_CHECK(pbo::validate_calendar_account(feed()).empty());
    auto both = server();
    both.feed_url = feed().feed_url;
    PB_CHECK(pbo::validate_calendar_account(both).empty());
}

PB_TEST(at_least_one_of_server_and_feed_is_needed) {
    pbo::CalendarAccountInput none;
    PB_CHECK(has_error(pbo::validate_calendar_account(none), "caldav_url", "calendar_source_missing"));
    none.caldav_url = "   ";
    PB_CHECK(has_error(pbo::validate_calendar_account(none), "caldav_url", "calendar_source_missing"));
}

PB_TEST(the_server_address_must_be_https_with_a_host_and_nothing_else) {
    for (const char* bad : {"http://cloud.example.org/dav", "ftp://cloud.example.org/", "cloud.example.org/dav", "https://", "https:///dav",
                            "https://alice:pw@cloud.example.org/dav", "https://alice@cloud.example.org/dav",
                            "https://cloud.example.org/dav?x=1", "https://cloud.example.org/dav#frag", "https://cloud example.org/dav",
                            "https://cloud.example.org/da v", "https://cloud.example.org/da\nv", "https://cloud.example.org/\x01",
                            "https://cloud.example.org:0/dav", "https://cloud.example.org:70000/dav", "https://cloud.example.org:abc/dav",
                            "https://-bad.example.org/dav", "https://[::1]/dav", "javascript:alert(1)", "file:///etc/passwd"}) {
        auto in = server();
        in.caldav_url = bad;
        if (pbo::validate_calendar_account(in).empty()) PB_FAIL_AT(std::string("accepted: ") + bad);
    }
    for (const char* ok : {"https://cloud.example.org", "https://cloud.example.org/", "https://cloud.example.org:8443/remote.php/dav/",
                           "https://192.0.2.10/dav", "HTTPS://cloud.example.org/dav", "https://cloud.example.org/.well-known/caldav"}) {
        auto in = server();
        in.caldav_url = ok;
        if (!pbo::validate_calendar_account(in).empty()) PB_FAIL_AT(std::string("rejected: ") + ok);
    }
    auto too_long = server();
    too_long.caldav_url = "https://cloud.example.org/" + std::string(3000, 'a');
    PB_CHECK(!pbo::validate_calendar_account(too_long).empty());
}

PB_TEST(plain_http_is_refused_with_its_own_key_before_any_connection) {
    auto in = server();
    in.caldav_url = "http://cloud.example.org/dav";
    PB_CHECK(has_error(pbo::validate_calendar_account(in), "caldav_url", "url_not_https"));
    in = feed();
    in.feed_url = "http://calendars.example.org/x.ics";
    PB_CHECK(has_error(pbo::validate_calendar_account(in), "feed_url", "url_not_https"));
}

PB_TEST(credentials_in_the_address_get_their_own_key) {
    auto in = server();
    in.caldav_url = "https://alice:pw@cloud.example.org/dav";
    PB_CHECK(has_error(pbo::validate_calendar_account(in), "caldav_url", "url_has_credentials"));
    in = feed();
    in.feed_url = "https://user:pass@calendars.example.org/x.ics";
    PB_CHECK(has_error(pbo::validate_calendar_account(in), "feed_url", "url_has_credentials"));
}

PB_TEST(a_server_account_needs_a_user_name_and_a_password_without_control_characters) {
    auto in = server();
    in.username = "";
    PB_CHECK(has_error(pbo::validate_calendar_account(in), "username", "username_invalid"));
    in = server();
    in.username = "al\nice";
    PB_CHECK(has_error(pbo::validate_calendar_account(in), "username", "username_invalid"));
    in = server();
    in.password = "";
    PB_CHECK(has_error(pbo::validate_calendar_account(in), "password", "password_empty"));
    in = server();
    in.password = "pw\x01";
    PB_CHECK(has_error(pbo::validate_calendar_account(in), "password", "password_invalid"));
    // a feed alone needs neither
    PB_CHECK(pbo::validate_calendar_account(feed()).empty());
}

PB_TEST(a_feed_may_carry_a_query_but_webcal_becomes_https) {
    auto in = feed();
    in.feed_url = "https://calendars.example.org/feed.ics?token=abc";
    PB_CHECK(pbo::validate_calendar_account(in).empty());
    in.feed_url = "webcal://calendars.example.org/feed.ics";
    PB_CHECK(pbo::validate_calendar_account(in).empty());
    PB_CHECK_EQ(pbo::normalize_calendar_account(in).feed_url, std::string("https://calendars.example.org/feed.ics"));
    in.feed_url = "  WebCal://calendars.example.org/feed.ics  ";
    PB_CHECK_EQ(pbo::normalize_calendar_account(in).feed_url, std::string("https://calendars.example.org/feed.ics"));
    in.feed_url = "https://calendars.example.org/feed.ics#frag";
    PB_CHECK(has_error(pbo::validate_calendar_account(in), "feed_url", "url_invalid"));
}

PB_TEST(normalize_trims_blanks_and_lower_cases_the_scheme_only) {
    auto in = server();
    in.caldav_url = "  HTTPS://Cloud.Example.org/Remote.php/dav/  ";
    in.username = "  alice ";
    const auto n = pbo::normalize_calendar_account(in);
    PB_CHECK_EQ(n.caldav_url, std::string("https://Cloud.Example.org/Remote.php/dav/"));
    PB_CHECK_EQ(n.username, std::string("alice"));
    PB_CHECK_EQ(n.password, in.password);                     // the password is never touched
}

PB_TEST(an_empty_server_part_clears_server_fields_when_only_a_feed_is_given) {
    auto in = feed();
    in.caldav_url = "  ";
    in.username = "x";
    const auto n = pbo::normalize_calendar_account(in);
    PB_CHECK_EQ(n.caldav_url, std::string(""));
    PB_CHECK_EQ(n.username, std::string(""));
    PB_CHECK(pbo::validate_calendar_account(in).empty());      // the unused username is not validated
}

PB_TEST_MAIN()
