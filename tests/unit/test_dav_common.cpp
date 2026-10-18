// T005 (spec 002, research R-03): the generic WebDAV plumbing shared by the CalDAV and the CardDAV client. The expected values are those of the
// behaviour of core/src/caldav.cpp before the extraction: the move changes nothing. Scripted transport only (no server involved).
#include <string>

#include "fake_transport.h"
#include "pbo/dav_common.h"
#include "test_support.h"

using pbo::DavSession;
using pbtest::FakeTransport;

PB_TEST(url_for_keeps_an_address_on_the_same_origin_and_makes_a_path_absolute) {
    FakeTransport http;
    DavSession s(http, "https://dav.example.org/dav/", "alice", "pw");
    PB_CHECK_EQ(s.origin(), std::string("https://dav.example.org"));
    PB_CHECK_EQ(s.url_for("/dav/calendars/alice/"), std::string("https://dav.example.org/dav/calendars/alice/"));
    PB_CHECK_EQ(s.url_for("https://dav.example.org/dav/x"), std::string("https://dav.example.org/dav/x"));
    PB_CHECK_EQ(s.url_for("https://dav.example.org"), std::string("https://dav.example.org"));
    PB_CHECK_EQ(s.url_for("relative/path"), std::string("https://dav.example.org/relative/path"));
}

PB_TEST(url_for_refuses_another_host_and_a_scheme_relative_address) {
    FakeTransport http;
    DavSession s(http, "https://dav.example.org", "alice", "pw");
    PB_CHECK_EQ(s.url_for("//other.example/dav/"), std::string());
    PB_CHECK_EQ(s.url_for("https://other.example/dav/"), std::string());
    PB_CHECK_EQ(s.url_for("https://dav.example.org.evil.example/dav/"), std::string());
    PB_CHECK_EQ(s.url_for("http://elsewhere.example/"), std::string());
    PB_CHECK_EQ(s.url_for(""), std::string());
}

PB_TEST(url_for_of_a_session_without_a_usable_base_address_is_empty) {
    FakeTransport http;
    DavSession s(http, "not an address", "alice", "pw");
    PB_CHECK_EQ(s.url_for("/dav/"), std::string());
}

PB_TEST(send_hands_the_credentials_to_the_transport_never_in_the_url_and_caps_the_answer) {
    FakeTransport http;
    http.set_handler([](const pbo::Request&) { return FakeTransport::status(207, "<x/>", {{"Date", "Wed, 14 Oct 2026 12:00:00 GMT"}}); });
    DavSession s(http, "https://dav.example.org/dav/", "alice", "secret-pw");
    const pbo::Response r = s.send("PROPFIND", "https://dav.example.org/dav/", "<body/>", {{"Depth", "0"}}, 1234);
    PB_CHECK_EQ(r.status, 207L);
    PB_CHECK_EQ(http.requests().size(), static_cast<size_t>(1));
    const pbo::Request& q = http.requests()[0];
    PB_CHECK_EQ(q.method, std::string("PROPFIND"));
    PB_CHECK_EQ(q.username, std::string("alice"));
    PB_CHECK_EQ(q.password, std::string("secret-pw"));
    PB_CHECK_EQ(q.max_response_bytes, 1234LL);
    PB_CHECK_EQ(q.body, std::string("<body/>"));
    PB_CHECK(q.url.find("alice") == std::string::npos);
    PB_CHECK(q.url.find("secret-pw") == std::string::npos);
    PB_CHECK_EQ(pbo::header_value(q.headers, "Depth"), std::string("0"));
}

PB_TEST(send_remembers_the_server_time_of_the_date_header) {
    FakeTransport http;
    http.set_handler([](const pbo::Request&) { return FakeTransport::status(207, "", {{"Date", "Wed, 14 Oct 2026 12:00:00 GMT"}}); });
    DavSession s(http, "https://dav.example.org", "a", "b");
    PB_CHECK(!s.last_server_time().has_value());
    s.send("PROPFIND", "https://dav.example.org/", "", {});
    PB_CHECK(s.last_server_time().has_value());
    PB_CHECK_EQ(*s.last_server_time(), 1791979200LL);
}

PB_TEST(send_without_an_http_answer_keeps_the_server_time_unset) {
    FakeTransport http;
    http.set_handler([](const pbo::Request&) {
        pbo::Response r;
        r.error = pbo::ErrorClass::Network;
        r.error_key = "server_unreachable";
        return r;
    });
    DavSession s(http, "https://dav.example.org", "a", "b");
    s.send("GET", "https://dav.example.org/", "", {});
    PB_CHECK(!s.last_server_time().has_value());
}

PB_TEST(status_error_maps_the_http_status_to_the_stable_keys) {
    FakeTransport http;
    DavSession s(http, "https://dav.example.org", "a", "b");
    auto key = [&](long status) { return s.status_error(FakeTransport::status(status)).key; };
    PB_CHECK_EQ(key(401), std::string("auth_failed"));
    PB_CHECK_EQ(key(403), std::string("forbidden"));
    PB_CHECK_EQ(key(404), std::string("not_found"));
    PB_CHECK_EQ(key(500), std::string("server_error"));
    PB_CHECK_EQ(key(503), std::string("server_error"));
    PB_CHECK_EQ(key(429), std::string("server_error"));
    PB_CHECK_EQ(key(408), std::string("server_error"));
    PB_CHECK_EQ(key(418), std::string("protocol_error"));
    PB_CHECK(s.status_error(FakeTransport::status(207)).ok());
    PB_CHECK(s.status_error(FakeTransport::status(204)).ok());

    bool handled = true;
    s.status_error(FakeTransport::status(207), &handled);
    PB_CHECK(!handled);   // a 2xx status is not handled here: the caller reads the answer
    s.status_error(FakeTransport::status(401), &handled);
    PB_CHECK(handled);

    pbo::Response big = FakeTransport::status(207);
    big.body_too_large = true;
    PB_CHECK_EQ(s.status_error(big).key, std::string("too_large"));

    pbo::Response none;
    none.error = pbo::ErrorClass::Tls;
    none.error_key = "cert_expired";
    const pbo::Error e = s.status_error(none);
    PB_CHECK_EQ(e.key, std::string("cert_expired"));
    PB_CHECK(e.cls == pbo::ErrorClass::Tls);
    pbo::Response unreachable;
    PB_CHECK_EQ(s.status_error(unreachable).key, std::string("server_unreachable"));
}

PB_TEST(parse_multistatus_accepts_only_a_multistatus_root) {
    const char* ok = "<?xml version=\"1.0\"?><d:multistatus xmlns:d=\"DAV:\"><d:response/></d:multistatus>";
    PB_CHECK(pbo::dav::parse_multistatus(FakeTransport::status(207, ok)).ok());
    const auto other = pbo::dav::parse_multistatus(FakeTransport::status(207, "<html><body/></html>"));
    PB_CHECK(!other.ok());
    PB_CHECK_EQ(other.error.key, std::string("malformed_xml"));
    const auto wrong_ns = pbo::dav::parse_multistatus(FakeTransport::status(207, "<multistatus xmlns=\"urn:other\"/>"));
    PB_CHECK_EQ(wrong_ns.error.key, std::string("malformed_xml"));
    PB_CHECK_EQ(pbo::dav::parse_multistatus(FakeTransport::status(207, "<d:multistatus")).error.key, std::string("malformed_xml"));
}

PB_TEST(prop_in_ok_propstat_ignores_a_propstat_with_a_failure_status) {
    const char* xml =
        "<d:multistatus xmlns:d=\"DAV:\" xmlns:cs=\"http://calendarserver.org/ns/\"><d:response>"
        "<d:propstat><d:prop><cs:getctag>stale</cs:getctag></d:prop><d:status>HTTP/1.1 404 Not Found</d:status></d:propstat>"
        "<d:propstat><d:prop><d:displayname>Work</d:displayname></d:prop><d:status>HTTP/1.1 200 OK</d:status></d:propstat>"
        "</d:response></d:multistatus>";
    const auto ms = pbo::dav::parse_multistatus(FakeTransport::status(207, xml));
    PB_CHECK(ms.ok());
    const pbo::XmlNode* resp = ms.value.child(pbo::dav::kNsDav, "response");
    PB_CHECK(resp != nullptr);
    PB_CHECK(pbo::dav::prop_in_ok_propstat(*resp, pbo::dav::kNsCs, "getctag") == nullptr);
    const pbo::XmlNode* name = pbo::dav::prop_in_ok_propstat(*resp, pbo::dav::kNsDav, "displayname");
    PB_CHECK(name != nullptr);
    PB_CHECK_EQ(pbo::dav::trim(name->text), std::string("Work"));
    PB_CHECK(pbo::dav::ok_prop_set(*resp) != nullptr);
}

PB_TEST(small_helpers_keep_their_behaviour) {
    PB_CHECK_EQ(pbo::dav::escape("a&b<c>\"d"), std::string("a&amp;b&lt;c&gt;&quot;d"));
    PB_CHECK_EQ(pbo::dav::trim("  x y \n"), std::string("x y"));
    PB_CHECK(pbo::dav::status_ok("HTTP/1.1 200 OK"));
    PB_CHECK(pbo::dav::status_ok("HTTP/1.1 204 No Content"));
    PB_CHECK(!pbo::dav::status_ok("HTTP/1.1 404 Not Found"));
    PB_CHECK(!pbo::dav::status_ok("garbage"));
    PB_CHECK_EQ(pbo::dav::status_code("HTTP/1.1 507 Insufficient Storage"), 507);
    PB_CHECK_EQ(pbo::dav::status_code("garbage"), 0);
    PB_CHECK_EQ(pbo::dav::path_of("https://dav.example.org", "https://dav.example.org/dav/x"), std::string("/dav/x"));
    PB_CHECK_EQ(pbo::dav::path_of("https://dav.example.org", "/dav/x"), std::string("/dav/x"));
    const pbo::Headers h = pbo::dav::dav_headers("1");
    PB_CHECK_EQ(pbo::header_value(h, "Depth"), std::string("1"));
    PB_CHECK_EQ(pbo::header_value(h, "Content-Type"), std::string("application/xml; charset=utf-8"));
    PB_CHECK(std::string(pbo::dav::kXmlHeader).find("<?xml") == 0);
}

PB_TEST_MAIN()
