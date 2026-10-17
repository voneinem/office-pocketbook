// T036: the real CurlImapTransport (libcurl) against the in-process fake IMAP server over TLS
// (contracts/core-interfaces.md 2.1, contracts/imap-smtp-subset.md section 1). UNVERIFIED against real servers.
#include <algorithm>
#include <string>

#include "fake_imap_server.h"
#include "fixtures.h"
#include "pbo/imap_transport.h"
#include "test_support.h"
#include "tls_test_server.h"
#include "tmp_dir.h"

using pbtest::FakeImapServer;

PBO_FAKE_SERVER_TEST();

namespace {

const char* kMail1 =
    "From: Anna <anna@example.org>\r\nTo: alice@example.org\r\nSubject: First\r\nDate: Mon, 05 Oct 2026 10:00:00 +0000\r\n"
    "Message-ID: <m1@example.org>\r\n\r\nHello one\r\n";
const char* kMail2 =
    "From: Ben <ben@example.org>\r\nTo: alice@example.org\r\nSubject: Second\r\nDate: Tue, 06 Oct 2026 10:00:00 +0000\r\n"
    "Message-ID: <m2@example.org>\r\n\r\nHello two, a little longer so a small limit stops it\r\n";

struct Env {
    pbtest::TmpDir dir;
    pbtest::TestPki pki{dir};
    pbtest::TestPki::Leaf leaf;
    Env() {
        if (!pki.available()) throw pbtest::Skip{"the openssl command is not available"};
        leaf = pki.issue("srv", "DNS:localhost,IP:127.0.0.1");
        if (!leaf.ok) throw pbtest::Skip{"cannot create a test certificate"};
    }
    FakeImapServer::Config config(FakeImapServer::Mode mode = FakeImapServer::Mode::ImplicitTls) const {
        FakeImapServer::Config c;
        c.cert = leaf.cert;
        c.key = leaf.key;
        c.mode = mode;
        return c;
    }
    pbo::ImapEndpoint endpoint(const FakeImapServer& s, pbo::Security sec = pbo::Security::Tls) const {
        pbo::ImapEndpoint e;
        e.host = "localhost";
        e.port = s.port();
        e.security = sec;
        e.username = "alice";
        return e;
    }
};

void fill(FakeImapServer& s) {
    s.set_capabilities("IMAP4rev1 UIDPLUS LITERAL+ CONDSTORE");
    s.add_folder("INBOX");
    s.add_folder("Sent", {"\\Sent"});
    s.add_message("INBOX", kMail1, {"\\Seen"});
    s.add_message("INBOX", kMail2, {});
}

bool contains(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

struct StringSink : pbo::ByteSink {
    std::string data;
    bool write(const char* p, size_t n) override {
        data.append(p, n);
        return true;
    }
};

}  // namespace

PB_TEST(connect_logs_in_and_caches_the_capabilities) {
    Env env;
    FakeImapServer srv(env.config());
    fill(srv);
    auto t = pbo::make_curl_imap_transport(env.pki.ca_cert());
    PB_CHECK(t->connect(env.endpoint(srv), "pw-for-tests").ok());
    PB_CHECK(t->capabilities().count("imap4rev1") == 1);
    PB_CHECK(t->capabilities().count("uidplus") == 1);
    PB_CHECK(t->capabilities().count("condstore") == 1);
    PB_CHECK(t->capabilities().count("IMAP4rev1") == 0);   // lowercase tokens
    t->disconnect();
}

PB_TEST(a_wrong_password_is_auth_failed_and_nothing_follows_the_login) {
    Env env;
    FakeImapServer srv(env.config());
    fill(srv);
    auto t = pbo::make_curl_imap_transport(env.pki.ca_cert());
    const pbo::Error e = t->connect(env.endpoint(srv), "wrong-password");
    PB_CHECK(!e.ok());
    PB_CHECK(e.cls == pbo::ErrorClass::Auth);
    PB_CHECK_EQ(e.key, std::string("auth_failed"));
    PB_CHECK(e.detail.find("wrong-password") == std::string::npos);
    const auto cmds = srv.commands();
    PB_CHECK(!cmds.empty());
    size_t login_at = cmds.size();
    for (size_t i = 0; i < cmds.size(); ++i)
        if (contains(cmds[i], "LOGIN") || contains(cmds[i], "AUTHENTICATE")) login_at = i;   // libcurl uses AUTHENTICATE PLAIN when the server offers it
    PB_CHECK(login_at < cmds.size());
    for (size_t i = login_at + 1; i < cmds.size(); ++i)   // only LOGOUT may follow
        if (!contains(cmds[i], "LOGOUT")) PB_FAIL_AT("command after the failed LOGIN: " + cmds[i]);
    for (const std::string& c : cmds) PB_CHECK(!contains(c, "wrong-password"));
}

PB_TEST(commands_return_the_raw_untagged_data) {
    Env env;
    FakeImapServer srv(env.config());
    fill(srv);
    auto t = pbo::make_curl_imap_transport(env.pki.ca_cert());
    PB_CHECK(t->connect(env.endpoint(srv), "pw-for-tests").ok());

    auto list = t->command("", "LIST \"\" \"*\"");
    PB_CHECK(list.ok());
    PB_CHECK(contains(list.value.data, "* LIST () \"/\" \"INBOX\""));
    PB_CHECK(contains(list.value.data, "\\Sent"));

    auto status = t->command("", "STATUS \"INBOX\" (UIDNEXT UIDVALIDITY MESSAGES)");
    PB_CHECK(status.ok());
    PB_CHECK(contains(status.value.data, "* STATUS \"INBOX\" (UIDNEXT 3 UIDVALIDITY 1 MESSAGES 2)"));

    auto search = t->command("INBOX", "UID SEARCH ALL");
    PB_CHECK(search.ok());
    PB_CHECK(contains(search.value.data, "* SEARCH 1 2"));

    // A response with a literal is requested for ONE message per command: libcurl does not deliver a response with
    // several literals completely and the rest leaks into the next command (observed with 8.18.0, research R-01).
    for (int uid = 1; uid <= 2; ++uid) {
        auto fetch = t->command("INBOX", "UID FETCH " + std::to_string(uid) + " (UID FLAGS BODY.PEEK[HEADER.FIELDS (SUBJECT)])");
        PB_CHECK(fetch.ok());
        PB_CHECK(contains(fetch.value.data, "* " + std::to_string(uid) + " FETCH (UID " + std::to_string(uid)));
        PB_CHECK(contains(fetch.value.data, uid == 1 ? "Subject: First" : "Subject: Second"));
        PB_CHECK(!contains(fetch.value.data, uid == 1 ? "Subject: Second" : "Subject: First"));
    }
    auto flags = t->command("INBOX", "UID FETCH 1:2 (UID FLAGS)");   // no literal: several messages per command are fine
    PB_CHECK(flags.ok());
    PB_CHECK(contains(flags.value.data, "UID 1 FLAGS (\\Seen)"));
    PB_CHECK(contains(flags.value.data, "UID 2 FLAGS ()"));

    // switching the mailbox and going back works on the same session
    auto again = t->command("Sent", "UID SEARCH ALL");
    PB_CHECK(again.ok());
    auto inbox_again = t->command("INBOX", "UID SEARCH ALL");
    PB_CHECK(inbox_again.ok());
    PB_CHECK_EQ(srv.connections(), 1);   // one connection for the whole session
}

PB_TEST(a_command_on_a_missing_mailbox_is_an_error_not_empty_data) {
    Env env;
    FakeImapServer srv(env.config());
    fill(srv);
    auto t = pbo::make_curl_imap_transport(env.pki.ca_cert());
    PB_CHECK(t->connect(env.endpoint(srv), "pw-for-tests").ok());
    auto r = t->command("Nope", "UID SEARCH ALL");
    PB_CHECK(!r.ok());
}

PB_TEST(fetch_to_streams_a_whole_message_and_stops_at_the_limit) {
    Env env;
    FakeImapServer srv(env.config());
    fill(srv);
    auto t = pbo::make_curl_imap_transport(env.pki.ca_cert());
    PB_CHECK(t->connect(env.endpoint(srv), "pw-for-tests").ok());

    StringSink whole;
    PB_CHECK(t->fetch_to("INBOX", 1, "", 1u << 20, whole).ok());
    PB_CHECK_EQ(whole.data, std::string(kMail1));

    StringSink small;
    const uint64_t limit = 40;
    const pbo::Error e = t->fetch_to("INBOX", 2, "", limit, small);
    PB_CHECK(!e.ok());
    PB_CHECK_EQ(e.key, std::string("too_large"));
    PB_CHECK(small.data.size() <= limit);   // no byte beyond the limit was accepted
}

PB_TEST(fetch_to_reads_a_part_without_marking_the_message_as_read) {
    Env env;
    FakeImapServer srv(env.config());
    srv.add_folder("INBOX");
    srv.add_message("INBOX", pbtest::fixture("mail/with_attachments.eml"));
    auto t = pbo::make_curl_imap_transport(env.pki.ca_cert());
    PB_CHECK(t->connect(env.endpoint(srv), "pw-for-tests").ok());
    StringSink pdf;
    PB_CHECK(t->fetch_to("INBOX", 1, "2", 1u << 20, pdf).ok());
    PB_CHECK(contains(pdf.data, "JVBERi0xLjQ"));                       // the part in its transfer encoding (base64)
    PB_CHECK(!contains(pdf.data, "Content-Type"));                     // only the part, not the whole message
    StringSink text;
    PB_CHECK(t->fetch_to("INBOX", 1, "1", 1u << 20, text).ok());
    PB_CHECK_EQ(text.data, std::string("Please find the documents attached."));
    PB_CHECK(srv.flags_of("INBOX", 1).count("\\Seen") == 0);           // BODY.PEEK: reading is not marking
    for (const std::string& c : srv.commands())
        if (contains(c, "BODY[") && !contains(c, "BODY[HEADER")) PB_FAIL_AT("a fetch without PEEK: " + c);
}

PB_TEST(fetch_to_refuses_a_part_larger_than_the_limit_before_any_byte_is_taken) {
    Env env;
    FakeImapServer srv(env.config());
    fill(srv);
    auto t = pbo::make_curl_imap_transport(env.pki.ca_cert());
    PB_CHECK(t->connect(env.endpoint(srv), "pw-for-tests").ok());
    StringSink s;
    const pbo::Error e = t->fetch_to("INBOX", 2, "", 10, s);
    PB_CHECK_EQ(e.key, std::string("too_large"));
    PB_CHECK_EQ(s.data.size(), static_cast<size_t>(0));
    StringSink after;                                                  // the session is still usable
    PB_CHECK(t->fetch_to("INBOX", 1, "", 1u << 20, after).ok());
    PB_CHECK_EQ(after.data, std::string(kMail1));
}

PB_TEST(fetch_to_on_a_missing_section_is_an_error_not_an_empty_file) {
    Env env;
    FakeImapServer srv(env.config());
    fill(srv);
    auto t = pbo::make_curl_imap_transport(env.pki.ca_cert());
    PB_CHECK(t->connect(env.endpoint(srv), "pw-for-tests").ok());
    StringSink s;
    PB_CHECK(!t->fetch_to("INBOX", 1, "7", 1u << 20, s).ok());
    PB_CHECK(!t->fetch_to("INBOX", 99, "", 1u << 20, s).ok());
}

PB_TEST(a_delayed_reply_is_a_timeout) {
    Env env;
    FakeImapServer srv(env.config());
    fill(srv);
    pbo::TransportTimeouts timeouts;
    timeouts.command_s = 1;
    auto t = pbo::make_curl_imap_transport(env.pki.ca_cert(), timeouts);
    PB_CHECK(t->connect(env.endpoint(srv), "pw-for-tests").ok());
    srv.delay_reply_ms = 3000;
    auto r = t->command("INBOX", "UID SEARCH ALL");
    PB_CHECK(!r.ok());
    PB_CHECK_EQ(r.error.key, std::string("timeout"));
    PB_CHECK(r.error.cls == pbo::ErrorClass::Network);
}

PB_TEST(a_dropped_connection_is_a_network_error_and_the_next_connect_works) {
    Env env;
    FakeImapServer srv(env.config());
    fill(srv);
    auto t = pbo::make_curl_imap_transport(env.pki.ca_cert());
    PB_CHECK(t->connect(env.endpoint(srv), "pw-for-tests").ok());
    srv.drop_after_commands = 2;   // already used: CAPABILITY and LOGIN of the session start
    auto first = t->command("INBOX", "UID SEARCH ALL");
    auto second = t->command("INBOX", "UID SEARCH ALL");
    PB_CHECK(!first.ok() || !second.ok());
    const pbo::Error& e = !first.ok() ? first.error : second.error;
    PB_CHECK(e.cls == pbo::ErrorClass::Network);
    srv.drop_after_commands = -1;
    PB_CHECK(t->connect(env.endpoint(srv), "pw-for-tests").ok());
    auto ok = t->command("INBOX", "UID SEARCH ALL");
    PB_CHECK(ok.ok());
    PB_CHECK(contains(ok.value.data, "* SEARCH 1 2"));
}

PB_TEST(append_uploads_a_message_without_a_date) {
    Env env;
    FakeImapServer srv(env.config());
    fill(srv);
    auto t = pbo::make_curl_imap_transport(env.pki.ca_cert());
    PB_CHECK(t->connect(env.endpoint(srv), "pw-for-tests").ok());
    PB_CHECK(t->append("Sent", "From: alice@example.org\r\nSubject: Out\r\n\r\nBody\r\n").ok());
    PB_CHECK_EQ(srv.message_count("Sent"), 1);
    PB_CHECK(srv.flags_of("Sent", 1).empty());   // libcurl sends no flags: the caller sets \Seen with UID STORE afterwards
    bool saw_append = false;
    for (const std::string& c : srv.commands())
        if (contains(c, "APPEND")) {
            saw_append = true;
            PB_CHECK(!contains(c, "-Oct-") && !contains(c, "-Jan-") && !contains(c, " \"2"));   // no date argument
        }
    PB_CHECK(saw_append);
}

PB_TEST(an_endpoint_with_a_bad_host_is_refused_before_any_connection) {
    Env env;
    auto t = pbo::make_curl_imap_transport(env.pki.ca_cert());
    pbo::ImapEndpoint e;
    e.host = "mail.example.org/INBOX";
    e.username = "alice";
    const pbo::Error err = t->connect(e, "x");
    PB_CHECK(!err.ok());
    PB_CHECK_EQ(err.key, std::string("host_invalid"));
}

PB_TEST_MAIN()
