// The sync that follows a change the user made on the reader (flag, move, delete, event ...): SyncBridge::noteChange. The real bridge, the real
// scheduler, a throw-away store; the account points to a port where nothing listens, so a sync starts and ends offline - which is all this test needs.
#include <QCoreApplication>
#include <QSignalSpy>
#include <QTest>

#include "pbo/store.h"
#include "pbo/vcard.h"
#include "sync_bridge.h"
#include "test_support.h"
#include "tmp_dir.h"
#include "worker.h"

namespace {

int g_argc = 1;
char g_arg0[] = "test_auto_sync";
char* g_argv[] = {g_arg0, nullptr};
QCoreApplication g_app(g_argc, g_argv);

struct Clock : pbo::Clock {
    int64_t now() const override { return 1791979200; }
};

struct Env {
    pbtest::TmpDir dir;
    Clock clock;
    std::unique_ptr<pbo::Store> store;
    Worker worker;
    int64_t inbox = 0;
    Env() {
        store = std::move(pbo::Store::open(dir.path(), clock).value);
        pbo::MailAccount a;
        a.imap_host = "localhost";
        a.imap_port = 1;   // nothing listens here
        a.smtp_host = "localhost";
        a.username = "alice";
        a.address = "alice@example.org";
        store->save_mail_account(a);
        pbo::FolderListItem i;
        i.raw_name = i.name = "INBOX";
        store->upsert_folders({i});
        inbox = store->folders()[0].id;
        pbo::PullBatch b;
        pbo::PulledMessage m;
        m.uid = 1;
        m.subject = "S";
        m.date_utc = 1790000000;
        b.messages.push_back(m);
        store->apply_pull(inbox, b);
    }
    SyncBridge::Config config(int quiet_ms = 40, int gap_ms = 0) {
        SyncBridge::Config c;
        c.store = store.get();
        c.data_dir = dir.path();
        c.ca_file = "/nonexistent";
        c.worker = &worker;
        c.clock = &clock;
        c.startup_retry_ms = 0;
        c.auto_sync_quiet_ms = quiet_ms;
        c.auto_sync_gap_ms = gap_ms;
        return c;
    }
    int64_t message() { return std::atoll(store->debug_query("SELECT id FROM message").c_str()); }
    void change() { PB_CHECK(store->set_flags_local(message(), pbo::kFlagSeen, 0).ok()); }
};

}  // namespace

PB_TEST(a_change_made_on_the_reader_starts_a_sync_shortly_afterwards_when_wifi_is_connected) {
    Env e;
    SyncBridge sync(e.config());
    QSignalSpy finished(&sync, &SyncBridge::syncFinished);
    e.change();
    sync.noteChange(true);
    PB_CHECK_EQ(finished.count(), 0);                       // not at once: changes close together share a sync
    PB_CHECK(finished.wait(3000));
    PB_CHECK_EQ(finished.count(), 1);
}

PB_TEST(changes_close_together_share_one_sync) {
    Env e;
    SyncBridge sync(e.config(120));
    QSignalSpy finished(&sync, &SyncBridge::syncFinished);
    e.change();
    sync.noteChange(true);
    QTest::qWait(60);
    sync.noteChange(true);
    QTest::qWait(60);
    sync.noteChange(true);
    PB_CHECK(finished.wait(3000));
    QTest::qWait(400);
    PB_CHECK_EQ(finished.count(), 1);
}

PB_TEST(offline_the_device_is_asked_to_connect_once_and_the_sync_follows_when_it_did) {
    Env e;
    SyncBridge sync(e.config());
    QSignalSpy finished(&sync, &SyncBridge::syncFinished);
    QSignalSpy wanted(&sync, &SyncBridge::connectRequested);
    e.change();
    sync.noteChange(false);
    PB_CHECK(wanted.wait(3000));
    PB_CHECK_EQ(wanted.count(), 1);
    PB_CHECK_EQ(finished.count(), 0);                       // not before the connection is there
    sync.connectResult(true);
    PB_CHECK(finished.wait(3000));
    PB_CHECK_EQ(finished.count(), 1);
    PB_CHECK_EQ(wanted.count(), 1);
}

PB_TEST(when_the_connection_cannot_be_made_the_change_waits_and_the_device_is_not_asked_again) {
    Env e;
    SyncBridge sync(e.config());
    QSignalSpy finished(&sync, &SyncBridge::syncFinished);
    QSignalSpy wanted(&sync, &SyncBridge::connectRequested);
    e.change();
    sync.noteChange(false);
    PB_CHECK(wanted.wait(3000));
    sync.connectResult(false);
    QTest::qWait(300);
    PB_CHECK_EQ(finished.count(), 0);
    PB_CHECK_EQ(wanted.count(), 1);
    PB_CHECK_EQ(sync.pendingCount(), 1);                    // still counted as waiting
    sync.connectResult(true);                               // an answer nobody asked for changes nothing
    QTest::qWait(300);
    PB_CHECK_EQ(finished.count(), 0);
}

PB_TEST(with_auto_sync_switched_off_nothing_starts) {
    Env e;
    e.store->set_setting("auto_sync", "0");
    SyncBridge sync(e.config());
    QSignalSpy finished(&sync, &SyncBridge::syncFinished);
    e.change();
    sync.noteChange(true);
    QTest::qWait(300);
    PB_CHECK_EQ(finished.count(), 0);
}

PB_TEST(when_nothing_is_waiting_no_sync_starts) {
    Env e;
    SyncBridge sync(e.config());
    QSignalSpy finished(&sync, &SyncBridge::syncFinished);
    sync.noteChange(true);                                   // nothing was changed
    QTest::qWait(300);
    PB_CHECK_EQ(finished.count(), 0);
}

PB_TEST(a_change_that_waits_for_the_user_does_not_keep_starting_syncs) {
    Env e;
    e.change();
    PB_CHECK(e.store->set_message_error(e.message(), "gone_on_server").ok());   // nothing for the server to do
    SyncBridge sync(e.config());
    QSignalSpy finished(&sync, &SyncBridge::syncFinished);
    sync.noteChange(true);
    QTest::qWait(300);
    PB_CHECK_EQ(finished.count(), 0);
}

PB_TEST(the_gap_after_a_sync_delays_the_next_automatic_one) {
    Env e;
    SyncBridge sync(e.config(30, 600));
    QSignalSpy finished(&sync, &SyncBridge::syncFinished);
    sync.syncNow();                                          // a sync starts now (any kind)
    PB_CHECK(finished.wait(3000));
    e.change();
    sync.noteChange(true);
    QTest::qWait(200);
    PB_CHECK_EQ(finished.count(), 1);                        // still inside the gap
    PB_CHECK(finished.wait(3000));
    PB_CHECK_EQ(finished.count(), 2);
}

PB_TEST(a_clock_that_is_off_by_more_than_ten_minutes_is_reported_with_its_direction) {
    Env e;
    SyncBridge sync(e.config());
    PB_CHECK(!sync.clockWarning());                          // nothing measured yet
    e.store->set_clock_skew(-4000);                          // the reader is behind
    PB_CHECK(sync.clockWarning());
    PB_CHECK_EQ(sync.clockSkewMinutes(), 67);
    PB_CHECK(!sync.clockAhead());
    e.store->set_clock_skew(900);
    PB_CHECK(sync.clockWarning() && sync.clockAhead());
    e.store->set_clock_skew(600);
    PB_CHECK(!sync.clockWarning());
    e.store->set_clock_skew(std::nullopt);
    PB_CHECK(!sync.clockWarning());
}

// ---- contacts wait for the server like events do (US5) ----------------------------------------------------------------------------------------

namespace {

// a contact row with the given sync state, as the sync tests of the contacts leave it
void add_waiting_contact(pbo::Store& s, const std::string& state) {
    pbo::AddressBook b;
    b.href = "/dav/addressbooks/alice/contacts/";
    b.name = "Contacts";
    b.writable = true;
    auto book = s.upsert_addressbook(b);
    PB_CHECK(book.ok());
    pbo::ContactPullBatch batch;
    batch.upserts.push_back(pbo::contact_upsert_from_card("/dav/addressbooks/alice/contacts/a.vcf", "\"e1\"", "BEGIN:VCARD\r\nVERSION:3.0\r\nUID:u-a\r\nFN:Anna Beispiel\r\nN:Beispiel;Anna;;;\r\nEND:VCARD\r\n"));
    PB_CHECK(s.apply_contact_pull(book.value, batch).ok());
    PB_CHECK(s.debug_exec("UPDATE contact SET sync_state = '" + state + "'").empty());
}

}  // namespace

PB_TEST(a_saved_changed_or_deleted_contact_counts_as_waiting_and_starts_a_sync_like_an_event) {
    for (const char* state : {"pending_create", "pending_update", "pending_delete"}) {
        Env e;
        // (the message of the fixture is synced: nothing but the contact waits)
        SyncBridge sync(e.config());
        QSignalSpy finished(&sync, &SyncBridge::syncFinished);
        PB_CHECK_EQ(sync.pendingCount(), 0);
        add_waiting_contact(*e.store, state);
        PB_CHECK_EQ(sync.pendingCount(), 1);
        sync.noteChange(true);
        PB_CHECK_EQ(finished.count(), 0);                   // a few seconds later, not at once
        PB_CHECK(finished.wait(3000));
        PB_CHECK_EQ(finished.count(), 1);
    }
}

PB_TEST(a_contact_in_conflict_or_removed_on_the_server_waits_for_the_user_not_for_a_sync) {
    for (const char* state : {"conflict", "server_deleted"}) {
        Env e;
        SyncBridge sync(e.config());
        QSignalSpy finished(&sync, &SyncBridge::syncFinished);
        add_waiting_contact(*e.store, state);
        PB_CHECK_EQ(sync.pendingCount(), 0);                // the count is of changes the next sync sends
        sync.noteChange(true);
        QTest::qWait(300);
        PB_CHECK_EQ(finished.count(), 0);
    }
}

PB_TEST(with_auto_sync_switched_off_a_waiting_contact_starts_nothing) {
    Env e;
    e.store->set_setting("auto_sync", "0");
    SyncBridge sync(e.config());
    QSignalSpy finished(&sync, &SyncBridge::syncFinished);
    add_waiting_contact(*e.store, "pending_update");
    sync.noteChange(true);
    QTest::qWait(300);
    PB_CHECK_EQ(finished.count(), 0);
    PB_CHECK_EQ(sync.pendingCount(), 1);
}

PB_TEST_MAIN()
