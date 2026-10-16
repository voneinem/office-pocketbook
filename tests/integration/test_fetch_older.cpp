// T066 (second part): older mail on demand. The window setting is unchanged; every call brings the next 50 older messages;
// older_on_server is cleared when nothing older is left. UNVERIFIED against real servers (fake server).
#include <memory>
#include <string>
#include <vector>

#include "fake_imap_server.h"
#include "fixtures.h"
#include "pbo/imap_transport.h"
#include "pbo/store.h"
#include "pbo/mail_sync.h"
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
    int next = 1;
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
    // a message of the given age (days)
    uint32_t add(int age_days) {
        const int n = next++;
        const std::string raw = "From: S <s@example.org>\r\nSubject: Subject " + std::to_string(n) + "\r\nDate: Mon, 05 Oct 2026 10:00:00 +0000\r\nMessage-ID: <m" +
                                std::to_string(n) + "@example.org>\r\n\r\nBody " + std::to_string(n) + "\r\n";
        return server->add_message("INBOX", raw, {}, kNow - static_cast<int64_t>(age_days) * 86400);
    }
    // asks for older mail once; returns the error
    pbo::Error older(pbo::MailCounts* counts = nullptr) {
        auto transport = pbo::make_curl_imap_transport(pki.ca_cert());
        pbo::Credentials creds;
        pbo::ImapEndpoint ep;
        ep.host = "localhost";
        ep.port = server->port();
        ep.username = "alice";
        pbo::Error e = transport->connect(ep, "pw-for-tests");
        if (!e.ok()) return e;
        pbo::UidvalidityGuard guard;
        const auto inbox = [&] {
            for (const pbo::Folder& f : store->folders())
                if (f.role == "inbox") return f;
            return pbo::Folder();
        }();
        e = pbo::fetch_older(*transport, *store, inbox, 50, cancel, guard, counts);
        transport->disconnect();
        return e;
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

PB_TEST(older_mail_is_fetched_in_steps_without_touching_the_window_setting) {
    Env env;
    for (int i = 0; i < 120; ++i) env.add(100 + i);   // older than the 30 days
    for (int i = 0; i < 4; ++i) env.add(1 + i);       // inside the window
    PB_CHECK(env.sync().first_problem.ok());
    PB_CHECK_EQ(env.store->debug_query("SELECT count(*) FROM message"), std::string("4"));
    PB_CHECK_EQ(env.store->debug_query("SELECT older_on_server FROM folder WHERE role = 'inbox'"), std::string("1"));

    pbo::MailCounts counts;
    PB_CHECK(env.older(&counts).ok());
    PB_CHECK_EQ(env.store->debug_query("SELECT count(*) FROM message"), std::string("54"));
    PB_CHECK_EQ(counts.new_messages, 50);
    PB_CHECK_EQ(env.store->debug_query("SELECT older_on_server FROM folder WHERE role = 'inbox'"), std::string("1"));
    PB_CHECK_EQ(env.store->debug_query("SELECT count(*) FROM message WHERE body_state = 'headers'"), std::string("0"));   // bodies too
    PB_CHECK_EQ(env.store->mail_account()->sync_days, 30);

    PB_CHECK(env.older().ok());
    PB_CHECK_EQ(env.store->debug_query("SELECT count(*) FROM message"), std::string("104"));
    PB_CHECK_EQ(env.store->debug_query("SELECT older_on_server FROM folder WHERE role = 'inbox'"), std::string("1"));

    PB_CHECK(env.older().ok());   // 20 left: all of them
    PB_CHECK_EQ(env.store->debug_query("SELECT count(*) FROM message"), std::string("124"));
    PB_CHECK_EQ(env.store->debug_query("SELECT older_on_server FROM folder WHERE role = 'inbox'"), std::string("0"));
    PB_CHECK_EQ(env.store->debug_query("SELECT count(DISTINCT uid) FROM message"), std::string("124"));   // no duplicates
    PB_CHECK_EQ(env.store->mail_account()->sync_days, 30);

    // nothing older left: no error, nothing added
    PB_CHECK(env.older().ok());
    PB_CHECK_EQ(env.store->debug_query("SELECT count(*) FROM message"), std::string("124"));

    // a normal sync afterwards neither removes the older messages nor fetches them again
    const size_t before = env.server->commands().size();
    PB_CHECK(env.sync().first_problem.ok());
    PB_CHECK_EQ(env.store->debug_query("SELECT count(*) FROM message"), std::string("124"));
    int fetches = 0;
    for (size_t i = before; i < env.server->commands().size(); ++i)
        if (env.server->commands()[i].find("UID FETCH") != std::string::npos && env.server->commands()[i].find("BODY") != std::string::npos) ++fetches;
    PB_CHECK_EQ(fetches, 0);
    PB_CHECK_EQ(env.store->debug_query("SELECT older_on_server FROM folder WHERE role = 'inbox'"), std::string("0"));
}

PB_TEST(a_folder_that_was_reset_is_not_extended_with_old_numbers) {
    Env env;
    for (int i = 0; i < 60; ++i) env.add(100 + i);
    env.add(1);
    PB_CHECK(env.sync().first_problem.ok());
    env.server->reset_folder("INBOX", 77);
    for (int i = 0; i < 3; ++i) env.add(100 + i);
    const pbo::Error e = env.older();   // the guard sees the new UIDVALIDITY: nothing is written by old numbers
    PB_CHECK(!e.ok());
    PB_CHECK_EQ(env.store->debug_query("SELECT count(*) FROM message WHERE uid > 0 AND stale = 0"), std::string("1"));
}

PB_TEST(without_a_connection_nothing_changes_and_the_error_is_reported) {
    Env env;
    for (int i = 0; i < 60; ++i) env.add(100 + i);
    env.add(1);
    PB_CHECK(env.sync().first_problem.ok());
    env.server->drop_after_commands = 3;
    PB_CHECK(!env.older().ok());
    env.server->drop_after_commands = -1;
    PB_CHECK(env.store->check_integrity().ok());
    PB_CHECK(env.older().ok());   // and the next try works
    PB_CHECK_EQ(env.store->debug_query("SELECT count(*) FROM message"), std::string("51"));
}

PB_TEST_MAIN()
