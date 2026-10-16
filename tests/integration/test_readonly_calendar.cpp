// T142 (part): feeds and calendars without write access refuse edits with a reason, and a refused write on the server keeps the change.
// UNVERIFIED against real servers (fake server).
#include <string>

#include "event_env.h"

PBO_FAKE_SERVER_TEST();

using pbtest::EventEnv;

PB_TEST(a_calendar_without_write_access_refuses_new_events_and_edits_with_the_reason_read_only) {
    EventEnv e(true);
    int64_t shared = 0;
    for (const auto& c : e.store->calendars())
        if (c.name == "Shared") shared = c.id;
    PB_CHECK(shared != 0);
    auto edep = e.edit_deps();
    PB_CHECK_EQ(pbo::create_event(edep, shared, EventEnv::edit("No")).error.key, std::string("read_only"));
    e.server->put_event("shared", "s.ics", pbtest::event_ics("r1", "Shared event", "20261015T093000", "20261015T100000"), pbtest::kEventNow, pbtest::kEventNow + 86400);
    PB_CHECK(e.pull().ok());
    const int64_t id = e.id_of("r1@example.org");
    PB_CHECK_EQ(pbo::event_edit_block_reason(*e.store->event(id), *e.tz), std::string("read_only"));
    auto edit = pbo::edit_of_event(*e.store, *e.tz, id);
    PB_CHECK(edit.ok());
    edit.value.title = "x";
    PB_CHECK_EQ(pbo::change_event(edep, id, edit.value).error.key, std::string("read_only"));
    PB_CHECK_EQ(pbo::delete_event(*e.store, id).key, std::string("read_only"));
    PB_CHECK_EQ(e.state_of(id), std::string("synced"));
}

PB_TEST(a_feed_is_read_only) {
    EventEnv e;
    e.server->set_feed("/feeds/h.ics", "BEGIN:VCALENDAR\r\nVERSION:2.0\r\nPRODID:-//x//y//EN\r\nBEGIN:VEVENT\r\nUID:f1@example.org\r\nDTSTAMP:20260101T000000Z\r\n"
                                       "SUMMARY:Holiday\r\nDTSTART;VALUE=DATE:20261020\r\nDTEND;VALUE=DATE:20261021\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n");
    pbo::CalendarAccount a = *e.store->calendar_account();
    a.feed_url = e.server->origin() + "/feeds/h.ics";
    PB_CHECK(e.store->save_calendar_account(a).ok());
    PB_CHECK(e.pull().ok());
    const int64_t id = e.id_of("f1@example.org");
    PB_CHECK(id != 0);
    PB_CHECK_EQ(pbo::event_edit_block_reason(*e.store->event(id), *e.tz), std::string("feed"));
    PB_CHECK_EQ(pbo::delete_event(*e.store, id).key, std::string("read_only"));
}

PB_TEST(a_repeating_event_is_read_only_but_can_be_deleted_as_a_whole) {
    EventEnv e;
    e.server->put_event("work", "w.ics", pbtest::event_ics("w1", "Weekly", "20261019T090000", "20261019T100000", "RRULE:FREQ=WEEKLY;BYDAY=MO\r\n"), 0);
    PB_CHECK(e.pull().ok());
    const int64_t id = e.id_of("w1@example.org");
    PB_CHECK_EQ(pbo::event_edit_block_reason(*e.store->event(id), *e.tz), std::string("series_read_only"));
    auto edit = pbo::edit_of_event(*e.store, *e.tz, id);
    edit.value.title = "x";
    auto edep = e.edit_deps();
    PB_CHECK_EQ(pbo::change_event(edep, id, edit.value).error.key, std::string("series_read_only"));
    PB_CHECK_EQ(e.state_of(id), std::string("synced"));
    PB_CHECK(pbo::delete_event(*e.store, id).ok());
    PB_CHECK(e.push().ok());
    PB_CHECK(!e.server->has_event("work", "w.ics"));
}

PB_TEST(a_403_on_a_calendar_that_claimed_write_access_keeps_the_change_pending) {
    EventEnv e;
    auto edep = e.edit_deps();
    auto r = pbo::create_event(edep, e.work, EventEnv::edit("Refused"));
    PB_CHECK(r.ok());
    pbtest::FakeCalDavServer::Fault f;
    f.method = "PUT";
    f.status = 403;
    f.times = 5;
    e.server->inject(f);
    pbo::PushCounts c;
    PB_CHECK_EQ(e.push(&c).key, std::string("read_only"));
    PB_CHECK_EQ(c.refused, 1);
    PB_CHECK_EQ(e.state_of(r.value), std::string("pending_create"));
    PB_CHECK_EQ(e.store->pending_events().size(), size_t(1));
}

PB_TEST_MAIN()
