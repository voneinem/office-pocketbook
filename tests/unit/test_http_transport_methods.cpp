// T097: the HTTP seam for WebDAV: PROPFIND, REPORT and OPTIONS with a body, multi-valued case-insensitive response headers, the
// 8 MiB cap for in-memory responses, redirects that keep method and body (and still do not forward credentials). Needs `openssl`.
#include <string>

#include "pbo/curl_transport.h"
#include "test_support.h"
#include "tls_test_server.h"

using pbo::CurlTransport;
using pbo::ErrorClass;
using pbo::Request;
using pbtest::TlsTestServer;

namespace {

std::string first_line(const std::string& req) { return req.substr(0, req.find("\r\n")); }
std::string body_of(const std::string& req) {
    const size_t p = req.find("\r\n\r\n");
    return p == std::string::npos ? std::string() : req.substr(p + 4);
}
std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

struct Fixture {
    pbtest::TmpDir dir;
    pbtest::TestPki pki{dir};
    pbtest::TestPki::Leaf leaf;
    Fixture() {
        if (!pki.available()) PB_SKIP("openssl CLI not available");
        leaf = pki.issue("good", "DNS:localhost,IP:127.0.0.1");
        PB_CHECK(leaf.ok);
    }
};

}  // namespace

PB_TEST(propfind_report_and_options_carry_their_method_headers_and_body) {
    Fixture f;
    TlsTestServer server(f.leaf.cert, f.leaf.key, [](const std::string&) { return TlsTestServer::http_response(207, "<multistatus/>"); });
    CurlTransport transport(f.pki.ca_cert());
    for (const char* method : {"PROPFIND", "REPORT", "OPTIONS"}) {
        Request req;
        req.method = method;
        req.url = server.url("/dav/");
        req.headers = {{"Depth", "1"}, {"Content-Type", "application/xml; charset=utf-8"}};
        req.body = "<d:propfind xmlns:d=\"DAV:\"><d:prop/></d:propfind>";
        const auto resp = transport.perform(req);
        PB_CHECK(resp.error == ErrorClass::None);
        PB_CHECK_EQ(resp.status, 207L);
        PB_CHECK_EQ(resp.body, std::string("<multistatus/>"));
        const std::string seen = server.requests().back();
        PB_CHECK_EQ(first_line(seen), std::string(method) + " /dav/ HTTP/1.1");
        PB_CHECK(lower(seen).find("\r\ndepth: 1\r\n") != std::string::npos);
        PB_CHECK(lower(seen).find("content-type: application/xml; charset=utf-8") != std::string::npos);
        PB_CHECK_EQ(body_of(seen), req.body);
    }
}

PB_TEST(put_and_delete_carry_the_conditional_headers) {
    Fixture f;
    TlsTestServer server(f.leaf.cert, f.leaf.key, [](const std::string&) { return TlsTestServer::http_response(204, ""); });
    CurlTransport transport(f.pki.ca_cert());
    Request put;
    put.method = "PUT";
    put.url = server.url("/dav/cal/new.ics");
    put.headers = {{"If-None-Match", "*"}, {"Content-Type", "text/calendar; charset=utf-8"}};
    put.body = "BEGIN:VCALENDAR\r\nEND:VCALENDAR\r\n";
    PB_CHECK_EQ(transport.perform(put).status, 204L);
    PB_CHECK(lower(server.requests().back()).find("\r\nif-none-match: *\r\n") != std::string::npos);
    PB_CHECK_EQ(body_of(server.requests().back()), put.body);
    Request del;
    del.method = "DELETE";
    del.url = server.url("/dav/cal/old.ics");
    del.headers = {{"If-Match", "\"abc\""}};
    PB_CHECK_EQ(transport.perform(del).status, 204L);
    PB_CHECK_EQ(first_line(server.requests().back()), std::string("DELETE /dav/cal/old.ics HTTP/1.1"));
    PB_CHECK(lower(server.requests().back()).find("\r\nif-match: \"abc\"\r\n") != std::string::npos);
}

PB_TEST(response_headers_are_multi_valued_and_looked_up_without_regard_to_case) {
    Fixture f;
    TlsTestServer server(f.leaf.cert, f.leaf.key, [](const std::string&) {
        return TlsTestServer::http_response(200, "ok", "DAV: 1, calendar-access\r\ndav: addressbook\r\nETag: \"v1\"\r\n");
    });
    CurlTransport transport(f.pki.ca_cert());
    Request req;
    req.url = server.url("/");
    const auto resp = transport.perform(req);
    const auto all = pbo::header_values(resp.headers, "Dav");
    PB_CHECK_EQ(all.size(), size_t(2));
    PB_CHECK_EQ(all[0], std::string("1, calendar-access"));
    PB_CHECK_EQ(all[1], std::string("addressbook"));
    PB_CHECK_EQ(pbo::header_value(resp.headers, "etag"), std::string("\"v1\""));
    PB_CHECK(pbo::header_values(resp.headers, "X-Missing").empty());
}

PB_TEST(the_default_cap_for_in_memory_responses_is_8_mib_and_is_enforced) {
    Request defaults;
    PB_CHECK_EQ(defaults.max_response_bytes, (long long)(8u << 20));
    Fixture f;
    TlsTestServer server(f.leaf.cert, f.leaf.key, [](const std::string& req) {
        const size_t n = req.find("/big") != std::string::npos ? (9u << 20) : (1u << 20);
        return TlsTestServer::http_response(200, std::string(n, 'x'));
    });
    CurlTransport transport(f.pki.ca_cert());
    Request req;
    req.url = server.url("/big");
    auto resp = transport.perform(req);
    PB_CHECK(resp.body_too_large);
    PB_CHECK(resp.body.empty());
    PB_CHECK_EQ(resp.status, 200L);
    req.url = server.url("/ok");
    resp = transport.perform(req);
    PB_CHECK(!resp.body_too_large);
    PB_CHECK_EQ(resp.body.size(), size_t(1u << 20));
    req.max_response_bytes = 1000;                                       // a smaller cap set by the caller
    resp = transport.perform(req);
    PB_CHECK(resp.body_too_large);
    req.max_response_bytes = 0;                                          // 0 = no cap
    req.url = server.url("/big");
    resp = transport.perform(req);
    PB_CHECK(!resp.body_too_large);
    PB_CHECK_EQ(resp.body.size(), size_t(9u << 20));
}

PB_TEST(a_redirect_keeps_the_method_and_the_body_of_a_propfind) {
    Fixture f;
    TlsTestServer server(f.leaf.cert, f.leaf.key, [](const std::string& req) {
        if (req.find("/.well-known/caldav") != std::string::npos) return TlsTestServer::http_response(301, "", "Location: /dav/\r\n");
        return TlsTestServer::http_response(207, "<multistatus/>");
    });
    CurlTransport transport(f.pki.ca_cert());
    Request req;
    req.method = "PROPFIND";
    req.url = server.url("/.well-known/caldav");
    req.headers = {{"Depth", "0"}};
    req.body = "<propfind/>";
    req.username = "alice";
    req.password = "app-secret";
    const auto resp = transport.perform(req);
    PB_CHECK_EQ(resp.status, 207L);
    const auto seen = server.requests();
    PB_CHECK_EQ(seen.size(), size_t(2));
    PB_CHECK_EQ(first_line(seen[1]), std::string("PROPFIND /dav/ HTTP/1.1"));
    PB_CHECK_EQ(body_of(seen[1]), std::string("<propfind/>"));
    PB_CHECK(lower(seen[1]).find("\r\ndepth: 0\r\n") != std::string::npos);
}

PB_TEST(a_redirect_of_a_propfind_to_another_host_does_not_forward_the_credentials) {
    Fixture f;
    TlsTestServer b(f.leaf.cert, f.leaf.key, [](const std::string&) { return TlsTestServer::http_response(207, "<other/>"); });
    const std::string target = b.url("/dav/");
    TlsTestServer a(f.leaf.cert, f.leaf.key, [&](const std::string&) { return TlsTestServer::http_response(307, "", "Location: " + target + "\r\n"); });
    CurlTransport transport(f.pki.ca_cert());
    Request req;
    req.method = "PROPFIND";
    req.url = a.url("/start");
    req.body = "<propfind/>";
    req.username = "alice";
    req.password = "app-secret";
    const auto resp = transport.perform(req);
    PB_CHECK_EQ(resp.status, 207L);
    PB_CHECK(a.saw_authorization());
    PB_CHECK(!b.saw_authorization());
}

PB_TEST_MAIN()
