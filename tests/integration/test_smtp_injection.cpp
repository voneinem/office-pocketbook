// T121 (transport part): SmtpTransport::send against the fake SMTP server with a failure injected at every point of the exchange. The rule
// (contracts/imap-smtp-subset.md section 5): Accepted only on the 250 after the final dot; CertainFailure only when nothing can have been accepted;
// everything else Unknown. UNVERIFIED against real servers (fake server).
#include <memory>
#include <string>

#include "fake_smtp_server.h"
#include "pbo/smtp_transport.h"
#include "test_support.h"
#include "tls_test_server.h"
#include "tmp_dir.h"

using pbtest::FakeSmtpServer;

PBO_FAKE_SERVER_TEST();

namespace {

struct Env {
    pbtest::TmpDir dir, other_dir;
    pbtest::TestPki pki{dir};
    pbtest::TestPki other_pki{other_dir, "other"};
    pbtest::TestPki::Leaf leaf, wrong_host, unknown;
    std::unique_ptr<FakeSmtpServer> server;
    pbo::SmtpEndpoint endpoint;
    Env(size_t max_size = 10u << 20, FakeSmtpServer::Mode mode = FakeSmtpServer::Mode::ImplicitTls, bool wrong_cert = false) {
        if (!pki.available() || !other_pki.available()) throw pbtest::Skip{"the openssl command is not available"};
        leaf = pki.issue("srv", "DNS:localhost,IP:127.0.0.1");
        wrong_host = pki.issue("wrong", "DNS:mail.other.example");
        unknown = other_pki.issue("unknown", "DNS:localhost,IP:127.0.0.1");
        if (!leaf.ok || !wrong_host.ok || !unknown.ok) throw pbtest::Skip{"cannot create test certificates"};
        FakeSmtpServer::Config c;
        c.cert = wrong_cert ? wrong_host.cert : leaf.cert;
        c.key = wrong_cert ? wrong_host.key : leaf.key;
        c.mode = mode;
        c.max_size = max_size;
        server = std::make_unique<FakeSmtpServer>(c);
        endpoint.host = "localhost";
        endpoint.port = server->port();
        endpoint.security = mode == FakeSmtpServer::Mode::StartTls ? pbo::Security::StartTls : pbo::Security::Tls;
        endpoint.username = "alice";
    }
    pbo::SendResult send(const std::string& raw, const std::vector<std::string>& to = {"bob@example.org"}, const std::string& password = "pw-for-tests",
                         pbo::TransportTimeouts t = {}) {
        auto transport = pbo::make_curl_smtp_transport(pki.ca_cert(), t);
        return transport->send(endpoint, password, "alice@example.org", to, raw);
    }
};

std::string message(const std::string& id, size_t body_bytes = 100) {
    std::string body;
    while (body.size() < body_bytes) body += "A line of the message body that is not too short.\r\n";
    return "From: alice@example.org\r\nTo: bob@example.org\r\nSubject: Test\r\nMessage-ID: <" + id + "@example.org>\r\nDate: Wed, 14 Oct 2026 12:00:00 +0000\r\n\r\n" + body;
}

pbo::TransportTimeouts short_timeouts() {
    pbo::TransportTimeouts t;
    t.connect_s = 5;
    t.low_speed_s = 2;
    t.command_s = 3;
    return t;
}

}  // namespace

PB_TEST(a_message_is_accepted_once_with_the_exact_bytes_and_envelope) {
    Env e;
    const std::string raw = message("ok1") + ".dot at the start of a line\r\n..two dots\r\n";
    const auto r = e.send(raw, {"bob@example.org", "carol@example.org"});
    PB_CHECK(r.outcome == pbo::SendOutcome::Accepted);
    PB_CHECK(r.error.ok());
    const auto acc = e.server->accepted();
    PB_CHECK_EQ(acc.size(), size_t(1));
    PB_CHECK_EQ(acc[0].envelope_from, std::string("alice@example.org"));
    PB_CHECK((acc[0].recipients == std::vector<std::string>{"bob@example.org", "carol@example.org"}));
    PB_CHECK_EQ(acc[0].data, raw);                                       // dot-stuffed on the wire, the same bytes after unstuffing
    PB_CHECK_EQ(e.server->accepted_count("<ok1@example.org>"), 1);
}

PB_TEST(starttls_submission_works_too) {
    Env e(10u << 20, FakeSmtpServer::Mode::StartTls);
    const auto r = e.send(message("st1"));
    PB_CHECK(r.outcome == pbo::SendOutcome::Accepted);
    PB_CHECK(!e.server->plaintext_credentials_seen());
}

PB_TEST(a_refused_recipient_fails_the_whole_message_with_nothing_accepted) {
    Env e;
    e.server->refuse_recipient("carol@example.org");
    const auto r = e.send(message("rr1"), {"bob@example.org", "carol@example.org"});
    PB_CHECK(r.outcome == pbo::SendOutcome::CertainFailure);
    PB_CHECK_EQ(r.error.key, std::string("refused_recipient"));
    PB_CHECK_EQ(e.server->accepted().size(), size_t(0));
    PB_CHECK(r.error.detail.find("carol") == std::string::npos);        // no address in the error text (FR-041)
}

PB_TEST(a_wrong_password_is_a_certain_failure_with_auth_failed) {
    Env e;
    const auto r = e.send(message("au1"), {"bob@example.org"}, "wrong");
    PB_CHECK(r.outcome == pbo::SendOutcome::CertainFailure);
    PB_CHECK_EQ(r.error.key, std::string("auth_failed"));
    PB_CHECK_EQ(e.server->accepted().size(), size_t(0));
}

PB_TEST(a_connection_dropped_after_the_354_is_a_certain_failure) {
    Env e;
    e.server->drop_after_354 = true;
    const auto r = e.send(message("d354", 300000));          // larger than libcurl's upload buffer: the end of the data was not reached
    PB_CHECK(r.outcome == pbo::SendOutcome::CertainFailure);
    PB_CHECK_EQ(e.server->accepted_count("<d354@example.org>"), 0);
}

PB_TEST(a_connection_dropped_in_the_body_of_a_large_message_is_a_certain_failure_and_never_accepted) {
    Env e;
    e.server->drop_in_body_after = 20000;
    const auto r = e.send(message("dbody", 4u << 20));
    PB_CHECK(r.outcome == pbo::SendOutcome::CertainFailure);
    PB_CHECK_EQ(e.server->accepted_count("<dbody@example.org>"), 0);
}

PB_TEST(a_connection_dropped_in_the_body_of_a_small_message_is_never_claimed_accepted) {
    Env e;
    e.server->drop_in_body_after = 40;
    const auto r = e.send(message("dsmall", 2000));
    // the whole text may have arrived in one piece before the server dropped (then it IS accepted), or not: what must hold is that the answer
    // never claims more than the server did
    const int n = e.server->accepted_count("<dsmall@example.org>");
    PB_CHECK((r.outcome == pbo::SendOutcome::Accepted) == (n == 1));
    PB_CHECK(n <= 1);
}

PB_TEST(a_connection_lost_after_the_final_dot_without_an_answer_is_unknown) {
    Env e;
    e.server->drop_before_final_reply = true;                 // the server HAS the message and counts it, but never says so
    const auto r = e.send(message("dunk"));
    PB_CHECK(r.outcome == pbo::SendOutcome::Unknown);
    PB_CHECK(!r.error.ok());
    PB_CHECK_EQ(e.server->accepted_count("<dunk@example.org>"), 1);
}

PB_TEST(an_answer_that_comes_too_late_is_unknown) {
    Env e;
    e.server->reply_delay_ms = 4000;
    const auto r = e.send(message("late"), {"bob@example.org"}, "pw-for-tests", short_timeouts());
    PB_CHECK(r.outcome != pbo::SendOutcome::Accepted);
    PB_CHECK(r.outcome == pbo::SendOutcome::CertainFailure || r.outcome == pbo::SendOutcome::Unknown);   // depends on which reply was late
    PB_CHECK(!r.error.ok());
}

PB_TEST(a_connection_closed_right_after_the_250_is_still_accepted) {
    Env e;
    e.server->drop_after_reply = true;
    const auto r = e.send(message("after"));
    PB_CHECK(r.outcome == pbo::SendOutcome::Accepted);
    PB_CHECK_EQ(e.server->accepted_count("<after@example.org>"), 1);
}

PB_TEST(a_final_error_reply_means_nothing_was_accepted) {
    Env e;
    e.server->reject_data_code = 554;
    auto r = e.send(message("rej"));
    PB_CHECK(r.outcome == pbo::SendOutcome::CertainFailure);
    PB_CHECK_EQ(r.error.key, std::string("rejected_by_server"));
    e.server->reject_data_code = 451;
    r = e.send(message("busy"));
    PB_CHECK(r.outcome == pbo::SendOutcome::CertainFailure);
    PB_CHECK_EQ(r.error.key, std::string("server_busy"));
    PB_CHECK_EQ(e.server->accepted().size(), size_t(0));
}

PB_TEST(a_message_over_the_servers_limit_is_too_large) {
    Env e(50000);
    const auto r = e.send(message("big", 120000));
    PB_CHECK(r.outcome == pbo::SendOutcome::CertainFailure);
    PB_CHECK_EQ(r.error.key, std::string("too_large"));
    PB_CHECK_EQ(e.server->accepted().size(), size_t(0));
}

PB_TEST(tls_problems_are_certain_failures_and_no_credentials_leave_the_device) {
    {
        Env e(10u << 20, FakeSmtpServer::Mode::ImplicitTls, true);   // a certificate for another name
        const auto r = e.send(message("tls1"));
        PB_CHECK(r.outcome == pbo::SendOutcome::CertainFailure);
        PB_CHECK_EQ(r.error.key, std::string("cert_host_mismatch"));
        PB_CHECK(!e.server->credentials_seen());
    }
    {
        Env e;
        auto transport = pbo::make_curl_smtp_transport(e.other_pki.ca_cert());   // a CA the server's certificate does not chain to
        const auto r = transport->send(e.endpoint, "pw-for-tests", "alice@example.org", {"bob@example.org"}, message("tls2"));
        PB_CHECK(r.outcome == pbo::SendOutcome::CertainFailure);
        PB_CHECK_EQ(r.error.key, std::string("cert_untrusted"));
        PB_CHECK(!e.server->credentials_seen());
    }
}

PB_TEST(an_unreachable_server_is_a_certain_failure) {
    Env e;
    e.endpoint.port = 1;
    const auto r = e.send(message("nr"), {"bob@example.org"}, "pw-for-tests", short_timeouts());
    PB_CHECK(r.outcome == pbo::SendOutcome::CertainFailure);
    PB_CHECK_EQ(r.error.key, std::string("server_unreachable"));
}

PB_TEST(bad_input_is_refused_before_any_connection) {
    Env e;
    PB_CHECK(e.send(message("x"), {}).outcome == pbo::SendOutcome::CertainFailure);
    PB_CHECK(e.send(message("x"), {"bob@example.org\r\nRCPT TO:<evil@example.org>"}).outcome == pbo::SendOutcome::CertainFailure);
    PB_CHECK(e.send("", {"bob@example.org"}).outcome == pbo::SendOutcome::CertainFailure);
    auto t = pbo::make_curl_smtp_transport(e.pki.ca_cert());
    PB_CHECK(t->send(e.endpoint, "pw-for-tests", "bad sender", {"bob@example.org"}, message("x")).outcome == pbo::SendOutcome::CertainFailure);
    PB_CHECK_EQ(e.server->connections(), 0);
}

PB_TEST_MAIN()
