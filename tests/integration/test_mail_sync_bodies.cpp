// The sync fetches the readable part of every message in the window (research R-06) with BODY.PEEK, lists the
// attachments from BODYSTRUCTURE without downloading them, falls back when the structure is unusable, resumes after a
// dropped connection, and gives the same result as parsing the whole message. UNVERIFIED against real servers (fake server).
#include <algorithm>
#include <set>
#include <string>
#include <vector>

#include "fake_imap_server.h"
#include "fixtures.h"
#include "pbo/imap_transport.h"
#include "pbo/mime.h"
#include "pbo/store.h"
#include "pbo/sync.h"
#include "test_support.h"
#include "tls_test_server.h"
#include "tmp_dir.h"

using pbtest::FakeImapServer;

PBO_FAKE_SERVER_TEST();

namespace {

constexpr int64_t kNow = 1791979200;   // 2026-10-14 12:00:00 UTC

struct FixedClock : pbo::Clock {
    int64_t now() const override { return kNow; }
};
struct NeverCancel : pbo::Cancel {
    bool requested() const override { return false; }
};

const char* kFixtures[] = {"plain_text", "html_only", "multipart_alternative", "with_attachments", "qp_latin1", "base64_utf8", "encoded_subject_2047",
                           "param_2231", "charset_windows1250", "charset_unknown", "nested_multipart", "message_rfc822_attachment", "empty_body",
                           "no_text_parts", "quoted_reply"};

struct Env {
    pbtest::TmpDir dir, pki_dir;
    pbtest::TestPki pki{pki_dir};
    pbtest::TestPki::Leaf leaf;
    FixedClock clock;
    NeverCancel cancel;
    std::unique_ptr<FakeImapServer> server;
    std::unique_ptr<pbo::Store> store;
    Env() {
        if (!pki.available()) throw pbtest::Skip{"the openssl command is not available"};
        leaf = pki.issue("srv", "DNS:localhost,IP:127.0.0.1");
        if (!leaf.ok) throw pbtest::Skip{"cannot create a test certificate"};
        FakeImapServer::Config c;
        c.cert = leaf.cert;
        c.key = leaf.key;
        server = std::make_unique<FakeImapServer>(c);
        server->set_capabilities("IMAP4rev1 UIDPLUS LITERAL+");
        server->add_folder("INBOX");
        store = std::move(pbo::Store::open(dir.path(), clock).value);
        pbo::MailAccount a;
        a.imap_host = "localhost";
        a.imap_port = server->port();
        a.smtp_host = "localhost";
        a.username = "alice";
        a.address = "alice@example.org";
        store->save_mail_account(a);
    }
    pbo::SyncReport sync() {
        auto transport = pbo::make_curl_imap_transport(pki.ca_cert());
        pbo::SyncDeps deps{*store, *transport, clock, cancel};
        pbo::Credentials creds;
        creds.mail_password = "pw-for-tests";
        return pbo::run_sync(deps, creds);
    }
    uint32_t add(const std::string& raw, int64_t age_s = 3600) { return server->add_message("INBOX", raw, {}, kNow - age_s); }
    int64_t id_of(uint32_t uid) const { return std::stoll(store->debug_query("SELECT id FROM message WHERE uid = " + std::to_string(uid))); }
};

}  // namespace

// The body that comes from BODYSTRUCTURE plus the fetched part equals the body parsed from the whole message.
PB_TEST(every_fixture_mail_reads_the_same_via_the_structure_as_by_parsing_the_whole_message) {
    Env env;
    std::vector<std::string> raws;
    for (const char* name : kFixtures) {
        raws.push_back(pbtest::fixture(std::string("mail/") + name + ".eml"));
        env.add(raws.back());
    }
    const auto report = env.sync();
    PB_CHECK(report.first_problem.ok());
    for (size_t i = 0; i < raws.size(); ++i) {
        const std::string label = kFixtures[i];
        const auto want = pbo::parse_message(raws[i]);
        const auto got = env.store->message(env.id_of(static_cast<uint32_t>(i + 1)));
        if (!got) PB_FAIL_AT(label + ": no message");
        if (got->body_text != want.text) PB_FAIL_AT(label + ": text differs: [" + got->body_text + "] vs [" + want.text + "]");
        const std::string want_note = want.body_kind == pbo::ParsedMessage::Body::NoReadableText ? "no_readable_text" : want.note;
        if (got->body_note != want_note) PB_FAIL_AT(label + ": note differs: " + got->body_note + " vs " + want_note);
        if (got->links != want.links) PB_FAIL_AT(label + ": links differ");
        if (got->header.body_state != "text") PB_FAIL_AT(label + ": body_state " + got->header.body_state);
        if (got->attachments.size() != want.attachments.size()) PB_FAIL_AT(label + ": attachment count differs");
        for (size_t k = 0; k < want.attachments.size(); ++k) {
            const auto& a = got->attachments[k];
            const auto& w = want.attachments[k];
            if (a.section != w.section || a.name != w.name || a.mime_type != w.mime_type || a.encoding != w.encoding) PB_FAIL_AT(label + ": attachment " + std::to_string(k) + " differs");
            const int64_t diff = a.size > static_cast<int64_t>(w.size) ? a.size - static_cast<int64_t>(w.size) : static_cast<int64_t>(w.size) - a.size;
            if (diff > static_cast<int64_t>(w.size) / 10 + 4) PB_FAIL_AT(label + ": attachment size differs too much");   // the structure only knows the transfer size
        }
    }
    PB_CHECK(env.store->messages_missing_body(env.store->folders().at(0).id, 100).empty());
}

PB_TEST(reading_never_marks_a_message_as_read_on_the_server_or_locally) {
    Env env;
    for (const char* name : kFixtures) env.add(pbtest::fixture(std::string("mail/") + name + ".eml"));
    PB_CHECK(env.sync().first_problem.ok());
    for (uint32_t uid = 1; uid <= std::size(kFixtures); ++uid) PB_CHECK(env.server->flags_of("INBOX", uid).count("\\Seen") == 0);
    PB_CHECK_EQ(env.store->debug_query("SELECT count(*) FROM message WHERE flags_local & 1"), std::string("0"));
    for (const std::string& cmd : env.server->commands())
        if (cmd.find("BODY[") != std::string::npos && cmd.find("BODY[HEADER") == std::string::npos) PB_FAIL_AT("a fetch without PEEK: " + cmd);
}

PB_TEST(attachments_are_listed_but_never_downloaded_by_the_sync) {
    Env env;
    std::string raw = pbtest::fixture("mail/with_attachments.eml");
    // a 3 MB attachment: it must not travel
    std::string big = "From: a@example.org\r\nSubject: Big\r\nContent-Type: multipart/mixed; boundary=\"BG\"\r\n\r\n--BG\r\nContent-Type: text/plain\r\n\r\nSee attachment.\r\n"
                      "--BG\r\nContent-Type: application/octet-stream; name=\"big.bin\"\r\nContent-Disposition: attachment; filename=\"big.bin\"\r\nContent-Transfer-Encoding: base64\r\n\r\n";
    for (size_t i = 0; i < 40000; ++i) big += std::string(76, 'A') + "\r\n";
    big += "--BG--\r\n";
    env.add(raw);
    const uint32_t big_uid = env.add(big);
    PB_CHECK(env.sync().first_problem.ok());
    const auto v = env.store->message(env.id_of(big_uid));
    PB_CHECK_EQ(v->body_text, std::string("See attachment."));
    PB_CHECK_EQ(v->attachments.size(), static_cast<size_t>(1));
    PB_CHECK_EQ(v->attachments[0].name, std::string("big.bin"));
    PB_CHECK(v->attachments[0].size > 2000000);
    PB_CHECK_EQ(v->attachments[0].state, std::string("remote"));
    for (const std::string& cmd : env.server->commands()) {
        if (cmd.find("BODY.PEEK[2") != std::string::npos || cmd.find("BODY.PEEK[]") != std::string::npos) PB_FAIL_AT("an attachment or the whole message was fetched: " + cmd);
        if (cmd.find("BODY.PEEK[1]") != std::string::npos && cmd.find("<0.2097152>") == std::string::npos) PB_FAIL_AT("a text part without the size limit: " + cmd);
    }
}

PB_TEST(a_garbled_structure_falls_back_to_the_start_of_the_whole_message) {
    Env env;
    env.add(pbtest::fixture("mail/with_attachments.eml"));
    env.server->corrupt_bodystructure = true;
    PB_CHECK(env.sync().first_problem.ok());
    const auto v = env.store->message(env.id_of(1));
    PB_CHECK_EQ(v->body_text, std::string("Please find the documents attached."));
    PB_CHECK_EQ(v->attachments.size(), static_cast<size_t>(2));
    PB_CHECK_EQ(v->attachments[0].name, std::string("report.pdf"));
    bool whole = false;
    for (const std::string& cmd : env.server->commands())
        if (cmd.find("BODY.PEEK[]<0.262144>") != std::string::npos) whole = true;
    PB_CHECK(whole);
}

PB_TEST(a_dropped_connection_is_resumed_without_duplicates_or_gaps) {
    Env env;
    for (const char* name : kFixtures) env.add(pbtest::fixture(std::string("mail/") + name + ".eml"));
    env.server->drop_after_commands = 14;                  // the first run dies somewhere in the middle
    const auto first = env.sync();
    PB_CHECK(!first.first_problem.ok());
    env.server->drop_after_commands = -1;
    const auto second = env.sync();
    PB_CHECK(second.first_problem.ok());
    PB_CHECK_EQ(env.store->debug_query("SELECT count(*) FROM message"), std::to_string(std::size(kFixtures)));
    PB_CHECK_EQ(env.store->debug_query("SELECT count(DISTINCT uid) FROM message"), std::to_string(std::size(kFixtures)));
    PB_CHECK_EQ(env.store->debug_query("SELECT count(*) FROM message WHERE body_state = 'headers'"), std::string("0"));
    PB_CHECK_EQ(env.store->debug_query("SELECT count(*) FROM (SELECT message_id, section FROM attachment GROUP BY message_id, section HAVING count(*) > 1)"), std::string("0"));
}

PB_TEST(messages_stored_without_a_body_by_an_earlier_version_get_theirs) {
    Env env;
    const uint32_t uid = env.add(pbtest::fixture("mail/plain_text.eml"));
    PB_CHECK(env.sync().first_problem.ok());
    // as an earlier build left it: the header row only
    PB_CHECK(env.store->debug_exec("UPDATE message SET body_state = 'headers', body_text = NULL, body_note = '', links_json = NULL").empty());
    PB_CHECK_EQ(env.store->messages_missing_body(env.store->folders().at(0).id, 10).size(), static_cast<size_t>(1));
    PB_CHECK(env.sync().first_problem.ok());               // the UID is known, the body is still fetched
    PB_CHECK_EQ(env.store->message(env.id_of(uid))->body_text, std::string("Hello Alice,\n\nthis is a plain message.\nSecond line.\n\nBye\nAnna"));
}

// A change in how bodies are read (the format number) makes the next sync read them again, once.
PB_TEST(bodies_stored_by_an_older_reading_are_read_again_once) {
    Env env;
    env.add(pbtest::fixture("mail/with_attachments.eml"));
    PB_CHECK(env.sync().first_problem.ok());
    PB_CHECK_EQ(env.store->setting("body_format"), std::string("2"));
    // as an older build left it: no format number, and a body that is wrong
    PB_CHECK(env.store->set_setting("body_format", "").ok());
    PB_CHECK(env.store->debug_exec("UPDATE message SET body_text = 'old reading'").empty());
    PB_CHECK(env.store->debug_exec("DELETE FROM attachment").empty());
    PB_CHECK(env.sync().first_problem.ok());
    const auto v = env.store->message(env.id_of(1));
    PB_CHECK_EQ(v->body_text, std::string("Please find the documents attached."));
    PB_CHECK_EQ(v->attachments.size(), static_cast<size_t>(2));
    PB_CHECK_EQ(env.store->setting("body_format"), std::string("2"));
    // the next sync does not read them again
    const size_t before = env.server->commands().size();
    PB_CHECK(env.sync().first_problem.ok());
    for (size_t i = before; i < env.server->commands().size(); ++i)
        if (env.server->commands()[i].find("BODY.PEEK[1]") != std::string::npos) PB_FAIL_AT("the body was fetched again: " + env.server->commands()[i]);
}

PB_TEST(a_message_without_readable_text_is_marked_so) {
    Env env;
    env.add(pbtest::fixture("mail/no_text_parts.eml"));
    PB_CHECK(env.sync().first_problem.ok());
    const auto v = env.store->message(env.id_of(1));
    PB_CHECK_EQ(v->header.body_state, std::string("text"));
    PB_CHECK_EQ(v->body_text, std::string(""));
    PB_CHECK_EQ(v->body_note, std::string("no_readable_text"));
    PB_CHECK_EQ(v->attachments.size(), static_cast<size_t>(1));
}

PB_TEST(the_first_message_is_readable_before_the_last_one_is_fetched) {
    // newest first: with a cancel after a few commands the newest message already has its body
    Env env;
    for (const char* name : kFixtures) env.add(pbtest::fixture(std::string("mail/") + name + ".eml"), 86400 * 3);
    const uint32_t newest = env.add(pbtest::fixture("mail/plain_text.eml"), 60);
    struct CancelAfter : pbo::Cancel {
        mutable int calls = 0;
        bool requested() const override { return ++calls > 60; }
    } stopper;
    auto transport = pbo::make_curl_imap_transport(env.pki.ca_cert());
    pbo::SyncDeps deps{*env.store, *transport, env.clock, stopper};
    pbo::Credentials creds;
    creds.mail_password = "pw-for-tests";
    pbo::run_sync(deps, creds);
    PB_CHECK_EQ(env.store->message(env.id_of(newest))->header.body_state, std::string("text"));
}

PB_TEST_MAIN()
