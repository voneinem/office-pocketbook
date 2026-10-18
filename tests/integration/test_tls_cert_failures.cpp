// T103: expired and wrong-hostname certificates (both signed by a CA the client trusts) are
// refused, and no Authorization header reaches the server. Needs the `openssl` CLI.
#include <string>

#include "pbo/carddav.h"
#include "pbo/curl_transport.h"
#include "test_support.h"
#include "tls_test_server.h"

using pbo::CurlTransport;
using pbo::ErrorClass;
using pbo::Request;
using pbtest::TlsTestServer;

namespace {
TlsTestServer::Handler ok_handler() {
    return [](const std::string&) { return TlsTestServer::http_response(200, "{}"); };
}
Request make_request(const TlsTestServer& s) {
    Request req;
    req.url = s.url("/x");
    req.username = "alice";
    req.password = "app-secret";
    return req;
}
}  // namespace

PB_TEST(control_valid_certificate_for_localhost_is_accepted) {
    pbtest::TmpDir d;
    pbtest::TestPki pki(d);
    if (!pki.available()) PB_SKIP("openssl CLI not available");
    auto leaf = pki.issue("good", "DNS:localhost,IP:127.0.0.1");
    PB_CHECK(leaf.ok);
    TlsTestServer server(leaf.cert, leaf.key, ok_handler());
    CurlTransport transport(pki.ca_cert());
    auto resp = transport.perform(make_request(server));
    PB_CHECK(resp.error == ErrorClass::None);  // proves the harness itself is sound
    PB_CHECK_EQ(resp.status, 200L);
}

PB_TEST(expired_certificate_is_refused_and_no_credentials_are_sent) {
    pbtest::TmpDir d;
    pbtest::TestPki pki(d);
    if (!pki.available()) PB_SKIP("openssl CLI not available");
    auto leaf = pki.issue("expired", "DNS:localhost,IP:127.0.0.1", "20200101000000Z", "20200102000000Z");
    PB_CHECK(leaf.ok);
    TlsTestServer server(leaf.cert, leaf.key, ok_handler());
    CurlTransport transport(pki.ca_cert());
    auto resp = transport.perform(make_request(server));
    PB_CHECK(resp.error == ErrorClass::Tls);
    PB_CHECK(!server.saw_authorization());
    PB_CHECK(server.requests().empty());
}

PB_TEST(wrong_hostname_is_refused_and_no_credentials_are_sent) {
    pbtest::TmpDir d;
    pbtest::TestPki pki(d);
    if (!pki.available()) PB_SKIP("openssl CLI not available");
    auto leaf = pki.issue("wronghost", "DNS:wrong.example.com");
    PB_CHECK(leaf.ok);
    TlsTestServer server(leaf.cert, leaf.key, ok_handler());
    CurlTransport transport(pki.ca_cert());
    auto resp = transport.perform(make_request(server));  // connects to "localhost"
    PB_CHECK(resp.error == ErrorClass::Tls);
    PB_CHECK(!server.saw_authorization());
    PB_CHECK(server.requests().empty());
}

PB_TEST(carddav_discovery_refuses_an_expired_certificate_and_sends_no_credentials) {
    pbtest::TmpDir d;
    pbtest::TestPki pki(d);
    if (!pki.available()) PB_SKIP("openssl CLI not available");
    auto leaf = pki.issue("expired", "DNS:localhost,IP:127.0.0.1", "20200101000000Z", "20200102000000Z");
    PB_CHECK(leaf.ok);
    TlsTestServer server(leaf.cert, leaf.key, ok_handler());
    CurlTransport transport(pki.ca_cert());
    pbo::CardDavClient client(transport, server.url("/dav/"), "alice", "app-secret");
    auto found = client.discover();
    PB_CHECK(!found.ok());
    PB_CHECK(found.error.cls == ErrorClass::Tls);
    PB_CHECK(!server.saw_authorization());
    PB_CHECK(server.requests().empty());
}

PB_TEST(carddav_discovery_refuses_a_certificate_for_another_host_and_sends_no_credentials) {
    pbtest::TmpDir d;
    pbtest::TestPki pki(d);
    if (!pki.available()) PB_SKIP("openssl CLI not available");
    auto leaf = pki.issue("wronghost", "DNS:wrong.example.com");
    PB_CHECK(leaf.ok);
    TlsTestServer server(leaf.cert, leaf.key, ok_handler());
    CurlTransport transport(pki.ca_cert());
    pbo::CardDavClient client(transport, server.url("/dav/"), "alice", "app-secret");
    auto found = client.discover();
    PB_CHECK(!found.ok());
    PB_CHECK(found.error.cls == ErrorClass::Tls);
    PB_CHECK(!server.saw_authorization());
    PB_CHECK(server.requests().empty());
}

PB_TEST(carddav_refuses_a_certificate_from_an_authority_the_client_does_not_trust) {
    pbtest::TmpDir d, other;
    pbtest::TestPki pki(d), stranger(other);
    if (!pki.available()) PB_SKIP("openssl CLI not available");
    auto leaf = stranger.issue("good", "DNS:localhost,IP:127.0.0.1");
    PB_CHECK(leaf.ok);
    TlsTestServer server(leaf.cert, leaf.key, ok_handler());
    CurlTransport transport(pki.ca_cert());   // trusts a different authority
    pbo::CardDavClient client(transport, server.url("/dav/"), "alice", "app-secret");
    auto found = client.discover();
    PB_CHECK(!found.ok());
    PB_CHECK(found.error.cls == ErrorClass::Tls);
    PB_CHECK(!server.saw_authorization());
}

PB_TEST_MAIN()
