// T125: drafts and the outbox in the store: the constraints of data-model.md, the one-transaction writes, and the rule that a draft is never lost
// before the server confirmed its mirrored copy.
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
    pbo::Draft make(const std::string& subject = "Hello", const std::string& body = "Text") {
        pbo::Draft d;
        d.kind = "new";
        d.message_id = "<d" + std::to_string(++counter) + "@example.org>";
        d.to_text = "bob@example.org";
        d.subject = subject;
        d.body = body;
        return d;
    }
    int64_t save(const pbo::Draft& d) {
        auto r = store->save_draft(d);
        PB_CHECK(r.ok());
        return r.value;
    }
    int count(const std::string& sql) { return std::atoi(store->debug_query(sql).c_str()); }
};

}  // namespace

PB_TEST(a_draft_is_saved_read_back_and_changed_in_one_transaction_each) {
    Env e;
    pbo::Draft d = e.make("Plan", "First text");
    d.cc_text = "carol@example.org";
    d.bcc_text = "dave@example.org";
    d.in_reply_to = "<p@example.org>";
    d.refs = "<r@example.org> <p@example.org>";
    d.kind = "reply";
    const int64_t id = e.save(d);
    PB_CHECK(id > 0);
    auto got = e.store->draft(id);
    PB_CHECK(got.has_value());
    PB_CHECK_EQ(got->subject, std::string("Plan"));
    PB_CHECK_EQ(got->body, std::string("First text"));
    PB_CHECK_EQ(got->kind, std::string("reply"));
    PB_CHECK_EQ(got->cc_text, std::string("carol@example.org"));
    PB_CHECK_EQ(got->bcc_text, std::string("dave@example.org"));
    PB_CHECK_EQ(got->in_reply_to, std::string("<p@example.org>"));
    PB_CHECK_EQ(got->server_state, std::string("none"));
    PB_CHECK_EQ(got->created, e.clock.t);
    PB_CHECK(!got->content_hash.empty());
    const std::string hash = got->content_hash;

    e.clock.t += 10;
    got->body = "Second text";
    PB_CHECK_EQ(e.save(*got), id);
    auto again = e.store->draft(id);
    PB_CHECK_EQ(again->body, std::string("Second text"));
    PB_CHECK_EQ(again->updated, e.clock.t);
    PB_CHECK_EQ(again->created, e.clock.t - 10);
    PB_CHECK(again->content_hash != hash);
    PB_CHECK_EQ(e.count("SELECT count(*) FROM draft"), 1);

    e.clock.t += 10;
    PB_CHECK_EQ(e.save(*again), id);                         // nothing changed: nothing written
    PB_CHECK_EQ(e.store->draft(id)->updated, e.clock.t - 10);
    PB_CHECK(!e.store->draft(999).has_value());
}

PB_TEST(bad_drafts_are_refused_and_change_nothing) {
    Env e;
    pbo::Draft d = e.make();
    d.kind = "bogus";
    auto r = e.store->save_draft(d);
    PB_CHECK_EQ(r.error.key, std::string("invalid_draft"));
    d = e.make();
    d.message_id = "";
    PB_CHECK_EQ(e.store->save_draft(d).error.key, std::string("invalid_draft"));
    d = e.make();
    d.body = std::string((1u << 20) + 1, 'x');
    PB_CHECK_EQ(e.store->save_draft(d).error.key, std::string("too_large"));
    d = e.make();
    d.to_text = std::string(20000, 'a');
    PB_CHECK_EQ(e.store->save_draft(d).error.key, std::string("too_large"));
    d = e.make();
    d.id = 4242;
    PB_CHECK_EQ(e.store->save_draft(d).error.key, std::string("not_found"));
    PB_CHECK_EQ(e.count("SELECT count(*) FROM draft"), 0);
    const pbo::Draft one = e.make();
    e.save(one);
    pbo::Draft same_id = e.make();
    same_id.message_id = one.message_id;                      // the Message-ID is the identity
    PB_CHECK(!e.store->save_draft(same_id).ok());
    PB_CHECK_EQ(e.count("SELECT count(*) FROM draft"), 1);
}

PB_TEST(a_body_of_exactly_one_mib_is_kept_whole) {
    Env e;
    pbo::Draft d = e.make("Big", std::string(1u << 20, 'x'));
    const int64_t id = e.save(d);
    PB_CHECK_EQ(e.store->draft(id)->body.size(), size_t(1u << 20));
}

PB_TEST(the_constraints_of_the_data_model_hold_below_the_api) {
    Env e;
    const int64_t id = e.save(e.make());
    PB_CHECK(!e.store->debug_exec("UPDATE draft SET kind = 'bogus'").empty());
    PB_CHECK(!e.store->debug_exec("UPDATE draft SET server_state = 'bogus'").empty());
    PB_CHECK(!e.store->debug_exec("UPDATE draft SET message_id = NULL").empty());
    PB_CHECK(!e.store->debug_exec("INSERT INTO draft(kind, message_id, created, updated) SELECT kind, message_id, created, updated FROM draft").empty());
    for (const char* state : {"none", "pending", "synced", "conflict", "pending_delete"})
        PB_CHECK(e.store->debug_exec(std::string("UPDATE draft SET server_state = '") + state + "'").empty());
    for (const char* kind : {"new", "reply", "reply_all", "forward"}) PB_CHECK(e.store->debug_exec(std::string("UPDATE draft SET kind = '") + kind + "'").empty());
    const std::string ins = "INSERT INTO outbox(draft_id, message_id, envelope_from, envelope_to, raw_message, state, queued_at, state_at) VALUES(";
    PB_CHECK(e.store->debug_exec(ins + std::to_string(id) + ", '<o1@example.org>', 'a@example.org', 'b@example.org', x'00', 'queued', 1, 1)").empty());
    PB_CHECK(!e.store->debug_exec(ins + std::to_string(id) + ", '<o2@example.org>', 'a@example.org', 'b@example.org', x'00', 'queued', 1, 1)").empty());    // UNIQUE(draft_id)
    PB_CHECK(!e.store->debug_exec(ins + "NULL, '<o1@example.org>', 'a@example.org', 'b@example.org', x'00', 'queued', 1, 1)").empty());                          // UNIQUE(message_id)
    PB_CHECK(!e.store->debug_exec(ins + "NULL, '<o3@example.org>', 'a@example.org', 'b@example.org', NULL, 'queued', 1, 1)").empty());                           // raw_message NOT NULL
    PB_CHECK(!e.store->debug_exec(ins + "NULL, '<o4@example.org>', 'a@example.org', 'b@example.org', x'00', 'bogus', 1, 1)").empty());                           // state values
    for (const char* st : {"queued", "sending", "sent_copy_pending", "failed", "uncertain"})
        PB_CHECK(e.store->debug_exec(std::string("UPDATE outbox SET state = '") + st + "'").empty());
}

PB_TEST(editing_a_mirrored_draft_makes_it_pending_again_and_an_unmirrored_one_stays_as_it_is) {
    Env e;
    const int64_t id = e.save(e.make("A", "one"));
    auto d = *e.store->draft(id);
    d.body = "two";
    e.save(d);
    PB_CHECK_EQ(e.store->draft(id)->server_state, std::string("none"));       // never mirrored: still to be mirrored for the first time
    PB_CHECK(e.store->debug_exec("UPDATE draft SET server_state = 'synced', server_uid = 7").empty());
    d = *e.store->draft(id);
    d.body = "three";
    e.save(d);
    PB_CHECK_EQ(e.store->draft(id)->server_state, std::string("pending"));
    PB_CHECK_EQ(e.store->draft(id)->server_uid, int64_t(7));                  // the server copy is still known
}

PB_TEST(the_mirror_marks_a_draft_synced_only_if_it_has_not_changed_since) {
    Env e;
    const int64_t id = e.save(e.make("A", "one"));
    const std::string hash = e.store->draft(id)->content_hash;
    auto d = *e.store->draft(id);
    d.body = "two";                                            // edited while the APPEND was running
    e.save(d);
    PB_CHECK(e.store->set_draft_mirrored(id, 11, hash).ok());
    auto got = e.store->draft(id);
    PB_CHECK_EQ(got->server_uid, int64_t(11));
    PB_CHECK_EQ(got->server_state, std::string("pending"));                   // the newer text still has to go
    PB_CHECK_EQ(got->server_hash, hash);
    PB_CHECK(e.store->set_draft_mirrored(id, 12, got->content_hash).ok());
    got = e.store->draft(id);
    PB_CHECK_EQ(got->server_state, std::string("synced"));
    PB_CHECK_EQ(got->server_uid, int64_t(12));
    PB_CHECK(!e.store->set_draft_mirrored(999, 1, "x").ok());
}

PB_TEST(the_user_deletes_a_draft_and_a_server_copy_goes_first) {
    Env e;
    const int64_t plain = e.save(e.make("Plain"));
    PB_CHECK(e.store->delete_draft(plain).ok());
    PB_CHECK(!e.store->draft(plain).has_value());
    const int64_t mirrored = e.save(e.make("Mirrored"));
    PB_CHECK(e.store->set_draft_mirrored(mirrored, 5, e.store->draft(mirrored)->content_hash).ok());
    PB_CHECK(e.store->delete_draft(mirrored).ok());
    PB_CHECK_EQ(e.store->draft(mirrored)->server_state, std::string("pending_delete"));
    PB_CHECK(e.store->drafts().empty());                                      // hidden from the user at once
    PB_CHECK_EQ(e.store->drafts_to_remove_on_server().size(), size_t(1));
    PB_CHECK(e.store->drafts_to_mirror().empty());
    PB_CHECK(e.store->drop_draft_row(mirrored).ok());                         // the mirror removed the server copy
    PB_CHECK_EQ(e.count("SELECT count(*) FROM draft"), 0);
    PB_CHECK(!e.store->delete_draft(12345).ok());
}

PB_TEST(a_draft_with_changes_the_server_has_not_seen_is_not_dropped_by_the_mirror) {
    Env e;
    const int64_t id = e.save(e.make("A", "one"));
    PB_CHECK(e.store->set_draft_mirrored(id, 5, e.store->draft(id)->content_hash).ok());
    auto d = *e.store->draft(id);
    d.body = "two";
    e.save(d);
    PB_CHECK_EQ(e.store->draft(id)->server_state, std::string("pending"));
    PB_CHECK_EQ(e.store->drop_draft_row(id).key, std::string("draft_pending"));
    PB_CHECK(e.store->draft(id).has_value());
}

PB_TEST(the_lists_follow_the_state_of_the_drafts) {
    Env e;
    const int64_t a = e.save(e.make("A"));
    e.clock.t += 5;
    const int64_t b = e.save(e.make("B"));
    e.clock.t += 5;
    const int64_t c = e.save(e.make("C"));
    const auto list = e.store->drafts();
    PB_CHECK_EQ(list.size(), size_t(3));
    PB_CHECK_EQ(list[0].id, c);                                               // the latest change first
    PB_CHECK_EQ(list[2].id, a);
    PB_CHECK_EQ(e.store->drafts_to_mirror().size(), size_t(3));
    PB_CHECK(e.store->set_draft_mirrored(b, 3, e.store->draft(b)->content_hash).ok());
    PB_CHECK_EQ(e.store->drafts_to_mirror().size(), size_t(2));
}
PB_TEST_MAIN()
