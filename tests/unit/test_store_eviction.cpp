// T167 (SC-013, FR-032): when the database is above the storage limit the oldest synced mail outside the window goes first, down to 90 % of the limit.
// Nothing with local work is ever chosen: messages with a pending change or an error, drafts, the outbox, events with local work. If the protected data
// alone exceed the limit nothing else is touched and the caller is told.
#include <string>

#include "pbo/store.h"
#include "test_support.h"
#include "tmp_dir.h"

namespace {

struct Clock : pbo::Clock {
    int64_t now() const override { return 1791979200; }
};

constexpr int64_t kDay = 86400;
constexpr int64_t kNow = 1791979200;

struct Env {
    pbtest::TmpDir dir;
    Clock clock;
    std::unique_ptr<pbo::Store> store;
    int64_t inbox = 0;
    Env() {
        store = std::move(pbo::Store::open(dir.path(), clock).value);
        pbo::FolderListItem i;
        i.raw_name = i.name = "INBOX";
        store->upsert_folders({i});
        inbox = store->folders()[0].id;
    }
    // a message `age_days` old with a body of about 60 KB
    int64_t add(uint32_t uid, int age_days, size_t body_bytes = 60000) {
        pbo::PullBatch b;
        pbo::PulledMessage m;
        m.uid = uid;
        m.subject = "Message " + std::to_string(uid);
        m.from_addr = "a@example.org";
        m.date_utc = kNow - age_days * kDay;
        b.messages.push_back(m);
        PB_CHECK(store->apply_pull(inbox, b).ok());
        const int64_t id = std::atoll(store->debug_query("SELECT id FROM message WHERE uid = " + std::to_string(uid)).c_str());
        pbo::BodyUpdate u;
        u.text.assign(body_bytes, 'x');
        for (size_t k = 0; k < u.text.size(); k += 70) u.text[k] = '\n';
        PB_CHECK(store->apply_body(id, u).ok());
        return id;
    }
    int count() { return std::atoi(store->debug_query("SELECT count(*) FROM message").c_str()); }
};

}  // namespace

PB_TEST(usage_is_the_used_part_of_the_database_and_nothing_happens_below_the_limit) {
    Env e;
    for (uint32_t i = 1; i <= 20; ++i) e.add(i, 100);
    const auto u = e.store->usage();
    PB_CHECK(u.database_bytes > 1000000);
    const auto r = e.store->evict_to_limit(u.database_bytes + 1, kNow - 30 * kDay);
    PB_CHECK_EQ(r.deleted, 0);
    PB_CHECK(!r.over_limit);
    PB_CHECK_EQ(e.count(), 20);
}

PB_TEST(above_the_limit_the_oldest_mail_outside_the_window_goes_first_down_to_ninety_percent) {
    Env e;
    for (uint32_t i = 1; i <= 20; ++i) e.add(i, 100 + static_cast<int>(i));   // uid 20 is the oldest
    const int64_t before = e.store->usage().database_bytes;
    const int64_t limit = before * 3 / 4;
    const auto r = e.store->evict_to_limit(limit, kNow - 30 * kDay);
    PB_CHECK(r.deleted > 0 && r.deleted < 20);
    PB_CHECK(r.used_after <= limit * 9 / 10);
    PB_CHECK(!r.over_limit);
    // the oldest were removed, the newer ones are still there
    PB_CHECK_EQ(e.store->debug_query("SELECT count(*) FROM message WHERE uid = 20"), std::string("0"));
    PB_CHECK_EQ(e.store->debug_query("SELECT count(*) FROM message WHERE uid = 1"), std::string("1"));
    PB_CHECK_EQ(e.store->debug_query("SELECT count(*) FROM message_fts"), std::to_string(e.count()));    // the search index followed
    PB_CHECK(e.store->check_integrity().ok());
}

PB_TEST(mail_inside_the_window_is_never_evicted) {
    Env e;
    for (uint32_t i = 1; i <= 10; ++i) e.add(i, 5);     // all inside a 30-day window
    const int64_t limit = e.store->usage().database_bytes / 2;
    const auto r = e.store->evict_to_limit(limit, kNow - 30 * kDay);
    PB_CHECK_EQ(r.deleted, 0);
    PB_CHECK(r.over_limit);                              // the caller shows the "storage full" warning
    PB_CHECK_EQ(e.count(), 10);
}

PB_TEST(old_mail_with_local_work_an_error_or_a_draft_is_protected) {
    Env e;
    const int64_t pending = e.add(1, 200), errored = e.add(2, 201), flagged = e.add(3, 202);
    for (uint32_t i = 4; i <= 12; ++i) e.add(i, 210 + static_cast<int>(i));
    PB_CHECK(e.store->set_flags_local(pending, pbo::kFlagSeen, 0).ok());
    PB_CHECK(e.store->set_flags_local(errored, pbo::kFlagSeen, 0).ok());
    PB_CHECK(e.store->set_message_error(errored, "gone_on_server").ok());
    PB_CHECK(e.store->request_delete(flagged, true).ok());       // a delete waiting to be sent
    pbo::Draft d;
    d.message_id = "<d@x>";
    d.subject = "Draft";
    d.body = std::string(200000, 'y');
    PB_CHECK(e.store->save_draft(d).ok());
    const auto r = e.store->evict_to_limit(1, kNow - 30 * kDay);      // an impossible limit: everything that may go, goes
    PB_CHECK_EQ(r.deleted, 9);
    PB_CHECK(r.over_limit);
    for (int64_t id : {pending, errored, flagged}) PB_CHECK_EQ(e.store->debug_query("SELECT count(*) FROM message WHERE id = " + std::to_string(id)), std::string("1"));
    PB_CHECK_EQ(e.store->debug_query("SELECT count(*) FROM draft"), std::string("1"));
}

PB_TEST(a_message_that_is_open_is_kept_and_a_limit_of_zero_means_no_limit) {
    Env e;
    const int64_t open_id = e.add(1, 300);
    for (uint32_t i = 2; i <= 8; ++i) e.add(i, 300 + static_cast<int>(i));
    const auto r = e.store->evict_to_limit(1, kNow - 30 * kDay, open_id);
    PB_CHECK_EQ(e.store->debug_query("SELECT count(*) FROM message WHERE id = " + std::to_string(open_id)), std::string("1"));
    PB_CHECK_EQ(r.deleted, 7);
    PB_CHECK_EQ(e.store->evict_to_limit(0, kNow - 30 * kDay).deleted, 0);
}

PB_TEST(events_and_outbox_rows_are_not_touched_by_eviction) {
    Env e;
    for (uint32_t i = 1; i <= 6; ++i) e.add(i, 400);
    PB_CHECK(e.store->debug_exec("INSERT INTO outbox(message_id, envelope_from, envelope_to, raw_message, state, queued_at, state_at) VALUES('<o@x>', 'a@x', 'b@x', x'41', 'queued', 1, 1)").empty());
    e.store->evict_to_limit(1, kNow - 30 * kDay);
    PB_CHECK_EQ(e.store->debug_query("SELECT count(*) FROM outbox"), std::string("1"));
}

PB_TEST_MAIN()
