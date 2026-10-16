// T123 (part): the Sent copy (research R-09). With the switch off the app APPENDs one copy, found first by Message-ID so that nothing is filed twice;
// with the switch on it never appends; a failed APPEND is retried alone, with no further SMTP session. UNVERIFIED against real servers (fake servers).
#include "outbox_env.h"

PBO_FAKE_SERVER_TEST();

using pbtest::OutboxEnv;

namespace {

int commands_with(OutboxEnv& e, const std::string& word) {
    int n = 0;
    for (const std::string& c : e.imap->commands())
        if (c.find(word) != std::string::npos) ++n;
    return n;
}

}  // namespace

PB_TEST(with_the_switch_off_one_copy_is_appended_marked_read_and_the_row_is_completed) {
    OutboxEnv e(false);
    const int64_t o = e.queue();
    const std::string raw = e.store->outbox_item(o)->raw_message;
    const auto c = e.run();
    PB_CHECK_EQ(c.sent, 1);
    PB_CHECK_EQ(c.copies, 1);
    PB_CHECK_EQ(e.state(o), std::string("gone"));
    PB_CHECK_EQ(e.appends(), 1);
    const auto sent = e.imap->messages("Sent");
    PB_CHECK_EQ(sent.size(), size_t(1));
    PB_CHECK_EQ(sent[0].second, raw);                                       // the same bytes that were sent
    PB_CHECK(e.imap->flags_of("Sent", sent[0].first).count("\\Seen") == 1);
    PB_CHECK_EQ(e.smtp->accepted_count(e.message_id(o)), 1);
    for (const std::string& cmd : e.imap->commands()) PB_CHECK(cmd.find("EXPUNGE") == std::string::npos);
}

PB_TEST(with_the_switch_on_the_provider_files_the_copy_and_the_app_never_appends) {
    OutboxEnv e(true);
    const int64_t o = e.queue();
    const auto c = e.run();
    PB_CHECK_EQ(c.sent, 1);
    PB_CHECK_EQ(c.copies, 1);
    PB_CHECK_EQ(e.state(o), std::string("gone"));
    PB_CHECK_EQ(e.appends(), 0);
    PB_CHECK_EQ(e.imap->message_count("Sent"), 0);
    PB_CHECK_EQ(e.imap->connections(), 0);                                  // not even a connection to the IMAP server
}

PB_TEST(a_failed_append_keeps_the_row_and_only_the_append_is_retried_without_another_smtp_session) {
    OutboxEnv e(false);
    const int64_t o = e.queue();
    e.imap->refuse_append = true;
    auto c = e.run();
    PB_CHECK_EQ(c.sent, 1);
    PB_CHECK_EQ(c.copies, 0);
    PB_CHECK_EQ(c.copies_failed, 1);
    PB_CHECK_EQ(e.state(o), std::string("sent_copy_pending"));
    PB_CHECK(!e.store->outbox_item(o)->error_key.empty());
    const int smtp_connections = e.smtp->connections();
    PB_CHECK_EQ(e.smtp->accepted_count(e.message_id(o)), 1);
    e.run();                                                                  // still refused
    PB_CHECK_EQ(e.state(o), std::string("sent_copy_pending"));
    e.imap->refuse_append = false;
    c = e.run();
    PB_CHECK_EQ(c.sent, 0);
    PB_CHECK_EQ(c.copies, 1);
    PB_CHECK_EQ(e.state(o), std::string("gone"));
    PB_CHECK_EQ(e.imap->message_count("Sent"), 1);
    PB_CHECK_EQ(e.smtp->connections(), smtp_connections);                    // zero further SMTP sessions
    PB_CHECK_EQ(e.smtp->accepted_count(e.message_id(o)), 1);
}

PB_TEST(an_append_whose_answer_was_lost_is_found_by_message_id_and_not_repeated) {
    OutboxEnv e(false);
    const int64_t o = e.queue();
    e.imap->drop_after_append = true;                                        // stored on the server, the app never learns it
    e.run();
    PB_CHECK_EQ(e.imap->message_count("Sent"), 1);
    PB_CHECK_EQ(e.state(o), std::string("sent_copy_pending"));
    const int appends = e.appends();
    e.imap->drop_after_append = false;
    const auto c = e.run();
    PB_CHECK_EQ(c.copies, 1);
    PB_CHECK_EQ(e.state(o), std::string("gone"));
    PB_CHECK_EQ(e.appends(), appends);                                        // no second APPEND
    PB_CHECK_EQ(e.imap->message_count("Sent"), 1);
    PB_CHECK_EQ(e.smtp->accepted_count(e.message_id(o)), 1);
}

PB_TEST(a_copy_that_is_already_in_sent_is_never_filed_twice) {
    OutboxEnv e(false);
    const int64_t o = e.queue();
    e.imap->refuse_append = true;
    e.run();
    e.imap->refuse_append = false;
    // the message appears in Sent by another way (another client, the provider after all)
    e.imap->add_message("Sent", e.store->outbox_item(o)->raw_message, {"\\Seen"});
    const int appends = e.appends();
    e.run();
    PB_CHECK_EQ(e.state(o), std::string("gone"));
    PB_CHECK_EQ(e.appends(), appends);
    PB_CHECK_EQ(e.imap->message_count("Sent"), 1);
}

PB_TEST(without_a_known_sent_folder_the_row_waits_and_nothing_is_appended_anywhere) {
    OutboxEnv e(false);
    e.store->debug_exec("DELETE FROM folder WHERE role = 'sent'");
    const int64_t o = e.queue();
    const auto c = e.run();
    PB_CHECK_EQ(c.sent, 1);
    PB_CHECK_EQ(c.copies_failed, 1);
    PB_CHECK_EQ(e.state(o), std::string("sent_copy_pending"));
    PB_CHECK_EQ(e.store->outbox_item(o)->error_key, std::string("sent_folder_missing"));
    PB_CHECK_EQ(e.appends(), 0);
}

PB_TEST(a_server_that_cannot_be_reached_for_the_copy_does_not_lose_the_message) {
    OutboxEnv e(false);
    const int64_t o = e.queue();
    auto a = *e.store->mail_account();
    const int good = a.imap_port;
    a.imap_port = 1;
    e.store->save_mail_account(a);
    const auto c = e.run();
    PB_CHECK_EQ(c.sent, 1);
    PB_CHECK_EQ(c.copies_failed, 1);
    PB_CHECK_EQ(e.state(o), std::string("sent_copy_pending"));
    a.imap_port = good;
    e.store->save_mail_account(a);
    e.run();
    PB_CHECK_EQ(e.state(o), std::string("gone"));
    PB_CHECK_EQ(e.imap->message_count("Sent"), 1);
    PB_CHECK_EQ(e.smtp->accepted_count(e.message_id(o)), 1);
}

PB_TEST(a_reset_sent_folder_gets_no_uid_addressed_command) {
    OutboxEnv e(false);
    // the folder was reset on the server since it was last read: UIDs mean something else now
    e.store->debug_exec("UPDATE folder SET uidvalidity = 77 WHERE role = 'sent'");
    e.imap->set_uidvalidity("Sent", 99);
    const int64_t o = e.queue();
    e.run();
    PB_CHECK_EQ(e.state(o), std::string("gone"));                            // the copy is filed (APPEND names no UID)
    PB_CHECK_EQ(commands_with(e, "UID STORE"), 0);                            // but nothing that names a UID is written
    PB_CHECK_EQ(e.imap->message_count("Sent"), 1);
}

PB_TEST_MAIN()
