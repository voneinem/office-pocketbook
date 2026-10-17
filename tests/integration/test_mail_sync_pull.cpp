// T062: the pull of a folder is incremental and idempotent: only the window is downloaded, flag changes and deletions on the
// server arrive (also without CONDSTORE, with the same result), local pending changes are never overwritten or lost, and a
// connection that dies anywhere converges. UNVERIFIED against real servers (fake server).
#include <sys/resource.h>

#include <cstdlib>
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

constexpr int64_t kStart = 1791979200;   // 2026-10-14 12:00:00 UTC

struct TestClock : pbo::Clock {
    int64_t t = kStart;
    int64_t now() const override { return t; }
};
struct NeverCancel : pbo::Cancel {
    bool requested() const override { return false; }
};

std::string mail(int n) {
    return "From: Sender <s" + std::to_string(n) + "@example.org>\r\nSubject: Subject " + std::to_string(n) + "\r\nDate: Mon, 05 Oct 2026 10:00:00 +0000\r\nMessage-ID: <m" +
           std::to_string(n) + "@example.org>\r\n\r\nBody " + std::to_string(n) + "\r\n";
}

struct Env {
    pbtest::TmpDir dir, pki_dir;
    pbtest::TestPki pki{pki_dir};
    pbtest::TestPki::Leaf leaf;
    TestClock clock;
    NeverCancel cancel;
    std::unique_ptr<FakeImapServer> server;
    std::unique_ptr<pbo::Store> store;
    int next = 1;
    explicit Env(bool condstore = true) {
        if (!pki.available()) throw pbtest::Skip{"the openssl command is not available"};
        leaf = pki.issue("srv", "DNS:localhost,IP:127.0.0.1");
        if (!leaf.ok) throw pbtest::Skip{"cannot create a test certificate"};
        FakeImapServer::Config c;
        c.cert = leaf.cert;
        c.key = leaf.key;
        server = std::make_unique<FakeImapServer>(c);
        server->set_capabilities(condstore ? "IMAP4rev1 UIDPLUS LITERAL+ CONDSTORE" : "IMAP4rev1 UIDPLUS LITERAL+");
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
    // a message of the given age (days); returns its UID
    uint32_t add(int age_days = 1, std::set<std::string> flags = {}) {
        const int64_t when = clock.t - age_days * 86400 + 3600;
        return server->add_message("INBOX", mail(next++), std::move(flags), when);
    }
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
    // the stored messages of INBOX as one comparable text
    std::string snapshot() const { return q("SELECT group_concat(uid || ':' || flags_base || ':' || flags_local || ':' || subject, ',') FROM (SELECT * FROM message WHERE stale = 0 ORDER BY uid)"); }
    int fetches_since(size_t from) const {
        int n = 0;
        const auto cmds = server->commands();
        for (size_t i = from; i < cmds.size(); ++i)
            if (cmds[i].find("UID FETCH") != std::string::npos) ++n;
        return n;
    }
};

}  // namespace

PB_TEST(only_the_window_is_downloaded_and_older_mail_on_the_server_is_noted) {
    Env env;
    for (int i = 0; i < 5; ++i) env.add(2 + i);        // inside the 30 days
    for (int i = 0; i < 3; ++i) env.add(60 + i * 10);  // older
    PB_CHECK(env.sync().first_problem.ok());
    PB_CHECK_EQ(env.q("SELECT count(*) FROM message"), std::string("5"));
    PB_CHECK_EQ(env.q("SELECT older_on_server FROM folder WHERE role = 'inbox'"), std::string("1"));
    // no older mail: the marker is off
    Env clean;
    for (int i = 0; i < 4; ++i) clean.add(3 + i);
    PB_CHECK(clean.sync().first_problem.ok());
    PB_CHECK_EQ(clean.q("SELECT older_on_server FROM folder WHERE role = 'inbox'"), std::string("0"));
}

PB_TEST(flag_changes_deletions_and_new_mail_arrive_and_both_server_kinds_end_in_the_same_store) {
    std::string snapshots[2];
    for (int variant = 0; variant < 2; ++variant) {
        Env env(variant == 0);                          // 0: with CONDSTORE, 1: without
        std::vector<uint32_t> uids;
        for (int i = 0; i < 10; ++i) uids.push_back(env.add(2 + i % 5));
        PB_CHECK(env.sync().first_problem.ok());
        PB_CHECK_EQ(env.q("SELECT count(*) FROM message"), std::string("10"));

        env.server->set_flags("INBOX", uids[2], {"\\Seen"});
        env.server->set_flags("INBOX", uids[4], {"\\Flagged", "\\Answered"});
        env.server->expunge("INBOX", uids[6]);
        env.add(1);
        env.add(1);
        PB_CHECK(env.sync().first_problem.ok());
        snapshots[variant] = env.snapshot();

        PB_CHECK_EQ(env.q("SELECT count(*) FROM message"), std::string("11"));                       // 10 - 1 + 2
        PB_CHECK_EQ(env.q("SELECT count(*) FROM message WHERE uid = " + std::to_string(uids[6])), std::string("0"));
        PB_CHECK_EQ(env.q("SELECT flags_local FROM message WHERE uid = " + std::to_string(uids[2])), std::to_string(pbo::kFlagSeen));
        PB_CHECK_EQ(env.q("SELECT flags_local FROM message WHERE uid = " + std::to_string(uids[4])), std::to_string(pbo::kFlagFlagged | pbo::kFlagAnswered));
        PB_CHECK_EQ(env.q("SELECT unread_count FROM folder WHERE role = 'inbox'"), std::string("10"));   // 11 messages, one is read
        PB_CHECK_EQ(env.q("SELECT count(*) FROM message WHERE body_state = 'headers'"), std::string("0"));
        PB_CHECK_EQ(env.q("SELECT count(*) FROM message_fts"), std::string("11"));                  // the index followed the deletion
    }
    PB_CHECK_EQ(snapshots[0], snapshots[1]);
}

PB_TEST(a_pending_local_change_is_not_overwritten_by_the_server_and_a_message_deleted_there_stays) {
    Env env;
    std::vector<uint32_t> uids;
    for (int i = 0; i < 4; ++i) uids.push_back(env.add(2));
    PB_CHECK(env.sync().first_problem.ok());
    // the user marked message 2 as read and message 3 as flagged; neither change is on the server yet
    PB_CHECK(env.store->debug_exec("UPDATE message SET local_state = 'pending_flags', flags_local = 1 WHERE uid = " + std::to_string(uids[1])).empty());
    PB_CHECK(env.store->debug_exec("UPDATE message SET local_state = 'pending_flags', flags_local = 4 WHERE uid = " + std::to_string(uids[2])).empty());
    env.server->set_flags("INBOX", uids[1], {"\\Flagged"});     // changed on the server meanwhile
    env.server->expunge("INBOX", uids[2]);                        // deleted on the server meanwhile
    PB_CHECK(env.sync().first_problem.ok());
    // message 2: the server's flags are known (base), the user's wish stays (local) and is still pending
    PB_CHECK_EQ(env.q("SELECT flags_base FROM message WHERE uid = " + std::to_string(uids[1])), std::to_string(pbo::kFlagFlagged));
    PB_CHECK_EQ(env.q("SELECT flags_local FROM message WHERE uid = " + std::to_string(uids[1])), std::string("1"));
    PB_CHECK_EQ(env.q("SELECT local_state FROM message WHERE uid = " + std::to_string(uids[1])), std::string("pending_flags"));
    // message 3: gone on the server, but not lost: kept, with the reason
    PB_CHECK_EQ(env.q("SELECT count(*) FROM message WHERE uid = " + std::to_string(uids[2])), std::string("1"));
    PB_CHECK_EQ(env.q("SELECT local_error FROM message WHERE uid = " + std::to_string(uids[2])), std::string("gone_on_server"));
    PB_CHECK_EQ(env.q("SELECT local_state FROM message WHERE uid = " + std::to_string(uids[2])), std::string("pending_flags"));
    // the others are untouched
    PB_CHECK_EQ(env.q("SELECT count(*) FROM message"), std::string("4"));
}

PB_TEST(a_sync_without_changes_asks_for_little) {
    Env env;
    for (int i = 0; i < 6; ++i) env.add(2);
    PB_CHECK(env.sync().first_problem.ok());
    const size_t before = env.server->commands().size();
    PB_CHECK(env.sync().first_problem.ok());
    PB_CHECK_EQ(env.fetches_since(before), 0);                    // CONDSTORE and unchanged: no flag fetch, no header fetch, no body fetch

    Env plain(false);
    for (int i = 0; i < 6; ++i) plain.add(2);
    PB_CHECK(plain.sync().first_problem.ok());
    const size_t before2 = plain.server->commands().size();
    PB_CHECK(plain.sync().first_problem.ok());
    PB_CHECK(plain.fetches_since(before2) <= 1);                  // without CONDSTORE: one flag fetch for the whole range
}

PB_TEST(a_window_that_moves_on_does_not_delete_or_refetch_what_is_still_on_the_server) {
    Env env;
    for (int i = 0; i < 5; ++i) env.add(10 + i);
    PB_CHECK(env.sync().first_problem.ok());
    env.clock.t += 40 * 86400;                                    // 40 days later the old mail is outside the window
    const size_t before = env.server->commands().size();
    PB_CHECK(env.sync().first_problem.ok());
    PB_CHECK_EQ(env.q("SELECT count(*) FROM message"), std::string("5"));
    PB_CHECK_EQ(env.fetches_since(before), 0);
}

PB_TEST(a_connection_that_dies_anywhere_converges_with_no_duplicates_and_no_gaps) {
    Env env;
    std::vector<uint32_t> uids;
    for (int i = 0; i < 8; ++i) uids.push_back(env.add(2 + i % 4));
    env.server->set_flags("INBOX", uids[1], {"\\Seen"});
    // the connection is cut after N commands, for many N, and every time the next sync continues
    for (int n = 3; n < 90; n += 4) {
        env.server->drop_after_commands = n;
        env.sync();
        env.server->drop_after_commands = -1;
    }
    // then a change on the server, again cut at various points
    env.server->expunge("INBOX", uids[3]);
    env.server->set_flags("INBOX", uids[5], {"\\Flagged"});
    env.add(1);
    for (int n = 3; n < 60; n += 5) {
        env.server->drop_after_commands = n;
        env.sync();
        env.server->drop_after_commands = -1;
    }
    PB_CHECK(env.sync().first_problem.ok());
    PB_CHECK_EQ(env.q("SELECT count(*) FROM message"), std::string("8"));                             // 8 - 1 + 1
    PB_CHECK_EQ(env.q("SELECT count(DISTINCT uid) FROM message"), std::string("8"));
    PB_CHECK_EQ(env.q("SELECT count(*) FROM message WHERE body_state = 'headers'"), std::string("0"));
    PB_CHECK_EQ(env.q("SELECT count(*) FROM message WHERE uid = " + std::to_string(uids[3])), std::string("0"));
    PB_CHECK_EQ(env.q("SELECT flags_local FROM message WHERE uid = " + std::to_string(uids[1])), std::to_string(pbo::kFlagSeen));
    PB_CHECK_EQ(env.q("SELECT flags_local FROM message WHERE uid = " + std::to_string(uids[5])), std::to_string(pbo::kFlagFlagged));
    PB_CHECK(env.store->check_integrity().ok());
    PB_CHECK_EQ(env.q("SELECT count(*) FROM message_fts"), std::string("8"));
}

PB_TEST(a_folder_of_50000_messages_downloads_only_the_window_within_a_memory_bound) {
    Env env;
    for (int i = 0; i < 49990; ++i) env.add(100 + i % 200);       // old mail
    for (int i = 0; i < 10; ++i) env.add(1 + i % 5);              // the window
    struct rusage before {};
    getrusage(RUSAGE_SELF, &before);
    PB_CHECK(env.sync().first_problem.ok());
    struct rusage after {};
    getrusage(RUSAGE_SELF, &after);
    PB_CHECK_EQ(env.q("SELECT count(*) FROM message"), std::string("10"));
    PB_CHECK_EQ(env.q("SELECT older_on_server FROM folder WHERE role = 'inbox'"), std::string("1"));
    // every FETCH names a UID of the window (49,991..50,000): 10 headers, then structure and text of each of the 10 messages
    int fetches = 0;
    for (const std::string& c : env.server->commands()) {
        const size_t p = c.find("UID FETCH ");
        if (p == std::string::npos) continue;
        ++fetches;
        PB_CHECK(std::atoi(c.c_str() + p + 10) >= 49991);
    }
    PB_CHECK(fetches <= 30);
    // the growth of the process stays far below what 50,000 headers would need
    PB_CHECK((after.ru_maxrss - before.ru_maxrss) < 60 * 1024);   // KB
}

PB_TEST_MAIN()
