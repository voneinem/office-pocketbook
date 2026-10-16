// T120: every transition of the outbox state machine (data-model.md, research R-08), the guards against a second Send, and the recovery of
// rows found in "sending" at the start of the app.
#include <string>

#include "pbo/store.h"
#include "test_support.h"
#include "tmp_dir.h"

namespace {

struct FixedClock : pbo::Clock {
    int64_t t = 1791979200;
    int64_t now() const override { return t; }
};

struct Env {
    pbtest::TmpDir dir;
    FixedClock clock;
    std::unique_ptr<pbo::Store> store;
    int counter = 0;
    Env() { store = std::move(pbo::Store::open(dir.path(), clock).value); }
    int64_t draft(const std::string& subject = "Hello") {
        pbo::Draft d;
        d.message_id = "<d" + std::to_string(++counter) + "@example.org>";
        d.to_text = "bob@example.org";
        d.subject = subject;
        d.body = "Text";
        auto r = store->save_draft(d);
        PB_CHECK(r.ok());
        return r.value;
    }
    pbo::OutboxRequest request(int64_t draft_id) {
        pbo::OutboxRequest q;
        q.expected_hash = store->draft(draft_id)->content_hash;
        q.envelope_from = "alice@example.org";
        q.recipients = {"bob@example.org", "carol@example.org"};
        q.subject_preview = "Hello";
        q.raw_message = "Subject: Hello\r\n\r\nText\r\n";
        return q;
    }
    int64_t queue(const std::string& subject = "Hello") {
        const int64_t d = draft(subject);
        auto r = store->queue_draft(d, request(d));
        PB_CHECK(r.ok());
        return r.value;
    }
    std::string state(int64_t id) {
        auto i = store->outbox_item(id);
        return i ? i->state : "gone";
    }
    int count(const std::string& sql) { return std::atoi(store->debug_query(sql).c_str()); }
};

}  // namespace

PB_TEST(send_moves_the_draft_into_the_outbox_once_with_the_message_built_at_that_moment) {
    Env e;
    const int64_t d = e.draft("Plan");
    auto r = e.store->queue_draft(d, e.request(d));
    PB_CHECK(r.ok());
    const auto item = e.store->outbox_item(r.value);
    PB_CHECK(item.has_value());
    PB_CHECK_EQ(item->state, std::string("queued"));
    PB_CHECK_EQ(item->draft_id, d);
    PB_CHECK_EQ(item->message_id, e.store->draft(d)->message_id);              // the identity is the draft's
    PB_CHECK_EQ(item->envelope_from, std::string("alice@example.org"));
    PB_CHECK((item->recipients == std::vector<std::string>{"bob@example.org", "carol@example.org"}));
    PB_CHECK_EQ(item->raw_message, std::string("Subject: Hello\r\n\r\nText\r\n"));
    PB_CHECK_EQ(item->attempts, 0);
    PB_CHECK_EQ(item->queued_at, e.clock.t);
    PB_CHECK(e.store->draft(d)->queued);
    PB_CHECK(e.store->drafts().empty());                                        // no longer a draft to edit
    PB_CHECK_EQ(e.store->unsynced_count(), 1);
}

PB_TEST(a_second_send_of_the_same_draft_is_ignored_and_a_queued_draft_is_not_edited) {
    Env e;
    const int64_t d = e.draft();
    PB_CHECK(e.store->queue_draft(d, e.request(d)).ok());
    auto second = e.store->queue_draft(d, e.request(d));
    PB_CHECK_EQ(second.error.key, std::string("already_queued"));
    PB_CHECK_EQ(e.count("SELECT count(*) FROM outbox"), 1);
    auto edit = *e.store->draft(d);
    edit.body = "changed after Send";
    PB_CHECK_EQ(e.store->save_draft(edit).error.key, std::string("already_queued"));
    PB_CHECK_EQ(e.store->draft(d)->body, std::string("Text"));
    PB_CHECK_EQ(e.store->delete_draft(d).key, std::string("already_queued"));
}

PB_TEST(a_draft_that_changed_since_the_message_was_built_is_not_queued) {
    Env e;
    const int64_t d = e.draft();
    const auto req = e.request(d);
    auto edit = *e.store->draft(d);
    edit.body = "typed meanwhile";
    PB_CHECK(e.store->save_draft(edit).ok());
    PB_CHECK_EQ(e.store->queue_draft(d, req).error.key, std::string("draft_changed"));
    PB_CHECK_EQ(e.count("SELECT count(*) FROM outbox"), 0);
    PB_CHECK(!e.store->queue_draft(4242, req).ok());
}

PB_TEST(a_request_without_recipients_or_text_is_refused) {
    Env e;
    const int64_t d = e.draft();
    auto req = e.request(d);
    req.recipients.clear();
    PB_CHECK_EQ(e.store->queue_draft(d, req).error.key, std::string("invalid_request"));
    req = e.request(d);
    req.raw_message.clear();
    PB_CHECK_EQ(e.store->queue_draft(d, req).error.key, std::string("invalid_request"));
    PB_CHECK_EQ(e.count("SELECT count(*) FROM outbox"), 0);
}

PB_TEST(sending_starts_with_a_conditional_update_that_only_one_caller_wins) {
    Env e;
    const int64_t o = e.queue();
    auto first = e.store->begin_sending(o);
    PB_CHECK(first.ok() && first.value);
    PB_CHECK_EQ(e.state(o), std::string("sending"));
    PB_CHECK_EQ(e.store->outbox_item(o)->attempts, 1);
    auto second = e.store->begin_sending(o);
    PB_CHECK(second.ok() && !second.value);                                       // somebody is sending already
    PB_CHECK_EQ(e.store->outbox_item(o)->attempts, 1);
    PB_CHECK(!e.store->begin_sending(9999).value);
}

PB_TEST(an_accepted_message_waits_for_its_sent_copy_and_then_leaves_with_its_draft) {
    Env e;
    const int64_t o = e.queue();
    const int64_t d = e.store->outbox_item(o)->draft_id;
    PB_CHECK(e.store->begin_sending(o).value);
    e.clock.t += 3;
    PB_CHECK(e.store->finish_sending(o, {pbo::SendFinish::Kind::Accepted, "", ""}).ok());
    PB_CHECK_EQ(e.state(o), std::string("sent_copy_pending"));
    PB_CHECK_EQ(e.store->outbox_item(o)->sent_at, e.clock.t);
    PB_CHECK_EQ(e.store->unsynced_count(), 1);
    PB_CHECK(e.store->complete_outbox(o).ok());
    PB_CHECK_EQ(e.state(o), std::string("gone"));
    PB_CHECK(!e.store->draft(d).has_value());
    PB_CHECK_EQ(e.store->unsynced_count(), 0);
    PB_CHECK_EQ(e.store->complete_outbox(o).key, std::string("not_found"));
}

PB_TEST(a_draft_with_a_server_copy_keeps_a_hidden_row_until_the_copy_is_removed) {
    Env e;
    const int64_t o = e.queue();
    const int64_t d = e.store->outbox_item(o)->draft_id;
    PB_CHECK(e.store->set_draft_mirrored(d, 5, e.store->draft(d)->content_hash).ok());
    PB_CHECK(e.store->begin_sending(o).value);
    PB_CHECK(e.store->finish_sending(o, {pbo::SendFinish::Kind::Accepted, "", ""}).ok());
    PB_CHECK(e.store->complete_outbox(o).ok());
    PB_CHECK_EQ(e.store->draft(d)->server_state, std::string("pending_delete"));
    PB_CHECK_EQ(e.store->drafts_to_remove_on_server().size(), size_t(1));
    PB_CHECK(e.store->drafts().empty());
}

PB_TEST(a_failure_before_the_message_left_is_failed_with_its_reason_and_can_be_retried) {
    Env e;
    const int64_t o = e.queue();
    PB_CHECK(e.store->begin_sending(o).value);
    PB_CHECK(e.store->finish_sending(o, {pbo::SendFinish::Kind::Failed, "refused_recipient", "a recipient was refused"}).ok());
    auto item = *e.store->outbox_item(o);
    PB_CHECK_EQ(item.state, std::string("failed"));
    PB_CHECK_EQ(item.error_key, std::string("refused_recipient"));
    PB_CHECK_EQ(item.error_detail, std::string("a recipient was refused"));
    PB_CHECK_EQ(e.store->unsynced_count(), 1);
    PB_CHECK(e.store->retry_outbox(o).ok());
    PB_CHECK_EQ(e.state(o), std::string("queued"));
    PB_CHECK(e.store->outbox_item(o)->error_key.empty());                         // the old reason is cleared
    PB_CHECK_EQ(e.store->outbox_item(o)->attempts, 1);
    PB_CHECK_EQ(e.store->retry_outbox(o).key, std::string("bad_state"));          // only failed rows are retried
}

PB_TEST(an_unknown_outcome_is_uncertain_and_only_the_user_can_resolve_it) {
    Env e;
    const int64_t o = e.queue();
    PB_CHECK(e.store->begin_sending(o).value);
    PB_CHECK(e.store->finish_sending(o, {pbo::SendFinish::Kind::Uncertain, "connection_lost", "the connection was lost"}).ok());
    PB_CHECK_EQ(e.state(o), std::string("uncertain"));
    PB_CHECK_EQ(e.store->retry_outbox(o).key, std::string("bad_state"));          // Retry is not offered for uncertain rows
    PB_CHECK_EQ(e.store->edit_outbox(o).key, std::string("bad_state"));
    PB_CHECK(!e.store->begin_sending(o).value);                                   // nothing sends it by itself
    PB_CHECK(e.store->send_again(o).ok());
    PB_CHECK_EQ(e.state(o), std::string("queued"));
    PB_CHECK_EQ(e.store->send_again(o).key, std::string("bad_state"));

    PB_CHECK(e.store->begin_sending(o).value);
    PB_CHECK(e.store->finish_sending(o, {pbo::SendFinish::Kind::Uncertain, "timeout", ""}).ok());
    PB_CHECK(e.store->mark_was_sent(o).ok());
    PB_CHECK_EQ(e.state(o), std::string("sent_copy_pending"));
    PB_CHECK_EQ(e.store->mark_was_sent(o).key, std::string("bad_state"));
}

PB_TEST(a_failure_before_any_connection_puts_the_message_back_in_the_queue_with_the_reason_visible) {
    Env e;
    const int64_t o = e.queue();
    PB_CHECK(e.store->begin_sending(o).value);
    PB_CHECK(e.store->finish_sending(o, {pbo::SendFinish::Kind::Requeue, "server_unreachable", "the server cannot be reached"}).ok());
    PB_CHECK_EQ(e.state(o), std::string("queued"));
    PB_CHECK_EQ(e.store->outbox_item(o)->error_key, std::string("server_unreachable"));
    PB_CHECK_EQ(e.store->outbox_item(o)->attempts, 1);
    PB_CHECK(e.store->begin_sending(o).value);                                    // the next sync sends it
    PB_CHECK_EQ(e.store->outbox_item(o)->attempts, 2);
}

PB_TEST(the_reason_a_sent_copy_could_not_be_filed_is_kept_on_its_row) {
    Env e;
    const int64_t o = e.queue();
    PB_CHECK_EQ(e.store->set_outbox_error(o, "x", "y").key, std::string("bad_state"));
    PB_CHECK(e.store->begin_sending(o).value);
    PB_CHECK(e.store->finish_sending(o, {pbo::SendFinish::Kind::Accepted, "", ""}).ok());
    PB_CHECK(e.store->set_outbox_error(o, "append_failed", "the folder is full").ok());
    PB_CHECK_EQ(e.store->outbox_item(o)->error_key, std::string("append_failed"));
    PB_CHECK_EQ(e.state(o), std::string("sent_copy_pending"));
}

PB_TEST(a_failed_message_goes_back_to_a_draft_or_is_deleted_with_it) {
    Env e;
    const int64_t o = e.queue("Back");
    const int64_t d = e.store->outbox_item(o)->draft_id;
    PB_CHECK(e.store->begin_sending(o).value);
    PB_CHECK(e.store->finish_sending(o, {pbo::SendFinish::Kind::Failed, "auth_failed", ""}).ok());
    PB_CHECK(e.store->edit_outbox(o).ok());
    PB_CHECK_EQ(e.state(o), std::string("gone"));
    PB_CHECK_EQ(e.store->drafts().size(), size_t(1));                            // editable again
    auto edit = *e.store->draft(d);
    edit.body = "fixed";
    PB_CHECK(e.store->save_draft(edit).ok());
    PB_CHECK(e.store->queue_draft(d, e.request(d)).ok());                        // and can be sent again

    const int64_t o2 = e.queue("Delete");
    const int64_t d2 = e.store->outbox_item(o2)->draft_id;
    PB_CHECK_EQ(e.store->delete_outbox(o2).key, std::string("bad_state"));       // queued rows are not deleted
    PB_CHECK(e.store->begin_sending(o2).value);
    PB_CHECK_EQ(e.store->delete_outbox(o2).key, std::string("bad_state"));       // nor rows being sent
    PB_CHECK(e.store->finish_sending(o2, {pbo::SendFinish::Kind::Uncertain, "timeout", ""}).ok());
    PB_CHECK(e.store->delete_outbox(o2).ok());
    PB_CHECK_EQ(e.state(o2), std::string("gone"));
    PB_CHECK(!e.store->draft(d2).has_value());
}

PB_TEST(a_failed_message_whose_draft_is_gone_cannot_go_back_to_editing) {
    Env e;
    const int64_t o = e.queue();
    PB_CHECK(e.store->begin_sending(o).value);
    PB_CHECK(e.store->finish_sending(o, {pbo::SendFinish::Kind::Failed, "auth_failed", ""}).ok());
    PB_CHECK(e.store->debug_exec("UPDATE outbox SET draft_id = NULL").empty());
    PB_CHECK_EQ(e.store->edit_outbox(o).key, std::string("draft_gone"));
    PB_CHECK_EQ(e.state(o), std::string("failed"));                                // nothing was lost
    PB_CHECK(e.store->retry_outbox(o).ok());                                       // the raw message is still there
}

PB_TEST(finish_sending_needs_the_row_to_be_in_sending) {
    Env e;
    const int64_t o = e.queue();
    PB_CHECK_EQ(e.store->finish_sending(o, {pbo::SendFinish::Kind::Accepted, "", ""}).key, std::string("bad_state"));
    PB_CHECK_EQ(e.state(o), std::string("queued"));
    PB_CHECK_EQ(e.store->finish_sending(9999, {pbo::SendFinish::Kind::Failed, "x", ""}).key, std::string("not_found"));
    PB_CHECK_EQ(e.store->complete_outbox(o).key, std::string("bad_state"));         // not before the copy step
}

PB_TEST(rows_found_in_sending_at_the_start_become_uncertain_and_nothing_resends_them) {
    pbtest::TmpDir dir;
    FixedClock clock;
    int64_t sending = 0, queued = 0, failed = 0;
    {
        auto opened = pbo::Store::open(dir.path(), clock);
        PB_CHECK(opened.ok());
        auto& s = *opened.value;
        for (int i = 1; i <= 3; ++i) {
            pbo::Draft d;
            d.message_id = "<r" + std::to_string(i) + "@example.org>";
            d.to_text = "bob@example.org";
            d.subject = "S";
            const int64_t id = s.save_draft(d).value;
            pbo::OutboxRequest q;
            q.expected_hash = s.draft(id)->content_hash;
            q.envelope_from = "alice@example.org";
            q.recipients = {"bob@example.org"};
            q.raw_message = "x";
            const int64_t o = s.queue_draft(id, q).value;
            if (i == 1) sending = o;
            if (i == 2) queued = o;
            if (i == 3) failed = o;
        }
        PB_CHECK(s.begin_sending(sending).value);                  // the app is "killed" here: the store is closed without finishing
        PB_CHECK(s.begin_sending(failed).value);
        PB_CHECK(s.finish_sending(failed, {pbo::SendFinish::Kind::Failed, "auth_failed", ""}).ok());
    }
    auto reopened = pbo::Store::open(dir.path(), clock);          // recover_interrupted runs at open
    PB_CHECK(reopened.ok());
    auto& s = *reopened.value;
    PB_CHECK_EQ(s.outbox_item(sending)->state, std::string("uncertain"));
    PB_CHECK_EQ(s.outbox_item(sending)->error_key, std::string("interrupted"));
    PB_CHECK_EQ(s.outbox_item(queued)->state, std::string("queued"));
    PB_CHECK_EQ(s.outbox_item(failed)->state, std::string("failed"));
    int n = -1;
    PB_CHECK(s.recover_interrupted(&n).ok());
    PB_CHECK_EQ(n, 0);                                             // idempotent
}

PB_TEST(rows_in_every_state_are_counted_as_waiting_work) {
    Env e;
    int64_t ids[5];
    for (auto& id : ids) id = e.queue();
    PB_CHECK(e.store->begin_sending(ids[1]).value);
    PB_CHECK(e.store->begin_sending(ids[2]).value);
    PB_CHECK(e.store->finish_sending(ids[2], {pbo::SendFinish::Kind::Accepted, "", ""}).ok());
    PB_CHECK(e.store->begin_sending(ids[3]).value);
    PB_CHECK(e.store->finish_sending(ids[3], {pbo::SendFinish::Kind::Failed, "x", ""}).ok());
    PB_CHECK(e.store->begin_sending(ids[4]).value);
    PB_CHECK(e.store->finish_sending(ids[4], {pbo::SendFinish::Kind::Uncertain, "x", ""}).ok());
    PB_CHECK_EQ(e.store->outbox().size(), size_t(5));
    PB_CHECK_EQ(e.store->unsynced_count(), 5);
}

PB_TEST_MAIN()
