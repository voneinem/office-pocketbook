// T152/T153/T157 (store part): the user's changes to mail on the reader - flags, moves, deletes - are local work with exact states, they never
// touch the Trash decision by themselves, and the "could not apply" marks keep every message visible until the user has seen it.
#include <string>

#include "pbo/store.h"
#include "test_support.h"
#include "tmp_dir.h"

namespace {

struct Clock : pbo::Clock {
    int64_t now() const override { return 1791979200; }
};

struct Env {
    pbtest::TmpDir dir;
    Clock clock;
    std::unique_ptr<pbo::Store> store;
    int64_t inbox = 0, trash = 0, archive = 0;
    explicit Env(bool with_trash = true) {
        store = std::move(pbo::Store::open(dir.path(), clock).value);
        pbo::MailAccount a;
        a.imap_host = "imap.example.org";
        a.smtp_host = "smtp.example.org";
        a.username = "alice";
        a.address = "alice@example.org";
        PB_CHECK(store->save_mail_account(a).ok());
        pbo::FolderListItem i, t, ar;
        i.raw_name = i.name = "INBOX";
        t.raw_name = t.name = "Trash";
        t.attributes = {"\\Trash"};
        ar.raw_name = ar.name = "Archive";
        ar.attributes = {"\\Archive"};
        std::vector<pbo::FolderListItem> list = {i, ar};
        if (with_trash) list.push_back(t);
        store->upsert_folders(list);
        for (const auto& f : store->folders()) {
            if (f.name == "INBOX") inbox = f.id;
            if (f.name == "Trash") trash = f.id;
            if (f.name == "Archive") archive = f.id;
        }
    }
    int64_t add(int64_t folder, uint32_t uid, const std::string& subject = "S", uint32_t flags = 0) {
        pbo::PullBatch b;
        pbo::PulledMessage m;
        m.uid = uid;
        m.subject = subject;
        m.message_id = "<m" + std::to_string(uid) + "@x>";
        m.from_name = "Anna";
        m.from_addr = "anna@example.org";
        m.date_utc = 1790000000 + uid;
        m.flags = flags;
        b.messages.push_back(m);
        PB_CHECK(store->apply_pull(folder, b).ok());
        return std::stoll(store->debug_query("SELECT id FROM message WHERE folder_id = " + std::to_string(folder) + " AND uid = " + std::to_string(uid)));
    }
    std::string q(const std::string& sql) { return store->debug_query(sql); }
    std::string row(int64_t id) { return q("SELECT local_state || '|' || flags_base || '|' || flags_local || '|' || local_error FROM message WHERE id = " + std::to_string(id)); }
};

}  // namespace

PB_TEST(marking_a_message_read_is_a_pending_flag_change_and_marking_it_unread_again_cancels_it) {
    Env e;
    const int64_t id = e.add(e.inbox, 1);
    PB_CHECK_EQ(e.store->folders()[0].unread_count + e.store->folders()[1].unread_count + e.store->folders()[2].unread_count, 1);
    PB_CHECK(e.store->set_flags_local(id, pbo::kFlagSeen, 0).ok());
    PB_CHECK_EQ(e.row(id), std::string("pending_flags|0|1|"));
    PB_CHECK_EQ(e.store->unsynced_count(), 1);
    PB_CHECK_EQ(e.store->pending_messages().size(), size_t(1));
    PB_CHECK(e.store->set_flags_local(id, 0, pbo::kFlagSeen).ok());
    PB_CHECK_EQ(e.row(id), std::string("synced|0|0|"));
    PB_CHECK_EQ(e.store->pending_messages().size(), size_t(0));
}

PB_TEST(the_unread_count_follows_the_users_flags_at_once) {
    Env e;
    const int64_t id = e.add(e.inbox, 1);
    e.add(e.inbox, 2);
    auto unread = [&] { for (const auto& f : e.store->folders()) if (f.id == e.inbox) return f.unread_count; return -1; };
    PB_CHECK_EQ(unread(), 2);
    PB_CHECK(e.store->set_flags_local(id, pbo::kFlagSeen, 0).ok());
    PB_CHECK_EQ(unread(), 1);
}

PB_TEST(flagging_adds_and_removes_single_bits_without_touching_the_others) {
    Env e;
    const int64_t id = e.add(e.inbox, 1, "S", pbo::kFlagSeen | pbo::kFlagAnswered);
    PB_CHECK(e.store->set_flags_local(id, pbo::kFlagFlagged, 0).ok());
    PB_CHECK_EQ(e.q("SELECT flags_local FROM message WHERE id = " + std::to_string(id)), std::to_string(pbo::kFlagSeen | pbo::kFlagAnswered | pbo::kFlagFlagged));
    PB_CHECK(e.store->set_flags_local(id, 0, pbo::kFlagSeen).ok());
    PB_CHECK_EQ(e.q("SELECT flags_local FROM message WHERE id = " + std::to_string(id)), std::to_string(pbo::kFlagAnswered | pbo::kFlagFlagged));
    PB_CHECK_EQ(e.store->set_flags_local(9999, 1, 0).key, std::string("not_found"));
}

PB_TEST(a_flag_change_that_was_pushed_merges_the_servers_other_flags_and_keeps_the_users_bits) {
    Env e;
    const int64_t id = e.add(e.inbox, 1, "S", pbo::kFlagFlagged);
    PB_CHECK(e.store->set_flags_local(id, pbo::kFlagSeen, pbo::kFlagFlagged).ok());   // read, and not starred any more
    // the server answers: Seen is set, the star is gone, and meanwhile someone else answered the message
    PB_CHECK(e.store->message_flags_pushed(id, pbo::kFlagSeen | pbo::kFlagAnswered).ok());
    PB_CHECK_EQ(e.row(id), std::string("synced|" + std::to_string(pbo::kFlagSeen | pbo::kFlagAnswered) + "|" + std::to_string(pbo::kFlagSeen | pbo::kFlagAnswered) + "|"));
}

PB_TEST(a_flag_the_server_did_not_apply_is_reported_as_refused_not_retried_for_ever) {
    Env e;
    const int64_t id = e.add(e.inbox, 1);
    PB_CHECK(e.store->set_flags_local(id, pbo::kFlagFlagged, 0).ok());
    PB_CHECK(e.store->message_flags_pushed(id, 0).ok());            // the server shows no star
    PB_CHECK_EQ(e.q("SELECT local_error FROM message WHERE id = " + std::to_string(id)), std::string("refused"));
    PB_CHECK_EQ(e.store->pending_messages().size(), size_t(0));      // not pending any more: it waits for the user
    PB_CHECK_EQ(e.store->messages_with_errors().size(), size_t(1));
    PB_CHECK(e.store->acknowledge_message_error(id).ok());          // the user saw it: the local change is dropped
    PB_CHECK_EQ(e.row(id), std::string("synced|0|0|"));
}

PB_TEST(a_moved_message_leaves_its_list_at_once_and_the_push_removes_the_row) {
    Env e;
    const int64_t id = e.add(e.inbox, 1);
    e.add(e.inbox, 2);
    PB_CHECK(e.store->request_move(id, e.archive).ok());
    PB_CHECK_EQ(e.q("SELECT local_state FROM message WHERE id = " + std::to_string(id)), std::string("pending_move"));
    PB_CHECK_EQ(e.store->list_messages(e.inbox, pbo::SortKey::DateDesc, 0, 10).items.size(), size_t(1));
    PB_CHECK_EQ(e.store->pending_messages()[0].move_target_folder_id.value_or(0), e.archive);
    PB_CHECK_EQ(e.store->request_move(id, e.inbox).key, std::string("same_folder"));
    PB_CHECK_EQ(e.store->request_move(id, 9999).key, std::string("bad_target"));
    PB_CHECK(e.store->message_left_folder(id).ok());
    PB_CHECK_EQ(e.q("SELECT count(*) FROM message WHERE id = " + std::to_string(id)), std::string("0"));
}

PB_TEST(delete_outside_the_trash_moves_to_the_trash_and_inside_it_needs_the_second_question) {
    Env e;
    const int64_t a = e.add(e.inbox, 1);
    PB_CHECK(e.store->request_delete(a, false).ok());
    PB_CHECK_EQ(e.q("SELECT local_state || '|' || move_target_folder_id FROM message WHERE id = " + std::to_string(a)), "pending_move|" + std::to_string(e.trash));
    const int64_t t = e.add(e.trash, 7);
    PB_CHECK_EQ(e.store->request_delete(t, false).key, std::string("confirm_required"));
    PB_CHECK_EQ(e.q("SELECT local_state FROM message WHERE id = " + std::to_string(t)), std::string("synced"));
    PB_CHECK(e.store->request_delete(t, true).ok());
    PB_CHECK_EQ(e.q("SELECT local_state FROM message WHERE id = " + std::to_string(t)), std::string("pending_delete"));
    PB_CHECK_EQ(e.store->list_messages(e.trash, pbo::SortKey::DateDesc, 0, 10).items.size(), size_t(0));
}

PB_TEST(without_a_trash_folder_deleting_is_permanent_and_says_so_first) {
    Env e(false);
    const int64_t a = e.add(e.inbox, 1);
    PB_CHECK_EQ(e.store->request_delete(a, false).key, std::string("no_trash"));
    PB_CHECK_EQ(e.q("SELECT local_state FROM message WHERE id = " + std::to_string(a)), std::string("synced"));
    PB_CHECK(e.store->request_delete(a, true).ok());
    PB_CHECK_EQ(e.q("SELECT local_state FROM message WHERE id = " + std::to_string(a)), std::string("pending_delete"));
    PB_CHECK_EQ(e.q("SELECT count(*) FROM folder WHERE role = 'trash'"), std::string("0"));   // the app creates none
}

PB_TEST(changes_to_a_message_with_a_move_or_delete_waiting_are_refused_and_flags_follow_a_move) {
    Env e;
    const int64_t id = e.add(e.inbox, 1);
    PB_CHECK(e.store->request_delete(id, false).ok());
    PB_CHECK_EQ(e.store->set_flags_local(id, 1, 0).key, std::string("bad_state"));
    const int64_t m = e.add(e.inbox, 2);
    PB_CHECK(e.store->set_flags_local(m, pbo::kFlagSeen, 0).ok());
    PB_CHECK(e.store->request_move(m, e.archive).ok());                // the read mark goes with the move
    PB_CHECK_EQ(e.q("SELECT local_state || '|' || flags_local FROM message WHERE id = " + std::to_string(m)), std::string("pending_move|1"));
}

PB_TEST(a_message_gone_or_moved_on_the_server_stays_listed_with_a_mark_until_the_user_has_seen_it) {
    Env e;
    const int64_t id = e.add(e.inbox, 1);
    PB_CHECK(e.store->set_flags_local(id, pbo::kFlagSeen, 0).ok());
    PB_CHECK(e.store->set_message_error(id, "gone_on_server").ok());
    PB_CHECK_EQ(e.store->pending_messages().size(), size_t(0));
    const auto page = e.store->list_messages(e.inbox, pbo::SortKey::DateDesc, 0, 10);
    PB_CHECK_EQ(page.items.size(), size_t(1));
    PB_CHECK_EQ(page.items[0].local_error, std::string("gone_on_server"));
    PB_CHECK_EQ(e.store->unsynced_count(), 1);                           // the status line still counts it
    PB_CHECK(e.store->acknowledge_message_error(id).ok());
    PB_CHECK_EQ(e.q("SELECT count(*) FROM message"), std::string("0"));
    // a delete that could not be applied also stays visible
    const int64_t d = e.add(e.inbox, 2);
    PB_CHECK(e.store->request_delete(d, false).ok());
    PB_CHECK(e.store->set_message_error(d, "refused").ok());
    PB_CHECK_EQ(e.store->list_messages(e.inbox, pbo::SortKey::DateDesc, 0, 10).items.size(), size_t(1));
    PB_CHECK(e.store->acknowledge_message_error(d).ok());
    PB_CHECK_EQ(e.row(d), std::string("synced|0|0|"));
    PB_CHECK_EQ(e.store->set_message_error(d, "nonsense").key, std::string("invalid_error"));
}

PB_TEST_MAIN()
