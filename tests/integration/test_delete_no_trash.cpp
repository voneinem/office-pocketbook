// T153 (part): deleting. Outside the Trash a delete is a move to the Trash; in the Trash it is permanent and needs its own confirmation; with
// no Trash folder the app creates none, asks first and reports. Permanent deletion is \Deleted + UID EXPUNGE <uid> (UIDPLUS), else \Deleted only;
// never a bare EXPUNGE; nothing for a folder whose UIDVALIDITY changed. UNVERIFIED against real servers (fake server).
#include "mail_env.h"

PBO_FAKE_SERVER_TEST();

using pbtest::MailEnv;

PB_TEST(a_delete_in_the_inbox_files_the_message_in_the_trash) {
    MailEnv e("IMAP4rev1 UIDPLUS MOVE LITERAL+");
    const uint32_t uid = e.add();
    e.sync();
    const int64_t id = e.id_of("INBOX", uid);
    PB_CHECK(e.store->request_delete(id, false).ok());
    e.sync();
    PB_CHECK_EQ(e.imap->message_count("INBOX"), 0);
    PB_CHECK_EQ(e.imap->message_count("Trash"), 1);
    PB_CHECK_EQ(e.q("SELECT count(*) FROM message WHERE folder_id = " + std::to_string(e.folder_id("Trash"))), std::string("1"));
}

PB_TEST(deleting_in_the_trash_needs_the_second_confirmation_and_then_expunges_only_that_uid) {
    MailEnv e;
    const uint32_t gone = e.add("Trash");
    const uint32_t stay = e.add("Trash");
    e.sync();
    const int64_t id = e.id_of("Trash", gone);
    PB_CHECK_EQ(e.store->request_delete(id, false).key, std::string("confirm_required"));
    PB_CHECK(e.store->request_delete(id, true).ok());
    const size_t mark = e.imap->commands().size();
    e.sync();
    const auto cmds = e.commands_since(mark);
    PB_CHECK(MailEnv::any(cmds, "UID STORE " + std::to_string(gone) + " +FLAGS.SILENT (\\Deleted)"));
    PB_CHECK(MailEnv::any(cmds, "UID EXPUNGE " + std::to_string(gone)));
    PB_CHECK_EQ(e.imap->bare_expunges.load(), 0);
    PB_CHECK_EQ(e.imap->message_count("Trash"), 1);
    PB_CHECK_EQ(e.imap->messages("Trash")[0].first, stay);
    PB_CHECK_EQ(e.q("SELECT count(*) FROM message WHERE id = " + std::to_string(id)), std::string("0"));
}

PB_TEST(without_uidplus_a_permanent_delete_only_marks_the_message_deleted) {
    MailEnv e("IMAP4rev1 LITERAL+");
    const uint32_t uid = e.add("Trash");
    e.sync();
    const int64_t id = e.id_of("Trash", uid);
    PB_CHECK(e.store->request_delete(id, true).ok());
    const size_t mark = e.imap->commands().size();
    e.sync();
    PB_CHECK(!MailEnv::any(e.commands_since(mark), "EXPUNGE"));
    PB_CHECK(e.imap->flags_of("Trash", uid).count("\\Deleted") == 1);
    PB_CHECK_EQ(e.imap->bare_expunges.load(), 0);
    e.sync();   // it does not come back into the list
    PB_CHECK_EQ(e.store->list_messages(e.folder_id("Trash"), pbo::SortKey::DateDesc, 0, 10).items.size(), size_t(0));
}

PB_TEST(with_no_trash_folder_the_app_creates_none_and_the_delete_waits_for_the_users_word) {
    MailEnv e("IMAP4rev1 UIDPLUS LITERAL+", false);
    const uint32_t uid = e.add();
    e.sync();
    const int64_t id = e.id_of("INBOX", uid);
    PB_CHECK_EQ(e.store->request_delete(id, false).key, std::string("no_trash"));
    PB_CHECK(e.store->request_delete(id, true).ok());
    const size_t mark = e.imap->commands().size();
    e.sync();
    PB_CHECK(!MailEnv::any(e.commands_since(mark), "CREATE"));        // no folder is created
    PB_CHECK_EQ(e.imap->message_count("INBOX"), 0);
    PB_CHECK_EQ(e.q("SELECT count(*) FROM folder WHERE role = 'trash'"), std::string("0"));
}

PB_TEST(after_a_uidvalidity_change_no_delete_command_is_sent) {
    MailEnv e;
    const uint32_t uid = e.add("Trash");
    e.sync();
    const int64_t id = e.id_of("Trash", uid);
    PB_CHECK(e.store->request_delete(id, true).ok());
    e.imap->set_uidvalidity("Trash", 55);
    const size_t mark = e.imap->commands().size();
    e.sync();
    const auto cmds = e.commands_since(mark);
    PB_CHECK(!MailEnv::any(cmds, "UID STORE") && !MailEnv::any(cmds, "UID EXPUNGE"));
    PB_CHECK_EQ(e.imap->bare_expunges.load(), 0);
}

PB_TEST_MAIN()
