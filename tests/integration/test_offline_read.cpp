// T065 (SC-002, FR-033): after a completed sync every message of the window opens completely with NO transport and NO server any
// more. UNVERIFIED against real servers (fake server).
#include <memory>
#include <string>
#include <vector>

#include "fake_imap_server.h"
#include "fixtures.h"
#include "pbo/imap_transport.h"
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
        auto transport = pbo::make_curl_imap_transport(pki.ca_cert());   // destroyed when the sync is over
        pbo::SyncDeps deps{*store, *transport, clock, cancel};
        pbo::Credentials creds;
        creds.mail_password = "pw-for-tests";
        return pbo::run_sync(deps, creds);
    }
};

}  // namespace

PB_TEST(every_message_of_the_window_opens_completely_without_any_server) {
    Env env;
    const char* names[] = {"base64_utf8", "charset_unknown", "charset_windows1250", "empty_body", "encoded_subject_2047", "html_only", "message_rfc822_attachment",
                           "multipart_alternative", "nested_multipart", "no_text_parts", "param_2231", "plain_text", "qp_latin1", "quoted_reply", "with_attachments"};
    int n = 0;
    for (const char* name : names) env.server->add_message("INBOX", pbtest::fixture(std::string("mail/") + name + ".eml"), {}, kNow - 86400 * (1 + n++));
    const auto report = env.sync();
    PB_CHECK(report.first_problem.ok());
    const int total = static_cast<int>(sizeof names / sizeof names[0]);
    PB_CHECK_EQ(env.store->debug_query("SELECT count(*) FROM message"), std::to_string(total));

    env.server.reset();   // the server is gone; the transports were destroyed with the sync

    const std::string ids = env.store->debug_query("SELECT group_concat(id) FROM message");
    int opened = 0, with_text = 0;
    size_t pos = 0;
    while (pos < ids.size()) {
        const size_t comma = ids.find(',', pos);
        const int64_t id = std::stoll(ids.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos));
        const auto view = env.store->message(id);
        PB_CHECK(view.has_value());
        if (view) {
            ++opened;
            PB_CHECK(view->header.body_state != "headers");            // the body was fetched during the sync
            PB_CHECK(!view->header.subject.empty() || !view->header.from_addr.empty());
            if (!view->body_text.empty()) ++with_text;
            for (const auto& a : view->attachments) PB_CHECK(!a.name.empty());   // listed from the structure, no server needed
        }
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    PB_CHECK_EQ(opened, total);
    PB_CHECK(with_text >= total - 3);   // only the empty, the no-text and the attachment-only messages have no text
    PB_CHECK_EQ(env.store->debug_query("SELECT count(*) FROM message WHERE body_state = 'headers'"), std::string("0"));
}

PB_TEST(a_second_start_without_a_network_still_shows_the_stored_messages) {
    Env env;
    env.server->add_message("INBOX", pbtest::fixture("mail/plain_text.eml"), {}, kNow - 86400);
    PB_CHECK(env.sync().first_problem.ok());
    env.server.reset();
    const auto report = env.sync();   // nothing answers: offline, local data stays intact (FR-035)
    PB_CHECK(!report.first_problem.ok());
    PB_CHECK_EQ(env.store->debug_query("SELECT count(*) FROM message WHERE body_state = 'text'"), std::string("1"));
}

PB_TEST_MAIN()
