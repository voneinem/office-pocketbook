// T068 (IMAP part, SC-012): every refused certificate (expired, wrong host name, unknown or self-signed authority) and every
// STARTTLS that cannot be enforced ends the connection with its own cause key, and the server NEVER receives a LOGIN. The
// SMTP variants follow with the SMTP transport. UNVERIFIED against real servers (fake server).
#include <string>

#include "fake_imap_server.h"
#include "pbo/imap_transport.h"
#include "test_support.h"
#include "tls_test_server.h"
#include "tmp_dir.h"

using pbtest::FakeImapServer;

PBO_FAKE_SERVER_TEST();

namespace {

struct Env {
    pbtest::TmpDir dir;
    pbtest::TestPki pki{dir};
    pbtest::TestPki other{dir, "other-ca"};   // an authority the client does not trust
    Env() {
        if (!pki.available() || !other.available()) throw pbtest::Skip{"the openssl command is not available"};
    }
    FakeImapServer::Config config(const pbtest::TestPki::Leaf& leaf, FakeImapServer::Mode mode = FakeImapServer::Mode::ImplicitTls) const {
        FakeImapServer::Config c;
        c.cert = leaf.cert;
        c.key = leaf.key;
        c.mode = mode;
        return c;
    }
    pbo::Error connect(const FakeImapServer& s, pbo::Security sec = pbo::Security::Tls) const {
        auto t = pbo::make_curl_imap_transport(pki.ca_cert());
        pbo::ImapEndpoint e;
        e.host = "localhost";
        e.port = s.port();
        e.security = sec;
        e.username = "alice";
        return t->connect(e, "pw-for-tests");
    }
};

// no password reached the server, in any form
void no_credentials(const FakeImapServer& s) {
    PB_CHECK(!s.credentials_seen());
    PB_CHECK(!s.plaintext_credentials_seen());
    for (const std::string& c : s.commands()) PB_CHECK(c.find("LOGIN") == std::string::npos && c.find("AUTHENTICATE") == std::string::npos);
}

}  // namespace

PB_TEST(control_a_valid_certificate_logs_in) {
    Env env;
    auto leaf = env.pki.issue("good", "DNS:localhost,IP:127.0.0.1");
    PB_CHECK(leaf.ok);
    FakeImapServer srv(env.config(leaf));
    PB_CHECK(env.connect(srv).ok());
    PB_CHECK(srv.credentials_seen());           // proves the harness sees credentials when they are sent
}

PB_TEST(an_expired_certificate_is_refused_before_the_login) {
    Env env;
    auto leaf = env.pki.issue("expired", "DNS:localhost,IP:127.0.0.1", "20200101000000Z", "20200102000000Z");
    PB_CHECK(leaf.ok);
    FakeImapServer srv(env.config(leaf));
    const auto e = env.connect(srv);
    PB_CHECK(e.cls == pbo::ErrorClass::Tls);
    PB_CHECK_EQ(e.key, std::string("cert_expired"));
    no_credentials(srv);
}

PB_TEST(a_certificate_for_another_name_is_refused_before_the_login) {
    Env env;
    auto leaf = env.pki.issue("wronghost", "DNS:wrong.example.org");
    PB_CHECK(leaf.ok);
    FakeImapServer srv(env.config(leaf));
    const auto e = env.connect(srv);
    PB_CHECK_EQ(e.key, std::string("cert_host_mismatch"));
    no_credentials(srv);
}

PB_TEST(a_certificate_from_an_unknown_authority_is_refused_before_the_login) {
    Env env;
    auto leaf = env.other.issue("stranger", "DNS:localhost,IP:127.0.0.1");
    PB_CHECK(leaf.ok);
    FakeImapServer srv(env.config(leaf));
    const auto e = env.connect(srv);
    PB_CHECK(e.cls == pbo::ErrorClass::Tls);
    PB_CHECK_EQ(e.key, std::string("cert_untrusted"));
    no_credentials(srv);
}

PB_TEST(a_self_signed_certificate_is_refused_before_the_login) {
    Env env;
    auto leaf = env.pki.self_signed("selfsigned", "DNS:localhost,IP:127.0.0.1");
    PB_CHECK(leaf.ok);
    FakeImapServer srv(env.config(leaf));
    PB_CHECK_EQ(env.connect(srv).key, std::string("cert_untrusted"));
    no_credentials(srv);
}

PB_TEST(starttls_with_a_valid_certificate_upgrades_and_logs_in) {
    Env env;
    auto leaf = env.pki.issue("good", "DNS:localhost,IP:127.0.0.1");
    FakeImapServer srv(env.config(leaf, FakeImapServer::Mode::StartTls));
    PB_CHECK(env.connect(srv, pbo::Security::StartTls).ok());
    PB_CHECK(srv.credentials_seen());
    PB_CHECK(!srv.plaintext_credentials_seen());       // the password travelled only after the upgrade
}

PB_TEST(starttls_that_the_server_refuses_is_an_error_and_no_password_is_sent_in_the_clear) {
    Env env;
    auto leaf = env.pki.issue("good", "DNS:localhost,IP:127.0.0.1");
    FakeImapServer srv(env.config(leaf, FakeImapServer::Mode::StartTls));
    srv.refuse_starttls = true;                         // not advertised, and the command is answered NO
    const auto e = env.connect(srv, pbo::Security::StartTls);
    PB_CHECK_EQ(e.key, std::string("starttls_unavailable"));
    no_credentials(srv);
}

PB_TEST(starttls_on_a_server_that_disables_the_login_without_it_sends_nothing) {
    Env env;
    auto leaf = env.pki.issue("good", "DNS:localhost,IP:127.0.0.1");
    FakeImapServer srv(env.config(leaf, FakeImapServer::Mode::StartTls));
    srv.refuse_starttls = true;
    srv.login_disabled = true;
    PB_CHECK_EQ(env.connect(srv, pbo::Security::StartTls).key, std::string("starttls_unavailable"));
    no_credentials(srv);
}

PB_TEST(starttls_with_a_bad_certificate_after_the_upgrade_is_refused_before_the_login) {
    Env env;
    auto leaf = env.pki.issue("expired", "DNS:localhost,IP:127.0.0.1", "20200101000000Z", "20200102000000Z");
    FakeImapServer srv(env.config(leaf, FakeImapServer::Mode::StartTls));
    PB_CHECK_EQ(env.connect(srv, pbo::Security::StartTls).key, std::string("cert_expired"));
    no_credentials(srv);
}

PB_TEST(implicit_tls_against_a_server_that_speaks_plain_text_first_is_an_error_without_a_password) {
    Env env;
    auto leaf = env.pki.issue("good", "DNS:localhost,IP:127.0.0.1");
    FakeImapServer srv(env.config(leaf, FakeImapServer::Mode::StartTls));   // greets in plain text
    const auto e = env.connect(srv, pbo::Security::Tls);
    PB_CHECK(!e.ok());
    no_credentials(srv);
}

PB_TEST_MAIN()
