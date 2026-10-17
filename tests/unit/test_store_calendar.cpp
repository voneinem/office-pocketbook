// T101: the calendar side of the store (data-model.md: account_calendar, calendar, event, occurrence): constraints, one-transaction pulls,
// pending events that no pull may touch, and the queries behind the agenda and the day marks.
#include <string>
#include <vector>

#include "pbo/store.h"
#include "test_support.h"
#include "tmp_dir.h"

namespace {

struct FixedClock : pbo::Clock {
    int64_t now() const override { return 1791979200; }   // 2026-10-14 12:00:00 UTC
};

constexpr int64_t kNow = 1791979200;
constexpr int64_t kHour = 3600;
constexpr int64_t kDay = 86400;

struct Env {
    pbtest::TmpDir dir;
    FixedClock clock;
    std::unique_ptr<pbo::Store> store;
    Env() {
        auto r = pbo::Store::open(dir.path(), clock);
        PB_CHECK(r.ok());
        store = std::move(r.value);
    }
    int64_t add_calendar(const std::string& href, const std::string& name, bool writable = true, const char* kind = "caldav") {
        pbo::Calendar c;
        c.kind = kind;
        c.href = href;
        c.name = name;
        c.writable = writable;
        auto r = store->upsert_calendar(c);
        PB_CHECK(r.ok());
        return r.value;
    }
};

pbo::EventUpsert ev(const std::string& uid, const std::string& summary, int64_t start, int64_t len = kHour, const std::string& etag = "\"1\"") {
    pbo::EventUpsert e;
    e.href = "/cal/" + uid + ".ics";
    e.etag = etag;
    e.uid = uid;
    e.ics = "BEGIN:VCALENDAR\r\nBEGIN:VEVENT\r\nUID:" + uid + "\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n";
    e.summary = summary;
    e.start_utc = start;
    e.end_utc = start + len;
    e.start_local = "2026-10-14T10:00:00";
    pbo::OccurrenceRow o;
    o.start_utc = start;
    o.end_utc = start + len;
    const int64_t days = start / kDay;
    char b[16];
    int y, m, d;
    // 1970-01-01 + days: a short civil conversion for the test dates
    int64_t z = days + 719468;
    const int64_t era = z / 146097, doe = z - era * 146097, yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
    m = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
    y = static_cast<int>(yoe + era * 400 + (m <= 2));
    snprintf(b, sizeof b, "%04d-%02d-%02d", y, m, d);
    o.local_date = b;
    e.occurrences.push_back(o);
    return e;
}

int count(Env& e, const std::string& sql) { return std::atoi(e.store->debug_query(sql).c_str()); }

}  // namespace

PB_TEST(a_calendar_account_is_saved_validated_and_never_replaced_by_a_rejected_one) {
    Env e;
    PB_CHECK(!e.store->calendar_account().has_value());
    pbo::CalendarAccount a;
    a.caldav_url = "https://cloud.example.org/remote.php/dav";
    a.username = "alice";
    PB_CHECK(e.store->save_calendar_account(a).ok());
    auto got = e.store->calendar_account();
    PB_CHECK(got.has_value());
    PB_CHECK_EQ(got->caldav_url, a.caldav_url);
    PB_CHECK_EQ(got->username, std::string("alice"));
    PB_CHECK_EQ(got->feed_url, std::string(""));
    PB_CHECK(got->created > 0);

    pbo::CalendarAccount bad = a;
    bad.caldav_url = "http://cloud.example.org/dav";
    auto err = e.store->save_calendar_account(bad);
    PB_CHECK(!err.ok());
    PB_CHECK_EQ(err.key, std::string("invalid_account"));
    pbo::CalendarAccount neither;
    PB_CHECK(!e.store->save_calendar_account(neither).ok());
    PB_CHECK_EQ(e.store->calendar_account()->caldav_url, a.caldav_url);

    pbo::CalendarAccount both = a;
    both.feed_url = "https://calendars.example.org/h.ics";
    both.feed_name = "Holidays";
    PB_CHECK(e.store->save_calendar_account(both).ok());
    got = e.store->calendar_account();
    PB_CHECK_EQ(got->feed_url, both.feed_url);
    PB_CHECK_EQ(got->feed_name, std::string("Holidays"));

    pbo::CalendarAccount feed_only;
    feed_only.feed_url = "https://calendars.example.org/h.ics";
    PB_CHECK(e.store->save_calendar_account(feed_only).ok());
    PB_CHECK_EQ(e.store->calendar_account()->caldav_url, std::string(""));
}

PB_TEST(calendars_keep_the_users_choice_and_the_cursor_when_they_are_listed_again) {
    Env e;
    const int64_t id = e.add_calendar("/cal/work/", "Work");
    PB_CHECK(e.store->set_calendar_shown(id, false).ok());
    pbo::CalendarPullBatch b;
    b.ctag = "c1";
    b.sync_token = "t1";
    b.synced_at = kNow;
    PB_CHECK(e.store->apply_calendar_pull(id, b).ok());
    const int64_t again = e.add_calendar("/cal/work/", "Work (renamed)", false);
    PB_CHECK_EQ(again, id);
    const auto cals = e.store->calendars();
    PB_CHECK_EQ(cals.size(), size_t(1));
    PB_CHECK_EQ(cals[0].name, std::string("Work (renamed)"));
    PB_CHECK(!cals[0].writable);
    PB_CHECK(!cals[0].shown);                         // the choice survives
    PB_CHECK_EQ(cals[0].ctag, std::string("c1"));
    PB_CHECK_EQ(cals[0].sync_token, std::string("t1"));
    PB_CHECK_EQ(cals[0].last_sync_at, kNow);
    PB_CHECK(!e.store->set_calendar_shown(9999, true).ok());
}

PB_TEST(calendars_that_disappear_are_marked_not_deleted) {
    Env e;
    e.add_calendar("/cal/work/", "Work");
    e.add_calendar("/cal/old/", "Old");
    e.add_calendar("https://example.org/feed.ics", "Feed", false, "feed");
    PB_CHECK(e.store->mark_calendars_present("caldav", {"/cal/work/"}).ok());
    for (const auto& c : e.store->calendars()) {
        if (c.href == "/cal/old/") PB_CHECK(!c.present_on_server);
        else PB_CHECK(c.present_on_server);
    }
    e.add_calendar("/cal/old/", "Old");               // back again
    for (const auto& c : e.store->calendars())
        if (c.href == "/cal/old/") PB_CHECK(c.present_on_server);
}

PB_TEST(a_pull_writes_events_occurrences_and_the_cursor_in_one_transaction) {
    Env e;
    const int64_t cal = e.add_calendar("/cal/work/", "Work");
    pbo::CalendarPullBatch b;
    b.upserts = {ev("a", "Standup", kNow + kHour), ev("b", "Review", kNow + 2 * kHour)};
    b.ctag = "c1";
    b.sync_token = "t1";
    b.synced_at = kNow;
    PB_CHECK(e.store->apply_calendar_pull(cal, b).ok());
    PB_CHECK_EQ(count(e, "SELECT count(*) FROM event"), 2);
    PB_CHECK_EQ(count(e, "SELECT count(*) FROM occurrence"), 2);
    PB_CHECK_EQ(e.store->calendars()[0].sync_token, std::string("t1"));
    const auto refs = e.store->event_refs(cal);
    PB_CHECK_EQ(refs.size(), size_t(2));
    PB_CHECK_EQ(refs[0].sync_state, std::string("synced"));
    PB_CHECK(!refs[0].etag.empty());

    // a failing batch (two occurrences with the same key) changes nothing, not even the cursor
    pbo::CalendarPullBatch broken;
    auto bad = ev("c", "Broken", kNow + 3 * kHour);
    bad.occurrences.push_back(bad.occurrences[0]);
    broken.upserts = {ev("d", "Fine", kNow + 4 * kHour), bad};
    broken.sync_token = "t2";
    broken.synced_at = kNow + 10;
    PB_CHECK(!e.store->apply_calendar_pull(cal, broken).ok());
    PB_CHECK_EQ(count(e, "SELECT count(*) FROM event"), 2);
    PB_CHECK_EQ(e.store->calendars()[0].sync_token, std::string("t1"));
}

PB_TEST(a_changed_event_replaces_its_occurrences_and_an_unknown_calendar_is_refused) {
    Env e;
    const int64_t cal = e.add_calendar("/cal/work/", "Work");
    pbo::CalendarPullBatch b;
    b.upserts = {ev("a", "Standup", kNow + kHour)};
    PB_CHECK(e.store->apply_calendar_pull(cal, b).ok());
    pbo::CalendarPullBatch c;
    c.upserts = {ev("a", "Standup (moved)", kNow + 5 * kHour, kHour, "\"2\"")};
    PB_CHECK(e.store->apply_calendar_pull(cal, c).ok());
    PB_CHECK_EQ(count(e, "SELECT count(*) FROM event"), 1);
    PB_CHECK_EQ(count(e, "SELECT count(*) FROM occurrence"), 1);
    PB_CHECK_EQ(e.store->debug_query("SELECT summary FROM event"), std::string("Standup (moved)"));
    PB_CHECK_EQ(e.store->debug_query("SELECT etag FROM event"), std::string("\"2\""));
    PB_CHECK_EQ(count(e, "SELECT start_utc FROM occurrence"), static_cast<int>(kNow + 5 * kHour));
    PB_CHECK(!e.store->apply_calendar_pull(777, c).ok());
}

PB_TEST(removals_and_events_that_left_the_window_disappear_but_pending_ones_stay) {
    Env e;
    const int64_t cal = e.add_calendar("/cal/work/", "Work");
    pbo::CalendarPullBatch b;
    b.upserts = {ev("a", "A", kNow + kHour), ev("b", "B", kNow + 2 * kHour), ev("c", "C", kNow + 3 * kHour), ev("d", "D", kNow + 4 * kHour)};
    PB_CHECK(e.store->apply_calendar_pull(cal, b).ok());
    PB_CHECK(e.store->debug_exec("UPDATE event SET sync_state = 'pending_update' WHERE uid = 'b'").empty());
    pbo::CalendarPullBatch r;
    r.removed_hrefs = {"/cal/a.ics", "/cal/b.ics"};                       // b has local work: it stays
    auto moved_out = ev("c", "C", kNow + 3 * kHour, kHour, "\"2\"");
    moved_out.occurrences.clear();                                         // no instance in the window any more
    r.upserts = {moved_out};
    PB_CHECK(e.store->apply_calendar_pull(cal, r).ok());
    PB_CHECK_EQ(count(e, "SELECT count(*) FROM event"), 2);
    PB_CHECK_EQ(e.store->debug_query("SELECT group_concat(uid) FROM (SELECT uid FROM event ORDER BY uid)"), std::string("b,d"));
    PB_CHECK_EQ(count(e, "SELECT count(*) FROM occurrence"), 2);
}

PB_TEST(a_pull_never_overwrites_an_event_with_local_work) {
    Env e;
    const int64_t cal = e.add_calendar("/cal/work/", "Work");
    pbo::CalendarPullBatch b;
    b.upserts = {ev("a", "Mine", kNow + kHour)};
    PB_CHECK(e.store->apply_calendar_pull(cal, b).ok());
    PB_CHECK(e.store->debug_exec("UPDATE event SET sync_state = 'pending_update', summary = 'Edited locally'").empty());
    pbo::CalendarPullBatch s;
    s.upserts = {ev("a", "Server version", kNow + 9 * kHour, kHour, "\"9\"")};
    PB_CHECK(e.store->apply_calendar_pull(cal, s).ok());
    PB_CHECK_EQ(e.store->debug_query("SELECT summary FROM event"), std::string("Edited locally"));
    PB_CHECK_EQ(e.store->debug_query("SELECT sync_state FROM event"), std::string("pending_update"));
}

PB_TEST(a_complete_list_removes_what_it_does_not_name_except_events_with_local_work) {
    Env e;
    const int64_t cal = e.add_calendar("/cal/work/", "Work");
    pbo::CalendarPullBatch b;
    b.upserts = {ev("a", "A", kNow + kHour), ev("b", "B", kNow + 2 * kHour), ev("c", "C", kNow + 3 * kHour)};
    PB_CHECK(e.store->apply_calendar_pull(cal, b).ok());
    PB_CHECK(e.store->debug_exec("UPDATE event SET sync_state = 'pending_delete' WHERE uid = 'c'").empty());
    pbo::CalendarPullBatch full;
    full.full = true;
    full.listed_hrefs = {"/cal/a.ics"};
    PB_CHECK(e.store->apply_calendar_pull(cal, full).ok());
    PB_CHECK_EQ(e.store->debug_query("SELECT group_concat(uid) FROM (SELECT uid FROM event ORDER BY uid)"), std::string("a,c"));
}

PB_TEST(the_same_uid_under_a_new_address_is_the_same_event) {
    Env e;
    const int64_t cal = e.add_calendar("/cal/work/", "Work");
    pbo::CalendarPullBatch b;
    b.upserts = {ev("a", "A", kNow + kHour)};
    PB_CHECK(e.store->apply_calendar_pull(cal, b).ok());
    auto moved = ev("a", "A", kNow + kHour);
    moved.href = "/cal/renamed.ics";
    pbo::CalendarPullBatch m;
    m.upserts = {moved};
    PB_CHECK(e.store->apply_calendar_pull(cal, m).ok());
    PB_CHECK_EQ(count(e, "SELECT count(*) FROM event"), 1);
    PB_CHECK_EQ(e.store->debug_query("SELECT href FROM event"), std::string("/cal/renamed.ics"));
}

PB_TEST(constraints_of_the_data_model_hold) {
    Env e;
    const int64_t cal = e.add_calendar("/cal/work/", "Work");
    pbo::CalendarPullBatch b;
    b.upserts = {ev("a", "A", kNow + kHour)};
    PB_CHECK(e.store->apply_calendar_pull(cal, b).ok());
    PB_CHECK(!e.store->debug_exec("UPDATE event SET sync_state = 'bogus'").empty());
    PB_CHECK(!e.store->debug_exec("INSERT INTO event(calendar_id, uid, ics, start_local) VALUES(" + std::to_string(cal) + ", 'a', 'x', 'y')").empty());   // UNIQUE(calendar_id, uid)
    PB_CHECK(!e.store->debug_exec("INSERT INTO occurrence(event_id, start_utc, end_utc, local_date, recurrence_id) SELECT event_id, start_utc, end_utc, local_date, recurrence_id FROM occurrence").empty());
    PB_CHECK(!e.store->debug_exec("INSERT INTO calendar(kind, href, name) VALUES('other', 'x', 'y')").empty());
    PB_CHECK(!e.store->debug_exec("INSERT INTO calendar(kind, href, name) VALUES('caldav', '/cal/work/', 'dup')").empty());
    PB_CHECK(e.store->debug_exec("DELETE FROM calendar").empty());                       // cascades
    PB_CHECK_EQ(count(e, "SELECT count(*) FROM event"), 0);
    PB_CHECK_EQ(count(e, "SELECT count(*) FROM occurrence"), 0);
}

PB_TEST(the_agenda_lists_what_is_upcoming_in_time_order_in_pages_for_shown_calendars_only) {
    Env e;
    const int64_t work = e.add_calendar("/cal/work/", "Work");
    const int64_t home = e.add_calendar("/cal/home/", "Home", false);
    pbo::CalendarPullBatch w, h;
    for (int i = 0; i < 8; ++i) w.upserts.push_back(ev("w" + std::to_string(i), "Work " + std::to_string(i), kNow + (i + 1) * 2 * kHour));
    w.upserts.push_back(ev("past", "Yesterday", kNow - kDay));
    for (int i = 0; i < 4; ++i) h.upserts.push_back(ev("h" + std::to_string(i), "Home " + std::to_string(i), kNow + (2 * i + 1) * 2 * kHour + kHour));
    for (auto& u : h.upserts) u.href = "/cal/home/" + u.uid + ".ics";
    PB_CHECK(e.store->apply_calendar_pull(work, w).ok());
    PB_CHECK(e.store->apply_calendar_pull(home, h).ok());

    auto p0 = e.store->agenda(kNow, 0, 5);
    PB_CHECK_EQ(p0.total, 12);
    PB_CHECK_EQ(p0.page_count, 3);
    PB_CHECK_EQ(p0.items.size(), size_t(5));
    int64_t last = 0;
    for (const auto& it : p0.items) {
        PB_CHECK(it.start_utc >= last);
        last = it.start_utc;
    }
    PB_CHECK_EQ(p0.items[0].summary, std::string("Work 0"));
    PB_CHECK_EQ(p0.items[0].calendar_name, std::string("Work"));
    PB_CHECK(p0.items[0].writable);
    auto p2 = e.store->agenda(kNow, 2, 5);
    PB_CHECK_EQ(p2.items.size(), size_t(2));
    PB_CHECK_EQ(e.store->agenda(kNow, 9, 5).page, 2);                            // a page beyond the last is the last
    PB_CHECK_EQ(e.store->agenda(kNow, -3, 5).page, 0);
    PB_CHECK_EQ(e.store->agenda(kNow - 2 * kDay, 0, 50).total, 13);               // with yesterday

    PB_CHECK(e.store->set_calendar_shown(home, false).ok());
    PB_CHECK_EQ(e.store->agenda(kNow, 0, 50).total, 8);
}

PB_TEST(an_event_in_progress_is_still_in_the_agenda_and_one_that_ended_is_not) {
    Env e;
    const int64_t cal = e.add_calendar("/cal/work/", "Work");
    pbo::CalendarPullBatch b;
    b.upserts = {ev("running", "Running", kNow - kHour, 3 * kHour), ev("over", "Over", kNow - 3 * kHour, kHour)};
    PB_CHECK(e.store->apply_calendar_pull(cal, b).ok());
    auto p = e.store->agenda(kNow, 0, 10);
    PB_CHECK_EQ(p.total, 1);
    PB_CHECK_EQ(p.items[0].summary, std::string("Running"));
}

PB_TEST(occurrences_between_and_the_marked_days_follow_the_users_dates) {
    Env e;
    const int64_t cal = e.add_calendar("/cal/work/", "Work");
    pbo::CalendarPullBatch b;
    b.upserts = {ev("a", "A", kNow + kHour), ev("b", "B", kNow + kDay + kHour), ev("c", "C", kNow + kDay + 2 * kHour), ev("d", "D", kNow + 20 * kDay)};
    PB_CHECK(e.store->apply_calendar_pull(cal, b).ok());
    const auto between = e.store->occurrences_between(kNow, kNow + 3 * kDay);
    PB_CHECK_EQ(between.size(), size_t(3));
    PB_CHECK_EQ(between[0].summary, std::string("A"));
    const auto days = e.store->marked_days("2026-10-01", "2026-10-31");
    PB_CHECK((days == std::vector<std::string>{"2026-10-14", "2026-10-15"}));
    PB_CHECK_EQ(e.store->marked_days("2026-10-15", "2026-10-15").size(), size_t(1));
    PB_CHECK(e.store->marked_days("2027-01-01", "2027-01-31").empty());
}

PB_TEST(rebuilding_the_occurrences_replaces_them_for_every_event_in_one_transaction) {
    Env e;
    const int64_t cal = e.add_calendar("/cal/work/", "Work");
    pbo::CalendarPullBatch b;
    b.upserts = {ev("a", "A", kNow + kHour), ev("b", "B", kNow + 2 * kHour)};
    PB_CHECK(e.store->apply_calendar_pull(cal, b).ok());
    PB_CHECK(e.store->debug_exec("UPDATE event SET sync_state = 'pending_update' WHERE uid = 'b'").empty());
    int calls = 0;
    PB_CHECK(e.store->rebuild_occurrences([&](const std::string& ics) {
                ++calls;
                PB_CHECK(ics.find("BEGIN:VCALENDAR") != std::string::npos);
                pbo::OccurrenceRow o;
                o.start_utc = kNow + 7 * kDay;
                o.end_utc = o.start_utc + kHour;
                o.local_date = "2026-10-21";
                return std::vector<pbo::OccurrenceRow>{o};
            }).ok());
    PB_CHECK_EQ(calls, 2);                                   // pending events are included
    PB_CHECK_EQ(count(e, "SELECT count(*) FROM occurrence WHERE local_date = '2026-10-21'"), 2);
    PB_CHECK_EQ(count(e, "SELECT count(*) FROM occurrence"), 2);
    // a callback that returns nothing for a synced event removes it (it has no instance in the window any more), a pending one stays
    PB_CHECK(e.store->rebuild_occurrences([](const std::string&) { return std::vector<pbo::OccurrenceRow>{}; }).ok());
    PB_CHECK_EQ(e.store->debug_query("SELECT group_concat(uid) FROM event"), std::string("b"));
}

PB_TEST(an_event_can_be_read_for_the_event_page) {
    Env e;
    const int64_t cal = e.add_calendar("/cal/work/", "Work");
    pbo::CalendarPullBatch b;
    auto u = ev("a", "Planning", kNow + kHour);
    u.location = "Room 4";
    u.description = "Agenda";
    u.attendees = "Jane <jane@example.org>";
    u.rrule = "FREQ=WEEKLY";
    u.tzid = "Europe/Berlin";
    b.upserts = {u};
    PB_CHECK(e.store->apply_calendar_pull(cal, b).ok());
    const int64_t id = std::atoll(e.store->debug_query("SELECT id FROM event").c_str());
    const auto v = e.store->event(id);
    PB_CHECK(v.has_value());
    PB_CHECK_EQ(v->summary, std::string("Planning"));
    PB_CHECK_EQ(v->location, std::string("Room 4"));
    PB_CHECK_EQ(v->tzid, std::string("Europe/Berlin"));
    PB_CHECK_EQ(v->calendar_name, std::string("Work"));
    PB_CHECK(v->calendar_writable);
    PB_CHECK_EQ(v->sync_state, std::string("synced"));
    PB_CHECK(v->ics.find("BEGIN:VCALENDAR") != std::string::npos);
    PB_CHECK(!e.store->event(424242).has_value());
}

PB_TEST(changing_the_server_removes_its_calendars_except_those_with_unsynced_work) {
    Env e;
    pbo::CalendarAccount a;
    a.caldav_url = "https://cloud.example.org/dav";
    a.username = "alice";
    a.feed_url = "https://calendars.example.org/h.ics";
    PB_CHECK(e.store->save_calendar_account(a).ok());
    const int64_t clean = e.add_calendar("/cal/clean/", "Clean");
    const int64_t busy = e.add_calendar("/cal/busy/", "Busy");
    const int64_t feed = e.add_calendar("https://calendars.example.org/h.ics", "Feed", false, "feed");
    pbo::CalendarPullBatch b;
    b.upserts = {ev("a", "A", kNow + kHour)};
    PB_CHECK(e.store->apply_calendar_pull(clean, b).ok());
    pbo::CalendarPullBatch c;
    c.upserts = {ev("b", "B", kNow + kHour)};
    PB_CHECK(e.store->apply_calendar_pull(busy, c).ok());
    PB_CHECK(e.store->debug_exec("UPDATE event SET sync_state = 'pending_update' WHERE uid = 'b'").empty());
    pbo::CalendarPullBatch f;
    f.upserts = {ev("f", "F", kNow + kHour)};
    PB_CHECK(e.store->apply_calendar_pull(feed, f).ok());

    pbo::CalendarAccount same = a;                       // nothing changes: nothing is removed
    PB_CHECK(e.store->save_calendar_account(same).ok());
    PB_CHECK_EQ(e.store->calendars().size(), size_t(3));

    pbo::CalendarAccount other = a;
    other.caldav_url = "https://other.example.org/dav";
    other.feed_url = "";
    PB_CHECK(e.store->save_calendar_account(other).ok());
    const auto cals = e.store->calendars();
    PB_CHECK_EQ(cals.size(), size_t(1));                 // the clean calendar and the feed are gone
    PB_CHECK_EQ(cals[0].name, std::string("Busy"));
    PB_CHECK(!cals[0].present_on_server);
    PB_CHECK_EQ(count(e, "SELECT count(*) FROM event"), 1);
    PB_CHECK_EQ(e.store->debug_query("SELECT uid FROM event"), std::string("b"));
}

PB_TEST_MAIN()
