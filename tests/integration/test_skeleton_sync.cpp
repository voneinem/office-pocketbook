// T037: the walking skeleton end to end on the host: account in the store, run_sync against the fake IMAP server
// (3 folders, 12 messages in INBOX), headers in the store in date order, a second run changes nothing.
// UNVERIFIED against real servers (fake-server evidence only, Principle IX).
#include <set>
#include <string>

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

constexpr int64_t kNow = 1791979200;   // 2026-10-14 12:00:00 UTC

struct FixedClock : pbo::Clock {
    int64_t now() const override { return kNow; }
};
struct NeverCancel : pbo::Cancel {
    bool requested() const override { return false; }
};

std::string rfc_date(int64_t epoch) {
    char buf[64];
    time_t t = static_cast<time_t>(epoch);
    tm tm{};
    gmtime_r(&t, &tm);
    std::strftime(buf, sizeof buf, "%a, %d %b %Y %H:%M:%S +0000", &tm);
    return buf;
}

std::string mail(int i, int64_t epoch) {
    return "From: Sender " + std::to_string(i) + " <sender" + std::to_string(i) + "@example.org>\r\nTo: alice@example.org\r\nSubject: Subject " +
           std::to_string(i) + "\r\nDate: " + rfc_date(epoch) + "\r\nMessage-ID: <m" + std::to_string(i) + "@example.org>\r\n\r\nBody " +
           std::to_string(i) + "\r\n";
}

struct Env {
    pbtest::TmpDir dir;
    pbtest::TmpDir pki_dir;
    pbtest::TestPki pki{pki_dir};
    pbtest::TestPki::Leaf leaf;
    FixedClock clock;
    NeverCancel cancel;
    Env() {
        if (!pki.available()) throw pbtest::Skip{"the openssl command is not available"};
        leaf = pki.issue("srv", "DNS:localhost,IP:127.0.0.1");
        if (!leaf.ok) throw pbtest::Skip{"cannot create a test certificate"};
    }
    FakeImapServer::Config config() const {
        FakeImapServer::Config c;
        c.cert = leaf.cert;
        c.key = leaf.key;
        return c;
    }
};

void fill(FakeImapServer& s) {
    s.set_capabilities("IMAP4rev1 UIDPLUS LITERAL+ CONDSTORE");
    s.add_folder("INBOX");
    s.add_folder("Sent", {"\\Sent"});
    s.add_folder("Drafts", {"\\Drafts"});
    for (int i = 1; i <= 12; ++i) {
        // message i is (13 - i) days old: message 12 is the newest; every 3rd one is already read
        const int64_t when = kNow - (13 - i) * 86400 + 3600;
        std::set<std::string> flags;
        if (i % 3 == 0) flags.insert("\\Seen");
        if (i == 5) flags.insert("\\Flagged");
        s.add_message("INBOX", mail(i, when), flags, when);
    }
}

pbo::MailAccount account() {
    pbo::MailAccount a;
    a.imap_host = "localhost";
    a.imap_port = 0;   // set per test
    a.smtp_host = "localhost";
    a.smtp_port = 465;
    a.username = "alice";
    a.address = "alice@example.org";
    return a;
}

}  // namespace

PB_TEST(a_sync_stores_the_folders_and_the_headers_of_the_window) {
    Env env;
    FakeImapServer srv(env.config());
    fill(srv);
    auto store = pbo::Store::open(env.dir.path(), env.clock);
    PB_CHECK(store.ok());
    auto acc = account();
    acc.imap_port = srv.port();
    PB_CHECK(store.value->save_mail_account(acc).ok());

    auto transport = pbo::make_curl_imap_transport(env.pki.ca_cert());
    pbo::SyncDeps deps{*store.value, *transport, env.clock, env.cancel};
    pbo::Credentials creds;
    creds.mail_password = "pw-for-tests";
    const pbo::SyncReport report = pbo::run_sync(deps, creds);
    PB_CHECK(report.first_problem.ok());
    PB_CHECK(report.outcome == pbo::SyncOutcome::Success);
    PB_CHECK_EQ(report.counts.new_messages, 12);

    const auto folders = store.value->folders();
    PB_CHECK_EQ(folders.size(), static_cast<size_t>(3));
    int64_t inbox = 0;
    std::set<std::string> roles;
    for (const auto& f : folders) {
        roles.insert(f.role);
        if (f.name == "INBOX") inbox = f.id;
    }
    PB_CHECK(inbox != 0);
    PB_CHECK(roles.count("inbox") == 1 && roles.count("sent") == 1 && roles.count("drafts") == 1);

    auto page = store.value->list_messages(inbox, pbo::SortKey::DateDesc, 0, 50);
    PB_CHECK_EQ(page.total, 12);
    PB_CHECK_EQ(page.items.size(), static_cast<size_t>(12));
    PB_CHECK_EQ(page.items[0].subject, std::string("Subject 12"));   // newest first
    PB_CHECK_EQ(page.items[11].subject, std::string("Subject 1"));
    PB_CHECK_EQ(page.items[0].from_name, std::string("Sender 12"));
    PB_CHECK_EQ(page.items[0].from_addr, std::string("sender12@example.org"));
    PB_CHECK_EQ(page.items[0].date_utc, kNow - 86400 + 3600 - 0);   // message 12: 1 day old, +1 h
    PB_CHECK(!page.items[0].unread());   // every 3rd message (3, 6, 9, 12) is read
    PB_CHECK(page.items[1].unread());
    PB_CHECK(!page.items[9].unread());   // Subject 3 is read
    bool flagged = false;
    for (const auto& m : page.items)
        if (m.subject == "Subject 5") flagged = (m.flags & pbo::kFlagFlagged) != 0;
    PB_CHECK(flagged);
    for (size_t i = 1; i < page.items.size(); ++i) PB_CHECK(page.items[i - 1].date_utc >= page.items[i].date_utc);

    // the folder cursor was stored with the rows, and the sync state says what happened
    for (const auto& f : store.value->folders())
        if (f.id == inbox) {
            PB_CHECK(f.uidvalidity.has_value());
            PB_CHECK_EQ(*f.uidvalidity, static_cast<int64_t>(1));
            PB_CHECK_EQ(*f.uidnext, static_cast<int64_t>(13));
            PB_CHECK_EQ(f.unread_count, 8);
        }
    const pbo::SyncState state = store.value->sync_state();
    PB_CHECK_EQ(state.outcome, std::string("success"));
    PB_CHECK(state.last_success.has_value());
    PB_CHECK_EQ(*state.last_success, kNow);
    PB_CHECK_EQ(state.problem_key, std::string(""));
}

PB_TEST(encoded_subjects_and_names_are_stored_decoded) {
    Env env;
    FakeImapServer srv(env.config());
    fill(srv);
    const int64_t when = kNow - 3600;
    srv.add_message("INBOX",
                    "From: =?UTF-8?Q?J=C3=BCrgen_M=C3=BCller?= <juergen@example.org>\r\nTo: =?ISO-8859-1?Q?Zo=EB?= <zoe@example.org>, bob@example.org\r\n"
                    "Subject: =?UTF-8?B?R3LDvMOfZSBhdXMgS8O2bG4=?=\r\nDate: " + rfc_date(when) + "\r\nMessage-ID: <enc@example.org>\r\n\r\nBody\r\n",
                    {}, when);
    auto store = pbo::Store::open(env.dir.path(), env.clock);
    auto acc = account();
    acc.imap_port = srv.port();
    PB_CHECK(store.value->save_mail_account(acc).ok());
    auto transport = pbo::make_curl_imap_transport(env.pki.ca_cert());
    pbo::SyncDeps deps{*store.value, *transport, env.clock, env.cancel};
    pbo::Credentials creds;
    creds.mail_password = "pw-for-tests";
    PB_CHECK(pbo::run_sync(deps, creds).first_problem.ok());
    PB_CHECK_EQ(store.value->debug_query("SELECT subject FROM message WHERE message_id = '<enc@example.org>'"), std::string("Gr\xC3\xBC\xC3\x9F" "e aus K\xC3\xB6ln"));
    PB_CHECK_EQ(store.value->debug_query("SELECT from_name FROM message WHERE message_id = '<enc@example.org>'"), std::string("J\xC3\xBCrgen M\xC3\xBCller"));
    PB_CHECK_EQ(store.value->debug_query("SELECT from_addr FROM message WHERE message_id = '<enc@example.org>'"), std::string("juergen@example.org"));
    PB_CHECK_EQ(store.value->debug_query("SELECT to_text FROM message WHERE message_id = '<enc@example.org>'"), std::string("Zo\xC3\xAB <zoe@example.org>, bob@example.org"));
}

// Rows stored by an earlier version kept RFC 2047 words as they came; the sync repairs them in place.
PB_TEST(headers_stored_undecoded_by_an_earlier_version_are_repaired) {
    Env env;
    auto store = pbo::Store::open(env.dir.path(), env.clock);
    pbo::FolderListItem inbox;
    inbox.raw_name = inbox.name = "INBOX";
    PB_CHECK(store.value->upsert_folders({inbox}).ok());
    const int64_t folder = store.value->folders().at(0).id;
    pbo::PullBatch batch;
    pbo::PulledMessage m;
    m.uid = 1;
    m.subject = "=?UTF-8?B?R3LDvMOfZSBhdXMgS8O2bG4=?=";
    m.from_name = "=?UTF-8?Q?J=C3=BCrgen?=";
    m.from_addr = "j@example.org";
    m.to_text = "=?UTF-8?Q?Zo=C3=AB?= <zoe@example.org>";
    m.date_utc = 1790000000;
    batch.messages.push_back(m);
    pbo::PulledMessage plain;
    plain.uid = 2;
    plain.subject = "Plain subject";
    plain.from_name = "Anna";
    plain.date_utc = 1790000100;
    batch.messages.push_back(plain);
    PB_CHECK(store.value->apply_pull(folder, batch).ok());

    int repaired = -1;
    PB_CHECK(store.value->repair_encoded_headers(&repaired).ok());
    PB_CHECK_EQ(repaired, 1);
    PB_CHECK_EQ(store.value->debug_query("SELECT subject FROM message WHERE uid = 1"), std::string("Gr\xC3\xBC\xC3\x9F" "e aus K\xC3\xB6ln"));
    PB_CHECK_EQ(store.value->debug_query("SELECT from_name FROM message WHERE uid = 1"), std::string("J\xC3\xBCrgen"));
    PB_CHECK_EQ(store.value->debug_query("SELECT to_text FROM message WHERE uid = 1"), std::string("Zo\xC3\xAB <zoe@example.org>"));
    PB_CHECK_EQ(store.value->debug_query("SELECT subject FROM message WHERE uid = 2"), std::string("Plain subject"));
    // the search index follows the repaired text
    PB_CHECK_EQ(store.value->debug_query("SELECT count(*) FROM message_fts WHERE message_fts MATCH 'k\xC3\xB6ln'"), std::string("1"));
    PB_CHECK(store.value->repair_encoded_headers(&repaired).ok());
    PB_CHECK_EQ(repaired, 0);   // idempotent
}

PB_TEST(a_second_run_changes_nothing) {
    Env env;
    FakeImapServer srv(env.config());
    fill(srv);
    auto store = pbo::Store::open(env.dir.path(), env.clock);
    auto acc = account();
    acc.imap_port = srv.port();
    PB_CHECK(store.value->save_mail_account(acc).ok());
    auto transport = pbo::make_curl_imap_transport(env.pki.ca_cert());
    pbo::SyncDeps deps{*store.value, *transport, env.clock, env.cancel};
    pbo::Credentials creds;
    creds.mail_password = "pw-for-tests";
    PB_CHECK(pbo::run_sync(deps, creds).first_problem.ok());
    const std::string before = store.value->debug_query("SELECT group_concat(uid || ':' || subject || ':' || flags_local, ',') FROM (SELECT * FROM message ORDER BY id)");
    const std::string folders_before = store.value->debug_query("SELECT group_concat(name || uidvalidity || uidnext, ',') FROM (SELECT * FROM folder ORDER BY id)");

    const auto second = pbo::run_sync(deps, creds);
    PB_CHECK(second.first_problem.ok());
    PB_CHECK_EQ(second.counts.new_messages, 0);
    PB_CHECK_EQ(store.value->debug_query("SELECT count(*) FROM message"), std::string("12"));
    PB_CHECK_EQ(store.value->debug_query("SELECT group_concat(uid || ':' || subject || ':' || flags_local, ',') FROM (SELECT * FROM message ORDER BY id)"), before);
    PB_CHECK_EQ(store.value->debug_query("SELECT group_concat(name || uidvalidity || uidnext, ',') FROM (SELECT * FROM folder ORDER BY id)"), folders_before);
}

PB_TEST(only_messages_inside_the_window_are_downloaded) {
    Env env;
    FakeImapServer srv(env.config());
    fill(srv);
    const int64_t old = kNow - 90ll * 86400;
    srv.add_message("INBOX", mail(99, old), {}, old);   // uid 13, 90 days old
    auto store = pbo::Store::open(env.dir.path(), env.clock);
    auto acc = account();
    acc.imap_port = srv.port();
    PB_CHECK(store.value->save_mail_account(acc).ok());
    auto transport = pbo::make_curl_imap_transport(env.pki.ca_cert());
    pbo::SyncDeps deps{*store.value, *transport, env.clock, env.cancel};
    pbo::Credentials creds;
    creds.mail_password = "pw-for-tests";
    PB_CHECK(pbo::run_sync(deps, creds).first_problem.ok());
    PB_CHECK_EQ(store.value->debug_query("SELECT count(*) FROM message"), std::string("12"));
    PB_CHECK_EQ(store.value->debug_query("SELECT count(*) FROM message WHERE subject = 'Subject 99'"), std::string("0"));
}

PB_TEST(a_wrong_password_is_recorded_and_local_data_stays) {
    Env env;
    FakeImapServer srv(env.config());
    fill(srv);
    auto store = pbo::Store::open(env.dir.path(), env.clock);
    auto acc = account();
    acc.imap_port = srv.port();
    PB_CHECK(store.value->save_mail_account(acc).ok());
    auto transport = pbo::make_curl_imap_transport(env.pki.ca_cert());
    pbo::SyncDeps deps{*store.value, *transport, env.clock, env.cancel};
    pbo::Credentials good;
    good.mail_password = "pw-for-tests";
    PB_CHECK(pbo::run_sync(deps, good).first_problem.ok());

    pbo::Credentials bad;
    bad.mail_password = "nope";
    const auto report = pbo::run_sync(deps, bad);
    PB_CHECK(!report.first_problem.ok());
    PB_CHECK_EQ(report.first_problem.key, std::string("auth_failed"));
    PB_CHECK(report.outcome == pbo::SyncOutcome::Failed);
    PB_CHECK_EQ(report.problem_area, std::string("mail"));
    PB_CHECK_EQ(store.value->debug_query("SELECT count(*) FROM message"), std::string("12"));   // nothing lost
    const pbo::SyncState state = store.value->sync_state();
    PB_CHECK_EQ(state.outcome, std::string("failed"));
    PB_CHECK_EQ(state.problem_key, std::string("auth_failed"));
    PB_CHECK_EQ(state.problem_area, std::string("mail"));
}

PB_TEST(no_account_means_nothing_to_do) {
    Env env;
    auto store = pbo::Store::open(env.dir.path(), env.clock);
    auto transport = pbo::make_curl_imap_transport(env.pki.ca_cert());
    pbo::SyncDeps deps{*store.value, *transport, env.clock, env.cancel};
    const auto report = pbo::run_sync(deps, pbo::Credentials{});
    PB_CHECK(report.first_problem.ok());
    PB_CHECK(report.outcome == pbo::SyncOutcome::Success);
    bool mail_ran = false;
    for (const auto& s : report.steps)
        if (s.name == "mail_pull") mail_ran = s.ran;
    PB_CHECK(!mail_ran);
}

PB_TEST(the_step_list_has_its_final_shape) {
    const auto& names = pbo::sync_step_names();
    const std::vector<std::string> want = {"outbox", "mail_push", "mail_pull", "calendar_push", "calendar_pull", "contacts_push", "contacts_pull", "finish"};
    PB_CHECK(names == want);
}

PB_TEST(a_cancel_request_stops_between_commands_and_keeps_what_was_committed) {
    Env env;
    FakeImapServer srv(env.config());
    fill(srv);
    auto store = pbo::Store::open(env.dir.path(), env.clock);
    auto acc = account();
    acc.imap_port = srv.port();
    PB_CHECK(store.value->save_mail_account(acc).ok());
    struct CancelNow : pbo::Cancel {
        bool requested() const override { return true; }
    } cancel;
    auto transport = pbo::make_curl_imap_transport(env.pki.ca_cert());
    pbo::SyncDeps deps{*store.value, *transport, env.clock, cancel};
    pbo::Credentials creds;
    creds.mail_password = "pw-for-tests";
    const auto report = pbo::run_sync(deps, creds);
    PB_CHECK(report.outcome == pbo::SyncOutcome::Cancelled);
    PB_CHECK(store.value->check_integrity().ok());
    PB_CHECK(store.value->sync_state().outcome != "running");
}

PB_TEST_MAIN()
