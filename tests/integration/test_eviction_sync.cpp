// T167/T173: the sync keeps the store below the storage limit: after the pull the oldest mail outside the window is removed, mail with local work is
// not, and a limit that cannot be met by removing old mail is reported. UNVERIFIED against real servers (fake server).
#include "mail_env.h"

PBO_FAKE_SERVER_TEST();

using pbtest::MailEnv;

PB_TEST(old_mail_goes_when_the_store_is_above_its_limit_and_new_mail_and_local_work_stay) {
    MailEnv e;
    // 40 old messages with bodies, a few recent ones; first a wide window so that everything is downloaded
    for (int i = 0; i < 40; ++i) e.add("INBOX", {}, 100 + i, true);
    for (int i = 0; i < 3; ++i) e.add("INBOX", {}, 2, true);
    e.store->debug_exec("UPDATE account_mail SET sync_days = 365");
    PB_CHECK(e.sync().outcome == pbo::SyncOutcome::Success);
    PB_CHECK_EQ(e.q("SELECT count(*) FROM message"), std::string("43"));
    const int64_t keep = e.id_of("INBOX", 1);                          // an old message with a change that the server refused
    PB_CHECK(e.store->set_flags_local(keep, pbo::kFlagSeen, 0).ok());
    PB_CHECK(e.store->set_message_error(keep, "refused").ok());       // waits for the user: protected (a change that the sync would send is synced afterwards and may go)
    // the window is 30 days and the limit is tiny; the old mail is held although it is outside the window (as after "load older"), so the shortening
    // of the window itself (test_window_change) is treated as done
    e.store->debug_exec("UPDATE account_mail SET sync_days = 30");
    PB_CHECK(e.store->set_setting("window_days_applied", "30").ok());
    e.storage_limit_override = 1000;                                    // the account's limit cannot be below 20 MiB
    const auto report = e.sync();
    PB_CHECK(report.evicted > 0);
    PB_CHECK_EQ(e.q("SELECT count(*) FROM message WHERE uid > 40"), std::string("3"));      // the recent ones
    PB_CHECK_EQ(e.q("SELECT count(*) FROM message WHERE uid = 1"), std::string("1"));       // the old one with a change waiting is protected
    PB_CHECK(report.storage_over);                                                         // the limit of 1000 bytes cannot be met
}

PB_TEST(a_comfortable_limit_removes_nothing) {
    MailEnv e;
    for (int i = 0; i < 5; ++i) e.add("INBOX", {}, 100 + i, true);
    e.store->debug_exec("UPDATE account_mail SET sync_days = 365");
    PB_CHECK(e.sync().outcome == pbo::SyncOutcome::Success);
    e.store->debug_exec("UPDATE account_mail SET sync_days = 30");
    PB_CHECK(e.store->set_setting("window_days_applied", "30").ok());   // old mail held outside the window, as after "load older"
    const auto report = e.sync();
    PB_CHECK_EQ(report.evicted, 0);
    PB_CHECK(!report.storage_over);
    PB_CHECK_EQ(e.q("SELECT count(*) FROM message"), std::string("5"));
}

PB_TEST_MAIN()
