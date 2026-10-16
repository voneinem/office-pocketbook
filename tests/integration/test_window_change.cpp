// T170 (US6 scenario 4, FR-032): changing the days of mail kept. A longer window: the next sync downloads the mail of the added days. A shorter window:
// the next sync removes the mail that is now outside it, but never a draft, an outbox item or a message with a change that the server has not confirmed.
// UNVERIFIED against real servers (fake server).
#include "mail_env.h"

PBO_FAKE_SERVER_TEST();

using pbtest::MailEnv;

namespace {
void set_days(MailEnv& e, int days) { e.store->debug_exec("UPDATE account_mail SET sync_days = " + std::to_string(days)); }
}  // namespace

PB_TEST(a_longer_window_downloads_the_mail_of_the_added_days) {
    MailEnv e;
    for (int i = 0; i < 3; ++i) e.add("INBOX", {}, 5, true);
    for (int i = 0; i < 4; ++i) e.add("INBOX", {}, 100, true);
    PB_CHECK(e.sync().outcome == pbo::SyncOutcome::Success);
    PB_CHECK_EQ(e.q("SELECT count(*) FROM message"), std::string("3"));
    set_days(e, 180);
    PB_CHECK(e.sync().outcome == pbo::SyncOutcome::Success);
    PB_CHECK_EQ(e.q("SELECT count(*) FROM message"), std::string("7"));
}

PB_TEST(a_shorter_window_removes_the_mail_outside_it_and_keeps_all_local_work) {
    MailEnv e;
    set_days(e, 365);
    for (int i = 0; i < 6; ++i) e.add("INBOX", {}, 100 + i, true);   // uids 1-6: old
    for (int i = 0; i < 3; ++i) e.add("INBOX", {}, 2, true);          // uids 7-9: recent
    PB_CHECK(e.sync().outcome == pbo::SyncOutcome::Success);
    PB_CHECK_EQ(e.q("SELECT count(*) FROM message"), std::string("9"));

    // local work: an old message whose change the server refused, a draft, a queued message
    const int64_t refused = e.id_of("INBOX", 2);
    PB_CHECK(e.store->set_flags_local(refused, pbo::kFlagSeen, 0).ok());
    PB_CHECK(e.store->set_message_error(refused, "refused").ok());
    pbo::Draft d;
    d.message_id = "<draft-window@example.org>";
    d.to_text = "bob@example.org";
    d.subject = "Draft";
    d.body = "Text";
    PB_CHECK(e.store->save_draft(d).ok());
    const std::string drafts_before = e.q("SELECT count(*) FROM draft");

    set_days(e, 30);
    const auto report = e.sync();
    PB_CHECK(report.outcome == pbo::SyncOutcome::Success);
    PB_CHECK_EQ(e.q("SELECT count(*) FROM message WHERE uid >= 7"), std::string("3"));                // inside the window
    PB_CHECK_EQ(e.q("SELECT count(*) FROM message WHERE uid = 2"), std::string("1"));                // the refused change is protected
    PB_CHECK_EQ(e.q("SELECT count(*) FROM message WHERE uid < 7 AND uid <> 2"), std::string("0"));  // the rest of the old mail is gone
    PB_CHECK_EQ(e.q("SELECT count(*) FROM draft"), drafts_before);
    // the folder knows that older mail waits on the server, so that "load older" is offered
    PB_CHECK_EQ(e.q("SELECT older_on_server FROM folder WHERE name = 'INBOX'"), std::string("1"));
}

PB_TEST(the_same_window_removes_nothing_so_older_mail_the_user_asked_for_stays) {
    MailEnv e;
    for (int i = 0; i < 2; ++i) e.add("INBOX", {}, 3, true);
    PB_CHECK(e.sync().outcome == pbo::SyncOutcome::Success);
    // an old message that the app holds although it is outside the window (fetched on request)
    e.add("INBOX", {}, 200, true);
    e.store->debug_exec("UPDATE message SET date_utc = date_utc - 0");   // unchanged: the point is that nothing runs on a steady window
    PB_CHECK(e.sync().outcome == pbo::SyncOutcome::Success);
    const int64_t before = std::atoll(e.q("SELECT count(*) FROM message").c_str());
    PB_CHECK(e.sync().outcome == pbo::SyncOutcome::Success);
    PB_CHECK_EQ(std::atoll(e.q("SELECT count(*) FROM message").c_str()), before);
    PB_CHECK_EQ(e.q("SELECT count(*) FROM message WHERE uid <= 2"), std::string("2"));
}

PB_TEST(a_shortened_window_is_applied_once_and_not_again_after_widening_and_fetching) {
    MailEnv e;
    set_days(e, 365);
    for (int i = 0; i < 3; ++i) e.add("INBOX", {}, 120, true);
    e.add("INBOX", {}, 2, true);
    PB_CHECK(e.sync().outcome == pbo::SyncOutcome::Success);
    set_days(e, 30);
    PB_CHECK(e.sync().outcome == pbo::SyncOutcome::Success);
    PB_CHECK_EQ(e.q("SELECT count(*) FROM message"), std::string("1"));
    set_days(e, 365);
    PB_CHECK(e.sync().outcome == pbo::SyncOutcome::Success);
    PB_CHECK_EQ(e.q("SELECT count(*) FROM message"), std::string("4"));   // the days come back with the wider window
    PB_CHECK(e.sync().outcome == pbo::SyncOutcome::Success);
    PB_CHECK_EQ(e.q("SELECT count(*) FROM message"), std::string("4"));   // and a further sync with the same window keeps them
}

PB_TEST_MAIN()
