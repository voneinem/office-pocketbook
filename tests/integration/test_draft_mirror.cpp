// T123 (part): the mirror of drafts in the Drafts folder. APPEND first, then the superseded copy goes (\Deleted + UID EXPUNGE of that UID only);
// an interruption leaves two copies, never none; a reset folder gets no UID-addressed command; a draft changed on both sides becomes two drafts;
// foreign drafts are never touched; a bare EXPUNGE never arrives. UNVERIFIED against real servers (fake servers).
#include "outbox_env.h"

PBO_FAKE_SERVER_TEST();

using pbtest::OutboxEnv;

namespace {

size_t index_of(const std::vector<std::string>& cmds, const std::string& word, size_t from = 0) {
    for (size_t i = from; i < cmds.size(); ++i)
        if (cmds[i].find(word) != std::string::npos) return i;
    return std::string::npos;
}

}  // namespace

PB_TEST(a_new_draft_is_appended_marked_as_draft_and_remembered) {
    OutboxEnv e;
    const int64_t id = e.draft("Plan", "First text.\n");
    const auto c = e.mirror();
    PB_CHECK_EQ(c.mirrored, 1);
    PB_CHECK_EQ(e.imap->message_count("Drafts"), 1);
    const auto d = *e.store->draft(id);
    PB_CHECK_EQ(d.server_state, std::string("synced"));
    PB_CHECK(d.server_uid > 0);
    PB_CHECK(!d.server_hash.empty());
    const auto flags = e.imap->flags_of("Drafts", static_cast<uint32_t>(d.server_uid));
    PB_CHECK(flags.count("\\Draft") == 1 && flags.count("\\Seen") == 1);
    const auto raw = e.imap->messages("Drafts")[0].second;
    PB_CHECK(raw.find("Subject: Plan") != std::string::npos);
    PB_CHECK(raw.find("Message-ID: " + d.message_id) != std::string::npos);
    PB_CHECK(e.store->drafts_to_mirror().empty());
    e.mirror();                                                // nothing changed: nothing happens
    PB_CHECK_EQ(e.appends(), 1);
}

PB_TEST(a_draft_that_is_not_ready_yet_is_mirrored_too) {
    OutboxEnv e;
    pbo::Draft d;
    d.message_id = "<unfinished@example.org>";
    d.to_text = "bob@exam";                                   // an address that is not finished
    d.subject = "";
    d.body = "";
    PB_CHECK(e.store->save_draft(d).ok());
    PB_CHECK_EQ(e.mirror().mirrored, 1);
    PB_CHECK_EQ(e.imap->message_count("Drafts"), 1);
}

PB_TEST(a_changed_draft_is_appended_first_and_then_the_old_copy_goes_by_its_uid) {
    OutboxEnv e;
    const int64_t id = e.draft("Plan", "First text.\n");
    e.mirror();
    const uint32_t first_uid = static_cast<uint32_t>(e.store->draft(id)->server_uid);
    e.edit(id, "Second text.\n");
    const size_t mark = e.imap->commands().size();
    const auto c = e.mirror();
    PB_CHECK_EQ(c.mirrored, 1);
    PB_CHECK_EQ(c.removed, 1);
    const auto cmds = e.imap->commands();
    const size_t append = index_of(cmds, "APPEND", mark);
    const size_t del = index_of(cmds, "UID STORE " + std::to_string(first_uid) + " +FLAGS.SILENT (\\Deleted)", mark);
    const size_t expunge = index_of(cmds, "UID EXPUNGE " + std::to_string(first_uid), mark);
    PB_CHECK(append != std::string::npos && del != std::string::npos && expunge != std::string::npos);
    PB_CHECK(append < del && del < expunge);                    // APPEND before anything is deleted (K2)
    PB_CHECK_EQ(e.imap->message_count("Drafts"), 1);
    PB_CHECK(e.imap->messages("Drafts")[0].second.find("Second text.") != std::string::npos);
    PB_CHECK_EQ(e.store->draft(id)->server_state, std::string("synced"));
    PB_CHECK(e.store->draft(id)->server_uid != first_uid);
    PB_CHECK_EQ(e.imap->bare_expunges.load(), 0);
}

PB_TEST(an_interruption_between_the_append_and_the_removal_leaves_two_copies_never_none) {
    OutboxEnv e;
    const int64_t id = e.draft("Plan", "First text.\n");
    e.mirror();
    e.edit(id, "Second text.\n");
    // the connection dies right after the APPEND was carried out
    e.imap->drop_after_append = true;
    e.mirror();
    PB_CHECK_EQ(e.imap->message_count("Drafts"), 2);            // old and new: nothing is lost
    e.imap->drop_after_append = false;
    e.mirror();                                                 // the retry adopts the new copy (found by its text) and removes the old one
    PB_CHECK_EQ(e.imap->message_count("Drafts"), 1);
    PB_CHECK(e.imap->messages("Drafts")[0].second.find("Second text.") != std::string::npos);
    PB_CHECK_EQ(e.store->draft(id)->server_state, std::string("synced"));
    PB_CHECK_EQ(e.appends(), 2);                                // the first draft and the second text: no duplicate APPEND
}

PB_TEST(a_refused_append_keeps_the_old_copy_and_deletes_nothing) {
    OutboxEnv e;
    const int64_t id = e.draft("Plan", "First text.\n");
    e.mirror();
    e.edit(id, "Second text.\n");
    e.imap->refuse_append = true;
    const size_t mark = e.imap->commands().size();
    const auto c = e.mirror();
    PB_CHECK_EQ(c.mirrored, 0);
    PB_CHECK_EQ(c.failed, 1);
    PB_CHECK_EQ(e.imap->message_count("Drafts"), 1);
    PB_CHECK(e.imap->messages("Drafts")[0].second.find("First text.") != std::string::npos);
    const auto cmds = e.imap->commands();
    PB_CHECK_EQ(index_of(cmds, "\\Deleted", mark), std::string::npos);
    PB_CHECK_EQ(index_of(cmds, "EXPUNGE", mark), std::string::npos);
    PB_CHECK_EQ(e.store->draft(id)->server_state, std::string("pending"));
}

PB_TEST(a_deleted_draft_takes_its_server_copy_with_it) {
    OutboxEnv e;
    const int64_t id = e.draft();
    e.mirror();
    PB_CHECK_EQ(e.imap->message_count("Drafts"), 1);
    PB_CHECK(e.store->delete_draft(id).ok());
    const auto c = e.mirror();
    PB_CHECK_EQ(c.removed, 1);
    PB_CHECK_EQ(e.imap->message_count("Drafts"), 0);
    PB_CHECK(!e.store->draft(id).has_value());
}

PB_TEST(a_sent_draft_leaves_the_drafts_folder) {
    OutboxEnv e;
    const int64_t id = e.draft("Letter", "Dear Bob.\n");
    e.mirror();
    PB_CHECK_EQ(e.imap->message_count("Drafts"), 1);
    // Send: the draft goes into the outbox, is sent, the copy is filed, the row completes
    const auto d = *e.store->draft(id);
    pbo::OutboxRequest q;
    q.expected_hash = d.content_hash;
    q.envelope_from = "alice@example.org";
    q.recipients = {"bob@example.org"};
    q.raw_message = "From: alice@example.org\r\nTo: bob@example.org\r\nSubject: Letter\r\nMessage-ID: " + d.message_id + "\r\n\r\nDear Bob.\r\n";
    auto queued = e.store->queue_draft(id, q);
    PB_CHECK(queued.ok());
    e.mirror();                                                  // while queued the draft is left alone
    PB_CHECK_EQ(e.imap->message_count("Drafts"), 1);
    e.run();
    PB_CHECK_EQ(e.store->draft(id)->server_state, std::string("pending_delete"));
    e.mirror();
    PB_CHECK_EQ(e.imap->message_count("Drafts"), 0);
    PB_CHECK(!e.store->draft(id).has_value());
}

PB_TEST(messages_in_the_drafts_folder_that_the_app_did_not_put_there_are_never_touched) {
    OutboxEnv e;
    const uint32_t foreign = e.imap->add_message("Drafts", "From: alice@example.org\r\nSubject: From my laptop\r\nMessage-ID: <laptop@example.org>\r\n\r\nText\r\n", {"\\Draft", "\\Seen"});
    const int64_t id = e.draft();
    e.mirror();
    e.edit(id, "Another text.\n");
    e.mirror();
    PB_CHECK(e.store->delete_draft(id).ok());
    e.mirror();
    PB_CHECK_EQ(e.imap->message_count("Drafts"), 1);
    PB_CHECK_EQ(e.imap->messages("Drafts")[0].first, foreign);
    PB_CHECK(e.imap->flags_of("Drafts", foreign).count("\\Deleted") == 0);
    PB_CHECK_EQ(e.imap->bare_expunges.load(), 0);
}

PB_TEST(a_folder_that_was_reset_on_the_server_gets_no_uid_addressed_command_and_no_append) {
    OutboxEnv e;
    const int64_t id = e.draft("Plan", "First text.\n");
    e.mirror();
    e.edit(id, "Second text.\n");
    e.store->debug_exec("UPDATE folder SET uidvalidity = 5 WHERE role = 'drafts'");
    e.imap->set_uidvalidity("Drafts", 6);
    const size_t mark = e.imap->commands().size();
    const auto c = e.mirror();
    PB_CHECK_EQ(c.mirrored, 0);
    const auto cmds = e.imap->commands();
    PB_CHECK_EQ(index_of(cmds, "UID STORE", mark), std::string::npos);
    PB_CHECK_EQ(index_of(cmds, "UID EXPUNGE", mark), std::string::npos);
    PB_CHECK_EQ(index_of(cmds, "APPEND", mark), std::string::npos);
    PB_CHECK_EQ(e.imap->message_count("Drafts"), 1);
    PB_CHECK_EQ(e.store->draft(id)->server_state, std::string("pending"));        // the draft is safe locally
    // a deletion by the user is held back as well
    PB_CHECK(e.store->delete_draft(id).ok());
    const size_t mark2 = e.imap->commands().size();
    e.mirror();
    PB_CHECK_EQ(index_of(e.imap->commands(), "UID STORE", mark2), std::string::npos);
    PB_CHECK(e.store->draft(id).has_value());
}

PB_TEST(a_server_without_uidplus_never_gets_an_expunge_at_all) {
    OutboxEnv e;
    e.imap->set_capabilities("IMAP4rev1 LITERAL+");
    const int64_t id = e.draft("Plan", "First text.\n");
    e.mirror();
    e.edit(id, "Second text.\n");
    e.mirror();
    PB_CHECK_EQ(index_of(e.imap->commands(), "EXPUNGE"), std::string::npos);
    PB_CHECK_EQ(e.imap->bare_expunges.load(), 0);
    PB_CHECK_EQ(e.imap->message_count("Drafts"), 2);              // the old copy stays, flagged \Deleted
    const auto msgs = e.imap->messages("Drafts");
    int deleted = 0;
    for (const auto& m : msgs) deleted += e.imap->flags_of("Drafts", m.first).count("\\Deleted");
    PB_CHECK_EQ(deleted, 1);
}

PB_TEST(a_draft_changed_on_both_sides_becomes_two_drafts) {
    OutboxEnv e;
    const int64_t id = e.draft("Plan", "First text.\n");
    e.mirror();
    const uint32_t uid = static_cast<uint32_t>(e.store->draft(id)->server_uid);
    // someone edits the copy in another client: the message in the folder is replaced by a different text under the same UID
    e.imap->expunge("Drafts", uid);
    e.imap->add_message("Drafts", "From: alice@example.org\r\nSubject: Plan\r\nMessage-ID: " + e.store->draft(id)->message_id + "\r\n\r\nEdited on the laptop.\r\n", {"\\Draft", "\\Seen"});
    // meanwhile the user edits here
    e.edit(id, "Edited on the reader.\n");
    const auto c = e.mirror();
    PB_CHECK_EQ(c.conflicts, 1);
    const auto drafts = e.store->drafts();
    PB_CHECK_EQ(drafts.size(), size_t(2));
    bool laptop = false, reader = false;
    for (const auto& d : drafts) {
        if (d.body.find("Edited on the laptop.") != std::string::npos) {
            laptop = true;
            PB_CHECK_EQ(d.conflict_of, id);
            PB_CHECK(d.message_id != e.store->draft(id)->message_id);
        }
        if (d.body.find("Edited on the reader.") != std::string::npos) reader = true;
    }
    PB_CHECK(laptop && reader);                                  // neither version was lost
    PB_CHECK(e.imap->message_count("Drafts") >= 1);
    bool have_laptop_text = false, have_reader_text = false;
    for (const auto& m : e.imap->messages("Drafts")) {
        if (m.second.find("Edited on the laptop.") != std::string::npos) have_laptop_text = true;
        if (m.second.find("Edited on the reader.") != std::string::npos) have_reader_text = true;
    }
    PB_CHECK(have_laptop_text && have_reader_text);
}

PB_TEST(no_bare_expunge_ever_arrives_in_any_scenario) {
    OutboxEnv e;
    const int64_t a = e.draft("A");
    e.draft("B");
    e.mirror();
    e.edit(a, "changed\n");
    e.mirror();
    e.store->delete_draft(a);
    e.mirror();
    PB_CHECK_EQ(e.imap->bare_expunges.load(), 0);
}

PB_TEST_MAIN()
