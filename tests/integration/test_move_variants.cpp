// T153 (part): moving a message uses UID MOVE when the server has it, else UID COPY + \Deleted + UID EXPUNGE <uid> (UIDPLUS), else UID COPY +
// \Deleted only. A bare EXPUNGE never arrives; the original is only removed after the copy succeeded (K2); a reset folder gets nothing (K1).
// UNVERIFIED against real servers (fake server).
#include "mail_env.h"

PBO_FAKE_SERVER_TEST();

using pbtest::MailEnv;

namespace {

int64_t prepare(MailEnv& e, uint32_t* uid) {
    *uid = e.add();
    e.sync();
    return e.id_of("INBOX", *uid);
}

}  // namespace

PB_TEST(uid_move_is_used_when_the_server_advertises_move) {
    MailEnv e("IMAP4rev1 UIDPLUS MOVE LITERAL+");
    uint32_t uid;
    const int64_t id = prepare(e, &uid);
    PB_CHECK(e.store->request_move(id, e.folder_id("Archive")).ok());
    const size_t mark = e.imap->commands().size();
    PB_CHECK(e.sync().outcome == pbo::SyncOutcome::Success);
    const auto cmds = e.commands_since(mark);
    PB_CHECK(MailEnv::any(cmds, "UID MOVE " + std::to_string(uid) + " \"Archive\""));
    PB_CHECK(!MailEnv::any(cmds, "UID COPY"));
    PB_CHECK(!MailEnv::any(cmds, "EXPUNGE"));
    PB_CHECK_EQ(e.imap->message_count("INBOX"), 0);
    PB_CHECK_EQ(e.imap->message_count("Archive"), 1);
    PB_CHECK_EQ(e.q("SELECT count(*) FROM message WHERE folder_id = " + std::to_string(e.folder_id("Archive"))), std::string("1"));   // the pull brought it to its new folder
    PB_CHECK_EQ(e.q("SELECT count(*) FROM message WHERE folder_id = " + std::to_string(e.folder_id("INBOX"))), std::string("0"));
}

PB_TEST(copy_then_deleted_then_uid_expunge_of_that_uid_with_uidplus) {
    MailEnv e("IMAP4rev1 UIDPLUS LITERAL+");
    uint32_t uid;
    const int64_t id = prepare(e, &uid);
    const uint32_t other = e.add();                     // a second message that must stay
    e.sync();
    PB_CHECK(e.store->request_move(id, e.folder_id("Archive")).ok());
    const size_t mark = e.imap->commands().size();
    e.sync();
    const auto cmds = e.commands_since(mark);
    size_t copy = std::string::npos, del = std::string::npos, expunge = std::string::npos;
    for (size_t i = 0; i < cmds.size(); ++i) {
        if (cmds[i].find("UID COPY " + std::to_string(uid) + " ") != std::string::npos) copy = i;
        if (cmds[i].find("UID STORE " + std::to_string(uid) + " +FLAGS.SILENT (\\Deleted)") != std::string::npos) del = i;
        if (cmds[i].find("UID EXPUNGE " + std::to_string(uid)) != std::string::npos) expunge = i;
    }
    PB_CHECK(copy != std::string::npos && del != std::string::npos && expunge != std::string::npos);
    PB_CHECK(copy < del && del < expunge);              // K2: the original is touched only after the copy
    PB_CHECK_EQ(e.imap->bare_expunges.load(), 0);
    PB_CHECK_EQ(e.imap->message_count("INBOX"), 1);
    PB_CHECK_EQ(e.imap->messages("INBOX")[0].first, other);
    PB_CHECK_EQ(e.imap->message_count("Archive"), 1);
}

PB_TEST(without_move_and_uidplus_the_original_is_marked_deleted_and_nothing_is_expunged) {
    MailEnv e("IMAP4rev1 LITERAL+");
    uint32_t uid;
    const int64_t id = prepare(e, &uid);
    PB_CHECK(e.store->request_move(id, e.folder_id("Archive")).ok());
    const size_t mark = e.imap->commands().size();
    e.sync();
    const auto cmds = e.commands_since(mark);
    PB_CHECK(MailEnv::any(cmds, "UID COPY " + std::to_string(uid)));
    PB_CHECK(MailEnv::any(cmds, "UID STORE " + std::to_string(uid) + " +FLAGS.SILENT (\\Deleted)"));
    PB_CHECK(!MailEnv::any(cmds, "EXPUNGE"));           // a bare EXPUNGE would remove other people's \Deleted messages too
    PB_CHECK_EQ(e.imap->bare_expunges.load(), 0);
    PB_CHECK_EQ(e.imap->message_count("Archive"), 1);
    PB_CHECK(e.imap->flags_of("INBOX", uid).count("\\Deleted") == 1);
    // the message is not listed in INBOX any more, and it does not come back at the next sync
    e.sync();
    PB_CHECK_EQ(e.q("SELECT count(*) FROM message WHERE folder_id = " + std::to_string(e.folder_id("INBOX")) + " AND local_state <> 'pending_delete' AND (flags_local & 8) = 0"), std::string("0"));
}

PB_TEST(a_refused_copy_leaves_the_original_alone_and_reports_the_message) {
    MailEnv e("IMAP4rev1 UIDPLUS LITERAL+");
    uint32_t uid;
    const int64_t id = prepare(e, &uid);
    e.imap->refuse_copy = true;
    PB_CHECK(e.store->request_move(id, e.folder_id("Archive")).ok());
    const size_t mark = e.imap->commands().size();
    e.sync();
    const auto cmds = e.commands_since(mark);
    PB_CHECK(!MailEnv::any(cmds, "\\Deleted"));          // K2: nothing was deleted after a failed copy
    PB_CHECK(!MailEnv::any(cmds, "EXPUNGE"));
    PB_CHECK_EQ(e.imap->message_count("INBOX"), 1);
    PB_CHECK_EQ(e.q("SELECT local_error FROM message WHERE id = " + std::to_string(id)), std::string("refused"));
    PB_CHECK_EQ(e.store->list_messages(e.folder_id("INBOX"), pbo::SortKey::DateDesc, 0, 10).items.size(), size_t(1));   // visible again, with the mark
    PB_CHECK(e.store->acknowledge_message_error(id).ok());
    PB_CHECK_EQ(e.q("SELECT local_state FROM message WHERE id = " + std::to_string(id)), std::string("synced"));
}

PB_TEST(a_message_gone_on_the_server_is_reported_not_copied) {
    MailEnv e("IMAP4rev1 UIDPLUS LITERAL+");
    uint32_t uid;
    const int64_t id = prepare(e, &uid);
    PB_CHECK(e.store->request_move(id, e.folder_id("Archive")).ok());
    e.imap->expunge("INBOX", uid);
    e.sync();
    PB_CHECK_EQ(e.imap->message_count("Archive"), 0);
    PB_CHECK_EQ(e.q("SELECT local_error FROM message WHERE id = " + std::to_string(id)), std::string("gone_on_server"));
}

PB_TEST(after_a_uidvalidity_change_no_copy_move_or_expunge_is_sent_for_that_folder) {
    MailEnv e("IMAP4rev1 UIDPLUS MOVE LITERAL+");
    uint32_t uid;
    const int64_t id = prepare(e, &uid);
    PB_CHECK(e.store->request_move(id, e.folder_id("Archive")).ok());
    e.imap->set_uidvalidity("INBOX", 99);               // the numbers mean something else now
    const size_t mark = e.imap->commands().size();
    e.sync();
    const auto cmds = e.commands_since(mark);
    PB_CHECK(!MailEnv::any(cmds, "UID COPY") && !MailEnv::any(cmds, "UID MOVE") && !MailEnv::any(cmds, "UID EXPUNGE") && !MailEnv::any(cmds, "UID STORE"));
    PB_CHECK_EQ(e.imap->message_count("Archive"), 0);
    PB_CHECK_EQ(e.imap->bare_expunges.load(), 0);
}

PB_TEST_MAIN()
