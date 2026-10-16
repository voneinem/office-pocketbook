// T141 (part): the same event changed on both sides is never overwritten (SC-008): a 412 keeps both versions, and the three resolutions
// each lose nothing the user chose to keep. UNVERIFIED against real servers (fake server).
#include <string>

#include "event_env.h"

PBO_FAKE_SERVER_TEST();

using pbtest::EventEnv;
using pbtest::event_ics;

namespace {

// a local edit of the title, and a different change made on the server meanwhile
int64_t conflicted(EventEnv& e, const std::string& name, const std::string& uid) {
    const int64_t id = e.server_event(name, uid, "Original");
    auto edit = pbo::edit_of_event(*e.store, *e.tz, id);
    edit.value.title = "Mine";
    auto edep = e.edit_deps();
    PB_CHECK(pbo::change_event(edep, id, edit.value).ok());
    e.server->put_event("work", name, event_ics(uid, "Theirs", "20261015T093000", "20261015T100000"), pbtest::kEventNow, pbtest::kEventNow + 86400);
    pbo::PushCounts c;
    PB_CHECK(e.push(&c).ok());
    PB_CHECK_EQ(c.conflicts, 1);
    return id;
}

}  // namespace

PB_TEST(a_412_keeps_both_versions_and_sends_nothing_more) {
    EventEnv e;
    const int64_t id = conflicted(e, "a.ics", "c1");
    PB_CHECK_EQ(e.state_of(id), std::string("conflict"));
    PB_CHECK(e.store->event(id)->ics.find("SUMMARY:Mine") != std::string::npos);                       // the user's version
    PB_CHECK(e.q("SELECT server_ics FROM event_conflict WHERE event_id = " + std::to_string(id)).find("SUMMARY:Theirs") != std::string::npos);   // the server's
    PB_CHECK(e.server->event("work", "a.ics").ics.find("SUMMARY:Theirs") != std::string::npos);         // the server was not overwritten
    const int puts = e.server->count("PUT");
    PB_CHECK(e.push().ok());
    PB_CHECK_EQ(e.server->count("PUT"), puts);            // waits for the user
    PB_CHECK(e.pull().ok());                              // and a pull leaves both versions alone
    PB_CHECK_EQ(e.state_of(id), std::string("conflict"));
    PB_CHECK_EQ(e.store->event_problems().size(), size_t(1));
}

PB_TEST(keep_mine_overwrites_the_server_only_on_the_users_word) {
    EventEnv e;
    const int64_t id = conflicted(e, "a.ics", "c2");
    auto edep = e.edit_deps();
    PB_CHECK(pbo::resolve_event_conflict(edep, id, pbo::ConflictChoice::KeepMine).ok());
    PB_CHECK_EQ(e.state_of(id), std::string("pending_update"));
    PB_CHECK(e.push().ok());
    PB_CHECK_EQ(e.state_of(id), std::string("synced"));
    PB_CHECK(e.server->event("work", "a.ics").ics.find("SUMMARY:Mine") != std::string::npos);
    PB_CHECK_EQ(e.count("SELECT count(*) FROM event_conflict"), 0);
}

PB_TEST(keep_server_takes_the_servers_version_and_drops_mine) {
    EventEnv e;
    const int64_t id = conflicted(e, "a.ics", "c3");
    auto edep = e.edit_deps();
    PB_CHECK(pbo::resolve_event_conflict(edep, id, pbo::ConflictChoice::KeepServer).ok());
    PB_CHECK_EQ(e.state_of(id), std::string("synced"));
    PB_CHECK_EQ(e.q("SELECT summary FROM event WHERE id = " + std::to_string(id)), std::string("Theirs"));
    PB_CHECK_EQ(e.q("SELECT etag FROM event WHERE id = " + std::to_string(id)), e.server->event("work", "a.ics").etag);
    const int puts = e.server->count("PUT");
    PB_CHECK(e.push().ok());
    PB_CHECK_EQ(e.server->count("PUT"), puts);
    PB_CHECK_EQ(e.count("SELECT count(*) FROM event_conflict"), 0);
}

PB_TEST(keep_both_leaves_the_server_version_and_sends_mine_as_a_new_event) {
    EventEnv e;
    const int64_t id = conflicted(e, "a.ics", "c4");
    auto edep = e.edit_deps();
    PB_CHECK(pbo::resolve_event_conflict(edep, id, pbo::ConflictChoice::KeepBoth).ok());
    PB_CHECK_EQ(e.state_of(id), std::string("synced"));
    PB_CHECK_EQ(e.q("SELECT summary FROM event WHERE id = " + std::to_string(id)), std::string("Theirs"));
    PB_CHECK_EQ(e.count("SELECT count(*) FROM event WHERE sync_state = 'pending_create'"), 1);
    pbo::PushCounts c;
    PB_CHECK(e.push(&c).ok());
    PB_CHECK_EQ(c.created, 1);
    PB_CHECK_EQ(e.server->event_count("work"), size_t(2));
    PB_CHECK_EQ(e.count("SELECT count(*) FROM event WHERE sync_state <> 'synced'"), 0);
    // two different UIDs, one with each text
    PB_CHECK_EQ(e.count("SELECT count(DISTINCT uid) FROM event"), 2);
    PB_CHECK_EQ(e.count("SELECT count(*) FROM event WHERE summary = 'Mine'"), 1);
    PB_CHECK_EQ(e.count("SELECT count(*) FROM event WHERE summary = 'Theirs'"), 1);
}

PB_TEST(a_delete_that_meets_a_changed_server_version_is_a_conflict_and_keep_mine_still_deletes) {
    EventEnv e;
    const int64_t id = e.server_event("a.ics", "c5", "Original");
    PB_CHECK(pbo::delete_event(*e.store, id).ok());
    e.server->put_event("work", "a.ics", event_ics("c5", "Theirs", "20261015T093000", "20261015T100000"), pbtest::kEventNow, pbtest::kEventNow + 86400);
    pbo::PushCounts c;
    PB_CHECK(e.push(&c).ok());
    PB_CHECK_EQ(c.conflicts, 1);
    PB_CHECK(e.server->has_event("work", "a.ics"));                // nothing deleted behind the user's back
    const auto problems = e.store->event_problems();
    PB_CHECK_EQ(problems.size(), size_t(1));
    PB_CHECK(problems[0].deleting);
    auto edep = e.edit_deps();
    PB_CHECK(pbo::resolve_event_conflict(edep, id, pbo::ConflictChoice::KeepMine).ok());
    PB_CHECK(e.push().ok());
    PB_CHECK(!e.server->has_event("work", "a.ics"));
    PB_CHECK_EQ(e.count("SELECT count(*) FROM event"), 0);
}

PB_TEST(a_conflict_can_be_resolved_after_a_restart_because_both_versions_are_in_the_store) {
    EventEnv e;
    const int64_t id = conflicted(e, "a.ics", "c6");
    e.store.reset();
    auto reopened = pbo::Store::open(e.data.path(), e.clock);
    PB_CHECK(reopened.ok());
    e.store = std::move(reopened.value);
    PB_CHECK_EQ(e.store->event_problems().size(), size_t(1));
    auto edep = e.edit_deps();
    PB_CHECK(pbo::resolve_event_conflict(edep, id, pbo::ConflictChoice::KeepBoth).ok());
    PB_CHECK(e.push().ok());
    PB_CHECK_EQ(e.server->event_count("work"), size_t(2));
}

PB_TEST_MAIN()
