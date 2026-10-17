// T145 (store part): the event sync states of data-model.md for edited events: create, update, delete, conflict, server_deleted, and the
// rule that no pull and no window change touches local work.
#include <string>
#include <vector>

#include "pbo/store.h"
#include "test_support.h"
#include "tmp_dir.h"

namespace {

struct FixedClock : pbo::Clock {
    int64_t now() const override { return 1791979200; }
};

constexpr int64_t kNow = 1791979200;
constexpr int64_t kHour = 3600;

struct Env {
    pbtest::TmpDir dir;
    FixedClock clock;
    std::unique_ptr<pbo::Store> store;
    int64_t cal = 0, feed = 0, ro = 0;
    Env() {
        auto r = pbo::Store::open(dir.path(), clock);
        PB_CHECK(r.ok());
        store = std::move(r.value);
        cal = add("/dav/work/", "Work", true, "caldav");
        ro = add("/dav/ro/", "Shared", false, "caldav");
        feed = add("https://example.org/h.ics", "Feed", false, "feed");
    }
    int64_t add(const std::string& href, const std::string& name, bool writable, const char* kind) {
        pbo::Calendar c;
        c.kind = kind;
        c.href = href;
        c.name = name;
        c.writable = writable;
        auto r = store->upsert_calendar(c);
        PB_CHECK(r.ok());
        return r.value;
    }
    std::string q(const std::string& sql) { return store->debug_query(sql); }
};

pbo::EventUpsert rec(const std::string& uid, const std::string& summary, int64_t start = kNow + kHour, const std::string& ics_tail = "") {
    pbo::EventUpsert e;
    e.uid = uid;
    e.ics = "BEGIN:VCALENDAR\r\nBEGIN:VEVENT\r\nUID:" + uid + "\r\nSUMMARY:" + summary + "\r\n" + ics_tail + "END:VEVENT\r\nEND:VCALENDAR\r\n";
    e.summary = summary;
    e.start_utc = start;
    e.end_utc = start + kHour;
    e.start_local = "2026-10-14T14:00:00";
    pbo::OccurrenceRow o;
    o.start_utc = start;
    o.end_utc = start + kHour;
    o.local_date = "2026-10-14";
    e.occurrences.push_back(o);
    return e;
}

// a synced event as a pull leaves it
int64_t synced(Env& e, int64_t cal, const std::string& uid, const std::string& summary) {
    pbo::CalendarPullBatch b;
    pbo::EventUpsert u = rec(uid, summary);
    u.href = "/dav/work/" + uid + ".ics";
    u.etag = "\"e1\"";
    b.upserts.push_back(u);
    PB_CHECK(e.store->apply_calendar_pull(cal, b).ok());
    return std::atoi(e.q("SELECT id FROM event WHERE uid = '" + uid + "'").c_str());
}

}  // namespace

PB_TEST(a_new_event_is_pending_create_with_its_occurrences_and_shows_in_the_agenda) {
    Env e;
    auto r = e.store->save_event_local(e.cal, 0, rec("n1@x", "Lunch"));
    PB_CHECK(r.ok());
    PB_CHECK_EQ(e.q("SELECT sync_state FROM event WHERE id = " + std::to_string(r.value)), std::string("pending_create"));
    PB_CHECK_EQ(e.q("SELECT count(*) FROM occurrence WHERE event_id = " + std::to_string(r.value)), std::string("1"));
    PB_CHECK_EQ(e.q("SELECT COALESCE(href,'-') FROM event WHERE id = " + std::to_string(r.value)), std::string("-"));
    const auto page = e.store->agenda(kNow, 0, 10);
    PB_CHECK_EQ(page.items.size(), size_t(1));
    PB_CHECK_EQ(page.items[0].sync_state, std::string("pending_create"));
    const auto pend = e.store->pending_events();
    PB_CHECK_EQ(pend.size(), size_t(1));
    PB_CHECK_EQ(pend[0].calendar_href, std::string("/dav/work/"));
    PB_CHECK_EQ(pend[0].sync_state, std::string("pending_create"));
}

PB_TEST(a_feed_a_read_only_calendar_and_a_missing_calendar_refuse_new_events) {
    Env e;
    PB_CHECK_EQ(e.store->save_event_local(e.feed, 0, rec("a@x", "A")).error.key, std::string("read_only"));
    PB_CHECK_EQ(e.store->save_event_local(e.ro, 0, rec("b@x", "B")).error.key, std::string("read_only"));
    PB_CHECK_EQ(e.store->save_event_local(9999, 0, rec("c@x", "C")).error.key, std::string("not_found"));
    PB_CHECK_EQ(e.q("SELECT count(*) FROM event"), std::string("0"));
}

PB_TEST(a_second_event_with_the_same_uid_in_a_calendar_is_refused) {
    Env e;
    PB_CHECK(e.store->save_event_local(e.cal, 0, rec("u@x", "One")).ok());
    PB_CHECK_EQ(e.store->save_event_local(e.cal, 0, rec("u@x", "Two")).error.key, std::string("duplicate_uid"));
}

PB_TEST(editing_a_synced_event_makes_it_pending_update_and_keeps_href_and_etag) {
    Env e;
    const int64_t id = synced(e, e.cal, "s1@x", "Old");
    auto r = e.store->save_event_local(e.cal, id, rec("s1@x", "New", kNow + 2 * kHour));
    PB_CHECK(r.ok());
    PB_CHECK_EQ(r.value, id);
    PB_CHECK_EQ(e.q("SELECT sync_state || '|' || summary || '|' || href || '|' || etag FROM event WHERE id = " + std::to_string(id)), std::string("pending_update|New|/dav/work/s1@x.ics|\"e1\""));
    PB_CHECK_EQ(e.q("SELECT start_utc FROM occurrence WHERE event_id = " + std::to_string(id)), std::to_string(kNow + 2 * kHour));
}

PB_TEST(editing_a_pending_create_stays_pending_create) {
    Env e;
    auto a = e.store->save_event_local(e.cal, 0, rec("p@x", "First"));
    PB_CHECK(a.ok());
    PB_CHECK(e.store->save_event_local(e.cal, a.value, rec("p@x", "Second")).ok());
    PB_CHECK_EQ(e.q("SELECT sync_state || '|' || summary FROM event WHERE id = " + std::to_string(a.value)), std::string("pending_create|Second"));
}

PB_TEST(a_pull_never_overwrites_or_removes_local_work) {
    Env e;
    const int64_t id = synced(e, e.cal, "w@x", "Mine");
    PB_CHECK(e.store->save_event_local(e.cal, id, rec("w@x", "Mine edited")).ok());
    pbo::CalendarPullBatch b;
    pbo::EventUpsert theirs = rec("w@x", "Theirs");
    theirs.href = "/dav/work/w@x.ics";
    theirs.etag = "\"e2\"";
    b.upserts.push_back(theirs);
    PB_CHECK(e.store->apply_calendar_pull(e.cal, b).ok());
    PB_CHECK_EQ(e.q("SELECT summary || '|' || etag || '|' || sync_state FROM event WHERE id = " + std::to_string(id)), std::string("Mine edited|\"e1\"|pending_update"));
    pbo::CalendarPullBatch gone;
    gone.full = true;   // a complete list that does not name it
    PB_CHECK(e.store->apply_calendar_pull(e.cal, gone).ok());
    PB_CHECK_EQ(e.q("SELECT count(*) FROM event WHERE id = " + std::to_string(id)), std::string("1"));
}

PB_TEST(deleting_removes_a_never_pushed_event_and_marks_a_synced_one_for_deletion) {
    Env e;
    auto a = e.store->save_event_local(e.cal, 0, rec("d1@x", "Never pushed"));
    PB_CHECK(e.store->request_event_delete(a.value).ok());
    PB_CHECK_EQ(e.q("SELECT count(*) FROM event"), std::string("0"));

    const int64_t id = synced(e, e.cal, "d2@x", "Pushed");
    PB_CHECK(e.store->request_event_delete(id).ok());
    PB_CHECK_EQ(e.q("SELECT sync_state FROM event WHERE id = " + std::to_string(id)), std::string("pending_delete"));
    PB_CHECK_EQ(e.q("SELECT count(*) FROM occurrence WHERE event_id = " + std::to_string(id)), std::string("0"));   // gone from every view at once
    PB_CHECK_EQ(e.store->agenda(0, 0, 10).items.size(), size_t(0));
    PB_CHECK_EQ(e.store->pending_events().size(), size_t(1));
    PB_CHECK_EQ(e.store->save_event_local(e.cal, id, rec("d2@x", "again")).error.key, std::string("delete_pending"));
}

PB_TEST(a_read_only_calendars_events_cannot_be_deleted_or_edited) {
    Env e;
    const int64_t id = synced(e, e.ro, "r@x", "Shared event");
    PB_CHECK_EQ(e.store->request_event_delete(id).key, std::string("read_only"));
    PB_CHECK_EQ(e.store->save_event_local(e.ro, id, rec("r@x", "x")).error.key, std::string("read_only"));
    PB_CHECK_EQ(e.q("SELECT sync_state FROM event WHERE id = " + std::to_string(id)), std::string("synced"));
}

PB_TEST(the_mark_clears_only_when_the_server_confirmed_and_an_edit_made_meanwhile_stays_pending) {
    Env e;
    const int64_t id = synced(e, e.cal, "m@x", "Base");
    const pbo::EventUpsert first = rec("m@x", "First edit");
    PB_CHECK(e.store->save_event_local(e.cal, id, first).ok());
    // the push of `first` is in flight; the user edits again
    PB_CHECK(e.store->save_event_local(e.cal, id, rec("m@x", "Second edit")).ok());
    PB_CHECK(e.store->event_pushed(id, first.ics, "/dav/work/m@x.ics", "\"e2\"").ok());
    PB_CHECK_EQ(e.q("SELECT sync_state || '|' || etag || '|' || summary FROM event WHERE id = " + std::to_string(id)), std::string("pending_update|\"e2\"|Second edit"));
    // and now the second edit goes up
    const auto now_text = e.store->event(id)->ics;
    PB_CHECK(e.store->event_pushed(id, now_text, "/dav/work/m@x.ics", "\"e3\"").ok());
    PB_CHECK_EQ(e.q("SELECT sync_state || '|' || etag FROM event WHERE id = " + std::to_string(id)), std::string("synced|\"e3\""));
    PB_CHECK_EQ(e.store->pending_events().size(), size_t(0));
}

PB_TEST(a_created_event_gets_its_href_and_etag_when_the_server_confirms) {
    Env e;
    auto a = e.store->save_event_local(e.cal, 0, rec("c@x", "New"));
    PB_CHECK(e.store->event_pushed(a.value, e.store->event(a.value)->ics, "/dav/work/c@x.ics", "\"e9\"").ok());
    PB_CHECK_EQ(e.q("SELECT sync_state || '|' || href || '|' || etag FROM event WHERE id = " + std::to_string(a.value)), std::string("synced|/dav/work/c@x.ics|\"e9\""));
}

PB_TEST(a_412_keeps_both_versions_side_by_side_and_lists_a_problem) {
    Env e;
    const int64_t id = synced(e, e.cal, "k@x", "Base");
    PB_CHECK(e.store->save_event_local(e.cal, id, rec("k@x", "Mine")).ok());
    const std::string theirs = rec("k@x", "Theirs").ics;
    PB_CHECK(e.store->event_conflicted(id, theirs, "\"e5\"", "/dav/work/k@x.ics").ok());
    PB_CHECK_EQ(e.q("SELECT sync_state FROM event WHERE id = " + std::to_string(id)), std::string("conflict"));
    PB_CHECK_EQ(e.q("SELECT server_ics FROM event_conflict WHERE event_id = " + std::to_string(id)), theirs);
    const auto problems = e.store->event_problems();
    PB_CHECK_EQ(problems.size(), size_t(1));
    PB_CHECK_EQ(problems[0].state, std::string("conflict"));
    PB_CHECK_EQ(problems[0].summary, std::string("Mine"));
    PB_CHECK_EQ(problems[0].server_etag, std::string("\"e5\""));
    PB_CHECK(!problems[0].deleting);
    PB_CHECK_EQ(e.store->pending_events().size(), size_t(0));     // nothing goes up until the user decides
    PB_CHECK_EQ(e.store->save_event_local(e.cal, id, rec("k@x", "more")).error.key, std::string("in_conflict"));
    PB_CHECK_EQ(e.store->request_event_delete(id).key, std::string("in_conflict"));
}

PB_TEST(keep_mine_goes_up_again_against_the_servers_etag) {
    Env e;
    const int64_t id = synced(e, e.cal, "k1@x", "Base");
    PB_CHECK(e.store->save_event_local(e.cal, id, rec("k1@x", "Mine")).ok());
    PB_CHECK(e.store->event_conflicted(id, rec("k1@x", "Theirs").ics, "\"e5\"", "/dav/work/x.ics").ok());
    PB_CHECK(e.store->keep_my_event(id).ok());
    PB_CHECK_EQ(e.q("SELECT sync_state || '|' || etag || '|' || summary FROM event WHERE id = " + std::to_string(id)), std::string("pending_update|\"e5\"|Mine"));
    PB_CHECK_EQ(e.q("SELECT count(*) FROM event_conflict"), std::string("0"));
}

PB_TEST(keep_server_replaces_the_local_version_and_keep_both_keeps_mine_as_a_new_event) {
    Env e;
    int64_t id = synced(e, e.cal, "k2@x", "Base");
    PB_CHECK(e.store->save_event_local(e.cal, id, rec("k2@x", "Mine")).ok());
    PB_CHECK(e.store->event_conflicted(id, rec("k2@x", "Theirs").ics, "\"e6\"", "/dav/work/x.ics").ok());
    pbo::EventUpsert server = rec("k2@x", "Theirs");
    server.href = "/dav/work/k2@x.ics";
    server.etag = "\"e6\"";
    PB_CHECK(e.store->keep_server_event(id, server).ok());
    PB_CHECK_EQ(e.q("SELECT sync_state || '|' || etag || '|' || summary FROM event WHERE id = " + std::to_string(id)), std::string("synced|\"e6\"|Theirs"));
    PB_CHECK_EQ(e.q("SELECT count(*) FROM event_conflict"), std::string("0"));

    id = synced(e, e.cal, "k3@x", "Base");
    PB_CHECK(e.store->save_event_local(e.cal, id, rec("k3@x", "Mine")).ok());
    PB_CHECK(e.store->event_conflicted(id, rec("k3@x", "Theirs").ics, "\"e7\"", "/dav/work/x.ics").ok());
    pbo::EventUpsert srv = rec("k3@x", "Theirs");
    srv.href = "/dav/work/k3@x.ics";
    srv.etag = "\"e7\"";
    const pbo::EventUpsert copy = rec("copy-k3@pboffice.invalid", "Mine");
    PB_CHECK(e.store->keep_both_events(id, srv, copy).ok());
    PB_CHECK_EQ(e.q("SELECT sync_state || '|' || summary FROM event WHERE uid = 'k3@x'"), std::string("synced|Theirs"));
    PB_CHECK_EQ(e.q("SELECT sync_state || '|' || summary FROM event WHERE uid = 'copy-k3@pboffice.invalid'"), std::string("pending_create|Mine"));
}

PB_TEST(a_delete_that_meets_a_server_change_is_a_conflict_and_keep_mine_stays_a_delete) {
    Env e;
    const int64_t id = synced(e, e.cal, "dc@x", "Base");
    PB_CHECK(e.store->request_event_delete(id).ok());
    PB_CHECK(e.store->event_conflicted(id, rec("dc@x", "Theirs").ics, "\"e8\"", "/dav/work/x.ics").ok());
    const auto p = e.store->event_problems();
    PB_CHECK_EQ(p.size(), size_t(1));
    PB_CHECK(p[0].deleting);
    PB_CHECK(e.store->keep_my_event(id).ok());
    PB_CHECK_EQ(e.q("SELECT sync_state || '|' || etag FROM event WHERE id = " + std::to_string(id)), std::string("pending_delete|\"e8\""));
}

PB_TEST(an_event_deleted_on_the_server_keeps_the_edited_version_until_the_user_decides) {
    Env e;
    const int64_t id = synced(e, e.cal, "g@x", "Base");
    PB_CHECK(e.store->save_event_local(e.cal, id, rec("g@x", "Edited")).ok());
    PB_CHECK(e.store->event_gone_on_server(id).ok());
    PB_CHECK_EQ(e.q("SELECT sync_state || '|' || summary FROM event WHERE id = " + std::to_string(id)), std::string("server_deleted|Edited"));
    PB_CHECK_EQ(e.store->event_problems().size(), size_t(1));
    PB_CHECK_EQ(e.store->pending_events().size(), size_t(0));
    // a pull that lists nothing for it does not remove it
    pbo::CalendarPullBatch full;
    full.full = true;
    PB_CHECK(e.store->apply_calendar_pull(e.cal, full).ok());
    PB_CHECK_EQ(e.q("SELECT count(*) FROM event WHERE id = " + std::to_string(id)), std::string("1"));
    PB_CHECK(e.store->recreate_event(id).ok());
    PB_CHECK_EQ(e.q("SELECT sync_state || '|' || COALESCE(href,'-') || '|' || COALESCE(etag,'-') FROM event WHERE id = " + std::to_string(id)), std::string("pending_create|-|-"));
}

PB_TEST(discarding_a_server_deleted_event_removes_it_and_deleting_a_gone_event_is_done) {
    Env e;
    const int64_t id = synced(e, e.cal, "g2@x", "Base");
    PB_CHECK(e.store->save_event_local(e.cal, id, rec("g2@x", "Edited")).ok());
    PB_CHECK(e.store->event_gone_on_server(id).ok());
    PB_CHECK(e.store->discard_event(id).ok());
    PB_CHECK_EQ(e.q("SELECT count(*) FROM event"), std::string("0"));
    PB_CHECK_EQ(e.store->discard_event(id).key, std::string("not_found"));

    const int64_t d = synced(e, e.cal, "g3@x", "Base");
    PB_CHECK(e.store->request_event_delete(d).ok());
    PB_CHECK(e.store->event_gone_on_server(d).ok());   // already gone on the server: nothing left to do
    PB_CHECK_EQ(e.q("SELECT count(*) FROM event"), std::string("0"));
    const int64_t d2 = synced(e, e.cal, "g4@x", "Base");
    PB_CHECK(e.store->request_event_delete(d2).ok());
    PB_CHECK(e.store->event_removed_on_server(d2).ok());
    PB_CHECK_EQ(e.q("SELECT count(*) FROM event"), std::string("0"));
}

PB_TEST(unsynced_events_survive_a_calendar_that_is_no_longer_on_the_server) {
    Env e;
    const int64_t id = synced(e, e.cal, "x@x", "Base");
    PB_CHECK(e.store->save_event_local(e.cal, id, rec("x@x", "Edited")).ok());
    PB_CHECK(e.store->mark_calendars_present("caldav", {"/dav/ro/"}).ok());   // "work" is gone
    PB_CHECK_EQ(e.q("SELECT present_on_server FROM calendar WHERE id = " + std::to_string(e.cal)), std::string("0"));
    const auto pend = e.store->pending_events();
    PB_CHECK_EQ(pend.size(), size_t(1));
    PB_CHECK(!pend[0].calendar_present);
    // a new event cannot be created in a calendar that is gone
    PB_CHECK_EQ(e.store->save_event_local(e.cal, 0, rec("y@x", "New")).error.key, std::string("read_only"));
}

PB_TEST_MAIN()
