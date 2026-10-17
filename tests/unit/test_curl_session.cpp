// The shared TLS and endpoint policy of every libcurl transport (research R-12, constitution Principle IV):
// verification is always on, only imaps/smtps/https or enforced STARTTLS are accepted, and no other scheme
// is ever used. Pure policy: nothing here opens a connection.
#include <curl/curl.h>

#include <string>

#include "pbo/curl_session.h"
#include "test_support.h"

using pbo::Protocol;
using pbo::Security;

PB_TEST(every_policy_verifies_peer_and_host_and_needs_tls_1_2) {
    const Protocol protocols[] = {Protocol::Http, Protocol::Imap, Protocol::Smtp};
    for (Protocol p : protocols) {
        auto r = pbo::make_tls_policy(p, Security::Tls);
        PB_CHECK(r.ok());
        PB_CHECK_EQ(r.value.verify_peer, 1L);
        PB_CHECK_EQ(r.value.verify_host, 2L);
        PB_CHECK_EQ(r.value.min_tls_version, static_cast<long>(CURL_SSLVERSION_TLSv1_2));
    }
}

PB_TEST(imap_and_smtp_always_enforce_tls_and_follow_no_redirects) {
    for (Protocol p : {Protocol::Imap, Protocol::Smtp}) {
        for (Security s : {Security::Tls, Security::StartTls}) {
            auto r = pbo::make_tls_policy(p, s);
            PB_CHECK(r.ok());
            PB_CHECK(r.value.use_ssl.has_value());
            PB_CHECK_EQ(*r.value.use_ssl, static_cast<long>(CURLUSESSL_ALL));  // no plaintext fallback
            PB_CHECK(!r.value.follow_redirects);
        }
    }
}

PB_TEST(allowed_protocols_name_exactly_one_scheme) {
    PB_CHECK_EQ(pbo::make_tls_policy(Protocol::Imap, Security::Tls).value.protocols, std::string("imaps"));
    PB_CHECK_EQ(pbo::make_tls_policy(Protocol::Imap, Security::StartTls).value.protocols, std::string("imap"));
    PB_CHECK_EQ(pbo::make_tls_policy(Protocol::Smtp, Security::Tls).value.protocols, std::string("smtps"));
    PB_CHECK_EQ(pbo::make_tls_policy(Protocol::Smtp, Security::StartTls).value.protocols, std::string("smtp"));
    auto http = pbo::make_tls_policy(Protocol::Http, Security::Tls);
    PB_CHECK_EQ(http.value.protocols, std::string("https"));
    PB_CHECK_EQ(http.value.redir_protocols, std::string("https"));  // a redirect to http:// is refused
    PB_CHECK(http.value.follow_redirects);
    PB_CHECK(!http.value.use_ssl.has_value());
}

PB_TEST(http_has_no_starttls_variant) {
    auto r = pbo::make_tls_policy(Protocol::Http, Security::StartTls);
    PB_CHECK(!r.ok());
    PB_CHECK(r.error.cls == pbo::ErrorClass::Unsupported);
}

PB_TEST(timeouts_follow_the_research_decision) {
    auto r = pbo::make_tls_policy(Protocol::Imap, Security::Tls);
    PB_CHECK_EQ(r.value.connect_timeout_s, 15L);
    PB_CHECK_EQ(r.value.low_speed_limit, 1L);       // 1 byte/s ...
    PB_CHECK_EQ(r.value.low_speed_time_s, 30L);     // ... for 30 s
    PB_CHECK_EQ(r.value.total_timeout_s, 120L);
}

PB_TEST(the_scheme_must_match_the_transport_and_the_security_mode) {
    // implicit TLS
    PB_CHECK(pbo::check_url_scheme(Protocol::Imap, Security::Tls, "imaps://mail.example.org:993/INBOX").ok());
    PB_CHECK(pbo::check_url_scheme(Protocol::Smtp, Security::Tls, "smtps://mail.example.org:465").ok());
    PB_CHECK(pbo::check_url_scheme(Protocol::Http, Security::Tls, "https://cal.example.org/dav").ok());
    PB_CHECK(pbo::check_url_scheme(Protocol::Http, Security::Tls, "HTTPS://cal.example.org/dav").ok());
    // enforced STARTTLS
    PB_CHECK(pbo::check_url_scheme(Protocol::Imap, Security::StartTls, "imap://mail.example.org:143/").ok());
    PB_CHECK(pbo::check_url_scheme(Protocol::Smtp, Security::StartTls, "smtp://mail.example.org:587").ok());
    // plaintext scheme without the STARTTLS mode, and the reverse
    PB_CHECK(!pbo::check_url_scheme(Protocol::Imap, Security::Tls, "imap://mail.example.org/").ok());
    PB_CHECK(!pbo::check_url_scheme(Protocol::Smtp, Security::Tls, "smtp://mail.example.org/").ok());
    PB_CHECK(!pbo::check_url_scheme(Protocol::Imap, Security::StartTls, "imaps://mail.example.org/").ok());
}

PB_TEST(every_other_scheme_is_refused_before_any_connection) {
    const char* bad[] = {"http://cal.example.org/", "ftp://example.org/x", "file:///etc/passwd", "gopher://example.org/",
                         "pop3://example.org/",     "ldap://example.org/", "imaps:/example.org",  "imaps:example.org",
                         "://example.org",          "example.org",         "",                    "imaps//example.org"};
    for (const char* url : bad) {
        for (Protocol p : {Protocol::Imap, Protocol::Smtp, Protocol::Http}) {
            for (Security s : {Security::Tls, Security::StartTls}) {
                auto e = pbo::check_url_scheme(p, s, url);
                PB_CHECK(!e.ok());
                PB_CHECK(e.cls == pbo::ErrorClass::Unsupported);
                PB_CHECK_EQ(e.key, std::string("scheme_not_allowed"));
            }
        }
    }
    // an http endpoint never accepts the mail schemes either
    PB_CHECK(!pbo::check_url_scheme(Protocol::Http, Security::Tls, "imaps://mail.example.org/").ok());
    PB_CHECK(!pbo::check_url_scheme(Protocol::Imap, Security::Tls, "https://mail.example.org/").ok());
}

PB_TEST(make_url_builds_the_scheme_from_the_security_mode) {
    auto a = pbo::make_url(Protocol::Imap, Security::Tls, "mail.example.org", 993, "/INBOX");
    PB_CHECK(a.ok());
    PB_CHECK_EQ(a.value, std::string("imaps://mail.example.org:993/INBOX"));
    auto b = pbo::make_url(Protocol::Imap, Security::StartTls, "mail.example.org", 143, "/");
    PB_CHECK_EQ(b.value, std::string("imap://mail.example.org:143/"));
    auto c = pbo::make_url(Protocol::Smtp, Security::Tls, "mail.example.org", 465, "");
    PB_CHECK_EQ(c.value, std::string("smtps://mail.example.org:465"));
    auto d = pbo::make_url(Protocol::Http, Security::Tls, "cal.example.org", 443, "/dav/");
    PB_CHECK_EQ(d.value, std::string("https://cal.example.org:443/dav/"));
}

PB_TEST(make_url_rejects_bad_hosts_and_ports) {
    const char* hosts[] = {"", "a b", "a/b", "user@host", "host:99", "ho\nst", "ho\tst", "ho\x01st"};
    for (const char* h : hosts) PB_CHECK(!pbo::make_url(Protocol::Imap, Security::Tls, h, 993, "/").ok());
    PB_CHECK(!pbo::make_url(Protocol::Imap, Security::Tls, "mail.example.org", 0, "/").ok());
    PB_CHECK(!pbo::make_url(Protocol::Imap, Security::Tls, "mail.example.org", 65536, "/").ok());
    PB_CHECK(!pbo::make_url(Protocol::Imap, Security::Tls, "mail.example.org", -1, "/").ok());
    PB_CHECK(!pbo::make_url(Protocol::Http, Security::StartTls, "cal.example.org", 443, "/").ok());
}

PB_TEST(apply_tls_policy_accepts_a_real_handle_and_a_ca_file) {
    CURL* curl = curl_easy_init();
    PB_CHECK(curl != nullptr);
    pbo::Error e = pbo::apply_tls_policy(curl, "/nonexistent/ca.pem", Protocol::Imap, Security::Tls);
    PB_CHECK(e.ok());  // the file is only read when a connection is made
    curl_easy_cleanup(curl);
}

PB_TEST(apply_tls_policy_refuses_an_empty_ca_file_name) {
    CURL* curl = curl_easy_init();
    pbo::Error e = pbo::apply_tls_policy(curl, "", Protocol::Imap, Security::Tls);
    PB_CHECK(!e.ok());  // there is no "no CA" setting (constitution Principle IV)
    curl_easy_cleanup(curl);
}

// Minimum libcurl for mail (research R-01 known limitation 5): 8.12.0 and 8.16.0 drop the contents of IMAP literals in
// custom requests (tested, docs/device-findings.md), 8.18.0 delivers them. The reader's own libcurl is 8.12.0, so the
// app carries 8.18.0 statically (scripts/build-curl-arm.sh); anything older is refused before any connection.
PB_TEST(runtime_libcurl_older_than_the_floor_is_refused) {
    PB_CHECK_EQ(pbo::kMinCurlVersionNum, 0x081200u);                      // 8.18.0
    PB_CHECK(pbo::check_curl_runtime(0x081200).ok());
    PB_CHECK(pbo::check_curl_runtime(0x081201).ok());                     // 8.18.1
    PB_CHECK(pbo::check_curl_runtime(0x090000).ok());
    const pbo::Error e = pbo::check_curl_runtime(0x081000);               // 8.16.0
    PB_CHECK(!e.ok());
    PB_CHECK(e.cls == pbo::ErrorClass::Unsupported);
    PB_CHECK_EQ(e.key, std::string("libcurl_too_old"));
    PB_CHECK(!pbo::check_curl_runtime(0x080C00).ok());                    // the reader's own 8.12.0
    PB_CHECK(!pbo::check_curl_runtime(0).ok());                           // unknown version: fail closed
    PB_CHECK(pbo::check_curl_runtime().ok());                             // the library this test is linked with
}

PB_TEST_MAIN()
