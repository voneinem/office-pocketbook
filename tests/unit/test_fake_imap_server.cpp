// Self-test of the in-process fake IMAP server (tests/support/fake_imap_server.h): it must behave like the IMAP
// subset the app uses (contracts/imap-smtp-subset.md) before any test relies on it. UNVERIFIED against real
// servers: this proves the harness, not a provider.
#include <chrono>
#include <string>

#include "fake_imap_server.h"
#include "fixtures.h"
#include "test_support.h"
#include "tls_line_client.h"
#include "tmp_dir.h"
#include "tls_test_server.h"

using pbtest::FakeImapServer;

PBO_FAKE_SERVER_TEST();

namespace {

const char* kMail1 =
    "From: Anna <anna@example.org>\r\nTo: alice@example.org\r\nSubject: First\r\nDate: Mon, 05 Oct 2026 10:00:00 +0000\r\n"
    "Message-ID: <m1@example.org>\r\n\r\nHello one\r\n";
const char* kMail2 =
    "From: Ben <ben@example.org>\r\nTo: alice@example.org\r\nSubject: Second\r\nDate: Tue, 06 Oct 2026 10:00:00 +0000\r\n"
    "Message-ID: <m2@example.org>\r\n\r\nHello two\r\n";

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
        c.user = "alice";
        c.password = "pw-for-tests";
        return c;
    }
};

bool contains(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

void fill(FakeImapServer& s) {
    s.add_folder("INBOX");
    s.add_folder("Sent", {"\\Sent"});
    s.add_message("INBOX", kMail1, {"\\Seen"});
    s.add_message("INBOX", kMail2, {});
}

}  // namespace

PB_TEST(it_greets_and_serves_the_implicit_tls_subset) {
    Env env;
    FakeImapServer srv(env.config());
    fill(srv);
    PB_CHECK(srv.running());
    pbtest::TlsLineClient c;
    PB_CHECK(c.connect(srv.port(), env.pki.ca_cert(), true));
    std::string line;
    PB_CHECK(c.read_line(line));
    PB_CHECK(line.compare(0, 4, "* OK") == 0);

    const std::string cap = c.command("A1", "CAPABILITY");
    PB_CHECK(contains(cap, "IMAP4rev1"));
    PB_CHECK(!contains(cap, "STARTTLS"));  // already encrypted

    PB_CHECK(contains(c.command("A2", "LOGIN alice pw-for-tests"), "A2 OK"));

    const std::string list = c.command("A3", "LIST \"\" \"*\"");
    PB_CHECK(contains(list, "* LIST () \"/\" \"INBOX\""));
    PB_CHECK(contains(list, "\\Sent"));

    const std::string status = c.command("A4", "STATUS INBOX (UIDNEXT UIDVALIDITY MESSAGES UNSEEN)");
    PB_CHECK(contains(status, "UIDNEXT 3"));
    PB_CHECK(contains(status, "MESSAGES 2"));
    PB_CHECK(contains(status, "UNSEEN 1"));

    const std::string sel = c.command("A5", "SELECT INBOX");
    PB_CHECK(contains(sel, "* 2 EXISTS"));
    PB_CHECK(contains(sel, "[UIDVALIDITY"));
    PB_CHECK(contains(sel, "A5 OK"));

    PB_CHECK(contains(c.command("A6", "UID SEARCH ALL"), "* SEARCH 1 2"));
    PB_CHECK(contains(c.command("A7", "UID SEARCH HEADER Message-ID <m2@example.org>"), "* SEARCH 2"));

    const std::string flags = c.command("A8", "UID FETCH 1:* (FLAGS)");
    PB_CHECK(contains(flags, "* 1 FETCH (UID 1 FLAGS (\\Seen))"));
    PB_CHECK(contains(flags, "* 2 FETCH (UID 2 FLAGS ())"));

    PB_CHECK(contains(c.command("A9", "LOGOUT"), "A9 OK"));
}

PB_TEST(it_returns_header_fields_and_whole_messages_as_literals) {
    Env env;
    FakeImapServer srv(env.config());
    fill(srv);
    pbtest::TlsLineClient c;
    PB_CHECK(c.connect(srv.port(), env.pki.ca_cert(), true));
    std::string line;
    c.read_line(line);
    c.command("A1", "LOGIN alice pw-for-tests");
    c.command("A2", "SELECT INBOX");

    c.send_line("A3 UID FETCH 1 (UID BODY.PEEK[HEADER.FIELDS (SUBJECT FROM)])");
    PB_CHECK(c.read_line(line));
    PB_CHECK(contains(line, "* 1 FETCH (UID 1 BODY[HEADER.FIELDS (SUBJECT FROM)] {"));
    const size_t open = line.rfind('{');
    const size_t n = static_cast<size_t>(std::atoi(line.c_str() + open + 1));
    std::string lit;
    PB_CHECK(c.read_bytes(n, lit));
    PB_CHECK(contains(lit, "Subject: First"));
    PB_CHECK(contains(lit, "From: Anna"));
    PB_CHECK(!contains(lit, "Message-ID"));
    PB_CHECK(c.read_line(line));  // ")"
    PB_CHECK_EQ(line, std::string(")"));
    PB_CHECK(c.read_line(line));
    PB_CHECK(contains(line, "A3 OK"));

    c.send_line("A4 UID FETCH 2 (BODY.PEEK[])");
    PB_CHECK(c.read_line(line));
    PB_CHECK(contains(line, "BODY[] {" + std::to_string(std::string(kMail2).size()) + "}"));
    PB_CHECK(c.read_bytes(std::string(kMail2).size(), lit));
    PB_CHECK_EQ(lit, std::string(kMail2));
}

PB_TEST(it_describes_the_mime_structure_and_serves_numbered_sections) {
    Env env;
    FakeImapServer srv(env.config());
    srv.add_folder("INBOX");
    srv.add_message("INBOX", pbtest::fixture("mail/with_attachments.eml"));
    srv.add_message("INBOX", pbtest::fixture("mail/plain_text.eml"));
    pbtest::TlsLineClient c;
    PB_CHECK(c.connect(srv.port(), env.pki.ca_cert(), true));
    std::string line;
    c.read_line(line);
    c.command("A1", "LOGIN alice pw-for-tests");
    c.command("A2", "SELECT INBOX");

    const std::string bs = c.command("A3", "UID FETCH 1 (BODYSTRUCTURE)");
    PB_CHECK(contains(bs, "BODYSTRUCTURE ((\"TEXT\" \"PLAIN\" (\"CHARSET\" \"utf-8\")"));
    PB_CHECK(contains(bs, "(\"APPLICATION\" \"PDF\" (\"NAME\" \"report.pdf\")"));
    PB_CHECK(contains(bs, "(\"ATTACHMENT\" (\"FILENAME\" \"report.pdf\"))"));
    PB_CHECK(contains(bs, "\"MIXED\" (\"BOUNDARY\" \"BOUNDARY-MIX\")"));
    PB_CHECK(contains(bs, "FILENAME*0*"));                      // extended parameters are passed on as written

    const std::string one = c.command("A4", "UID FETCH 1 (BODY.PEEK[1])");
    PB_CHECK(contains(one, "Please find the documents attached."));
    PB_CHECK(!contains(one, "JVBERi"));
    const std::string two = c.command("A5", "UID FETCH 1 (BODY.PEEK[2])");
    PB_CHECK(contains(two, "JVBERi0xLjQ"));                      // base64 of "%PDF-1.4": the transfer encoding is kept
    const std::string part = c.command("A6", "UID FETCH 1 (BODY.PEEK[1]<0.6>)");
    PB_CHECK(contains(part, "BODY[1]<0> {6}"));
    PB_CHECK(contains(part, "Please"));

    // a fetch without PEEK marks the message as read, like a real server; with PEEK it does not
    PB_CHECK(srv.flags_of("INBOX", 1).count("\\Seen") == 0);
    c.command("A6b", "UID FETCH 1 (BODY[HEADER])");
    PB_CHECK(srv.flags_of("INBOX", 1).count("\\Seen") == 0);       // the header alone does not count
    c.command("A6c", "UID FETCH 1 (BODY[1])");
    PB_CHECK(srv.flags_of("INBOX", 1).count("\\Seen") == 1);

    const std::string single = c.command("A7", "UID FETCH 2 (BODYSTRUCTURE BODY.PEEK[1])");
    PB_CHECK(contains(single, "BODYSTRUCTURE (\"TEXT\" \"PLAIN\" (\"CHARSET\" \"us-ascii\") NIL NIL \"7BIT\""));
    PB_CHECK(contains(single, "this is a plain message."));
}

PB_TEST(it_rejects_a_wrong_password_and_never_logs_the_password) {
    Env env;
    FakeImapServer srv(env.config());
    fill(srv);
    pbtest::TlsLineClient c;
    PB_CHECK(c.connect(srv.port(), env.pki.ca_cert(), true));
    std::string line;
    c.read_line(line);
    PB_CHECK(contains(c.command("A1", "LOGIN alice nope"), "A1 NO"));
    PB_CHECK(srv.credentials_seen());
    for (const std::string& cmd : srv.commands()) PB_CHECK(!contains(cmd, "pw-for-tests") && !contains(cmd, "nope"));
}

PB_TEST(starttls_mode_upgrades_before_it_accepts_a_login) {
    Env env;
    FakeImapServer srv(env.config(FakeImapServer::Mode::StartTls));
    fill(srv);
    pbtest::TlsLineClient c;
    PB_CHECK(c.connect(srv.port(), env.pki.ca_cert(), false));
    std::string line;
    PB_CHECK(c.read_line(line));
    const std::string cap = c.command("A1", "CAPABILITY");
    PB_CHECK(contains(cap, "STARTTLS"));
    PB_CHECK(contains(cap, "LOGINDISABLED"));
    PB_CHECK(contains(c.command("A2", "LOGIN alice pw-for-tests"), "A2 NO"));  // refused in plaintext
    PB_CHECK(srv.plaintext_credentials_seen());
    PB_CHECK(contains(c.command("A3", "STARTTLS"), "A3 OK"));
    PB_CHECK(c.start_tls());
    PB_CHECK(!contains(c.command("A4", "CAPABILITY"), "STARTTLS"));
    PB_CHECK(contains(c.command("A5", "LOGIN alice pw-for-tests"), "A5 OK"));
}

PB_TEST(it_can_refuse_starttls_and_hide_the_capability) {
    Env env;
    auto cfg = env.config(FakeImapServer::Mode::StartTls);
    FakeImapServer srv(cfg);
    srv.refuse_starttls = true;
    pbtest::TlsLineClient c;
    PB_CHECK(c.connect(srv.port(), env.pki.ca_cert(), false));
    std::string line;
    c.read_line(line);
    PB_CHECK(!contains(c.command("A1", "CAPABILITY"), "STARTTLS"));
    PB_CHECK(contains(c.command("A2", "STARTTLS"), "A2 NO"));
}

PB_TEST(it_can_delay_a_reply) {
    Env env;
    FakeImapServer srv(env.config());
    srv.delay_reply_ms = 300;
    pbtest::TlsLineClient c;
    PB_CHECK(c.connect(srv.port(), env.pki.ca_cert(), true));
    std::string line;
    c.read_line(line);
    const auto t0 = std::chrono::steady_clock::now();
    c.command("A1", "CAPABILITY");
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    PB_CHECK(ms >= 250);
}

PB_TEST(it_can_drop_the_connection_after_n_commands) {
    Env env;
    FakeImapServer srv(env.config());
    fill(srv);
    srv.drop_after_commands = 2;
    pbtest::TlsLineClient c;
    PB_CHECK(c.connect(srv.port(), env.pki.ca_cert(), true));
    std::string line;
    c.read_line(line);
    PB_CHECK(contains(c.command("A1", "CAPABILITY"), "A1 OK"));
    PB_CHECK(contains(c.command("A2", "LOGIN alice pw-for-tests"), "A2 OK"));
    c.send_line("A3 LIST \"\" \"*\"");  // the third command is received but never answered
    PB_CHECK(!c.read_line(line, 1000));
    PB_CHECK(c.closed_by_peer());
}

PB_TEST(it_can_drop_the_connection_after_n_bytes) {
    Env env;
    FakeImapServer srv(env.config());
    fill(srv);
    srv.drop_after_bytes = 60;  // the greeting and a part of the next answer
    pbtest::TlsLineClient c;
    PB_CHECK(c.connect(srv.port(), env.pki.ca_cert(), true));
    std::string line, all;
    while (c.read_line(line, 500)) all += line + "\n";
    c.send_line("A1 CAPABILITY");
    PB_CHECK(!c.read_line(line, 500) || !contains(line, "A1 OK"));
}

PB_TEST(append_stores_a_message_and_stamps_it_with_the_server_clock) {
    Env env;
    FakeImapServer srv(env.config());
    fill(srv);
    pbtest::TlsLineClient c;
    PB_CHECK(c.connect(srv.port(), env.pki.ca_cert(), true));
    std::string line;
    c.read_line(line);
    c.command("A1", "LOGIN alice pw-for-tests");
    const std::string body = "Subject: Sent copy\r\nMessage-ID: <s1@example.org>\r\n\r\nbody\r\n";
    c.send_line("A2 APPEND Sent (\\Seen) {" + std::to_string(body.size()) + "}");
    PB_CHECK(c.read_line(line));
    PB_CHECK_EQ(line.substr(0, 1), std::string("+"));
    c.write_all(body + "\r\n");
    std::string all;
    while (c.read_line(line)) {
        all += line + "\n";
        if (line.compare(0, 3, "A2 ") == 0) break;
    }
    PB_CHECK(contains(all, "A2 OK"));
    PB_CHECK_EQ(srv.message_count("Sent"), 1);
    PB_CHECK(srv.internal_date("Sent", 1) > 1700000000);  // stamped by the server clock, not by the client
    // no date argument was sent, so the server clock decides; an offset makes the clock testable
    for (const std::string& cmd : srv.commands()) PB_CHECK(!contains(cmd, "APPEND Sent (\\Seen) \""));
}

PB_TEST(uid_store_changes_flags_and_the_command_log_records_every_command) {
    Env env;
    FakeImapServer srv(env.config());
    fill(srv);
    pbtest::TlsLineClient c;
    PB_CHECK(c.connect(srv.port(), env.pki.ca_cert(), true));
    std::string line;
    c.read_line(line);
    c.command("A1", "LOGIN alice pw-for-tests");
    c.command("A2", "SELECT INBOX");
    const std::string r = c.command("A3", "UID STORE 2 +FLAGS.SILENT (\\Flagged)");
    PB_CHECK(contains(r, "A3 OK"));
    PB_CHECK(contains(c.command("A4", "UID FETCH 2 (FLAGS)"), "\\Flagged"));
    const auto cmds = srv.commands();
    PB_CHECK(cmds.size() >= 4);
    bool saw_store = false;
    for (const auto& cmd : cmds) saw_store = saw_store || contains(cmd, "UID STORE 2 +FLAGS.SILENT");
    PB_CHECK(saw_store);
}

PB_TEST_MAIN()
