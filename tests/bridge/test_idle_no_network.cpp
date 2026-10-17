// T166 (SC-011): an idle app does not use the network. With the app open and the automatic sync of a change finished, the servers see no further
// connection attempt; with Auto Sync off a change sends nothing until the user syncs. UNVERIFIED against real servers (fake server).
#include <QCoreApplication>
#include <QSignalSpy>
#include <QTest>

#include "mail_env.h"
#include "sync_bridge.h"
#include "worker.h"

PBO_FAKE_SERVER_TEST();

namespace {

int g_argc = 1;
char g_arg0[] = "test_idle_no_network";
char* g_argv[] = {g_arg0, nullptr};
QCoreApplication g_app(g_argc, g_argv);

SyncBridge::Config bridge_config(pbtest::MailEnv& e, Worker& w, int quiet_ms) {
    SyncBridge::Config c;
    c.store = e.store.get();
    c.data_dir = e.dir.path();
    c.ca_file = e.pki.ca_cert();
    c.worker = &w;
    c.clock = &e.clock;
    c.startup_retry_ms = 0;
    c.auto_sync_quiet_ms = quiet_ms;
    c.auto_sync_gap_ms = 0;
    return c;
}

}  // namespace

PB_TEST(after_the_automatic_sync_of_a_change_the_servers_see_nothing_more) {
    pbtest::MailEnv e;
    e.add();
    e.sync();                                             // the message is on the reader
    pbo::Credentials creds;
    creds.mail_password = "pw-for-tests";
    pbo::save_credentials(e.dir.path(), creds);
    Worker worker;
    SyncBridge sync(bridge_config(e, worker, 50));
    QSignalSpy finished(&sync, &SyncBridge::syncFinished);
    PB_CHECK(e.store->set_flags_local(e.id_of("INBOX", 1), pbo::kFlagSeen, 0).ok());
    sync.noteChange(true);
    PB_CHECK(finished.wait(20000));
    PB_CHECK_EQ(finished.count(), 1);
    PB_CHECK(e.imap->flags_of("INBOX", 1).count("\\Seen") == 1);   // the change did reach the server
    const int connections = e.imap->connections();
    QTest::qWait(1500);                                              // the app stays open and idle
    PB_CHECK_EQ(e.imap->connections(), connections);
    PB_CHECK_EQ(finished.count(), 1);
}

PB_TEST(with_auto_sync_off_a_change_sends_nothing_until_the_user_syncs) {
    pbtest::MailEnv e;
    e.add();
    e.sync();
    pbo::Credentials creds;
    creds.mail_password = "pw-for-tests";
    pbo::save_credentials(e.dir.path(), creds);
    e.store->set_setting("auto_sync", "0");
    Worker worker;
    SyncBridge sync(bridge_config(e, worker, 50));
    QSignalSpy finished(&sync, &SyncBridge::syncFinished);
    const int connections = e.imap->connections();
    PB_CHECK(e.store->set_flags_local(e.id_of("INBOX", 1), pbo::kFlagSeen, 0).ok());
    sync.noteChange(true);
    QTest::qWait(800);
    PB_CHECK_EQ(e.imap->connections(), connections);
    PB_CHECK(e.imap->flags_of("INBOX", 1).count("\\Seen") == 0);
    sync.syncNow();                                                  // the user's own sync
    PB_CHECK(finished.wait(20000));
    PB_CHECK(e.imap->flags_of("INBOX", 1).count("\\Seen") == 1);
}

PB_TEST_MAIN()
