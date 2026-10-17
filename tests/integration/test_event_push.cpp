// T141 (part): creating, changing and deleting events reach the server with conditional requests, once, and the "not yet synced" mark clears
// only after the server confirmed. UNVERIFIED against real servers (fake server).
#include <string>

#include "event_env.h"

PBO_FAKE_SERVER_TEST();

using pbtest::EventEnv;
using pbtest::event_ics;

namespace {

std::string header(const pbtest::FakeCalDavServer::Logged& l, const char* name) {
    auto it = l.headers.find(name);
    return it == l.headers.end() ? std::string() : it->second;
}

}  // namespace

PB_TEST(a_new_event_is_sent_once_with_if_none_match_and_then_marked_synced) {
    EventEnv e;
    auto edep = e.edit_deps();
    auto r = pbo::create_event(edep, e.work, EventEnv::edit("Dentist"));
    PB_CHECK(r.ok());
    PB_CHECK_EQ(e.state_of(r.value), std::string("pending_create"));
    pbo::PushCounts c;
    PB_CHECK(e.push(&c).ok());
    PB_CHECK_EQ(c.created, 1);
    PB_CHECK_EQ(e.server->count("PUT"), 1);
    PB_CHECK_EQ(e.state_of(r.value), std::string("synced"));
    PB_CHECK_EQ(e.server->event_count("work"), size_t(1));
    bool saw = false;
    for (const auto& l : e.server->log())
        if (l.method == "PUT") {
            saw = true;
            PB_CHECK_EQ(header(l, "if-none-match"), std::string("*"));
            PB_CHECK(header(l, "if-match").empty());
            PB_CHECK(l.path.find("/dav/calendars/alice/work/") == 0);
            PB_CHECK(l.body.find("SUMMARY:Dentist") != std::string::npos);
        }
    PB_CHECK(saw);
    PB_CHECK(!e.q("SELECT etag FROM event WHERE id = " + std::to_string(r.value)).empty());
    // nothing is waiting any more: another push makes no request
    const int before = e.server->count("PUT") + e.server->count("DELETE");
    PB_CHECK(e.push().ok());
    PB_CHECK_EQ(e.server->count("PUT") + e.server->count("DELETE"), before);
    // and the next pull sees the event as the server's, unchanged
    PB_CHECK(e.pull().ok());
    PB_CHECK_EQ(e.count("SELECT count(*) FROM event"), 1);
    PB_CHECK_EQ(e.state_of(r.value), std::string("synced"));
}

PB_TEST(a_changed_event_is_sent_with_if_match_the_stored_etag_and_a_raised_sequence) {
    EventEnv e;
    const int64_t id = e.server_event("a.ics", "a1", "Standup");
    const std::string etag = e.q("SELECT etag FROM event WHERE id = " + std::to_string(id));
    auto edit = pbo::edit_of_event(*e.store, *e.tz, id);
    PB_CHECK(edit.ok());
    edit.value.title = "Standup (moved)";
    edit.value.start = {2026, 10, 15, 10, 0, 0};
    edit.value.end = {2026, 10, 15, 10, 30, 0};
    auto edep = e.edit_deps();
    PB_CHECK(pbo::change_event(edep, id, edit.value).ok());
    PB_CHECK_EQ(e.state_of(id), std::string("pending_update"));
    pbo::PushCounts c;
    PB_CHECK(e.push(&c).ok());
    PB_CHECK_EQ(c.updated, 1);
    PB_CHECK_EQ(e.state_of(id), std::string("synced"));
    for (const auto& l : e.server->log())
        if (l.method == "PUT") {
            PB_CHECK_EQ(header(l, "if-match"), etag);
            PB_CHECK(header(l, "if-none-match").empty());
        }
    const auto server_copy = e.server->event("work", "a.ics");
    PB_CHECK(server_copy.ics.find("SUMMARY:Standup (moved)") != std::string::npos);
    PB_CHECK(server_copy.ics.find("SEQUENCE:1") != std::string::npos);
    PB_CHECK(server_copy.ics.find("DTSTART;TZID=Europe/Berlin:20261015T100000") != std::string::npos);
    PB_CHECK_EQ(e.q("SELECT etag FROM event WHERE id = " + std::to_string(id)), server_copy.etag);
}

PB_TEST(a_deleted_event_is_removed_with_if_match_and_the_row_goes_when_the_server_confirmed) {
    EventEnv e;
    const int64_t id = e.server_event("a.ics", "a1", "Standup");
    const std::string etag = e.q("SELECT etag FROM event WHERE id = " + std::to_string(id));
    PB_CHECK(pbo::delete_event(*e.store, id).ok());
    PB_CHECK_EQ(e.state_of(id), std::string("pending_delete"));
    PB_CHECK(e.server->has_event("work", "a.ics"));     // not before the push
    pbo::PushCounts c;
    PB_CHECK(e.push(&c).ok());
    PB_CHECK_EQ(c.deleted, 1);
    PB_CHECK(!e.server->has_event("work", "a.ics"));
    PB_CHECK_EQ(e.count("SELECT count(*) FROM event"), 0);
    for (const auto& l : e.server->log())
        if (l.method == "DELETE") PB_CHECK_EQ(header(l, "if-match"), etag);
    PB_CHECK(e.pull().ok());
    PB_CHECK_EQ(e.count("SELECT count(*) FROM event"), 0);
}

PB_TEST(with_nothing_waiting_the_push_makes_no_request_at_all) {
    EventEnv e;
    e.server_event("a.ics", "a1", "Standup");
    const size_t before = e.server->log().size();
    PB_CHECK(e.push().ok());
    PB_CHECK_EQ(e.server->log().size(), before);
}

PB_TEST(a_failed_request_keeps_the_event_waiting_and_a_retry_sends_it_exactly_once) {
    EventEnv e;
    auto edep = e.edit_deps();
    auto r = pbo::create_event(edep, e.work, EventEnv::edit("Retry me"));
    PB_CHECK(r.ok());
    pbtest::FakeCalDavServer::Fault f;
    f.method = "PUT";
    f.status = 500;
    e.server->inject(f);
    pbo::PushCounts c;
    const pbo::Error err = e.push(&c);
    PB_CHECK(!err.ok());
    PB_CHECK_EQ(c.created, 0);
    PB_CHECK_EQ(e.state_of(r.value), std::string("pending_create"));
    PB_CHECK_EQ(e.server->event_count("work"), size_t(0));
    PB_CHECK(e.push(&c).ok());
    PB_CHECK_EQ(c.created, 1);
    PB_CHECK_EQ(e.state_of(r.value), std::string("synced"));
    PB_CHECK_EQ(e.server->event_count("work"), size_t(1));
}

PB_TEST(a_refused_write_keeps_the_change_waiting_and_says_why) {
    EventEnv e;
    const int64_t id = e.server_event("a.ics", "a1", "Standup");
    auto edit = pbo::edit_of_event(*e.store, *e.tz, id);
    edit.value.title = "Changed";
    auto edep = e.edit_deps();
    PB_CHECK(pbo::change_event(edep, id, edit.value).ok());
    pbtest::FakeCalDavServer::Fault f;
    f.method = "PUT";
    f.status = 403;
    e.server->inject(f);
    pbo::PushCounts c;
    const pbo::Error err = e.push(&c);
    PB_CHECK_EQ(err.key, std::string("read_only"));
    PB_CHECK_EQ(c.refused, 1);
    PB_CHECK_EQ(e.state_of(id), std::string("pending_update"));
    PB_CHECK(e.server->event("work", "a.ics").ics.find("Changed") == std::string::npos);
}

PB_TEST(an_answer_lost_after_a_create_does_not_make_a_duplicate_at_the_next_push) {
    EventEnv e;
    auto edep = e.edit_deps();
    auto r = pbo::create_event(edep, e.work, EventEnv::edit("Once only"));
    PB_CHECK(r.ok());
    // the server stored the event, but the reply never reached the app: the row is still pending_create
    const std::string text = e.store->event(r.value)->ics;
    const std::string uid = e.q("SELECT uid FROM event WHERE id = " + std::to_string(r.value));
    e.server->put_event("work", uid + ".ics", text);
    pbo::PushCounts c;
    PB_CHECK(e.push(&c).ok());
    PB_CHECK_EQ(c.conflicts, 0);
    PB_CHECK_EQ(e.state_of(r.value), std::string("synced"));
    PB_CHECK_EQ(e.server->event_count("work"), size_t(1));
    PB_CHECK_EQ(e.count("SELECT count(*) FROM event"), 1);
}

PB_TEST(an_answer_lost_after_an_update_is_recognised_by_the_content) {
    EventEnv e;
    const int64_t id = e.server_event("a.ics", "a1", "Standup");
    auto edit = pbo::edit_of_event(*e.store, *e.tz, id);
    edit.value.title = "Standup 2";
    auto edep = e.edit_deps();
    PB_CHECK(pbo::change_event(edep, id, edit.value).ok());
    // the server applied it (new etag) but the app never heard
    e.server->put_event("work", "a.ics", e.store->event(id)->ics);
    pbo::PushCounts c;
    PB_CHECK(e.push(&c).ok());
    PB_CHECK_EQ(c.conflicts, 0);
    PB_CHECK_EQ(e.state_of(id), std::string("synced"));
    PB_CHECK_EQ(e.q("SELECT etag FROM event WHERE id = " + std::to_string(id)), e.server->event("work", "a.ics").etag);
}

PB_TEST(an_edit_made_while_the_push_is_running_is_sent_by_the_next_push) {
    EventEnv e;
    const int64_t id = e.server_event("a.ics", "a1", "Standup");
    auto edit = pbo::edit_of_event(*e.store, *e.tz, id);
    edit.value.title = "First edit";
    auto edep = e.edit_deps();
    PB_CHECK(pbo::change_event(edep, id, edit.value).ok());
    bool once = false;
    e.server->before_write([&](pbtest::FakeCalDavServer&) {
        if (once) return;
        once = true;
        auto again = pbo::edit_of_event(*e.store, *e.tz, id);
        again.value.title = "Second edit";
        auto d2 = e.edit_deps();
        pbo::change_event(d2, id, again.value);
    });
    PB_CHECK(e.push().ok());
    PB_CHECK_EQ(e.state_of(id), std::string("pending_update"));     // the first edit is up, the second still waits
    PB_CHECK(e.server->event("work", "a.ics").ics.find("First edit") != std::string::npos);
    PB_CHECK(e.push().ok());
    PB_CHECK_EQ(e.state_of(id), std::string("synced"));
    PB_CHECK(e.server->event("work", "a.ics").ics.find("Second edit") != std::string::npos);
    PB_CHECK_EQ(e.server->event_count("work"), size_t(1));
}

PB_TEST_MAIN()
