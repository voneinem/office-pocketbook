// T152 (part): the push of flag changes: only the difference is sent (UID STORE +FLAGS.SILENT / -FLAGS.SILENT, never a full FLAGS replace), the
// answer is read back, a change on both sides loses nothing, a message that is gone is reported (not dropped), and a folder whose UIDVALIDITY
// changed gets no UID-addressed command at all. UNVERIFIED against real servers (fake server).
#include "mail_env.h"

PBO_FAKE_SERVER_TEST();

using pbtest::MailEnv;

PB_TEST(marking_read_sends_only_the_seen_flag_with_plus_flags_silent_and_reads_the_result_back) {
    MailEnv e;
    const uint32_t uid = e.add("INBOX", {"\\Flagged"});
    PB_CHECK(e.sync().outcome == pbo::SyncOutcome::Success);
    const int64_t id = e.id_of("INBOX", uid);
    PB_CHECK(e.store->set_flags_local(id, pbo::kFlagSeen, 0).ok());
    const size_t mark = e.imap->commands().size();
    PB_CHECK(e.sync().outcome == pbo::SyncOutcome::Success);
    const auto cmds = e.commands_since(mark);
    PB_CHECK(MailEnv::any(cmds, "UID STORE " + std::to_string(uid) + " +FLAGS.SILENT (\\Seen)"));
    PB_CHECK(!MailEnv::any(cmds, "-FLAGS"));
    for (const auto& c : cmds) PB_CHECK(c.find("STORE") == std::string::npos || c.find("+FLAGS.SILENT") != std::string::npos);   // never a bare FLAGS (...) replace
    PB_CHECK(e.imap->flags_of("INBOX", uid).count("\\Seen") == 1);
    PB_CHECK(e.imap->flags_of("INBOX", uid).count("\\Flagged") == 1);     // untouched
    PB_CHECK_EQ(e.q("SELECT local_state || '|' || (flags_base = flags_local) FROM message WHERE id = " + std::to_string(id)), std::string("synced|1"));
    PB_CHECK_EQ(e.store->unsynced_count(), 0);
}

PB_TEST(marking_unread_and_unflagging_send_minus_flags) {
    MailEnv e;
    const uint32_t uid = e.add("INBOX", {"\\Seen", "\\Flagged"});
    e.sync();
    const int64_t id = e.id_of("INBOX", uid);
    PB_CHECK(e.store->set_flags_local(id, 0, pbo::kFlagSeen | pbo::kFlagFlagged).ok());
    const size_t mark = e.imap->commands().size();
    e.sync();
    const auto cmds = e.commands_since(mark);
    PB_CHECK(MailEnv::any(cmds, "UID STORE " + std::to_string(uid) + " -FLAGS.SILENT ("));
    PB_CHECK(e.imap->flags_of("INBOX", uid).empty());
}

PB_TEST(a_flag_changed_on_both_sides_keeps_the_users_bits_and_takes_the_rest_from_the_server) {
    MailEnv e;
    const uint32_t uid = e.add("INBOX", {"\\Flagged"});
    e.sync();
    const int64_t id = e.id_of("INBOX", uid);
    PB_CHECK(e.store->set_flags_local(id, pbo::kFlagSeen, pbo::kFlagFlagged).ok());   // read, and the star removed
    e.imap->set_flags("INBOX", uid, {"\\Flagged", "\\Answered"});                       // meanwhile: answered elsewhere
    e.sync();
    const auto flags = e.imap->flags_of("INBOX", uid);
    PB_CHECK(flags.count("\\Seen") == 1);          // the user's change is there
    PB_CHECK(flags.count("\\Flagged") == 0);       // the star is gone, as the user wanted
    PB_CHECK(flags.count("\\Answered") == 1);      // the server's change is not lost (no full FLAGS replace)
    PB_CHECK_EQ(e.q("SELECT local_state || '|' || flags_local FROM message WHERE id = " + std::to_string(id)), "synced|" + std::to_string(pbo::kFlagSeen | pbo::kFlagAnswered));
}

PB_TEST(a_message_that_is_gone_on_the_server_is_reported_and_kept_until_the_user_has_seen_it) {
    MailEnv e;
    const uint32_t uid = e.add();
    e.sync();
    const int64_t id = e.id_of("INBOX", uid);
    PB_CHECK(e.store->set_flags_local(id, pbo::kFlagSeen, 0).ok());
    e.imap->expunge("INBOX", uid);
    e.sync();
    PB_CHECK_EQ(e.q("SELECT local_error FROM message WHERE id = " + std::to_string(id)), std::string("gone_on_server"));
    PB_CHECK_EQ(e.store->messages_with_errors().size(), size_t(1));
    PB_CHECK_EQ(e.store->list_messages(e.folder_id("INBOX"), pbo::SortKey::DateDesc, 0, 10).items.size(), size_t(1));   // still listed, with the mark
    e.sync();                                                                                                            // and not asked about again
    PB_CHECK_EQ(e.store->messages_with_errors().size(), size_t(1));
    PB_CHECK(e.store->acknowledge_message_error(id).ok());
    PB_CHECK_EQ(e.q("SELECT count(*) FROM message"), std::string("0"));
}

PB_TEST(after_a_uidvalidity_change_no_uid_command_is_sent_and_the_change_is_applied_to_the_rematched_message) {
    MailEnv e;
    const uint32_t uid = e.add("INBOX");
    e.sync();
    const int64_t id = e.id_of("INBOX", uid);
    PB_CHECK(e.store->set_flags_local(id, pbo::kFlagSeen, 0).ok());
    // the folder is reset on the server: new UIDVALIDITY, the same message under a new UID
    e.imap->reset_folder("INBOX", 77);
    const uint32_t new_uid = e.imap->add_message("INBOX", MailEnv::mail(1), {}, e.clock.t - 86400 + 3600);
    const size_t mark = e.imap->commands().size();
    e.sync();
    const auto first = e.commands_since(mark);
    PB_CHECK(!MailEnv::any(first, "UID STORE"));                       // K1: not one UID-addressed write in the folder that was reset
    PB_CHECK(!MailEnv::any(first, "UID COPY") && !MailEnv::any(first, "UID MOVE") && !MailEnv::any(first, "UID EXPUNGE"));
    PB_CHECK(e.imap->flags_of("INBOX", new_uid).empty());
    e.sync();                                                          // now the change meets the message under its new UID
    PB_CHECK(e.imap->flags_of("INBOX", new_uid).count("\\Seen") == 1);
    PB_CHECK_EQ(e.q("SELECT count(*) FROM message WHERE local_state <> 'synced'"), std::string("0"));
}

PB_TEST(after_a_uidvalidity_change_a_change_for_a_message_that_is_not_there_any_more_is_reported_gone) {
    MailEnv e;
    const uint32_t uid = e.add("INBOX");
    e.sync();
    const int64_t id = e.id_of("INBOX", uid);
    PB_CHECK(e.store->set_flags_local(id, pbo::kFlagSeen, 0).ok());
    e.imap->reset_folder("INBOX", 78);
    e.sync();
    e.sync();
    PB_CHECK_EQ(e.q("SELECT count(*) FROM message WHERE local_error = 'gone_on_server'"), std::string("1"));
    PB_CHECK_EQ(e.imap->bare_expunges.load(), 0);
}

PB_TEST(nothing_waiting_means_no_store_command) {
    MailEnv e;
    e.add();
    e.sync();
    const size_t mark = e.imap->commands().size();
    e.sync();
    PB_CHECK(!MailEnv::any(e.commands_since(mark), "STORE"));
}

PB_TEST_MAIN()
