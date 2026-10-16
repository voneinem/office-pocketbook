// Self-test of the in-process fake SMTP server (tests/support/fake_smtp_server.h): it must behave like the SMTP subset the app
// uses before any test relies on it. UNVERIFIED against real servers: this proves the harness, not a provider.
#include <string>

#include "fake_smtp_server.h"
#include "test_support.h"
#include "tls_line_client.h"
#include "tls_test_server.h"
#include "tmp_dir.h"

using pbtest::FakeSmtpServer;

PBO_FAKE_SERVER_TEST();

namespace {

struct Env {
    pbtest::TmpDir dir;
    pbtest::TestPki pki{dir};
    pbtest::TestPki::Leaf leaf;
    Env() {
        if (!pki.available()) throw pbtest::Skip{"the openssl command is not available"};
        leaf = pki.issue("srv", "DNS:localhost,IP:127.0.0.1");
        if (!leaf.ok) throw pbtest::Skip{"cannot create a test certificate"};
    }
    FakeSmtpServer::Config config(FakeSmtpServer::Mode mode = FakeSmtpServer::Mode::ImplicitTls) const {
        FakeSmtpServer::Config c;
        c.cert = leaf.cert;
        c.key = leaf.key;
        c.mode = mode;
        return c;
    }
};

// the base64 of "\0alice\0pw-for-tests"
const char* kPlainGood = "AGFsaWNlAHB3LWZvci10ZXN0cw==";
const char* kPlainBad = "AGFsaWNlAHdyb25n";   // "\0alice\0wrong"

std::string reply(pbtest::TlsLineClient& c) {   // one complete (possibly multi-line) SMTP reply
    std::string all, line;
    while (c.read_line(line)) {
        all += line + "\n";
        if (line.size() >= 4 && line[3] == ' ') break;
    }
    return all;
}
std::string say(pbtest::TlsLineClient& c, const std::string& line) {
    c.send_line(line);
    return reply(c);
}
bool has(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

const char* kMessage = "From: alice@example.org\r\nTo: bob@example.org\r\nSubject: Hi\r\nMessage-ID: <id1@example.org>\r\n\r\nHello\r\n.. dotted\r\n";

}  // namespace

PB_TEST(it_greets_authenticates_and_accepts_a_message_over_implicit_tls) {
    Env env;
    FakeSmtpServer srv(env.config());
    PB_CHECK(srv.running());
    pbtest::TlsLineClient c;
    PB_CHECK(c.connect(srv.port(), env.pki.ca_cert(), true));
    PB_CHECK(has(reply(c), "220 "));
    const std::string ehlo = say(c, "EHLO client.example");
    PB_CHECK(has(ehlo, "250-AUTH PLAIN LOGIN"));
    PB_CHECK(!has(ehlo, "STARTTLS"));
    PB_CHECK(has(say(c, "MAIL FROM:<alice@example.org>"), "530"));   // authentication first
    PB_CHECK(has(say(c, std::string("AUTH PLAIN ") + kPlainBad), "535"));
    PB_CHECK(has(say(c, std::string("AUTH PLAIN ") + kPlainGood), "235"));
    PB_CHECK(has(say(c, "MAIL FROM:<alice@example.org>"), "250"));
    PB_CHECK(has(say(c, "RCPT TO:<bob@example.org>"), "250"));
    PB_CHECK(has(say(c, "DATA"), "354"));
    c.send_line(std::string(kMessage).substr(0, std::string(kMessage).size() - 2));   // without the final line break: send_line adds it
    PB_CHECK(has(say(c, "."), "250"));
    PB_CHECK(has(say(c, "QUIT"), "221"));

    const auto accepted = srv.accepted();
    PB_CHECK_EQ(accepted.size(), 1u);
    PB_CHECK_EQ(accepted[0].envelope_from, std::string("alice@example.org"));
    PB_CHECK_EQ(accepted[0].recipients.size(), 1u);
    PB_CHECK_EQ(accepted[0].recipients[0], std::string("bob@example.org"));
    PB_CHECK(has(accepted[0].data, "\r\n. dotted\r\n"));   // dot-unstuffed
    PB_CHECK_EQ(srv.accepted_count("<id1@example.org>"), 1);
    PB_CHECK(srv.credentials_seen());
    PB_CHECK(!srv.plaintext_credentials_seen());
    for (const std::string& cmd : srv.commands()) PB_CHECK(!has(cmd, kPlainGood) && !has(cmd, "pw-for-tests"));   // never logged
}

PB_TEST(starttls_is_offered_first_and_authentication_only_after_the_upgrade) {
    Env env;
    FakeSmtpServer srv(env.config(FakeSmtpServer::Mode::StartTls));
    pbtest::TlsLineClient c;
    PB_CHECK(c.connect(srv.port(), env.pki.ca_cert(), false));
    PB_CHECK(has(reply(c), "220 "));
    const std::string before = say(c, "EHLO client.example");
    PB_CHECK(has(before, "STARTTLS"));
    PB_CHECK(!has(before, "AUTH"));
    PB_CHECK(has(say(c, "STARTTLS"), "220"));
    PB_CHECK(c.start_tls());
    const std::string after = say(c, "EHLO client.example");
    PB_CHECK(has(after, "AUTH PLAIN LOGIN"));
    PB_CHECK(!has(after, "STARTTLS"));
    c.send_line("AUTH LOGIN");
    PB_CHECK(has(reply(c), "334 VXNlcm5hbWU6"));
    PB_CHECK(has(say(c, "YWxpY2U="), "334"));                    // "alice"
    PB_CHECK(has(say(c, "cHctZm9yLXRlc3Rz"), "235"));            // "pw-for-tests"
    PB_CHECK(!srv.plaintext_credentials_seen());
}

PB_TEST(a_refused_starttls_is_neither_offered_nor_accepted) {
    Env env;
    FakeSmtpServer srv(env.config(FakeSmtpServer::Mode::StartTls));
    srv.refuse_starttls = true;
    pbtest::TlsLineClient c;
    PB_CHECK(c.connect(srv.port(), env.pki.ca_cert(), false));
    reply(c);
    PB_CHECK(!has(say(c, "EHLO client.example"), "STARTTLS"));
    PB_CHECK(has(say(c, "STARTTLS"), "502"));
}

PB_TEST(a_refused_recipient_is_answered_550_and_the_others_are_kept) {
    Env env;
    FakeSmtpServer srv(env.config());
    srv.refuse_recipient("Bad@Example.org");
    pbtest::TlsLineClient c;
    PB_CHECK(c.connect(srv.port(), env.pki.ca_cert(), true));
    reply(c);
    say(c, "EHLO x");
    say(c, std::string("AUTH PLAIN ") + kPlainGood);
    say(c, "MAIL FROM:<alice@example.org>");
    PB_CHECK(has(say(c, "RCPT TO:<bad@example.org>"), "550"));
    PB_CHECK(has(say(c, "RCPT TO:<good@example.org>"), "250"));
    PB_CHECK(has(say(c, "DATA"), "354"));
    c.send_line("Message-ID: <id2@example.org>");
    c.send_line("");
    c.send_line("body");
    PB_CHECK(has(say(c, "."), "250"));
    PB_CHECK_EQ(srv.accepted()[0].recipients.size(), 1u);
    PB_CHECK_EQ(srv.accepted()[0].recipients[0], std::string("good@example.org"));
}

PB_TEST(the_failure_hooks_cut_the_connection_where_they_say) {
    Env env;
    {   // after the 354 reply: nothing is accepted
        FakeSmtpServer srv(env.config());
        srv.drop_after_354 = true;
        pbtest::TlsLineClient c;
        PB_CHECK(c.connect(srv.port(), env.pki.ca_cert(), true));
        reply(c);
        say(c, "EHLO x");
        say(c, std::string("AUTH PLAIN ") + kPlainGood);
        say(c, "MAIL FROM:<a@example.org>");
        say(c, "RCPT TO:<b@example.org>");
        PB_CHECK(has(say(c, "DATA"), "354"));
        c.send_line("Message-ID: <id3@example.org>");
        c.send_line("");
        c.send_line(".");
        PB_CHECK_EQ(srv.accepted().size(), 0u);
    }
    {   // after the final dot, before the reply: the message WAS accepted, the client cannot know
        FakeSmtpServer srv(env.config());
        srv.drop_before_final_reply = true;
        pbtest::TlsLineClient c;
        PB_CHECK(c.connect(srv.port(), env.pki.ca_cert(), true));
        reply(c);
        say(c, "EHLO x");
        say(c, std::string("AUTH PLAIN ") + kPlainGood);
        say(c, "MAIL FROM:<a@example.org>");
        say(c, "RCPT TO:<b@example.org>");
        say(c, "DATA");
        c.send_line("Message-ID: <id4@example.org>");
        c.send_line("");
        c.send_line("body");
        c.send_line(".");
        std::string line;
        PB_CHECK(!c.read_line(line, 1000));   // no reply
        PB_CHECK_EQ(srv.accepted_count("<id4@example.org>"), 1);
    }
    {   // in the middle of the body
        FakeSmtpServer srv(env.config());
        srv.drop_in_body_after = 20;
        pbtest::TlsLineClient c;
        PB_CHECK(c.connect(srv.port(), env.pki.ca_cert(), true));
        reply(c);
        say(c, "EHLO x");
        say(c, std::string("AUTH PLAIN ") + kPlainGood);
        say(c, "MAIL FROM:<a@example.org>");
        say(c, "RCPT TO:<b@example.org>");
        say(c, "DATA");
        c.send_line("Message-ID: <id5@example.org>");
        c.send_line("a long second line that is cut off");
        std::string line;
        PB_CHECK(!c.read_line(line, 1000));
        PB_CHECK_EQ(srv.accepted().size(), 0u);
    }
}

PB_TEST(a_second_acceptance_of_the_same_message_id_shows_as_a_duplicate) {
    Env env;
    FakeSmtpServer srv(env.config());
    for (int i = 0; i < 2; ++i) {
        pbtest::TlsLineClient c;
        PB_CHECK(c.connect(srv.port(), env.pki.ca_cert(), true));
        reply(c);
        say(c, "EHLO x");
        say(c, std::string("AUTH PLAIN ") + kPlainGood);
        say(c, "MAIL FROM:<a@example.org>");
        say(c, "RCPT TO:<b@example.org>");
        say(c, "DATA");
        c.send_line("Message-ID: <dup@example.org>");
        c.send_line("");
        c.send_line("body");
        PB_CHECK(has(say(c, "."), "250"));
    }
    PB_CHECK_EQ(srv.accepted_count("<dup@example.org>"), 2);
}

PB_TEST_MAIN()
