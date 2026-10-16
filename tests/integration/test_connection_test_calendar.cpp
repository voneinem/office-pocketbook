// US2 scenario 1: the connection test of the calendar tab. Fake servers only (UNVERIFIED against real servers).
#include <string>

#include "fake_caldav_server.h"
#include "pbo/connection_test.h"
#include "pbo/curl_transport.h"
#include "test_support.h"

using pbtest::FakeCalDavServer;

PBO_FAKE_SERVER_TEST();

namespace {

struct Env {
    pbtest::TmpDir dir;
    pbtest::TestPki pki{dir};
    pbtest::TestPki::Leaf leaf;
    std::unique_ptr<FakeCalDavServer> server;
    std::unique_ptr<pbo::CurlTransport> http;
    Env() {
        if (!pki.available()) PB_SKIP("openssl CLI not available");
        leaf = pki.issue("srv", "DNS:localhost,IP:127.0.0.1");
        PB_CHECK(leaf.ok);
        FakeCalDavServer::Config c;
        c.cert = leaf.cert;
        c.key = leaf.key;
        server = std::make_unique<FakeCalDavServer>(c);
        http = std::make_unique<pbo::CurlTransport>(pki.ca_cert());
        FakeCalDavServer::CalendarOptions work;
        work.name = "Work";
        server->add_calendar("work", work);
        FakeCalDavServer::CalendarOptions ro;
        ro.name = "Holidays";
        ro.writable = false;
        server->add_calendar("holidays", ro);
        server->set_feed("/feeds/h.ics", "BEGIN:VCALENDAR\r\nVERSION:2.0\r\nEND:VCALENDAR\r\n");
    }
    pbo::CalendarAccountInput input() const {
        pbo::CalendarAccountInput in;
        in.caldav_url = server->origin();
        in.username = "alice";
        in.password = "pw-for-tests";
        return in;
    }
};

}  // namespace

PB_TEST(the_server_is_found_and_its_calendars_are_listed) {
    Env e;
    auto r = pbo::test_calendar_connection(*e.http, e.input());
    PB_CHECK(r.ok());
    PB_CHECK(r.value.server_tested && r.value.server_ok && !r.value.feed_tested);
    PB_CHECK_EQ(r.value.calendars.size(), size_t(2));
    PB_CHECK_EQ(r.value.failed_side, std::string(""));
}

PB_TEST(a_wrong_password_is_auth_failed_on_the_server_side) {
    Env e;
    auto in = e.input();
    in.password = "wrong";
    auto r = pbo::test_calendar_connection(*e.http, in);
    PB_CHECK(!r.ok());
    PB_CHECK_EQ(r.error.key, std::string("auth_failed"));
    PB_CHECK_EQ(r.value.failed_side, std::string("caldav"));
}

PB_TEST(a_feed_alone_is_tested_without_a_login) {
    Env e;
    pbo::CalendarAccountInput in;
    in.feed_url = e.server->origin() + "/feeds/h.ics";
    auto r = pbo::test_calendar_connection(*e.http, in);
    PB_CHECK(r.ok());
    PB_CHECK(r.value.feed_tested && r.value.feed_ok && !r.value.server_tested);
    in.feed_url = e.server->origin() + "/feeds/missing.ics";
    r = pbo::test_calendar_connection(*e.http, in);
    PB_CHECK_EQ(r.error.key, std::string("feed_not_found"));
    PB_CHECK_EQ(r.value.failed_side, std::string("feed"));
}

PB_TEST(both_sides_are_tried_and_the_server_failure_comes_first) {
    Env e;
    auto in = e.input();
    in.password = "wrong";
    in.feed_url = e.server->origin() + "/feeds/missing.ics";
    auto r = pbo::test_calendar_connection(*e.http, in);
    PB_CHECK_EQ(r.value.failed_side, std::string("caldav"));
    PB_CHECK(r.value.feed_tested && !r.value.feed_ok);
    PB_CHECK_EQ(r.value.feed_error.key, std::string("feed_not_found"));
}

PB_TEST(an_invalid_form_or_no_network_means_no_request_at_all) {
    Env e;
    auto in = e.input();
    in.caldav_url = "http://insecure.example.org/dav";
    auto r = pbo::test_calendar_connection(*e.http, in);
    PB_CHECK_EQ(r.error.key, std::string("url_not_https"));
    PB_CHECK_EQ(r.value.failed_side, std::string("caldav_url"));
    r = pbo::test_calendar_connection(*e.http, e.input(), false);
    PB_CHECK_EQ(r.error.key, std::string("no_network"));
    PB_CHECK_EQ(e.server->log().size(), size_t(0));
}

PB_TEST_MAIN()
