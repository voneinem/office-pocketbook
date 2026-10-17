// T171 (home part): HomeBridge - the numbers and lists of the start page: unread mail, the events of today and tomorrow (soonest first, six rows and a
// "+ n more" row), and whether the calendar still waits for the time zone. The real core and a throw-away store.
#include <QCoreApplication>
#include <QVariantMap>

#include "home_bridge.h"
#include "pbo/calendar_sync.h"
#include "pbo/store.h"
#include "test_support.h"
#include "tmp_dir.h"

namespace {

int g_argc = 1;
char g_arg0[] = "test_home_status";
char* g_argv[] = {g_arg0, nullptr};
QCoreApplication g_app(g_argc, g_argv);

constexpr int64_t kNow = 1791979200;   // 2026-10-14 12:00:00 UTC = 14:00 Berlin

struct Clock : pbo::Clock {
    int64_t now() const override { return kNow; }
};
pbo::ZoneEnvironment no_zone() { return {"", ""}; }

struct Env {
    pbtest::TmpDir dir;
    Clock clock;
    std::unique_ptr<pbo::Store> store;
    std::unique_ptr<pbo::TzDatabase> tz;
    int64_t cal = 0;
    Env() {
        store = std::move(pbo::Store::open(dir.path(), clock).value);
        tz = pbo::TzDatabase::open(std::string(PBO_SOURCE_DIR) + "/third_party/tzdata/zoneinfo");
        PB_CHECK(tz != nullptr);
        store->set_setting("tz_override", "Europe/Berlin");
        pbo::Calendar c;
        c.kind = "caldav";
        c.href = "/cal/work/";
        c.name = "Work";
        c.writable = true;
        cal = store->upsert_calendar(c).value;
        pbo::CalendarAccount a;
        a.caldav_url = "https://cal.example.org";
        a.username = "alice";
        PB_CHECK(store->save_calendar_account(a).ok());
    }
    void event(const std::string& uid, const std::string& title, pbo::LocalDateTime start, int minutes, bool all_day = false) {
        char a[32], b[32];
        std::string times;
        if (all_day) {
            snprintf(a, sizeof a, "%04d%02d%02d", start.year, start.month, start.day);
            times = std::string("DTSTART;VALUE=DATE:") + a + "\r\n";
        } else {
            const pbo::LocalDateTime end = tz->utc_to_local("Europe/Berlin", tz->local_to_utc("Europe/Berlin", start) + minutes * 60);
            snprintf(a, sizeof a, "%04d%02d%02dT%02d%02d00", start.year, start.month, start.day, start.hour, start.minute);
            snprintf(b, sizeof b, "%04d%02d%02dT%02d%02d00", end.year, end.month, end.day, end.hour, end.minute);
            times = std::string("DTSTART;TZID=Europe/Berlin:") + a + "\r\nDTEND;TZID=Europe/Berlin:" + b + "\r\n";
        }
        const std::string ics = "BEGIN:VCALENDAR\r\nVERSION:2.0\r\nPRODID:-//x//y//EN\r\nBEGIN:VEVENT\r\nUID:" + uid + "\r\nDTSTAMP:20260101T000000Z\r\nSUMMARY:" + title + "\r\n" + times +
                                "END:VEVENT\r\nEND:VCALENDAR\r\n";
        pbo::EventUpsert u;
        PB_CHECK(pbo::build_event_upsert("/cal/" + uid + ".ics", "\"e\"", ics, *tz, "Europe/Berlin", kNow, &u));
        pbo::CalendarPullBatch batch;
        batch.upserts.push_back(u);
        PB_CHECK(store->apply_calendar_pull(cal, batch).ok());
    }
    void mail(uint32_t uid, bool read) {
        if (store->folders().empty()) {
            pbo::FolderListItem i;
            i.raw_name = i.name = "INBOX";
            store->upsert_folders({i});
        }
        pbo::PullBatch b;
        pbo::PulledMessage m;
        m.uid = uid;
        m.subject = "S";
        m.date_utc = kNow - 100;
        m.flags = read ? pbo::kFlagSeen : 0;
        b.messages.push_back(m);
        PB_CHECK(store->apply_pull(store->folders()[0].id, b).ok());
    }
    HomeBridge::Config config(pbo::ZoneEnvironment (*env)() = nullptr) {
        HomeBridge::Config c;
        c.store = store.get();
        c.tz = tz.get();
        c.clock = &clock;
        c.environment = env;
        return c;
    }
};

QVariantMap row(const QVariantList& l, int i) { return l.at(i).toMap(); }

}  // namespace

PB_TEST(the_unread_count_is_the_inboxs) {
    Env e;
    HomeBridge h(e.config());
    h.refresh();
    PB_CHECK_EQ(h.unreadCount(), 0);
    e.mail(1, false);
    e.mail(2, false);
    e.mail(3, true);
    h.refresh();
    PB_CHECK_EQ(h.unreadCount(), 2);
}

PB_TEST(the_events_of_today_and_tomorrow_are_listed_soonest_first_with_their_day) {
    Env e;
    e.event("a", "Later today", {2026, 10, 14, 16, 0, 0}, 60);
    e.event("b", "Tomorrow morning", {2026, 10, 15, 8, 30, 0}, 30);
    e.event("c", "Over", {2026, 10, 14, 8, 0, 0}, 30);              // earlier today: ended at 08:30 Berlin, 14:00 now
    e.event("d", "Day after", {2026, 10, 16, 9, 0, 0}, 30);
    HomeBridge h(e.config());
    h.refresh();
    PB_CHECK(!h.needsZone());
    PB_CHECK_EQ(h.events().size(), 3);                               // today's finished event stays: it is still today
    PB_CHECK_EQ(row(h.events(), 0)["title"].toString().toStdString(), std::string("Over"));
    PB_CHECK_EQ(row(h.events(), 0)["dayText"].toString().toStdString(), std::string("Today"));
    PB_CHECK_EQ(row(h.events(), 1)["title"].toString().toStdString(), std::string("Later today"));
    PB_CHECK_EQ(row(h.events(), 2)["dayText"].toString().toStdString(), std::string("Tomorrow"));
    PB_CHECK_EQ(row(h.events(), 2)["timeText"].toString().toStdString(), std::string("08:30 – 09:00"));
    PB_CHECK_EQ(h.moreCount(), 0);
}

PB_TEST(eight_events_give_six_rows_and_a_plus_two_row_and_six_or_fewer_give_none) {
    Env e;
    for (int i = 0; i < 8; ++i) e.event("e" + std::to_string(i), "Event " + std::to_string(i), {2026, 10, 14, 15 + (10 * i) / 60, (10 * i) % 60, 0}, 5);
    HomeBridge h(e.config());
    h.refresh();
    PB_CHECK_EQ(h.events().size(), 6);
    PB_CHECK_EQ(h.moreCount(), 2);
    Env f;
    for (int i = 0; i < 6; ++i) f.event("e" + std::to_string(i), "Event " + std::to_string(i), {2026, 10, 14, 15 + (10 * i) / 60, (10 * i) % 60, 0}, 5);
    HomeBridge g(f.config());
    g.refresh();
    PB_CHECK_EQ(g.events().size(), 6);
    PB_CHECK_EQ(g.moreCount(), 0);
}

PB_TEST(all_day_events_come_first_and_without_a_zone_no_times_are_listed) {
    Env e;
    e.event("t", "Timed", {2026, 10, 14, 15, 0, 0}, 60);
    e.event("h", "Holiday", {2026, 10, 14, 0, 0, 0}, 0, true);
    HomeBridge h(e.config());
    h.refresh();
    PB_CHECK_EQ(row(h.events(), 0)["title"].toString().toStdString(), std::string("Holiday"));
    PB_CHECK_EQ(row(h.events(), 0)["timeText"].toString().toStdString(), std::string("All day"));
    e.store->set_setting("tz_override", "");
    HomeBridge z(e.config(no_zone));
    z.refresh();
    PB_CHECK(z.needsZone());
    PB_CHECK_EQ(z.events().size(), 0);
}

PB_TEST_MAIN()
