// T100 (part): the agenda as the user sees it, after a real pull: time order, paging, all-day and floating placement, recurring events on the
// right dates, read-only marking of feeds, the calendar choice, and the dates in the user's zone. Fake servers only (UNVERIFIED).
#include <string>

#include "fake_caldav_server.h"
#include "pbo/calendar_sync.h"
#include "pbo/curl_transport.h"
#include "test_support.h"
#include "tmp_dir.h"

using pbtest::FakeCalDavServer;

PBO_FAKE_SERVER_TEST();

namespace {

constexpr int64_t kNow = 1791979200;   // 2026-10-14 12:00:00 UTC = 14:00 in Berlin
constexpr int64_t kDay = 86400;

struct Clock : pbo::Clock {
    int64_t now() const override { return kNow; }
};
struct NoCancel : pbo::Cancel {
    bool requested() const override { return false; }
};

std::string vevent(const std::string& uid, const std::string& summary, const std::string& dt, const std::string& extra = "") {
    return "BEGIN:VCALENDAR\r\nVERSION:2.0\r\nBEGIN:VEVENT\r\nUID:" + uid + "@example.org\r\nSUMMARY:" + summary + "\r\n" + dt + "\r\n" + extra + "END:VEVENT\r\nEND:VCALENDAR\r\n";
}

struct Env {
    pbtest::TmpDir data, pki_dir;
    pbtest::TestPki pki{pki_dir};
    pbtest::TestPki::Leaf leaf;
    Clock clock;
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
        store = std::move(pbo::Store::open(data.path(), clock).value);
        tz = pbo::TzDatabase::open(std::string(PBO_SOURCE_DIR) + "/third_party/tzdata/zoneinfo");
        http = std::make_unique<pbo::CurlTransport>(pki.ca_cert());
        store->set_setting("tz_override", "Europe/Berlin");
        FakeCalDavServer::CalendarOptions work;
        work.name = "Work";
        server->add_calendar("work", work);
        pbo::CalendarAccount a;
        a.caldav_url = server->origin();
        a.username = "alice";
        a.feed_url = server->origin() + "/feeds/h.ics";
        a.feed_name = "Holidays";
        PB_CHECK(store->save_calendar_account(a).ok());
        server->set_feed("/feeds/h.ics", "BEGIN:VCALENDAR\r\nVERSION:2.0\r\n" + std::string("BEGIN:VEVENT\r\nUID:h1@example.org\r\nSUMMARY:Reformation Day\r\nDTSTART;VALUE=DATE:20261031\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n"));
    }
    void sync() {
        pbo::CalendarSyncDeps d{*store, *http, *tz, clock, cancel};
        PB_CHECK(pbo::pull_calendars(d, "pw-for-tests", nullptr).ok());
    }
    void add(const std::string& name, const std::string& ics_text) { server->put_event("work", name, ics_text, 0); }
};

}  // namespace

PB_TEST(the_agenda_shows_the_next_events_in_time_order_with_all_day_events_at_midnight) {
    Env e;
    e.add("late.ics", vevent("late", "Evening", "DTSTART;TZID=Europe/Berlin:20261014T200000\r\nDTEND;TZID=Europe/Berlin:20261014T210000"));
    e.add("allday.ics", vevent("allday", "Conference", "DTSTART;VALUE=DATE:20261015\r\nDTEND;VALUE=DATE:20261017"));
    e.add("morning.ics", vevent("morning", "Breakfast", "DTSTART;TZID=Europe/Berlin:20261015T080000\r\nDTEND;TZID=Europe/Berlin:20261015T090000"));
    e.add("past.ics", vevent("past", "Earlier today", "DTSTART;TZID=Europe/Berlin:20261014T090000\r\nDTEND;TZID=Europe/Berlin:20261014T100000"));
    e.sync();
    const auto page = e.store->agenda(kNow, 0, 10);
    std::vector<std::string> names;
    for (const auto& it : page.items) names.push_back(it.summary);
    PB_CHECK((names == std::vector<std::string>{"Evening", "Conference", "Breakfast", "Reformation Day"}));
    PB_CHECK(page.items[1].all_day);
    PB_CHECK_EQ(page.items[1].local_date, std::string("2026-10-15"));
    PB_CHECK_EQ(page.items[1].start_utc, e.tz->local_to_utc("Europe/Berlin", {2026, 10, 15, 0, 0, 0}));
    PB_CHECK_EQ(page.items[1].end_utc, e.tz->local_to_utc("Europe/Berlin", {2026, 10, 17, 0, 0, 0}));
}

PB_TEST(feeds_are_marked_read_only_and_calendars_can_be_hidden) {
    Env e;
    e.add("a.ics", vevent("a", "Mine", "DTSTART;TZID=Europe/Berlin:20261015T080000\r\nDTEND;TZID=Europe/Berlin:20261015T090000"));
    e.sync();
    const auto page = e.store->agenda(kNow, 0, 10);
    PB_CHECK_EQ(page.total, 2);
    for (const auto& it : page.items) PB_CHECK(it.writable == (it.summary == "Mine"));
    PB_CHECK_EQ(page.items[1].calendar_name, std::string("Holidays"));
    for (const auto& c : e.store->calendars())
        if (c.kind == "feed") PB_CHECK(e.store->set_calendar_shown(c.id, false).ok());
    PB_CHECK_EQ(e.store->agenda(kNow, 0, 10).total, 1);
    e.sync();                                                  // the choice survives a sync
    PB_CHECK_EQ(e.store->agenda(kNow, 0, 10).total, 1);
}

PB_TEST(a_recurring_event_appears_on_every_right_date_across_the_change_of_summer_time) {
    Env e;
    e.add("weekly.ics", vevent("weekly", "Weekly", "DTSTART;TZID=Europe/Berlin:20261012T100000\r\nDTEND;TZID=Europe/Berlin:20261012T110000", "RRULE:FREQ=WEEKLY;COUNT=4\r\n"));
    e.sync();
    std::vector<std::string> dates;
    for (const auto& it : e.store->occurrences_between(kNow, kNow + 40 * kDay))
        if (it.summary == "Weekly") dates.push_back(it.local_date + "/" + std::to_string(it.start_utc % kDay / 3600));
    // summer time ends on 2026-10-25: 2026-10-19 is 08:00 UTC, 2026-10-26 and 2026-11-02 are 09:00 UTC (always 10:00 in Berlin)
    PB_CHECK((dates == std::vector<std::string>{"2026-10-19/8", "2026-10-26/9", "2026-11-02/9"}));
}

PB_TEST(floating_events_stay_at_the_same_wall_clock_in_the_users_zone) {
    Env e;
    e.add("floating.ics", vevent("floating", "Lunch", "DTSTART:20261015T120000\r\nDTEND:20261015T130000"));
    e.sync();
    auto page = e.store->agenda(kNow, 0, 10);
    PB_CHECK_EQ(page.items[0].summary, std::string("Lunch"));
    PB_CHECK_EQ(page.items[0].start_utc, e.tz->local_to_utc("Europe/Berlin", {2026, 10, 15, 12, 0, 0}));
    e.store->set_setting("tz_override", "Asia/Tokyo");
    e.sync();
    page = e.store->agenda(kNow, 0, 10);
    PB_CHECK_EQ(page.items[0].start_utc, e.tz->local_to_utc("Asia/Tokyo", {2026, 10, 15, 12, 0, 0}));
    PB_CHECK_EQ(page.items[0].local_date, std::string("2026-10-15"));
}

PB_TEST(the_dates_follow_the_zone_the_user_chose) {
    Env e;
    e.add("night.ics", vevent("night", "Night shift", "DTSTART;TZID=America/New_York:20261015T200000\r\nDTEND;TZID=America/New_York:20261015T230000"));
    e.sync();
    PB_CHECK_EQ(e.store->agenda(kNow, 0, 10).items[0].local_date, std::string("2026-10-16"));   // 02:00 in Berlin
    e.store->set_setting("tz_override", "America/New_York");
    e.sync();
    PB_CHECK_EQ(e.store->agenda(kNow, 0, 10).items[0].local_date, std::string("2026-10-15"));
}

PB_TEST(marked_days_cover_the_dates_with_occurrences) {
    Env e;
    e.add("a.ics", vevent("a", "A", "DTSTART;TZID=Europe/Berlin:20261015T080000\r\nDTEND;TZID=Europe/Berlin:20261015T090000"));
    e.add("b.ics", vevent("b", "B", "DTSTART;TZID=Europe/Berlin:20261020T080000\r\nDTEND;TZID=Europe/Berlin:20261020T090000"));
    e.sync();
    const auto days = e.store->marked_days("2026-10-01", "2026-10-31");
    PB_CHECK((days == std::vector<std::string>{"2026-10-15", "2026-10-20", "2026-10-31"}));
}

PB_TEST_MAIN()
