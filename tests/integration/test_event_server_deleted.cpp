// T142 (part): an event or a calendar that disappeared on the server never takes the user's edits with it. UNVERIFIED (fake server).
#include <string>

#include "event_env.h"

PBO_FAKE_SERVER_TEST();

using pbtest::EventEnv;

PB_TEST(an_edited_event_deleted_on_the_server_is_kept_and_marked) {
    EventEnv e;
    const int64_t id = e.server_event("a.ics", "g1", "Original");
    auto edit = pbo::edit_of_event(*e.store, *e.tz, id);
    edit.value.title = "Edited";
    auto edep = e.edit_deps();
    PB_CHECK(pbo::change_event(edep, id, edit.value).ok());
    e.server->remove_event("work", "a.ics");
    pbo::PushCounts c;
    PB_CHECK(e.push(&c).ok());
    PB_CHECK_EQ(c.server_deleted, 1);
    PB_CHECK_EQ(e.state_of(id), std::string("server_deleted"));
    PB_CHECK_EQ(e.q("SELECT summary FROM event WHERE id = " + std::to_string(id)), std::string("Edited"));
    PB_CHECK(e.pull().ok());                                           // a pull does not remove it either
    PB_CHECK_EQ(e.state_of(id), std::string("server_deleted"));
    PB_CHECK_EQ(e.store->event_problems().size(), size_t(1));
}

PB_TEST(re_creating_sends_it_again_as_a_new_resource) {
    EventEnv e;
    const int64_t id = e.server_event("a.ics", "g2", "Original");
    auto edit = pbo::edit_of_event(*e.store, *e.tz, id);
    edit.value.title = "Edited";
    auto edep = e.edit_deps();
    PB_CHECK(pbo::change_event(edep, id, edit.value).ok());
    e.server->remove_event("work", "a.ics");
    PB_CHECK(e.push().ok());
    PB_CHECK(e.store->recreate_event(id).ok());
    pbo::PushCounts c;
    PB_CHECK(e.push(&c).ok());
    PB_CHECK_EQ(c.created, 1);
    PB_CHECK_EQ(e.state_of(id), std::string("synced"));
    PB_CHECK_EQ(e.server->event_count("work"), size_t(1));
}

PB_TEST(discarding_removes_the_kept_version) {
    EventEnv e;
    const int64_t id = e.server_event("a.ics", "g3", "Original");
    auto edit = pbo::edit_of_event(*e.store, *e.tz, id);
    edit.value.title = "Edited";
    auto edep = e.edit_deps();
    PB_CHECK(pbo::change_event(edep, id, edit.value).ok());
    e.server->remove_event("work", "a.ics");
    PB_CHECK(e.push().ok());
    PB_CHECK(e.store->discard_event(id).ok());
    PB_CHECK_EQ(e.count("SELECT count(*) FROM event"), 0);
}

PB_TEST(deleting_an_event_that_is_already_gone_is_simply_done) {
    EventEnv e;
    const int64_t id = e.server_event("a.ics", "g4", "Original");
    PB_CHECK(pbo::delete_event(*e.store, id).ok());
    e.server->remove_event("work", "a.ics");
    PB_CHECK(e.push().ok());
    PB_CHECK_EQ(e.count("SELECT count(*) FROM event"), 0);
}

PB_TEST(a_calendar_removed_on_the_server_keeps_events_with_unsynced_changes) {
    EventEnv e;
    const int64_t id = e.server_event("a.ics", "g5", "Original");
    auto edit = pbo::edit_of_event(*e.store, *e.tz, id);
    edit.value.title = "Edited offline";
    auto edep = e.edit_deps();
    PB_CHECK(pbo::change_event(edep, id, edit.value).ok());
    const int64_t other = e.server_event("b.ics", "g6", "Untouched", "20261016T093000", "20261016T100000");
    e.server->remove_calendar("work");
    pbtest::FakeCalDavServer::CalendarOptions o;
    o.name = "Home";
    e.server->add_calendar("home", o);                                 // another calendar remains, so discovery works
    PB_CHECK(e.pull().ok());
    PB_CHECK_EQ(e.q("SELECT present_on_server FROM calendar WHERE id = " + std::to_string(e.work)), std::string("0"));
    pbo::PushCounts c;
    PB_CHECK(e.push(&c).ok());
    PB_CHECK_EQ(c.server_deleted, 1);
    PB_CHECK_EQ(e.state_of(id), std::string("server_deleted"));
    PB_CHECK_EQ(e.q("SELECT summary FROM event WHERE id = " + std::to_string(id)), std::string("Edited offline"));
    PB_CHECK_EQ(e.count("SELECT count(*) FROM event WHERE id = " + std::to_string(other)), 1);   // synced copies stay too (a calendar that vanishes keeps its events)
}

PB_TEST_MAIN()
