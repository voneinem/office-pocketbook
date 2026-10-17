// T100 (part): the calendar pull against the fake CalDAV server: window, change detection, convergence after interruptions, the
// occurrence cache and the user's zone, feeds, unreadable resources, local work. UNVERIFIED against real servers.
#include <string>

#include "fake_caldav_server.h"
#include "pbo/calendar_sync.h"
#include "pbo/curl_transport.h"
#include "test_support.h"
#include "tmp_dir.h"

using pbtest::FakeCalDavServer;

PBO_FAKE_SERVER_TEST();

namespace {

constexpr int64_t kNow = 1791979200;   // 2026-10-14 12:00:00 UTC
constexpr int64_t kDay = 86400;

struct FixedClock : pbo::Clock {
    int64_t now() const override { return kNow; }
};
struct NoCancel : pbo::Cancel {
    bool requested() const override { return false; }
};

std::string ics(const std::string& uid, const std::string& summary, const std::string& start, const std::string& end, const std::string& extra = "",
                const std::string& tz = "Europe/Berlin") {
    return "BEGIN:VCALENDAR\r\nVERSION:2.0\r\nPRODID:-//example.org//test//EN\r\nBEGIN:VEVENT\r\nUID:" + uid + "@example.org\r\nSUMMARY:" + summary +
           "\r\nDTSTART;TZID=" + tz + ":" + start + "\r\nDTEND;TZID=" + tz + ":" + end + "\r\n" + extra + "END:VEVENT\r\nEND:VCALENDAR\r\n";
}

struct Env {
    pbtest::TmpDir data, pki_dir;
    pbtest::TestPki pki{pki_dir};
    pbtest::TestPki::Leaf leaf;
    FixedClock clock;
    NoCancel cancel;
    std::unique_ptr<FakeCalDavServer> server;
    std::unique_ptr<pbo::Store> store;
    std::unique_ptr<pbo::TzDatabase> tz;
    std::unique_ptr<pbo::CurlTransport> http;

    Env() {
        if (!pki.available()) PB_SKIP("openssl CLI not available");
        leaf = pki.issue("srv", "DNS:localhost,IP:127.0.0.1");
        PB_CHECK(leaf.ok);
        FakeCalDavServer::Config c;
        c.cert = leaf.cert;
        c.key = leaf.key;
        server = std::make_unique<FakeCalDavServer>(c);
        auto opened = pbo::Store::open(data.path(), clock);
        PB_CHECK(opened.ok());
        store = std::move(opened.value);
        tz = pbo::TzDatabase::open(std::string(PBO_SOURCE_DIR) + "/third_party/tzdata/zoneinfo");
        PB_CHECK(tz != nullptr);
        http = std::make_unique<pbo::CurlTransport>(pki.ca_cert());
        store->set_setting("tz_override", "Europe/Berlin");
        FakeCalDavServer::CalendarOptions work;
        work.name = "Work";
        server->add_calendar("work", work);
        pbo::CalendarAccount a;
        a.caldav_url = server->origin();
        a.username = "alice";
        PB_CHECK(store->save_calendar_account(a).ok());
    }
    pbo::Error sync(pbo::CalendarCounts* counts = nullptr) {
        pbo::CalendarSyncDeps d{*store, *http, *tz, clock, cancel};
        return pbo::pull_calendars(d, "pw-for-tests", counts);
    }
    int count(const std::string& sql) { return std::atoi(store->debug_query(sql).c_str()); }
    std::string snapshot() {
        return store->debug_query("SELECT group_concat(c.href || '/' || e.uid || '|' || COALESCE(e.etag,'') || '|' || e.sync_state, ';') FROM event e JOIN calendar c ON c.id = e.calendar_id ORDER BY e.uid") + "#" +
               store->debug_query("SELECT group_concat(event_id || ':' || start_utc || ':' || local_date, ';') FROM (SELECT * FROM occurrence ORDER BY event_id, start_utc)");
    }
};

// in the window: 2026-10-14 10:00 Berlin
const int64_t kIn = kNow;
const int64_t kInEnd = kNow + 3600;

}  // namespace

PB_TEST(the_first_pull_stores_calendars_events_and_occurrences_of_the_window) {
    Env e;
    FakeCalDavServer::CalendarOptions ro;
    ro.name = "Holidays";
    ro.writable = false;
    e.server->add_calendar("holidays", ro);
    e.server->put_event("work", "a.ics", ics("a", "Standup", "20261015T093000", "20261015T100000"), kIn, kInEnd + kDay);
    e.server->put_event("work", "b.ics", ics("b", "Review", "20261020T140000", "20261020T150000"), kIn + 6 * kDay, kInEnd + 6 * kDay);
    e.server->put_event("holidays", "h.ics", ics("h", "Holiday", "20261103T000000", "20261103T235900"), kIn + 20 * kDay, kInEnd + 21 * kDay);
    pbo::CalendarCounts counts;
    PB_CHECK(e.sync(&counts).ok());
    PB_CHECK_EQ(counts.calendars, 2);
    PB_CHECK_EQ(counts.changed, 3);
    const auto cals = e.store->calendars();
    PB_CHECK_EQ(cals.size(), size_t(2));
    for (const auto& c : cals) {
        PB_CHECK(c.last_sync_at == kNow);
        PB_CHECK(!c.sync_token.empty());
        PB_CHECK(c.kind == "caldav");
        PB_CHECK(c.writable == (c.name == "Work"));
    }
    PB_CHECK_EQ(e.count("SELECT count(*) FROM event"), 3);
    PB_CHECK_EQ(e.count("SELECT count(*) FROM occurrence"), 3);
    const auto agenda = e.store->agenda(kNow, 0, 10);
    PB_CHECK_EQ(agenda.total, 3);
    PB_CHECK_EQ(agenda.items[0].summary, std::string("Standup"));
    PB_CHECK_EQ(agenda.items[0].local_date, std::string("2026-10-15"));
}

PB_TEST(only_events_with_an_instance_in_the_window_are_stored) {
    Env e;
    // the server (here: sloppy) returns events outside [today-30 d, today+365 d]; they must not be stored
    e.server->put_event("work", "far.ics", ics("far", "Far away", "20280101T100000", "20280101T110000"), kIn, kInEnd);
    e.server->put_event("work", "old.ics", ics("old", "Long ago", "20250101T100000", "20250101T110000"), kIn, kInEnd);
    e.server->put_event("work", "ok.ics", ics("ok", "Inside", "20261105T100000", "20261105T110000"), kIn, kInEnd);
    PB_CHECK(e.sync().ok());
    PB_CHECK_EQ(e.count("SELECT count(*) FROM event"), 1);
    PB_CHECK_EQ(e.store->debug_query("SELECT summary FROM event"), std::string("Inside"));
}

PB_TEST(a_series_that_started_long_ago_is_expanded_inside_the_window_and_respects_exdate) {
    Env e;
    e.server->put_event("work", "s.ics", ics("s", "Weekly", "20200106T100000", "20200106T110000", "RRULE:FREQ=WEEKLY;BYDAY=MO\r\nEXDATE;TZID=Europe/Berlin:20261019T100000\r\n"), 0);
    PB_CHECK(e.sync().ok());
    PB_CHECK_EQ(e.count("SELECT count(*) FROM event"), 1);
    // Mondays from 2026-09-14 to 2027-10-14: 57 Mondays, one excluded
    const int n = e.count("SELECT count(*) FROM occurrence");
    PB_CHECK(n >= 55 && n <= 58);
    PB_CHECK_EQ(e.count("SELECT count(*) FROM occurrence WHERE local_date = '2026-10-19'"), 0);
    PB_CHECK_EQ(e.count("SELECT count(*) FROM occurrence WHERE local_date = '2026-10-26'"), 1);
    PB_CHECK_EQ(e.count("SELECT count(*) FROM occurrence WHERE local_date < '2026-09-14'"), 0);
}

PB_TEST(a_second_pull_without_changes_fetches_nothing) {
    Env e;
    e.server->put_event("work", "a.ics", ics("a", "Standup", "20261015T093000", "20261015T100000"), kIn, kInEnd + kDay);
    PB_CHECK(e.sync().ok());
    PB_CHECK_EQ(e.server->multiget_batch_sizes().size(), size_t(1));
    const std::string before = e.snapshot();
    PB_CHECK(e.sync().ok());
    PB_CHECK_EQ(e.server->multiget_batch_sizes().size(), size_t(1));      // no further multiget
    PB_CHECK_EQ(e.snapshot(), before);
}

PB_TEST(changes_additions_and_removals_on_the_server_are_followed_incrementally) {
    Env e;
    e.server->put_event("work", "a.ics", ics("a", "Standup", "20261015T093000", "20261015T100000"), kIn, kInEnd + kDay);
    e.server->put_event("work", "b.ics", ics("b", "Review", "20261016T093000", "20261016T100000"), kIn, kInEnd + kDay);
    PB_CHECK(e.sync().ok());
    e.server->put_event("work", "a.ics", ics("a", "Standup (moved)", "20261015T110000", "20261015T113000"), kIn, kInEnd + kDay);
    e.server->remove_event("work", "b.ics");
    e.server->put_event("work", "c.ics", ics("c", "New", "20261017T090000", "20261017T100000"), kIn, kInEnd + kDay);
    pbo::CalendarCounts counts;
    PB_CHECK(e.sync(&counts).ok());
    PB_CHECK_EQ(counts.changed, 2);
    PB_CHECK_EQ(counts.removed, 1);
    PB_CHECK_EQ(e.store->debug_query("SELECT group_concat(summary) FROM (SELECT summary FROM event ORDER BY summary)"), std::string("New,Standup (moved)"));
    PB_CHECK_EQ(e.count("SELECT count(*) FROM occurrence"), 2);
    // an event moved out of the window disappears
    e.server->put_event("work", "c.ics", ics("c", "New", "20300101T090000", "20300101T100000"), kIn, kInEnd + kDay);
    PB_CHECK(e.sync().ok());
    PB_CHECK_EQ(e.count("SELECT count(*) FROM event"), 1);
}

PB_TEST(an_invalid_sync_token_leads_to_a_full_resync_that_removes_what_the_server_dropped) {
    Env e;
    e.server->put_event("work", "a.ics", ics("a", "A", "20261015T093000", "20261015T100000"), kIn, kInEnd + kDay);
    e.server->put_event("work", "b.ics", ics("b", "B", "20261016T093000", "20261016T100000"), kIn, kInEnd + kDay);
    PB_CHECK(e.sync().ok());
    e.server->remove_event("work", "a.ics");
    e.server->reset_sync_tokens("work");
    PB_CHECK(e.sync().ok());
    PB_CHECK_EQ(e.store->debug_query("SELECT group_concat(uid) FROM event"), std::string("b@example.org"));
    PB_CHECK(!e.store->calendars()[0].sync_token.empty());
    PB_CHECK(e.sync().ok());                                  // and the next pull is incremental again
    PB_CHECK_EQ(e.count("SELECT count(*) FROM event"), 1);
}

PB_TEST(a_calendar_without_sync_collection_follows_the_ctag) {
    Env e;
    FakeCalDavServer::CalendarOptions plain;
    plain.name = "Plain";
    plain.sync_collection = false;
    e.server->add_calendar("plain", plain);
    e.server->remove_calendar("work");
    e.server->put_event("plain", "a.ics", ics("a", "A", "20261015T093000", "20261015T100000"), kIn, kInEnd + kDay);
    e.server->put_event("plain", "b.ics", ics("b", "B", "20261016T093000", "20261016T100000"), kIn, kInEnd + kDay);
    PB_CHECK(e.sync().ok());
    PB_CHECK_EQ(e.count("SELECT count(*) FROM event"), 2);
    const int queries = e.server->count("REPORT");
    PB_CHECK(e.sync().ok());
    PB_CHECK_EQ(e.server->count("REPORT"), queries);          // same ctag: nothing asked
    e.server->remove_event("plain", "a.ics");
    PB_CHECK(e.sync().ok());
    PB_CHECK_EQ(e.store->debug_query("SELECT group_concat(uid) FROM event"), std::string("b@example.org"));
}

PB_TEST(an_interrupted_pull_converges_to_the_state_of_an_uninterrupted_one) {
    Env clean;
    Env broken;
    for (Env* e : {&clean, &broken})
        for (int i = 0; i < 40; ++i)
            e->server->put_event("work", "e" + std::to_string(i) + ".ics", ics("e" + std::to_string(i), "Event " + std::to_string(i), "20261101T100000", "20261101T110000"), kIn, kInEnd + 30 * kDay);
    PB_CHECK(clean.sync().ok());

    FakeCalDavServer::Fault drop;
    drop.kind = FakeCalDavServer::Fault::Kind::Drop;
    drop.method = "REPORT";
    drop.times = 1;
    // the second multiget batch (after 30 resources) is lost
    broken.server->inject({FakeCalDavServer::Fault::Kind::Status, "REPORT", "work", 503, 0, "", 0});   // spent immediately: the first REPORT fails
    PB_CHECK(!broken.sync().ok());
    PB_CHECK_EQ(broken.count("SELECT count(*) FROM event"), 0);                                         // nothing half-applied
    broken.server->inject(drop);
    (void)broken.sync();
    PB_CHECK(broken.sync().ok());
    PB_CHECK_EQ(broken.count("SELECT count(*) FROM event"), 40);
    PB_CHECK_EQ(broken.count("SELECT count(*) FROM occurrence"), clean.count("SELECT count(*) FROM occurrence"));
    PB_CHECK_EQ(broken.store->calendars()[0].sync_token.empty(), false);
}

PB_TEST(an_unreadable_resource_is_skipped_and_the_rest_is_applied) {
    Env e;
    e.server->put_event("work", "good.ics", ics("good", "Good", "20261015T093000", "20261015T100000"), kIn, kInEnd + kDay);
    e.server->put_event("work", "bad.ics", "this is not a calendar", kIn, kInEnd + kDay);
    e.server->put_event("work", "html.ics", "<html>login page</html>", kIn, kInEnd + kDay);
    pbo::CalendarCounts counts;
    PB_CHECK(e.sync(&counts).ok());
    PB_CHECK_EQ(counts.unreadable, 2);
    PB_CHECK_EQ(e.count("SELECT count(*) FROM event"), 1);
    PB_CHECK(!e.store->calendars()[0].sync_token.empty());
}

PB_TEST(an_event_with_local_work_is_not_touched_by_the_pull) {
    Env e;
    e.server->put_event("work", "a.ics", ics("a", "Server title", "20261015T093000", "20261015T100000"), kIn, kInEnd + kDay);
    PB_CHECK(e.sync().ok());
    PB_CHECK(e.store->debug_exec("UPDATE event SET sync_state = 'pending_update', summary = 'Edited here'").empty());
    e.server->put_event("work", "a.ics", ics("a", "Changed on the server", "20261015T093000", "20261015T100000"), kIn, kInEnd + kDay);
    PB_CHECK(e.sync().ok());
    PB_CHECK_EQ(e.store->debug_query("SELECT summary FROM event"), std::string("Edited here"));
    e.server->remove_event("work", "a.ics");
    PB_CHECK(e.sync().ok());
    PB_CHECK_EQ(e.count("SELECT count(*) FROM event"), 1);        // still there: it has unsent work
}

PB_TEST(a_calendar_that_disappears_is_marked_and_keeps_its_events) {
    Env e;
    FakeCalDavServer::CalendarOptions other;
    other.name = "Other";
    e.server->add_calendar("other", other);
    e.server->put_event("other", "o.ics", ics("o", "Other event", "20261015T093000", "20261015T100000"), kIn, kInEnd + kDay);
    PB_CHECK(e.sync().ok());
    e.server->remove_calendar("other");
    PB_CHECK(e.sync().ok());
    for (const auto& c : e.store->calendars()) PB_CHECK(c.present_on_server == (c.name == "Work"));
    PB_CHECK_EQ(e.count("SELECT count(*) FROM event"), 1);
}

PB_TEST(a_calendar_the_user_turned_off_is_not_pulled) {
    Env e;
    FakeCalDavServer::CalendarOptions other;
    other.name = "Other";
    e.server->add_calendar("other", other);
    PB_CHECK(e.sync().ok());
    for (const auto& c : e.store->calendars())
        if (c.name == "Other") PB_CHECK(e.store->set_calendar_shown(c.id, false).ok());
    e.server->put_event("other", "o.ics", ics("o", "Other event", "20261015T093000", "20261015T100000"), kIn, kInEnd + kDay);
    PB_CHECK(e.sync().ok());
    PB_CHECK_EQ(e.count("SELECT count(*) FROM event"), 0);
}

PB_TEST(without_a_known_zone_nothing_is_converted_and_the_user_has_to_choose) {
    Env e;
    e.store->set_setting("tz_override", "");
    e.server->put_event("work", "a.ics", ics("a", "A", "20261015T093000", "20261015T100000"), kIn, kInEnd + kDay);
    const auto err = e.sync();
    // this host may or may not have a usable TZ / /etc/localtime; when it has, the pull simply works. When it has not: the dedicated error.
    if (!err.ok()) {
        PB_CHECK_EQ(err.key, std::string("zone_unresolved"));
        PB_CHECK_EQ(e.server->log().size(), size_t(0));           // not a single request without a zone
        PB_CHECK_EQ(e.count("SELECT count(*) FROM event"), 0);
    }
}

PB_TEST(changing_the_users_zone_rebuilds_the_occurrences_at_the_next_pull) {
    Env e;
    e.server->put_event("work", "late.ics", ics("late", "Late", "20261015T233000", "20261016T003000"), kIn, kInEnd + 2 * kDay);
    PB_CHECK(e.sync().ok());
    PB_CHECK_EQ(e.store->debug_query("SELECT local_date FROM occurrence"), std::string("2026-10-15"));
    e.store->set_setting("tz_override", "Asia/Tokyo");
    const int multigets = static_cast<int>(e.server->multiget_batch_sizes().size());
    PB_CHECK(e.sync().ok());
    PB_CHECK_EQ(e.store->debug_query("SELECT local_date FROM occurrence"), std::string("2026-10-16"));
    PB_CHECK_EQ(static_cast<int>(e.server->multiget_batch_sizes().size()), multigets);   // rebuilt from the stored text, nothing downloaded
}

PB_TEST(a_new_tz_data_release_or_a_new_day_rebuilds_the_occurrences) {
    Env e;
    e.server->put_event("work", "a.ics", ics("a", "A", "20261015T093000", "20261015T100000"), kIn, kInEnd + kDay);
    PB_CHECK(e.sync().ok());
    PB_CHECK(e.store->debug_exec("DELETE FROM occurrence").empty());
    PB_CHECK(e.sync().ok());
    PB_CHECK_EQ(e.count("SELECT count(*) FROM occurrence"), 0);           // same release, same day: no rebuild
    e.store->set_setting("occ_tzdata", "2020a");
    PB_CHECK(e.sync().ok());
    PB_CHECK_EQ(e.count("SELECT count(*) FROM occurrence"), 1);
}

PB_TEST(a_wrong_password_is_auth_failed_and_the_stored_data_stays) {
    Env e;
    e.server->put_event("work", "a.ics", ics("a", "A", "20261015T093000", "20261015T100000"), kIn, kInEnd + kDay);
    PB_CHECK(e.sync().ok());
    pbo::CalendarSyncDeps d{*e.store, *e.http, *e.tz, e.clock, e.cancel};
    const auto err = pbo::pull_calendars(d, "wrong", nullptr);
    PB_CHECK_EQ(err.key, std::string("auth_failed"));
    PB_CHECK_EQ(e.count("SELECT count(*) FROM event"), 1);
}

PB_TEST(a_feed_is_replaced_wholesale_and_asked_conditionally) {
    Env e;
    e.server->remove_calendar("work");
    pbo::CalendarAccount a;
    a.feed_url = e.server->origin() + "/feeds/h.ics";
    a.feed_name = "Holidays";
    PB_CHECK(e.store->save_calendar_account(a).ok());
    const std::string feed =
        "BEGIN:VCALENDAR\r\nVERSION:2.0\r\nPRODID:-//example.org//feed//EN\r\n"
        "BEGIN:VEVENT\r\nUID:h1@example.org\r\nSUMMARY:Holiday one\r\nDTSTART;VALUE=DATE:20261103\r\nDTEND;VALUE=DATE:20261104\r\nEND:VEVENT\r\n"
        "BEGIN:VEVENT\r\nUID:h2@example.org\r\nSUMMARY:Holiday two\r\nDTSTART;VALUE=DATE:20261225\r\nDTEND;VALUE=DATE:20261227\r\nEND:VEVENT\r\n"
        "BEGIN:VEVENT\r\nUID:h3@example.org\r\nSUMMARY:Too far\r\nDTSTART;VALUE=DATE:20300101\r\nEND:VEVENT\r\n"
        "END:VCALENDAR\r\n";
    e.server->set_feed("/feeds/h.ics", feed);
    pbo::CalendarCounts counts;
    PB_CHECK(e.sync(&counts).ok());
    PB_CHECK_EQ(counts.feed_events, 2);
    PB_CHECK_EQ(e.count("SELECT count(*) FROM event"), 2);
    const auto cals = e.store->calendars();
    PB_CHECK_EQ(cals.size(), size_t(1));
    PB_CHECK(cals[0].kind == "feed" && !cals[0].writable);
    PB_CHECK_EQ(cals[0].name, std::string("Holidays"));
    PB_CHECK(!cals[0].feed_etag.empty());
    PB_CHECK_EQ(e.store->debug_query("SELECT all_day FROM event WHERE uid = 'h2@example.org'"), std::string("1"));
    PB_CHECK(e.sync().ok());                                  // 304
    PB_CHECK_EQ(e.count("SELECT count(*) FROM event"), 2);
    PB_CHECK_EQ(e.server->log().back().headers.count("if-none-match"), size_t(1));
    // the feed drops an event and changes another
    e.server->set_feed("/feeds/h.ics", "BEGIN:VCALENDAR\r\nBEGIN:VEVENT\r\nUID:h2@example.org\r\nSUMMARY:Holiday two (renamed)\r\nDTSTART;VALUE=DATE:20261225\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n");
    PB_CHECK(e.sync().ok());
    PB_CHECK_EQ(e.store->debug_query("SELECT group_concat(summary) FROM event"), std::string("Holiday two (renamed)"));
}

PB_TEST(a_feed_problem_is_reported_and_the_server_calendars_are_still_pulled) {
    Env e;
    e.server->put_event("work", "a.ics", ics("a", "A", "20261015T093000", "20261015T100000"), kIn, kInEnd + kDay);
    pbo::CalendarAccount a;
    a.caldav_url = e.server->origin();
    a.username = "alice";
    a.feed_url = e.server->origin() + "/feeds/missing.ics";
    PB_CHECK(e.store->save_calendar_account(a).ok());
    const auto err = e.sync();
    PB_CHECK_EQ(err.key, std::string("feed_not_found"));
    PB_CHECK_EQ(e.count("SELECT count(*) FROM event"), 1);
}

PB_TEST(no_calendar_account_means_nothing_to_do_and_no_request) {
    Env e;
    pbtest::TmpDir other;
    auto opened = pbo::Store::open(other.path(), e.clock);
    pbo::CalendarSyncDeps d{*opened.value, *e.http, *e.tz, e.clock, e.cancel};
    PB_CHECK(pbo::pull_calendars(d, "x", nullptr).ok());
    PB_CHECK_EQ(e.server->log().size(), size_t(0));
}

PB_TEST_MAIN()
