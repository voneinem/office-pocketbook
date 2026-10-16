// T098: self-test of the fake CalDAV server harness. If this fails, calendar tests built on it prove nothing.
#include <string>

#include "fake_caldav_server.h"
#include "pbo/curl_transport.h"
#include "pbo/xml_min.h"
#include "test_support.h"

using pbo::CurlTransport;
using pbo::Request;
using pbtest::FakeCalDavServer;

PBO_FAKE_SERVER_TEST();

namespace {

struct Env {
    pbtest::TmpDir dir;
    pbtest::TestPki pki{dir};
    pbtest::TestPki::Leaf leaf;
    std::unique_ptr<FakeCalDavServer> server;
    std::unique_ptr<CurlTransport> http;
    Env() {
        if (!pki.available()) PB_SKIP("openssl CLI not available");
        leaf = pki.issue("srv", "DNS:localhost,IP:127.0.0.1");
        PB_CHECK(leaf.ok);
        FakeCalDavServer::Config c;
        c.cert = leaf.cert;
        c.key = leaf.key;
        server = std::make_unique<FakeCalDavServer>(c);
        http = std::make_unique<CurlTransport>(pki.ca_cert());
        FakeCalDavServer::CalendarOptions work;
        work.name = "Work";
        server->add_calendar("work", work);
        FakeCalDavServer::CalendarOptions ro;
        ro.name = "Holidays";
        ro.writable = false;
        server->add_calendar("holidays", ro);
    }
    pbo::Response call(const std::string& method, const std::string& url, const std::string& body = "", pbo::Headers h = {}, bool login = true) {
        Request r;
        r.method = method;
        r.url = url;
        r.body = body;
        r.headers = std::move(h);
        if (login) {
            r.username = "alice";
            r.password = "pw-for-tests";
        }
        return http->perform(r);
    }
};

const char* kEvent = "BEGIN:VCALENDAR\r\nBEGIN:VEVENT\r\nUID:a@example.org\r\nDTSTART:20261014T093000Z\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n";

}  // namespace

PB_TEST(a_wrong_login_is_refused_and_counted) {
    Env e;
    PB_CHECK_EQ(e.call("PROPFIND", e.server->base_url(), "<x/>", {{"Depth", "0"}}, false).status, 401L);
    PB_CHECK_EQ(e.server->rejected_logins(), 1);
}

PB_TEST(discovery_walks_from_the_well_known_address_to_the_calendars) {
    Env e;
    auto r = e.call("PROPFIND", e.server->origin() + "/.well-known/caldav", "<d:propfind xmlns:d=\"DAV:\"><d:prop><d:current-user-principal/></d:prop></d:propfind>", {{"Depth", "0"}});
    PB_CHECK_EQ(r.status, 207L);                                    // the redirect was followed
    auto x = pbo::parse_xml(r.body);
    PB_CHECK(x.ok());
    PB_CHECK(r.body.find("/dav/principals/alice/") != std::string::npos);
    r = e.call("PROPFIND", e.server->origin() + "/dav/principals/alice/", "<c:calendar-home-set xmlns:c=\"urn:ietf:params:xml:ns:caldav\"/>", {{"Depth", "0"}});
    PB_CHECK(r.body.find("/dav/calendars/alice/") != std::string::npos);
    r = e.call("PROPFIND", e.server->origin() + "/dav/calendars/alice/", "<d:propfind xmlns:d=\"DAV:\"><d:allprop/></d:propfind>", {{"Depth", "1"}});
    PB_CHECK_EQ(r.status, 207L);
    x = pbo::parse_xml(r.body);
    PB_CHECK(x.ok());
    PB_CHECK_EQ(x.value.children_named("DAV:", "response").size(), size_t(3));   // the home and two calendars
    PB_CHECK(r.body.find("<d:write-content/>") != std::string::npos);
    PB_CHECK(r.body.find("<cs:getctag>") != std::string::npos);
    PB_CHECK(r.body.find("<d:sync-collection/>") != std::string::npos);
}

PB_TEST(sync_collection_reports_changes_removals_and_an_invalid_token) {
    Env e;
    e.server->put_event("work", "a.ics", kEvent);
    e.server->put_event("work", "b.ics", kEvent);
    const std::string cal = e.server->origin() + e.server->calendar_href("work");
    auto r = e.call("REPORT", cal, "<d:sync-collection xmlns:d=\"DAV:\"><d:sync-token/><d:sync-level>1</d:sync-level></d:sync-collection>", {{"Depth", "1"}});
    PB_CHECK_EQ(r.status, 207L);
    auto x = pbo::parse_xml(r.body);
    PB_CHECK(x.ok());
    PB_CHECK_EQ(x.value.children_named("DAV:", "response").size(), size_t(2));
    const std::string token = x.value.child("DAV:", "sync-token")->text;
    PB_CHECK(!token.empty());

    e.server->remove_event("work", "a.ics");
    e.server->put_event("work", "c.ics", kEvent);
    r = e.call("REPORT", cal, "<d:sync-collection xmlns:d=\"DAV:\"><d:sync-token>" + token + "</d:sync-token></d:sync-collection>", {{"Depth", "1"}});
    x = pbo::parse_xml(r.body);
    PB_CHECK(x.ok());
    PB_CHECK_EQ(x.value.children_named("DAV:", "response").size(), size_t(2));   // c changed, a removed
    PB_CHECK(r.body.find("404 Not Found") != std::string::npos);

    e.server->reset_sync_tokens("work");
    r = e.call("REPORT", cal, "<d:sync-collection xmlns:d=\"DAV:\"><d:sync-token>" + token + "</d:sync-token></d:sync-collection>", {{"Depth", "1"}});
    PB_CHECK_EQ(r.status, 409L);
    PB_CHECK(r.body.find("valid-sync-token") != std::string::npos);

    FakeCalDavServer::CalendarOptions none;
    none.name = "Old";
    none.sync_collection = false;
    e.server->add_calendar("old", none);
    r = e.call("REPORT", e.server->origin() + e.server->calendar_href("old"), "<d:sync-collection xmlns:d=\"DAV:\"/>", {{"Depth", "1"}});
    PB_CHECK_EQ(r.status, 403L);
}

PB_TEST(calendar_query_filters_by_time_range_and_multiget_returns_the_calendar_data) {
    Env e;
    e.server->put_event("work", "old.ics", kEvent, 1000000000, 1000003600);
    e.server->put_event("work", "now.ics", kEvent, 1791979200, 1791982800);
    const std::string cal = e.server->origin() + e.server->calendar_href("work");
    auto r = e.call("REPORT", cal, "<c:calendar-query xmlns:c=\"urn:ietf:params:xml:ns:caldav\"><c:filter><c:time-range start=\"20261001T000000Z\" end=\"20261101T000000Z\"/></c:filter></c:calendar-query>", {{"Depth", "1"}});
    PB_CHECK(r.body.find("now.ics") != std::string::npos);
    PB_CHECK(r.body.find("old.ics") == std::string::npos);
    r = e.call("REPORT", cal, "<c:calendar-multiget xmlns:c=\"urn:ietf:params:xml:ns:caldav\" xmlns:d=\"DAV:\"><d:href>" + e.server->event_href("work", "now.ics") + "</d:href><d:href>" +
                                    e.server->event_href("work", "gone.ics") + "</d:href></c:calendar-multiget>", {{"Depth", "1"}});
    auto x = pbo::parse_xml(r.body);
    PB_CHECK(x.ok());
    PB_CHECK(r.body.find("BEGIN:VCALENDAR") != std::string::npos);
    PB_CHECK(r.body.find("404 Not Found") != std::string::npos);
    PB_CHECK_EQ(e.server->multiget_batch_sizes().size(), size_t(1));
    PB_CHECK_EQ(e.server->multiget_batch_sizes()[0], size_t(2));
}

PB_TEST(put_and_delete_are_conditional_and_privileges_are_enforced) {
    Env e;
    const std::string url = e.server->origin() + e.server->event_href("work", "n.ics");
    auto r = e.call("PUT", url, kEvent, {{"If-None-Match", "*"}, {"Content-Type", "text/calendar"}});
    PB_CHECK_EQ(r.status, 201L);
    const std::string etag = pbo::header_value(r.headers, "ETag");
    PB_CHECK(!etag.empty());
    PB_CHECK_EQ(e.call("PUT", url, kEvent, {{"If-None-Match", "*"}}).status, 412L);       // already there
    PB_CHECK_EQ(e.call("PUT", url, "X", {{"If-Match", "\"wrong\""}}).status, 412L);
    r = e.call("PUT", url, kEvent, {{"If-Match", etag}});
    PB_CHECK_EQ(r.status, 204L);
    PB_CHECK(pbo::header_value(r.headers, "ETag") != etag);
    PB_CHECK_EQ(e.call("PUT", e.server->origin() + e.server->event_href("work", "none.ics"), kEvent, {{"If-Match", "\"x\""}}).status, 404L);
    PB_CHECK_EQ(e.call("DELETE", url, "", {{"If-Match", "\"stale\""}}).status, 412L);
    PB_CHECK_EQ(e.call("DELETE", url, "", {{"If-Match", pbo::header_value(e.call("GET", url).headers, "ETag")}}).status, 204L);
    PB_CHECK(!e.server->has_event("work", "n.ics"));
    PB_CHECK_EQ(e.call("PUT", e.server->origin() + e.server->event_href("holidays", "h.ics"), kEvent, {{"If-None-Match", "*"}}).status, 403L);   // read-only
}

PB_TEST(a_server_side_edit_between_read_and_write_is_possible) {
    Env e;
    const std::string etag = e.server->put_event("work", "a.ics", kEvent);
    e.server->before_write([](FakeCalDavServer& s) { s.put_event("work", "a.ics", "BEGIN:VCALENDAR\r\nEND:VCALENDAR\r\n"); });
    const auto r = e.call("PUT", e.server->origin() + e.server->event_href("work", "a.ics"), kEvent, {{"If-Match", etag}});
    PB_CHECK_EQ(r.status, 412L);
}

PB_TEST(feeds_answer_conditional_requests) {
    Env e;
    e.server->set_feed("/feeds/holidays.ics", kEvent);
    auto r = e.call("GET", e.server->origin() + "/feeds/holidays.ics");
    PB_CHECK_EQ(r.status, 200L);
    const std::string etag = pbo::header_value(r.headers, "ETag");
    PB_CHECK_EQ(e.call("GET", e.server->origin() + "/feeds/holidays.ics", "", {{"If-None-Match", etag}}).status, 304L);
    PB_CHECK_EQ(e.call("GET", e.server->origin() + "/feeds/holidays.ics", "", {{"If-Modified-Since", pbo::header_value(r.headers, "Last-Modified")}}).status, 304L);
    e.server->set_feed("/feeds/holidays.ics", "BEGIN:VCALENDAR\r\nEND:VCALENDAR\r\n");
    PB_CHECK_EQ(e.call("GET", e.server->origin() + "/feeds/holidays.ics", "", {{"If-None-Match", etag}}).status, 200L);
}

PB_TEST(faults_fire_the_given_number_of_times_and_are_then_spent) {
    Env e;
    FakeCalDavServer::Fault f;
    f.kind = FakeCalDavServer::Fault::Kind::Status;
    f.status = 503;
    f.path_contains = "/dav/";
    f.times = 2;
    e.server->inject(f);
    PB_CHECK_EQ(e.call("PROPFIND", e.server->base_url(), "<x/>", {{"Depth", "0"}}).status, 503L);
    PB_CHECK_EQ(e.call("PROPFIND", e.server->base_url(), "<x/>", {{"Depth", "0"}}).status, 503L);
    PB_CHECK_EQ(e.call("PROPFIND", e.server->base_url(), "<x/>", {{"Depth", "0"}}).status, 207L);

    FakeCalDavServer::Fault d;
    d.kind = FakeCalDavServer::Fault::Kind::Drop;
    e.server->inject(d);
    const auto dropped = e.call("PROPFIND", e.server->base_url(), "<x/>", {{"Depth", "0"}});
    PB_CHECK(dropped.status == 0);

    FakeCalDavServer::Fault m;
    m.kind = FakeCalDavServer::Fault::Kind::MalformedXml;
    e.server->inject(m);
    const auto bad = e.call("PROPFIND", e.server->base_url(), "<x/>", {{"Depth", "0"}});
    PB_CHECK_EQ(bad.status, 207L);
    PB_CHECK(!pbo::parse_xml(bad.body).ok());
}

PB_TEST(the_log_records_methods_paths_and_headers) {
    Env e;
    e.call("PROPFIND", e.server->base_url(), "<x/>", {{"Depth", "0"}});
    const auto log = e.server->log();
    PB_CHECK_EQ(log.size(), size_t(1));
    PB_CHECK_EQ(log[0].method, std::string("PROPFIND"));
    PB_CHECK_EQ(log[0].path, std::string("/dav/"));
    PB_CHECK_EQ(log[0].headers.at("depth"), std::string("0"));
    PB_CHECK_EQ(log[0].body, std::string("<x/>"));
    PB_CHECK(!e.server->saw_credentials_in_url());
}

PB_TEST_MAIN()
