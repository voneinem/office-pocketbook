// libcurl result codes mapped to the stable cause keys the user sees (spec US1 scenario 1, FR-035), and the
// guarantee that no error text or log line can carry credentials, host names or addresses (FR-041).
#include <curl/curl.h>

#include <string>
#include <vector>

#include "pbo/curl_session.h"
#include "pbo/log.h"
#include "test_support.h"

namespace {
// OpenSSL X509 verification results as libcurl reports them through CURLINFO_SSL_VERIFYRESULT.
const long kExpired = 10;           // X509_V_ERR_CERT_HAS_EXPIRED
const long kSelfSigned = 18;        // X509_V_ERR_DEPTH_ZERO_SELF_SIGNED_CERT
const long kUnknownIssuer = 20;     // X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT_LOCALLY
const long kHostMismatch = 62;      // X509_V_ERR_HOSTNAME_MISMATCH
const long kIpMismatch = 64;        // X509_V_ERR_IP_ADDRESS_MISMATCH
}  // namespace

PB_TEST(certificate_failures_get_their_own_cause) {
    auto expired = pbo::map_curl_error(CURLE_PEER_FAILED_VERIFICATION, kExpired, true);
    PB_CHECK(expired.cls == pbo::ErrorClass::Tls);
    PB_CHECK_EQ(expired.key, std::string("cert_expired"));
    PB_CHECK_EQ(pbo::map_curl_error(CURLE_PEER_FAILED_VERIFICATION, kHostMismatch, true).key, std::string("cert_host_mismatch"));
    PB_CHECK_EQ(pbo::map_curl_error(CURLE_PEER_FAILED_VERIFICATION, kIpMismatch, true).key, std::string("cert_host_mismatch"));
    PB_CHECK_EQ(pbo::map_curl_error(CURLE_PEER_FAILED_VERIFICATION, kSelfSigned, true).key, std::string("cert_untrusted"));
    PB_CHECK_EQ(pbo::map_curl_error(CURLE_PEER_FAILED_VERIFICATION, kUnknownIssuer, true).key, std::string("cert_untrusted"));
    // libcurl checks the host name AFTER the chain verified fine (SSL_VERIFYRESULT is then 0): that is the name mismatch
    PB_CHECK_EQ(pbo::map_curl_error(CURLE_PEER_FAILED_VERIFICATION, 0, true).key, std::string("cert_host_mismatch"));
    PB_CHECK_EQ(pbo::map_curl_error(CURLE_PEER_FAILED_VERIFICATION, 1, true).key, std::string("cert_host_mismatch"));   // what libcurl 8.18.0 reports
    PB_CHECK(pbo::map_curl_error(CURLE_SSL_CACERT_BADFILE, 0, true).cls == pbo::ErrorClass::Tls);
}

PB_TEST(a_refused_or_failed_starttls_is_its_own_cause) {
    auto e = pbo::map_curl_error(CURLE_USE_SSL_FAILED, 0, true);
    PB_CHECK(e.cls == pbo::ErrorClass::Tls);
    PB_CHECK_EQ(e.key, std::string("starttls_unavailable"));
}

PB_TEST(wrong_credentials_are_an_authentication_problem_not_a_network_problem) {
    auto e = pbo::map_curl_error(CURLE_LOGIN_DENIED, 0, true);
    PB_CHECK(e.cls == pbo::ErrorClass::Auth);
    PB_CHECK_EQ(e.key, std::string("auth_failed"));
}

PB_TEST(unreachable_servers_depend_on_whether_wifi_is_up) {
    for (CURLcode c : {CURLE_COULDNT_RESOLVE_HOST, CURLE_COULDNT_CONNECT}) {
        auto up = pbo::map_curl_error(c, 0, true);
        PB_CHECK(up.cls == pbo::ErrorClass::Network);
        PB_CHECK_EQ(up.key, std::string("server_unreachable"));
        auto down = pbo::map_curl_error(c, 0, false);
        PB_CHECK_EQ(down.key, std::string("no_network"));
    }
}

PB_TEST(timeouts_and_lost_connections) {
    auto t = pbo::map_curl_error(CURLE_OPERATION_TIMEDOUT, 0, true);
    PB_CHECK(t.cls == pbo::ErrorClass::Network);
    PB_CHECK_EQ(t.key, std::string("timeout"));
    for (CURLcode c : {CURLE_RECV_ERROR, CURLE_SEND_ERROR, CURLE_GOT_NOTHING}) {
        PB_CHECK_EQ(pbo::map_curl_error(c, 0, true).key, std::string("connection_lost"));
    }
}

PB_TEST(other_cases_have_stable_keys) {
    PB_CHECK(pbo::map_curl_error(CURLE_UNSUPPORTED_PROTOCOL, 0, true).cls == pbo::ErrorClass::Tls);  // a redirect to http://
    PB_CHECK_EQ(pbo::map_curl_error(CURLE_UNSUPPORTED_PROTOCOL, 0, true).key, std::string("scheme_not_allowed"));
    PB_CHECK_EQ(pbo::map_curl_error(CURLE_REMOTE_FILE_NOT_FOUND, 0, true).key, std::string("not_found"));
    PB_CHECK_EQ(pbo::map_curl_error(CURLE_FILESIZE_EXCEEDED, 0, true).key, std::string("too_large"));
    PB_CHECK(pbo::map_curl_error(CURLE_TOO_MANY_REDIRECTS, 0, true).cls == pbo::ErrorClass::Server);
    auto unknown = pbo::map_curl_error(static_cast<CURLcode>(9999), 0, true);
    PB_CHECK(!unknown.ok());
    PB_CHECK_EQ(unknown.key, std::string("protocol_error"));
    PB_CHECK(pbo::map_curl_error(CURLE_OK, 0, true).ok());
}

PB_TEST(error_detail_never_carries_curl_text_hosts_or_credentials) {
    // curl's own error buffer contains host names and, for some failures, protocol lines. The mapper takes no
    // error text at all, so none can leak; this test pins the property for every code the mapper knows.
    const CURLcode codes[] = {CURLE_PEER_FAILED_VERIFICATION, CURLE_SSL_CACERT_BADFILE, CURLE_USE_SSL_FAILED,
                              CURLE_LOGIN_DENIED,             CURLE_COULDNT_RESOLVE_HOST, CURLE_COULDNT_CONNECT,
                              CURLE_OPERATION_TIMEDOUT,       CURLE_RECV_ERROR,        CURLE_UNSUPPORTED_PROTOCOL,
                              CURLE_REMOTE_FILE_NOT_FOUND,    CURLE_FILESIZE_EXCEEDED, CURLE_TOO_MANY_REDIRECTS};
    const std::vector<std::string> forbidden = {"mail.example.org", "example.org", "alice", "AUTH PLAIN", "LOGIN", "secret", "@", "://"};
    for (CURLcode c : codes) {
        for (long v : {0L, kExpired, kHostMismatch}) {
            const pbo::Error e = pbo::map_curl_error(c, v, true);
            for (const std::string& f : forbidden) {
                PB_CHECK(e.detail.find(f) == std::string::npos);
                PB_CHECK(e.key.find(f) == std::string::npos);
            }
        }
    }
}

PB_TEST(logging_a_failure_writes_only_the_class_and_the_cause) {
    std::vector<std::string> lines;
    pbo::log::set_sink([&](const std::string& line) { lines.push_back(line); });
    pbo::Error e = pbo::map_curl_error(CURLE_LOGIN_DENIED, 0, true);
    e.detail = "mail.example.org alice AUTH PLAIN secret";  // even a careless detail must not reach the log
    pbo::log_failure("sync.step_failed", e);
    pbo::log::reset_sink();
    PB_CHECK_EQ(lines.size(), size_t(1));
    PB_CHECK(lines[0].find("sync.step_failed") != std::string::npos);
    PB_CHECK(lines[0].find("auth_failed") != std::string::npos);
    for (const char* f : {"mail.example.org", "alice", "AUTH PLAIN", "secret"}) PB_CHECK(lines[0].find(f) == std::string::npos);
}

PB_TEST_MAIN()
