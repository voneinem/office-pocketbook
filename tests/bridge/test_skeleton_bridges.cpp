// T047: the Qt bridges of the walking skeleton with the real core and the in-process fake IMAP server:
// AccountBridge (test / save / protection state), SyncBridge (runs on the worker thread, signals on the UI thread),
// MessageListModel (paged rows). UNVERIFIED against real servers (fake-server evidence only, Principle IX).
#include <QCoreApplication>
#include <QSignalSpy>
#include <QTest>
#include <QThread>
#include <QVariantMap>
#include <thread>

#include "account_bridge.h"
#include "fake_imap_server.h"
#include "fake_smtp_server.h"
#include "message_list_model.h"
#include "pbo/credentials_file.h"
#include "sync_bridge.h"
#include "test_support.h"
#include "tls_test_server.h"
#include "tmp_dir.h"
#include "worker.h"

using pbtest::FakeImapServer;
using pbtest::FakeSmtpServer;

PBO_FAKE_SERVER_TEST();

namespace {

// The Qt application object exists before main() starts the tests (the macro's main() cannot create it).
int g_argc = 1;
char g_arg0[] = "test_skeleton_bridges";
char* g_argv[] = {g_arg0, nullptr};
QCoreApplication g_app(g_argc, g_argv);

constexpr int64_t kNow = 1791979200;   // 2026-10-14 12:00:00 UTC

struct FixedClock : pbo::Clock {
    int64_t now() const override { return kNow; }
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
           std::to_string(i) + "\r\nDate: " + rfc_date(epoch) + "\r\nMessage-ID: <m" + std::to_string(i) + "@example.org>\r\n\r\nBody\r\n";
}

struct Env {
    pbtest::TmpDir data;
    pbtest::TmpDir pki_dir;
    pbtest::TestPki pki{pki_dir};
    pbtest::TestPki::Leaf leaf;
    FixedClock clock;
    std::unique_ptr<FakeImapServer> server;
    std::unique_ptr<FakeSmtpServer> smtp;
    std::unique_ptr<pbo::Store> store;
    std::unique_ptr<Worker> worker;

    Env() {
        if (!pki.available()) throw pbtest::Skip{"the openssl command is not available"};
        leaf = pki.issue("srv", "DNS:localhost,IP:127.0.0.1");
        if (!leaf.ok) throw pbtest::Skip{"cannot create a test certificate"};
        FakeImapServer::Config c;
        c.cert = leaf.cert;
        c.key = leaf.key;
        server = std::make_unique<FakeImapServer>(c);
        FakeSmtpServer::Config sc;
        sc.cert = leaf.cert;
        sc.key = leaf.key;
        smtp = std::make_unique<FakeSmtpServer>(sc);
        server->set_capabilities("IMAP4rev1 UIDPLUS LITERAL+");
        server->add_folder("INBOX");
        for (int i = 1; i <= 12; ++i) {
            const int64_t when = kNow - (13 - i) * 86400 + 3600;
            server->add_message("INBOX", mail(i, when), i % 3 == 0 ? std::set<std::string>{"\\Seen"} : std::set<std::string>{}, when);
        }
        auto opened = pbo::Store::open(data.path(), clock);
        store = std::move(opened.value);
        worker = std::make_unique<Worker>();
    }
    AccountBridge::Config account_config() {
        AccountBridge::Config c;
        c.store = store.get();
        c.data_dir = data.path();
        c.ca_file = pki.ca_cert();
        c.worker = worker.get();
        return c;
    }
    SyncBridge::Config sync_config() {
        SyncBridge::Config c;
        c.store = store.get();
        c.data_dir = data.path();
        c.ca_file = pki.ca_cert();
        c.worker = worker.get();
        c.clock = &clock;
        return c;
    }
    QVariantMap form(const QString& password = "pw-for-tests") const {
        QVariantMap m;
        m["imapHost"] = "localhost";
        m["imapPort"] = server->port();
        m["imapSecurity"] = "tls";
        m["smtpHost"] = "localhost";
        m["smtpPort"] = smtp->port();
        m["smtpSecurity"] = "tls";
        m["username"] = "alice";
        m["password"] = password;
        m["address"] = "alice@example.org";
        m["displayName"] = "Alice Example";
        return m;
    }
};

// Waits until the spy has at least `count` signals.
bool wait_for(QSignalSpy& spy, int count = 1, int ms = 20000) {
    while (spy.count() < count)
        if (!spy.wait(ms)) return false;
    return true;
}

}  // namespace

PB_TEST(the_worker_runs_jobs_on_its_own_thread_and_in_order) {
    Worker w;
    std::vector<int> seen;
    std::thread::id worker_id;
    for (int i = 0; i < 5; ++i) w.post([&, i] {
        seen.push_back(i);
        worker_id = std::this_thread::get_id();
    });
    w.wait_idle();
    PB_CHECK_EQ(seen.size(), static_cast<size_t>(5));
    for (int i = 0; i < 5; ++i) PB_CHECK_EQ(seen[static_cast<size_t>(i)], i);
    PB_CHECK(worker_id != std::this_thread::get_id());
}

PB_TEST(test_mail_reports_ok_and_does_not_save_anything) {
    Env env;
    AccountBridge bridge(env.account_config());
    QSignalSpy spy(&bridge, &AccountBridge::testResult);
    bridge.testMail(env.form());
    PB_CHECK(wait_for(spy));
    PB_CHECK_EQ(spy.at(0).at(0).toString().toStdString(), std::string("mail"));
    PB_CHECK_EQ(spy.at(0).at(1).toString().toStdString(), std::string("ok"));
    PB_CHECK(!bridge.mailConfigured());
    PB_CHECK(!env.store->mail_account().has_value());
    PB_CHECK(!env.data.exists("credentials"));
}

PB_TEST(test_mail_reports_a_wrong_password_and_invalid_input_with_keys) {
    Env env;
    AccountBridge bridge(env.account_config());
    QSignalSpy spy(&bridge, &AccountBridge::testResult);
    bridge.testMail(env.form("wrong-password"));
    PB_CHECK(wait_for(spy));
    PB_CHECK_EQ(spy.at(0).at(1).toString().toStdString(), std::string("auth_failed"));

    QVariantMap bad = env.form();
    bad["imapHost"] = "evil host/INBOX";
    bridge.testMail(bad);
    PB_CHECK(wait_for(spy, 2));
    PB_CHECK_EQ(spy.last().at(1).toString().toStdString(), std::string("invalid_input"));
    PB_CHECK_EQ(spy.last().at(2).toString().toStdString(), std::string("imap_host:host_invalid"));
}

PB_TEST(test_mail_names_the_outgoing_server_when_only_it_fails) {
    Env env;
    AccountBridge bridge(env.account_config());
    QSignalSpy spy(&bridge, &AccountBridge::testResult);
    QVariantMap f = env.form();
    f["smtpPort"] = 1;   // nothing listens there
    bridge.testMail(f);
    PB_CHECK(wait_for(spy));
    PB_CHECK_EQ(spy.at(0).at(1).toString().toStdString(), std::string("server_unreachable"));
    PB_CHECK_EQ(spy.at(0).at(2).toString().toStdString(), std::string("smtp"));
}

PB_TEST(save_mail_stores_the_account_and_the_password_separately) {
    Env env;
    AccountBridge bridge(env.account_config());
    PB_CHECK(!bridge.mailConfigured());
    QSignalSpy saved(&bridge, &AccountBridge::saveResult);
    QSignalSpy changed(&bridge, &AccountBridge::mailConfiguredChanged);
    bridge.saveMail(env.form());
    PB_CHECK_EQ(saved.count(), 1);
    PB_CHECK(saved.at(0).at(0).toBool());
    PB_CHECK(bridge.mailConfigured());
    PB_CHECK_EQ(changed.count(), 1);
    auto acc = env.store->mail_account();
    PB_CHECK(acc.has_value());
    PB_CHECK_EQ(acc->imap_host, std::string("localhost"));
    PB_CHECK_EQ(acc->address, std::string("alice@example.org"));
    PB_CHECK_EQ(env.store->debug_query("SELECT count(*) FROM account_mail WHERE imap_host LIKE '%pw-for-tests%' OR username LIKE '%pw-for-tests%'"), std::string("0"));
    PB_CHECK_EQ(pbo::load_credentials(env.data.path()).value.mail_password, std::string("pw-for-tests"));
    PB_CHECK_EQ(bridge.protectionState().toStdString(), std::string("enforced"));   // /tmp keeps the mode
    // the form for pre-fill has everything but the password
    QVariantMap form = bridge.mailForm();
    PB_CHECK_EQ(form.value("imapHost").toString().toStdString(), std::string("localhost"));
    PB_CHECK(!form.contains("password"));
}

PB_TEST(save_mail_with_a_rejected_value_changes_nothing) {
    Env env;
    AccountBridge bridge(env.account_config());
    bridge.saveMail(env.form());
    QSignalSpy saved(&bridge, &AccountBridge::saveResult);
    QVariantMap bad = env.form("other-password");
    bad["imapPort"] = 70000;
    bridge.saveMail(bad);
    PB_CHECK_EQ(saved.count(), 1);
    PB_CHECK(!saved.at(0).at(0).toBool());
    PB_CHECK_EQ(saved.at(0).at(1).toString().toStdString(), std::string("imap_port"));
    PB_CHECK_EQ(saved.at(0).at(2).toString().toStdString(), std::string("port_invalid"));
    PB_CHECK_EQ(pbo::load_credentials(env.data.path()).value.mail_password, std::string("pw-for-tests"));   // unchanged
    PB_CHECK_EQ(env.store->mail_account()->imap_port, env.server->port());
}

PB_TEST(sync_now_runs_on_the_worker_and_signals_on_the_ui_thread) {
    Env env;
    AccountBridge account(env.account_config());
    account.saveMail(env.form());
    SyncBridge sync(env.sync_config());
    PB_CHECK(!sync.syncing());
    QSignalSpy finished(&sync, &SyncBridge::syncFinished);
    QSignalSpy busy(&sync, &SyncBridge::syncingChanged);
    sync.syncNow();
    PB_CHECK(sync.syncing());               // set at once, before the worker has done anything
    sync.syncNow();                         // a second tap while running is ignored
    PB_CHECK(wait_for(finished));
    PB_CHECK_EQ(finished.count(), 1);
    PB_CHECK_EQ(finished.at(0).at(0).toString().toStdString(), std::string("success"));
    PB_CHECK(!sync.syncing());
    PB_CHECK_EQ(busy.count(), 2);           // true, then false
    PB_CHECK(sync.lastSyncText().size() > 0);
    PB_CHECK(sync.problemKey().isEmpty());
    PB_CHECK_EQ(env.store->debug_query("SELECT count(*) FROM message"), std::string("12"));
}

PB_TEST(older_mail_is_fetched_on_demand_and_the_result_is_signalled) {
    Env env;
    // arrival order = UID order: the 70 old messages come first, the 12 recent ones are delivered again after them
    for (uint32_t uid = 1; uid <= 12; ++uid) env.server->expunge("INBOX", uid);
    for (int i = 0; i < 70; ++i) env.server->add_message("INBOX", mail(100 + i, kNow - (130 - i) * 86400), {}, kNow - (130 - i) * 86400);
    for (int i = 1; i <= 12; ++i) {
        const int64_t when = kNow - (13 - i) * 86400 + 3600;
        env.server->add_message("INBOX", mail(i, when), {}, when);
    }
    AccountBridge account(env.account_config());
    account.saveMail(env.form());
    SyncBridge sync(env.sync_config());
    QSignalSpy finished(&sync, &SyncBridge::syncFinished);
    sync.syncNow();
    PB_CHECK(wait_for(finished));
    PB_CHECK_EQ(env.store->debug_query("SELECT count(*) FROM message"), std::string("12"));
    const qlonglong inbox = std::stoll(env.store->debug_query("SELECT id FROM folder WHERE role = 'inbox'"));

    QSignalSpy older(&sync, &SyncBridge::olderFinished);
    sync.fetchOlder(inbox);
    PB_CHECK(sync.syncing());               // one thing at a time
    sync.fetchOlder(inbox);                 // ignored while running
    PB_CHECK(wait_for(older));
    PB_CHECK_EQ(older.count(), 1);
    PB_CHECK_EQ(older.at(0).at(0).toString().toStdString(), std::string("ok"));
    PB_CHECK_EQ(older.at(0).at(1).toInt(), 50);
    PB_CHECK(!sync.syncing());
    PB_CHECK_EQ(env.store->debug_query("SELECT count(*) FROM message"), std::string("62"));
    PB_CHECK_EQ(env.store->debug_query("SELECT older_on_server FROM folder WHERE role = 'inbox'"), std::string("1"));

    sync.fetchOlder(inbox);
    PB_CHECK(wait_for(older, 2));
    PB_CHECK_EQ(older.at(1).at(1).toInt(), 20);
    PB_CHECK_EQ(env.store->debug_query("SELECT older_on_server FROM folder WHERE role = 'inbox'"), std::string("0"));
}

PB_TEST(fetching_older_mail_names_the_cause_when_it_fails) {
    Env env;
    AccountBridge account(env.account_config());
    account.saveMail(env.form("wrong-password"));
    SyncBridge sync(env.sync_config());
    pbo::FolderListItem inbox;
    inbox.raw_name = inbox.name = "INBOX";
    env.store->upsert_folders({inbox});
    QSignalSpy older(&sync, &SyncBridge::olderFinished);
    sync.fetchOlder(std::stoll(env.store->debug_query("SELECT id FROM folder")));
    PB_CHECK(wait_for(older));
    PB_CHECK_EQ(older.at(0).at(0).toString().toStdString(), std::string("auth_failed"));
    PB_CHECK_EQ(older.at(0).at(1).toInt(), 0);
}

PB_TEST(the_connection_state_and_the_unsynced_count_are_properties) {
    Env env;
    SyncBridge sync(env.sync_config());
    QSignalSpy online(&sync, &SyncBridge::onlineChanged);
    PB_CHECK(!sync.online());
    sync.setOnline(true);
    sync.setOnline(true);                   // no change, no signal
    PB_CHECK(sync.online());
    PB_CHECK_EQ(online.count(), 1);
    PB_CHECK_EQ(sync.pendingCount(), 0);
    AccountBridge account(env.account_config());
    account.saveMail(env.form());
    QSignalSpy finished(&sync, &SyncBridge::syncFinished);
    sync.syncNow();
    PB_CHECK(wait_for(finished));
    env.store->debug_exec("UPDATE message SET local_state = 'pending_flags' WHERE uid IN (1, 2)");
    sync.refreshState();                    // after a local change
    PB_CHECK_EQ(sync.pendingCount(), 2);
}

// FR-030: a sync runs when the app starts and a connection exists; otherwise nothing is started and nothing is asked.
PB_TEST(the_app_start_syncs_once_when_an_account_exists_and_wifi_is_connected) {
    Env env;
    AccountBridge account(env.account_config());
    SyncBridge sync(env.sync_config());
    QSignalSpy finished(&sync, &SyncBridge::syncFinished);

    PB_CHECK(!sync.startupSync(true));                       // no account yet: nothing to do
    PB_CHECK(!sync.syncing());
    account.saveMail(env.form());
    PB_CHECK(!sync.startupSync(true));                       // the start-up chance is used up by the first call
}

PB_TEST(the_app_start_syncs_when_there_is_an_account_and_a_connection) {
    Env env;
    AccountBridge account(env.account_config());
    account.saveMail(env.form());
    SyncBridge sync(env.sync_config());
    QSignalSpy finished(&sync, &SyncBridge::syncFinished);
    PB_CHECK(sync.startupSync(true));
    PB_CHECK(sync.syncing());
    PB_CHECK(wait_for(finished));
    PB_CHECK_EQ(finished.at(0).at(0).toString().toStdString(), std::string("success"));
    PB_CHECK_EQ(env.store->debug_query("SELECT count(*) FROM message"), std::string("12"));
    PB_CHECK(!sync.startupSync(true));                       // only once per start of the app
    PB_CHECK_EQ(finished.count(), 1);
    // SC-011: when it is finished and nothing is asked, the app makes no connection
    const int connections = env.server->connections();
    QTest::qWait(600);
    PB_CHECK_EQ(env.server->connections(), connections);
}

PB_TEST(the_app_start_does_not_sync_without_a_connection_and_asks_for_none) {
    Env env;
    AccountBridge account(env.account_config());
    account.saveMail(env.form());
    SyncBridge sync(env.sync_config());
    PB_CHECK(!sync.startupSync(false));                      // Wi-Fi is off: no sync, no connection attempt
    PB_CHECK(!sync.syncing());
    QTest::qWait(300);
    PB_CHECK_EQ(env.server->connections(), 0);
    PB_CHECK_EQ(env.store->sync_state().outcome, std::string("never"));
    PB_CHECK(!sync.startupSync(true));                       // no second chance later in the same run
}

// The network is often not usable yet when Wi-Fi first shows "connected": a start-up sync that ends offline is tried once more.
PB_TEST(the_app_start_sync_that_ends_offline_is_tried_once_more_and_then_succeeds) {
    Env env;
    AccountBridge account(env.account_config());
    QVariantMap bad = env.form();
    bad["imapPort"] = 1;                                     // nothing listens there: server_unreachable -> offline
    account.saveMail(bad);
    SyncBridge::Config c = env.sync_config();
    c.startup_retry_ms = 200;
    SyncBridge sync(c);
    QSignalSpy finished(&sync, &SyncBridge::syncFinished);
    PB_CHECK(sync.startupSync(true));
    PB_CHECK(wait_for(finished));
    PB_CHECK_EQ(finished.at(0).at(0).toString().toStdString(), std::string("offline"));
    account.saveMail(env.form());                            // the network is usable now
    PB_CHECK(wait_for(finished, 2));
    PB_CHECK_EQ(finished.at(1).at(0).toString().toStdString(), std::string("success"));
    QTest::qWait(500);
    PB_CHECK_EQ(finished.count(), 2);                        // not a third time
}

PB_TEST(the_app_start_retry_happens_only_once) {
    Env env;
    AccountBridge account(env.account_config());
    QVariantMap bad = env.form();
    bad["imapPort"] = 1;
    account.saveMail(bad);
    SyncBridge::Config c = env.sync_config();
    c.startup_retry_ms = 100;
    SyncBridge sync(c);
    QSignalSpy finished(&sync, &SyncBridge::syncFinished);
    PB_CHECK(sync.startupSync(true));
    PB_CHECK(wait_for(finished, 2));
    PB_CHECK_EQ(finished.at(1).at(0).toString().toStdString(), std::string("offline"));
    QTest::qWait(600);
    PB_CHECK_EQ(finished.count(), 2);                        // the retry failed too: the user's Sync button is next
}

PB_TEST(the_app_start_retry_is_skipped_when_another_sync_reached_the_server_meanwhile) {
    Env env;
    AccountBridge account(env.account_config());
    QVariantMap bad = env.form();
    bad["imapPort"] = 1;
    account.saveMail(bad);
    SyncBridge::Config c = env.sync_config();
    c.startup_retry_ms = 400;
    SyncBridge sync(c);
    QSignalSpy finished(&sync, &SyncBridge::syncFinished);
    PB_CHECK(sync.startupSync(true));
    PB_CHECK(wait_for(finished));
    account.saveMail(env.form());
    sync.syncNow();                                          // the user presses Sync before the retry is due
    PB_CHECK(wait_for(finished, 2));
    PB_CHECK_EQ(finished.at(1).at(0).toString().toStdString(), std::string("success"));
    QTest::qWait(800);
    PB_CHECK_EQ(finished.count(), 2);
}

PB_TEST(the_app_start_without_retry_setting_does_not_retry) {
    Env env;
    AccountBridge account(env.account_config());
    QVariantMap bad = env.form();
    bad["imapPort"] = 1;
    account.saveMail(bad);
    SyncBridge::Config c = env.sync_config();
    c.startup_retry_ms = 0;
    SyncBridge sync(c);
    QSignalSpy finished(&sync, &SyncBridge::syncFinished);
    PB_CHECK(sync.startupSync(true));
    PB_CHECK(wait_for(finished));
    QTest::qWait(400);
    PB_CHECK_EQ(finished.count(), 1);
}

// E1 / FR-016: the first save sets the "sent copy" switch from the provider; a later save of the form keeps what the user chose elsewhere.
PB_TEST(saving_the_account_applies_the_provider_preset_once_and_keeps_later_choices) {
    Env env;
    AccountBridge account(env.account_config());
    QVariantMap gmail = env.form();
    gmail["imapHost"] = "imap.gmail.com";
    gmail["smtpHost"] = "smtp.gmail.com";
    QSignalSpy saved(&account, &AccountBridge::saveResult);
    account.saveMail(gmail);
    PB_CHECK(saved.at(0).at(0).toBool());
    PB_CHECK_EQ(env.store->mail_account()->sent_by_server, true);
    // the user switches it off and changes the days of mail kept (Settings); the form is saved again
    auto a = *env.store->mail_account();
    a.sent_by_server = false;
    a.sync_days = 90;
    env.store->save_mail_account(a);
    account.saveMail(gmail);
    PB_CHECK(saved.at(1).at(0).toBool());
    PB_CHECK_EQ(env.store->mail_account()->sent_by_server, false);
    PB_CHECK_EQ(env.store->mail_account()->sync_days, 90);
    // another provider starts with the switch off
    pbtest::TmpDir other;
    auto opened = pbo::Store::open(other.path(), env.clock);
    AccountBridge::Config c = env.account_config();
    c.store = opened.value.get();
    c.data_dir = other.path();
    AccountBridge second(c);
    second.saveMail(env.form());
    PB_CHECK_EQ(opened.value->mail_account()->sent_by_server, false);
}

PB_TEST(sync_now_without_an_account_says_so) {
    Env env;
    SyncBridge sync(env.sync_config());
    QSignalSpy finished(&sync, &SyncBridge::syncFinished);
    sync.syncNow();
    PB_CHECK(wait_for(finished));
    PB_CHECK_EQ(finished.at(0).at(0).toString().toStdString(), std::string("not_configured"));
    PB_CHECK(!sync.syncing());
}

PB_TEST(a_failed_sync_shows_its_cause_key_and_keeps_the_data) {
    Env env;
    AccountBridge account(env.account_config());
    account.saveMail(env.form());
    SyncBridge sync(env.sync_config());
    QSignalSpy finished(&sync, &SyncBridge::syncFinished);
    sync.syncNow();
    PB_CHECK(wait_for(finished));
    pbo::Credentials wrong;
    wrong.mail_password = "changed-on-the-server";
    PB_CHECK(pbo::save_credentials(env.data.path(), wrong).ok());
    sync.syncNow();
    PB_CHECK(wait_for(finished, 2));
    PB_CHECK_EQ(finished.at(1).at(0).toString().toStdString(), std::string("failed"));
    PB_CHECK_EQ(sync.problemKey().toStdString(), std::string("auth_failed"));
    PB_CHECK_EQ(env.store->debug_query("SELECT count(*) FROM message"), std::string("12"));
}

PB_TEST(the_message_list_exposes_rows_and_pages) {
    Env env;
    AccountBridge account(env.account_config());
    account.saveMail(env.form());
    SyncBridge sync(env.sync_config());
    QSignalSpy finished(&sync, &SyncBridge::syncFinished);
    sync.syncNow();
    PB_CHECK(wait_for(finished));

    MessageListModel::Config cfg;
    cfg.store = env.store.get();
    MessageListModel model(cfg);
    PB_CHECK(model.rows().isEmpty());
    model.setPageSize(5);
    model.openInbox();
    PB_CHECK_EQ(model.total(), 12);
    PB_CHECK_EQ(model.pageCount(), 3);
    PB_CHECK_EQ(model.page(), 0);
    PB_CHECK_EQ(model.rows().size(), 5);
    const QVariantMap first = model.rows().at(0).toMap();
    PB_CHECK_EQ(first.value("sender").toString().toStdString(), std::string("Sender 12"));
    PB_CHECK_EQ(first.value("subject").toString().toStdString(), std::string("Subject 12"));
    PB_CHECK(!first.value("unread").toBool());   // message 12 is read
    PB_CHECK(model.rows().at(1).toMap().value("unread").toBool());
    PB_CHECK(!first.value("dateText").toString().isEmpty());
    PB_CHECK(!first.value("hasAttachments").toBool());
    PB_CHECK_EQ(model.folderName().toStdString(), std::string("INBOX"));
    PB_CHECK_EQ(model.folderRole().toStdString(), std::string("inbox"));
    PB_CHECK(model.folderId() != 0);
    PB_CHECK(!model.olderOnServer());       // the window holds everything the server has

    model.goToPage(2);
    PB_CHECK_EQ(model.page(), 2);
    PB_CHECK_EQ(model.rows().size(), 2);
    PB_CHECK_EQ(model.rows().at(1).toMap().value("subject").toString().toStdString(), std::string("Subject 1"));
    model.goToPage(99);                     // out of range: the last page
    PB_CHECK_EQ(model.page(), 2);
    model.goToPage(-4);
    PB_CHECK_EQ(model.page(), 0);
    PB_CHECK(!model.empty());

    env.store->debug_exec("UPDATE folder SET older_on_server = 1");
    model.refresh();
    PB_CHECK(model.olderOnServer());        // "load older messages" is offered
    model.open(987654);                     // a folder that does not exist: empty, not a crash
    PB_CHECK(model.empty());
    PB_CHECK(model.folderName().isEmpty());
}

PB_TEST(an_empty_folder_is_an_explicit_empty_state) {
    Env env;
    MessageListModel::Config cfg;
    cfg.store = env.store.get();
    MessageListModel model(cfg);
    model.openInbox();                      // no folders yet
    PB_CHECK(model.empty());
    PB_CHECK_EQ(model.pageCount(), 1);
    PB_CHECK(model.errorKey().isEmpty());
}

PB_TEST_MAIN()
