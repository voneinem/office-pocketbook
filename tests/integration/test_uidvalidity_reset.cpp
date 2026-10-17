// T063 (research R-04, Principle III): when the server resets a folder (new UIDVALIDITY, numbering starts again) the stored
// UIDs mean other messages. Nothing is sent by UID; the folder is rebuilt from the window; messages are re-matched by
// Message-ID (date and size as tie-break) so that pending local work is carried over, nothing pending is lost, and drafts,
// the outbox and the rest of the database are not touched. UNVERIFIED against real servers (fake server).
#include <set>
#include <string>
#include <vector>

#include "fake_imap_server.h"
#include "pbo/imap_transport.h"
#include "pbo/store.h"
#include "pbo/sync.h"
#include "test_support.h"
#include "tls_test_server.h"
#include "tmp_dir.h"

using pbtest::FakeImapServer;

PBO_FAKE_SERVER_TEST();

namespace {

constexpr int64_t kNow = 1791979200;

struct FixedClock : pbo::Clock {
    int64_t now() const override { return kNow; }
};
struct NeverCancel : pbo::Cancel {
    bool requested() const override { return false; }
};

// message n: the Message-ID and subject follow n; `day` sets its date
std::string mail(int n, int day = 5) {
    return "From: Sender <s" + std::to_string(n) + "@example.org>\r\nSubject: Subject " + std::to_string(n) + "\r\nDate: " + std::string(day < 10 ? "0" : "") + std::to_string(day) +
           " Oct 2026 10:00:00 +0000\r\nMessage-ID: <m" + std::to_string(n) + "@example.org>\r\n\r\nBody " + std::to_string(n) + "\r\n";
}

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
        server->set_capabilities("IMAP4rev1 UIDPLUS LITERAL+ CONDSTORE");
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
    void add(int n, int day = 5) { server->add_message("INBOX", mail(n, day), {}, kNow - 86400 * 3); }
    pbo::SyncReport sync() {
        auto transport = pbo::make_curl_imap_transport(pki.ca_cert());
        pbo::SyncDeps deps{*store, *transport, clock, cancel};
        pbo::Credentials creds;
        creds.mail_password = "pw-for-tests";
        pbo::SyncOptions options;
        options.push = false;   // these tests are about the pull: what the user changed stays where it is
        return pbo::run_sync(deps, creds, options);
    }
    std::string q(const std::string& sql) const { return store->debug_query(sql); }
    std::string by_id(int n, const char* column) const { return q(std::string("SELECT ") + column + " FROM message WHERE message_id = '<m" + std::to_string(n) + "@example.org>' AND stale = 0"); }
    int stored_commands_with(const char* word) const {
        int n = 0;
        for (const std::string& c : server->commands())
            if (c.find(word) != std::string::npos) ++n;
        return n;
    }
};

}  // namespace

PB_TEST(after_a_reset_the_folder_is_rebuilt_and_pending_local_work_is_carried_to_the_new_numbers) {
    Env env;
    for (int n = 1; n <= 6; ++n) env.add(n);
    PB_CHECK(env.sync().first_problem.ok());
    PB_CHECK_EQ(env.q("SELECT uidvalidity FROM folder WHERE role = 'inbox'"), std::string("1"));
    // the user's wishes that the server has not seen yet
    PB_CHECK(env.store->debug_exec("UPDATE message SET local_state = 'pending_flags', flags_local = 1 WHERE message_id = '<m3@example.org>'").empty());
    PB_CHECK(env.store->debug_exec("UPDATE message SET local_state = 'pending_delete' WHERE message_id = '<m5@example.org>'").empty());
    const std::string old_uid_of_m6 = env.by_id(6, "uid");

    env.server->reset_folder("INBOX", 9);                       // the mailbox was rebuilt on the server: other numbering
    for (int n : {6, 5, 4, 3, 2, 1}) env.add(n);                 // the same messages, in another order, with new UIDs 1..6
    const auto report = env.sync();
    PB_CHECK(report.first_problem.ok());

    PB_CHECK_EQ(env.q("SELECT count(*) FROM message"), std::string("6"));                         // no duplicates, no stale rows left
    PB_CHECK_EQ(env.q("SELECT count(*) FROM message WHERE stale = 1"), std::string("0"));
    PB_CHECK_EQ(env.q("SELECT uidvalidity FROM folder WHERE role = 'inbox'"), std::string("9"));
    PB_CHECK_EQ(env.by_id(6, "uid"), std::string("1"));                                            // numbered anew
    PB_CHECK(env.by_id(6, "uid") != old_uid_of_m6);
    PB_CHECK_EQ(env.by_id(3, "local_state"), std::string("pending_flags"));                        // carried over to the NEW row of that message
    PB_CHECK_EQ(env.by_id(3, "flags_local"), std::string("1"));
    PB_CHECK_EQ(env.by_id(3, "flags_base"), std::string("0"));                                     // what the server says
    PB_CHECK_EQ(env.by_id(5, "local_state"), std::string("pending_delete"));
    PB_CHECK_EQ(env.by_id(1, "local_state"), std::string("synced"));
    PB_CHECK_EQ(env.q("SELECT count(*) FROM message WHERE body_state = 'headers' AND local_state <> 'pending_delete'"), std::string("0"));   // bodies were fetched again (a message that is to be deleted needs none)
    PB_CHECK_EQ(env.q("SELECT count(*) FROM message_fts"), std::string("6"));
    PB_CHECK_EQ(env.stored_commands_with("STORE"), 0);                                             // nothing was written to the server by UID
    PB_CHECK_EQ(env.stored_commands_with("EXPUNGE"), 0);
}

PB_TEST(a_message_with_pending_work_that_is_gone_after_the_reset_stays_visible_with_the_reason) {
    Env env;
    for (int n = 1; n <= 5; ++n) env.add(n);
    PB_CHECK(env.sync().first_problem.ok());
    PB_CHECK(env.store->debug_exec("UPDATE message SET local_state = 'pending_flags', flags_local = 1 WHERE message_id = '<m4@example.org>'").empty());
    env.server->reset_folder("INBOX", 3);
    for (int n : {1, 2, 3, 5}) env.add(n);                       // message 4 did not survive the reset
    PB_CHECK(env.sync().first_problem.ok());

    PB_CHECK_EQ(env.q("SELECT count(*) FROM message"), std::string("5"));                          // 4 re-downloaded + the one with pending work
    PB_CHECK_EQ(env.q("SELECT count(*) FROM message WHERE message_id = '<m4@example.org>'"), std::string("1"));
    PB_CHECK_EQ(env.q("SELECT local_error FROM message WHERE message_id = '<m4@example.org>'"), std::string("gone_on_server"));
    PB_CHECK_EQ(env.q("SELECT local_state FROM message WHERE message_id = '<m4@example.org>'"), std::string("pending_flags"));
    // it is shown in the folder list of the app, the others too
    const auto folder = env.store->folders().at(0);
    const auto page = env.store->list_messages(folder.id, pbo::SortKey::DateDesc, 0, 50);
    PB_CHECK_EQ(page.total, 5);
    bool seen = false;
    for (const auto& m : page.items)
        if (m.message_id == "<m4@example.org>") {
            seen = true;
            PB_CHECK_EQ(m.local_error, std::string("gone_on_server"));
        }
    PB_CHECK(seen);
    // a later normal sync leaves it alone (it is not a message of the current numbering)
    PB_CHECK(env.sync().first_problem.ok());
    PB_CHECK_EQ(env.q("SELECT count(*) FROM message WHERE message_id = '<m4@example.org>'"), std::string("1"));
}

PB_TEST(old_rows_without_pending_work_are_replaced_by_the_new_download) {
    Env env;
    for (int n = 1; n <= 4; ++n) env.add(n);
    PB_CHECK(env.sync().first_problem.ok());
    env.server->reset_folder("INBOX", 2);
    for (int n : {1, 2}) env.add(n);                             // 3 and 4 are gone for good, nothing pending on them
    PB_CHECK(env.sync().first_problem.ok());
    PB_CHECK_EQ(env.q("SELECT count(*) FROM message"), std::string("2"));
    PB_CHECK_EQ(env.q("SELECT count(*) FROM message WHERE message_id IN ('<m3@example.org>', '<m4@example.org>')"), std::string("0"));
}

PB_TEST(equal_message_ids_are_told_apart_by_date_and_size) {
    Env env;
    env.server->add_message("INBOX", "From: a@example.org\r\nSubject: Twin\r\nDate: 01 Oct 2026 10:00:00 +0000\r\nMessage-ID: <twin@example.org>\r\n\r\nshort\r\n", {}, kNow - 86400 * 5);
    env.server->add_message("INBOX", "From: a@example.org\r\nSubject: Twin\r\nDate: 02 Oct 2026 10:00:00 +0000\r\nMessage-ID: <twin@example.org>\r\n\r\na much longer second copy of the message\r\n", {}, kNow - 86400 * 4);
    PB_CHECK(env.sync().first_problem.ok());
    PB_CHECK(env.store->debug_exec("UPDATE message SET local_state = 'pending_flags', flags_local = 4 WHERE date_utc = (SELECT max(date_utc) FROM message)").empty());   // the second one
    env.server->reset_folder("INBOX", 5);
    env.server->add_message("INBOX", "From: a@example.org\r\nSubject: Twin\r\nDate: 02 Oct 2026 10:00:00 +0000\r\nMessage-ID: <twin@example.org>\r\n\r\na much longer second copy of the message\r\n", {}, kNow - 86400 * 4);
    env.server->add_message("INBOX", "From: a@example.org\r\nSubject: Twin\r\nDate: 01 Oct 2026 10:00:00 +0000\r\nMessage-ID: <twin@example.org>\r\n\r\nshort\r\n", {}, kNow - 86400 * 5);
    PB_CHECK(env.sync().first_problem.ok());
    PB_CHECK_EQ(env.q("SELECT count(*) FROM message"), std::string("2"));
    PB_CHECK_EQ(env.q("SELECT local_state FROM message WHERE date_utc = (SELECT max(date_utc) FROM message)"), std::string("pending_flags"));   // on the right one
    PB_CHECK_EQ(env.q("SELECT local_state FROM message WHERE date_utc = (SELECT min(date_utc) FROM message)"), std::string("synced"));
}

PB_TEST(a_rebuild_that_is_cut_off_continues_without_duplicates_and_without_losing_pending_work) {
    Env env;
    for (int n = 1; n <= 6; ++n) env.add(n);
    PB_CHECK(env.sync().first_problem.ok());
    PB_CHECK(env.store->debug_exec("UPDATE message SET local_state = 'pending_flags', flags_local = 1 WHERE message_id = '<m2@example.org>'").empty());
    env.server->reset_folder("INBOX", 4);
    for (int n = 6; n >= 1; --n) env.add(n);
    for (int cut = 3; cut < 70; cut += 3) {                       // the connection dies at many different points of the rebuild
        env.server->drop_after_commands = cut;
        env.sync();
        env.server->drop_after_commands = -1;
    }
    PB_CHECK(env.sync().first_problem.ok());
    PB_CHECK_EQ(env.q("SELECT count(*) FROM message"), std::string("6"));
    PB_CHECK_EQ(env.q("SELECT count(*) FROM message WHERE stale = 1"), std::string("0"));
    PB_CHECK_EQ(env.by_id(2, "local_state"), std::string("pending_flags"));
    PB_CHECK_EQ(env.by_id(2, "flags_local"), std::string("1"));
    PB_CHECK_EQ(env.q("SELECT count(*) FROM message WHERE body_state = 'headers'"), std::string("0"));
    PB_CHECK(env.store->check_integrity().ok());
}

PB_TEST(drafts_the_outbox_and_the_other_tables_are_not_touched_by_a_rebuild) {
    Env env;
    for (int n = 1; n <= 3; ++n) env.add(n);
    PB_CHECK(env.sync().first_problem.ok());
    PB_CHECK(env.store->debug_exec("INSERT INTO draft(kind, message_id, to_text, subject, body, created, updated) VALUES('new', '<draft1@example.org>', 'b@example.org', 'Draft', 'text', 1, 1)").empty());
    PB_CHECK(env.store->debug_exec("INSERT INTO outbox(message_id, envelope_from, envelope_to, raw_message, state, queued_at, state_at) VALUES('<out1@example.org>', 'a@example.org', 'b@example.org', x'41', 'queued', 1, 1)").empty());
    PB_CHECK(env.store->debug_exec("INSERT INTO settings(key, value) VALUES('tz_override', 'Europe/Berlin')").empty());
    const std::string draft_before = env.q("SELECT group_concat(id || message_id || subject || body || server_state, '|') FROM draft");
    const std::string outbox_before = env.q("SELECT group_concat(id || message_id || state || length(raw_message), '|') FROM outbox");
    env.server->reset_folder("INBOX", 8);
    for (int n : {3, 2, 1}) env.add(n);
    PB_CHECK(env.sync().first_problem.ok());
    PB_CHECK_EQ(env.q("SELECT group_concat(id || message_id || subject || body || server_state, '|') FROM draft"), draft_before);
    PB_CHECK_EQ(env.q("SELECT group_concat(id || message_id || state || length(raw_message), '|') FROM outbox"), outbox_before);
    PB_CHECK_EQ(env.q("SELECT value FROM settings WHERE key = 'tz_override'"), std::string("Europe/Berlin"));
}

PB_TEST(a_status_that_cannot_be_read_writes_nothing_for_the_folder) {
    Env env;
    for (int n = 1; n <= 3; ++n) env.add(n);
    PB_CHECK(env.sync().first_problem.ok());
    const std::string before = env.q("SELECT group_concat(uid || ':' || stale || ':' || message_id, ',') FROM (SELECT * FROM message ORDER BY id)");
    env.server->reset_folder("INBOX", 6);
    env.server->drop_after_commands = 0;                         // every command is cut: the answer to STATUS never comes
    const auto failed = env.sync();
    env.server->drop_after_commands = -1;
    PB_CHECK(!failed.first_problem.ok());
    PB_CHECK_EQ(env.q("SELECT group_concat(uid || ':' || stale || ':' || message_id, ',') FROM (SELECT * FROM message ORDER BY id)"), before);   // untouched
    PB_CHECK_EQ(env.q("SELECT uidvalidity FROM folder WHERE role = 'inbox'"), std::string("1"));
}

PB_TEST_MAIN()
