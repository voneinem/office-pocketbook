// T067 (second part): test_mail_connection tries the IMAP and the SMTP server of the account form and reports success or ONE cause
// key with the side it belongs to (US1 scenario 1). Nothing is stored, and no credential leaves the device on a TLS failure.
// UNVERIFIED against real servers (fake servers).
#include <memory>
#include <string>

#include "fake_imap_server.h"
#include "fake_smtp_server.h"
#include "pbo/connection_test.h"
#include "pbo/imap_transport.h"
#include "pbo/smtp_transport.h"
#include "test_support.h"
#include "tls_test_server.h"
#include "tmp_dir.h"

using pbtest::FakeImapServer;
using pbtest::FakeSmtpServer;

PBO_FAKE_SERVER_TEST();

namespace {

struct Env {
    pbtest::TmpDir dir, other_dir;
    pbtest::TestPki pki{dir};
    pbtest::TestPki other_pki{other_dir, "other"};   // a CA the app does not trust
    pbtest::TestPki::Leaf leaf, expired, wrong_host, unknown;
    Env() {
        if (!pki.available() || !other_pki.available()) throw pbtest::Skip{"the openssl command is not available"};
        leaf = pki.issue("srv", "DNS:localhost,IP:127.0.0.1");
        expired = pki.issue("expired", "DNS:localhost,IP:127.0.0.1", "20200101000000Z", "20200201000000Z");
        wrong_host = pki.issue("wrong", "DNS:mail.other.example");
        unknown = other_pki.issue("unknown", "DNS:localhost,IP:127.0.0.1");
        if (!leaf.ok || !expired.ok || !wrong_host.ok || !unknown.ok) throw pbtest::Skip{"cannot create test certificates"};
    }
    std::unique_ptr<FakeImapServer> imap(const pbtest::TestPki::Leaf& l, FakeImapServer::Mode mode = FakeImapServer::Mode::ImplicitTls) const {
        FakeImapServer::Config c;
        c.cert = l.cert;
        c.key = l.key;
        c.mode = mode;
        auto s = std::make_unique<FakeImapServer>(c);
        s->set_capabilities("IMAP4rev1 UIDPLUS LITERAL+");
        s->add_folder("INBOX");
        return s;
    }
    std::unique_ptr<FakeSmtpServer> smtp(const pbtest::TestPki::Leaf& l, FakeSmtpServer::Mode mode = FakeSmtpServer::Mode::ImplicitTls) const {
        FakeSmtpServer::Config c;
        c.cert = l.cert;
        c.key = l.key;
        c.mode = mode;
        return std::make_unique<FakeSmtpServer>(c);
    }
};

pbo::MailAccountInput form(int imap_port, int smtp_port, pbo::Security imap_sec = pbo::Security::Tls, pbo::Security smtp_sec = pbo::Security::Tls) {
    pbo::MailAccountInput in;
    in.account.imap_host = "localhost";
    in.account.imap_port = imap_port;
    in.account.imap_security = imap_sec;
    in.account.smtp_host = "localhost";
    in.account.smtp_port = smtp_port;
    in.account.smtp_security = smtp_sec;
    in.account.username = "alice";
    in.account.address = "alice@example.org";
    in.imap_password = "pw-for-tests";
    return in;
}

pbo::Result<pbo::ConnectionReport> run(const Env& env, const pbo::MailAccountInput& in, bool network_up = true) {
    auto imap = pbo::make_curl_imap_transport(env.pki.ca_cert());
    auto smtp = pbo::make_curl_smtp_transport(env.pki.ca_cert());
    return pbo::test_mail_connection(*imap, *smtp, in, network_up);
}

}  // namespace

PB_TEST(both_servers_reachable_and_the_password_right_is_a_success) {
    Env env;
    auto imap = env.imap(env.leaf);
    auto smtp = env.smtp(env.leaf);
    const auto r = run(env, form(imap->port(), smtp->port()));
    PB_CHECK(r.ok());
    PB_CHECK(r.value.imap_ok);
    PB_CHECK(r.value.smtp_ok);
    PB_CHECK(r.value.imap_capabilities.count("imap4rev1") != 0);
    PB_CHECK(smtp->connections() >= 1);
}

// The form only allows STARTTLS on the ports 143 and 587 (validate_mail_account), the fake servers listen on random ports, so the
// STARTTLS cases drive the two transports directly, exactly as test_mail_connection does.
PB_TEST(starttls_on_both_servers_works_and_nothing_is_sent_before_the_upgrade) {
    Env env;
    auto imap = env.imap(env.leaf, FakeImapServer::Mode::StartTls);
    auto smtp = env.smtp(env.leaf, FakeSmtpServer::Mode::StartTls);
    pbo::ImapEndpoint ie;
    ie.host = "localhost";
    ie.port = imap->port();
    ie.security = pbo::Security::StartTls;
    ie.username = "alice";
    auto it = pbo::make_curl_imap_transport(env.pki.ca_cert());
    PB_CHECK(it->connect(ie, "pw-for-tests").ok());
    pbo::SmtpEndpoint se;
    se.host = "localhost";
    se.port = smtp->port();
    se.security = pbo::Security::StartTls;
    se.username = "alice";
    auto st = pbo::make_curl_smtp_transport(env.pki.ca_cert());
    PB_CHECK(st->test_connection(se, "pw-for-tests").ok());
    PB_CHECK(!imap->plaintext_credentials_seen());
    PB_CHECK(!smtp->plaintext_credentials_seen());
    PB_CHECK(smtp->credentials_seen());
}

PB_TEST(the_form_refuses_starttls_on_other_ports_before_any_connection) {
    Env env;
    auto imap = env.imap(env.leaf);
    auto smtp = env.smtp(env.leaf);
    const auto r = run(env, form(imap->port(), smtp->port(), pbo::Security::StartTls, pbo::Security::Tls));
    PB_CHECK_EQ(r.error.key, std::string("starttls_port_not_allowed"));
    PB_CHECK_EQ(imap->connections(), 0);
}

PB_TEST(a_wrong_password_names_the_side_that_refused_it) {
    Env env;
    auto imap = env.imap(env.leaf);
    auto smtp = env.smtp(env.leaf);
    auto in = form(imap->port(), smtp->port());
    in.imap_password = "wrong";
    in.smtp_password = "pw-for-tests";
    auto r = run(env, in);
    PB_CHECK(!r.ok());
    PB_CHECK_EQ(r.error.key, std::string("auth_failed"));
    PB_CHECK_EQ(r.value.failed_side, std::string("imap"));
    PB_CHECK(!r.value.imap_ok);
    PB_CHECK(r.value.smtp_ok);   // the other side was still tried: the user learns about both problems at once

    in.imap_password = "pw-for-tests";
    in.smtp_password = "wrong";
    r = run(env, in);
    PB_CHECK_EQ(r.error.key, std::string("auth_failed"));
    PB_CHECK_EQ(r.value.failed_side, std::string("smtp"));
    PB_CHECK(r.value.imap_ok);
}

PB_TEST(an_empty_smtp_password_means_the_same_as_the_mail_password_and_another_user_name_is_used) {
    Env env;
    auto imap = env.imap(env.leaf);
    auto smtp = env.smtp(env.leaf);
    auto in = form(imap->port(), smtp->port());
    in.smtp_password.clear();
    PB_CHECK(run(env, in).ok());
    in.account.smtp_username = "bob";   // a different SMTP user: the fake knows only alice
    const auto r = run(env, in);
    PB_CHECK_EQ(r.error.key, std::string("auth_failed"));
    PB_CHECK_EQ(r.value.failed_side, std::string("smtp"));
}

PB_TEST(certificate_problems_have_their_own_cause_keys_on_both_protocols_and_no_credential_is_sent) {
    Env env;
    struct Case {
        const pbtest::TestPki::Leaf* leaf;
        const char* key;
    };
    const Case cases[] = {{&env.expired, "cert_expired"}, {&env.wrong_host, "cert_host_mismatch"}, {&env.unknown, "cert_untrusted"}};
    for (const Case& c : cases) {
        {   // IMAP side
            auto imap = env.imap(*c.leaf);
            auto smtp = env.smtp(env.leaf);
            const auto r = run(env, form(imap->port(), smtp->port()));
            PB_CHECK_EQ(r.error.key, std::string(c.key));
            PB_CHECK_EQ(r.value.failed_side, std::string("imap"));
            PB_CHECK(!imap->credentials_seen());
            PB_CHECK(imap->commands().empty() || imap->commands().front() != "LOGIN");
        }
        {   // SMTP side
            auto imap = env.imap(env.leaf);
            auto smtp = env.smtp(*c.leaf);
            const auto r = run(env, form(imap->port(), smtp->port()));
            PB_CHECK_EQ(r.error.key, std::string(c.key));
            PB_CHECK_EQ(r.value.failed_side, std::string("smtp"));
            PB_CHECK(!smtp->credentials_seen());
            PB_CHECK(smtp->accepted().empty());
        }
    }
}

PB_TEST(a_server_without_starttls_is_refused_and_no_credential_is_sent_in_the_clear) {
    Env env;
    {
        auto imap = env.imap(env.leaf, FakeImapServer::Mode::StartTls);
        imap->refuse_starttls = true;
        pbo::ImapEndpoint ie;
        ie.host = "localhost";
        ie.port = imap->port();
        ie.security = pbo::Security::StartTls;
        ie.username = "alice";
        auto it = pbo::make_curl_imap_transport(env.pki.ca_cert());
        PB_CHECK_EQ(it->connect(ie, "pw-for-tests").key, std::string("starttls_unavailable"));
        PB_CHECK(!imap->credentials_seen());
    }
    {
        auto smtp = env.smtp(env.leaf, FakeSmtpServer::Mode::StartTls);
        smtp->refuse_starttls = true;
        pbo::SmtpEndpoint se;
        se.host = "localhost";
        se.port = smtp->port();
        se.security = pbo::Security::StartTls;
        se.username = "alice";
        auto st = pbo::make_curl_smtp_transport(env.pki.ca_cert());
        PB_CHECK_EQ(st->test_connection(se, "pw-for-tests").key, std::string("starttls_unavailable"));
        PB_CHECK(!smtp->credentials_seen());
        PB_CHECK(smtp->accepted().empty());
    }
}

PB_TEST(a_server_that_is_not_there_is_unreachable_and_without_a_network_nothing_is_tried) {
    Env env;
    int dead_imap_port, dead_smtp_port;
    {
        auto imap = env.imap(env.leaf);
        auto smtp = env.smtp(env.leaf);
        dead_imap_port = imap->port();
        dead_smtp_port = smtp->port();
    }   // both servers are gone: the ports are closed
    const auto r = run(env, form(dead_imap_port, dead_smtp_port));
    PB_CHECK_EQ(r.error.key, std::string("server_unreachable"));

    auto imap = env.imap(env.leaf);
    auto smtp = env.smtp(env.leaf);
    const auto offline = run(env, form(imap->port(), smtp->port()), /*network_up=*/false);
    PB_CHECK_EQ(offline.error.key, std::string("no_network"));
    PB_CHECK_EQ(imap->connections(), 0);
    PB_CHECK_EQ(smtp->connections(), 0);
}

PB_TEST(an_invalid_form_is_refused_before_any_connection) {
    Env env;
    auto imap = env.imap(env.leaf);
    auto smtp = env.smtp(env.leaf);
    auto in = form(imap->port(), smtp->port());
    in.account.imap_host = "bad host";
    const auto r = run(env, in);
    PB_CHECK(!r.ok());
    PB_CHECK_EQ(r.error.key, std::string("host_invalid"));
    PB_CHECK_EQ(imap->connections(), 0);
    PB_CHECK_EQ(smtp->connections(), 0);
}

PB_TEST_MAIN()
