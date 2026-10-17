// T111: the calendar bridges - AgendaModel, DayModel, MonthModel, EventBridge (view side), TimeZoneBridge and the calendar tab of AccountBridge -
// with the real core and a throw-away store. Server behaviour is covered by the core tests (fake CalDAV server).
#include <QCoreApplication>
#include <QSignalSpy>
#include <QTest>
#include <QVariantMap>

#include "account_bridge.h"
#include "agenda_model.h"
#include "day_model.h"
#include "event_bridge.h"
#include "fake_caldav_server.h"
#include "month_model.h"
#include "pbo/calendar_sync.h"
#include "pbo/credentials_file.h"
#include "test_support.h"
#include "timezone_bridge.h"
#include "tmp_dir.h"
#include "worker.h"

PBO_FAKE_SERVER_TEST();

namespace {

int g_argc = 1;
char g_arg0[] = "test_calendar_bridges";
char* g_argv[] = {g_arg0, nullptr};
QCoreApplication g_app(g_argc, g_argv);

constexpr int64_t kNow = 1791979200;   // 2026-10-14 12:00:00 UTC = 14:00 CEST

struct Clock : pbo::Clock {
    int64_t now() const override { return kNow; }
};

pbo::ZoneEnvironment no_zone() { return {"", ""}; }
pbo::ZoneEnvironment berlin_tz() { return {"Europe/Berlin", ""}; }

struct Env {
    pbtest::TmpDir dir;
    Clock clock;
    std::unique_ptr<pbo::Store> store;
    std::unique_ptr<pbo::TzDatabase> tz;
    Worker worker;
    int64_t cal = 0, feed = 0;

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
        pbo::Calendar f;
        f.kind = "feed";
        f.href = "https://calendars.example.org/h.ics";
        f.name = "Holidays";
        feed = store->upsert_calendar(f).value;
    }
    // an event written like a pull does; times given as Berlin wall clock
    void add(int64_t calendar, const std::string& uid, const std::string& title, pbo::LocalDateTime start, pbo::LocalDateTime end, bool all_day = false,
             const std::string& tzid = "Europe/Berlin", const std::string& rrule = "", const std::string& state = "synced") {
        pbo::EventUpsert u;
        u.href = "/cal/" + uid + ".ics";
        u.uid = uid;
        u.ics = "BEGIN:VCALENDAR\r\nBEGIN:VEVENT\r\nUID:" + uid + "\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n";
        u.summary = title;
        u.all_day = all_day;
        u.tzid = all_day ? std::string() : tzid;
        u.rrule = rrule;
        pbo::OccurrenceRow o;
        o.all_day = all_day;
        if (all_day) {
            o.start_utc = tz->local_to_utc("Europe/Berlin", start);
            o.end_utc = tz->local_to_utc("Europe/Berlin", end);
        } else {
            o.start_utc = tz->local_to_utc(tzid, start);
            o.end_utc = tz->local_to_utc(tzid, end);
            u.start_utc = o.start_utc;
            u.end_utc = o.end_utc;
        }
        const pbo::LocalDateTime l = tz->utc_to_local("Europe/Berlin", o.start_utc);
        o.local_date = pbo::civil::date_text(l.year, l.month, l.day);
        u.start_local = o.local_date + "T00:00:00";
        u.occurrences.push_back(o);
        pbo::CalendarPullBatch b;
        b.upserts.push_back(u);
        PB_CHECK(store->apply_calendar_pull(calendar, b).ok());
        if (state != "synced") PB_CHECK(store->debug_exec("UPDATE event SET sync_state = '" + state + "' WHERE uid = '" + uid + "'").empty());
    }
    AgendaModel::Config agenda_config(pbo::ZoneEnvironment (*env)() = nullptr) {
        AgendaModel::Config c;
        c.store = store.get();
        c.tz = tz.get();
        c.clock = &clock;
        c.environment = env;
        return c;
    }
};

QVariantMap row(const QVariantList& l, int i) { return l.at(i).toMap(); }

}  // namespace

PB_TEST(the_agenda_groups_events_under_date_headers_in_time_order) {
    Env e;
    e.add(e.cal, "a", "Standup", {2026, 10, 15, 9, 30, 0}, {2026, 10, 15, 10, 0, 0});
    e.add(e.cal, "b", "Review", {2026, 10, 15, 14, 0, 0}, {2026, 10, 15, 15, 30, 0});
    e.add(e.feed, "c", "Holiday", {2026, 10, 16, 0, 0, 0}, {2026, 10, 17, 0, 0, 0}, true);
    e.add(e.cal, "past", "Last week", {2026, 10, 7, 9, 0, 0}, {2026, 10, 7, 10, 0, 0});
    AgendaModel m(e.agenda_config());
    m.refresh();
    PB_CHECK(!m.needsZone());
    PB_CHECK_EQ(m.total(), 3);
    PB_CHECK_EQ(m.rows().size(), 5);                          // two headers and three events
    PB_CHECK_EQ(row(m.rows(), 0)["kind"].toString().toStdString(), std::string("header"));
    PB_CHECK_EQ(row(m.rows(), 0)["dateText"].toString().toStdString(), std::string("Thursday, 15 October 2026"));
    PB_CHECK_EQ(row(m.rows(), 1)["title"].toString().toStdString(), std::string("Standup"));
    PB_CHECK_EQ(row(m.rows(), 1)["timeText"].toString().toStdString(), std::string("09:30 – 10:00"));
    PB_CHECK_EQ(row(m.rows(), 1)["calendarName"].toString().toStdString(), std::string("Work"));
    PB_CHECK(!row(m.rows(), 1)["readOnly"].toBool());
    PB_CHECK_EQ(row(m.rows(), 3)["kind"].toString().toStdString(), std::string("header"));
    PB_CHECK_EQ(row(m.rows(), 4)["timeText"].toString().toStdString(), std::string("All day"));
    PB_CHECK(row(m.rows(), 4)["readOnly"].toBool());           // the feed
}

PB_TEST(earlier_events_of_today_are_still_listed) {
    Env e;
    e.add(e.cal, "morning", "Breakfast", {2026, 10, 14, 8, 0, 0}, {2026, 10, 14, 9, 0, 0});
    e.add(e.cal, "yesterday", "Yesterday", {2026, 10, 13, 8, 0, 0}, {2026, 10, 13, 9, 0, 0});
    AgendaModel m(e.agenda_config());
    m.refresh();
    PB_CHECK_EQ(m.total(), 1);
    PB_CHECK_EQ(row(m.rows(), 1)["title"].toString().toStdString(), std::string("Breakfast"));
}

PB_TEST(pages_hold_at_most_the_given_rows_and_a_continued_day_repeats_its_header) {
    Env e;
    for (int i = 0; i < 5; ++i) e.add(e.cal, "e" + std::to_string(i), "Event " + std::to_string(i), {2026, 10, 15, 8 + i, 0, 0}, {2026, 10, 15, 8 + i, 30, 0});
    e.add(e.cal, "later", "Later", {2026, 10, 16, 8, 0, 0}, {2026, 10, 16, 9, 0, 0});
    AgendaModel m(e.agenda_config());
    m.setPageSize(4);
    m.refresh();
    PB_CHECK_EQ(m.total(), 6);
    PB_CHECK_EQ(m.rows().size(), 4);                                    // header + 3 events
    PB_CHECK(m.pageCount() >= 3);
    m.nextPage();
    PB_CHECK_EQ(m.page(), 1);
    PB_CHECK_EQ(row(m.rows(), 0)["kind"].toString().toStdString(), std::string("header"));   // the same day continues
    PB_CHECK_EQ(row(m.rows(), 0)["localDate"].toString().toStdString(), std::string("2026-10-15"));
    PB_CHECK_EQ(row(m.rows(), 1)["title"].toString().toStdString(), std::string("Event 3"));
    for (int p = 0; p < m.pageCount(); ++p) {
        m.goToPage(p);
        PB_CHECK(m.rows().size() <= 4);
    }
    m.goToPage(99);
    PB_CHECK_EQ(m.page(), m.pageCount() - 1);
    m.previousPage();
    m.previousPage();
    m.previousPage();
    m.previousPage();
    PB_CHECK_EQ(m.page(), 0);
}

PB_TEST(pending_and_conflicting_events_are_flagged) {
    Env e;
    e.add(e.cal, "a", "Mine", {2026, 10, 15, 8, 0, 0}, {2026, 10, 15, 9, 0, 0}, false, "Europe/Berlin", "", "pending_update");
    e.add(e.cal, "b", "Both", {2026, 10, 15, 10, 0, 0}, {2026, 10, 15, 11, 0, 0}, false, "Europe/Berlin", "", "conflict");
    AgendaModel m(e.agenda_config());
    m.refresh();
    PB_CHECK(row(m.rows(), 1)["pending"].toBool() && !row(m.rows(), 1)["conflict"].toBool());
    PB_CHECK(row(m.rows(), 2)["conflict"].toBool());
}

PB_TEST(without_a_known_zone_the_models_show_nothing_and_ask_for_one) {
    Env e;
    e.store->set_setting("tz_override", "");
    e.add(e.cal, "a", "A", {2026, 10, 15, 8, 0, 0}, {2026, 10, 15, 9, 0, 0});
    AgendaModel agenda(e.agenda_config(no_zone));
    agenda.refresh();
    PB_CHECK(agenda.needsZone());
    PB_CHECK(agenda.rows().isEmpty());
    DayModel::Config dc;
    dc.store = e.store.get();
    dc.tz = e.tz.get();
    dc.clock = &e.clock;
    dc.environment = no_zone;
    DayModel day(dc);
    day.goToday();
    PB_CHECK(day.needsZone() && day.rows().isEmpty());
    MonthModel::Config mc;
    mc.store = e.store.get();
    mc.tz = e.tz.get();
    mc.clock = &e.clock;
    mc.environment = no_zone;
    MonthModel month(mc);
    month.refresh();
    PB_CHECK(month.needsZone() && month.weeks().isEmpty());
    // TZ names a zone: no question needed
    AgendaModel with_tz(e.agenda_config(berlin_tz));
    e.store->set_setting("tz_override", "");
    with_tz.refresh();
    PB_CHECK(!with_tz.needsZone());
    PB_CHECK_EQ(with_tz.zoneName().toStdString(), std::string("Europe/Berlin"));
}

PB_TEST(the_day_lists_all_day_events_first_and_clips_events_that_cross_midnight) {
    Env e;
    e.add(e.cal, "a", "Meeting", {2026, 10, 15, 10, 0, 0}, {2026, 10, 15, 11, 0, 0});
    e.add(e.cal, "b", "Holiday", {2026, 10, 15, 0, 0, 0}, {2026, 10, 16, 0, 0, 0}, true);
    e.add(e.cal, "c", "Night", {2026, 10, 15, 22, 0, 0}, {2026, 10, 16, 2, 0, 0});
    e.add(e.cal, "d", "Early", {2026, 10, 14, 22, 0, 0}, {2026, 10, 15, 2, 0, 0});
    DayModel::Config dc;
    dc.store = e.store.get();
    dc.tz = e.tz.get();
    dc.clock = &e.clock;
    DayModel m(dc);
    m.goToday();
    PB_CHECK_EQ(m.selectedDate().toStdString(), std::string("2026-10-14"));
    PB_CHECK(m.isToday());
    PB_CHECK_EQ(m.dateText().toStdString(), std::string("Wednesday, 14 October 2026"));
    PB_CHECK_EQ(m.total(), 1);                                  // "Early" starts today
    PB_CHECK_EQ(row(m.rows(), 0)["timeText"].toString().toStdString(), std::string("from 22:00"));
    m.nextDay();
    PB_CHECK_EQ(m.selectedDate().toStdString(), std::string("2026-10-15"));
    PB_CHECK(!m.isToday());
    PB_CHECK_EQ(m.total(), 4);
    PB_CHECK_EQ(row(m.rows(), 0)["title"].toString().toStdString(), std::string("Holiday"));      // all-day first
    PB_CHECK_EQ(row(m.rows(), 1)["title"].toString().toStdString(), std::string("Early"));
    PB_CHECK_EQ(row(m.rows(), 1)["timeText"].toString().toStdString(), std::string("until 02:00"));
    PB_CHECK_EQ(row(m.rows(), 3)["timeText"].toString().toStdString(), std::string("from 22:00"));
    m.previousDay();
    m.previousDay();
    PB_CHECK_EQ(m.selectedDate().toStdString(), std::string("2026-10-13"));
    m.selectDate("garbage");
    PB_CHECK_EQ(m.selectedDate().toStdString(), std::string("2026-10-13"));
    m.selectDate("2026-02-30");
    PB_CHECK_EQ(m.selectedDate().toStdString(), std::string("2026-10-13"));
    m.goToday();
    PB_CHECK_EQ(m.selectedDate().toStdString(), std::string("2026-10-14"));
}

PB_TEST(the_day_pages_its_rows) {
    Env e;
    for (int i = 0; i < 7; ++i) e.add(e.cal, "e" + std::to_string(i), "E" + std::to_string(i), {2026, 10, 15, 8 + i, 0, 0}, {2026, 10, 15, 8 + i, 30, 0});
    DayModel::Config dc;
    dc.store = e.store.get();
    dc.tz = e.tz.get();
    dc.clock = &e.clock;
    DayModel m(dc);
    m.setPageSize(3);
    m.selectDate("2026-10-15");
    PB_CHECK_EQ(m.pageCount(), 3);
    PB_CHECK_EQ(m.rows().size(), 3);
    m.nextPage();
    m.nextPage();
    PB_CHECK_EQ(m.rows().size(), 1);
    m.nextPage();
    PB_CHECK_EQ(m.page(), 2);
}

PB_TEST(the_month_marks_the_days_with_events_including_every_day_of_a_long_event) {
    Env e;
    e.add(e.cal, "a", "A", {2026, 10, 15, 9, 0, 0}, {2026, 10, 15, 10, 0, 0});
    e.add(e.cal, "trip", "Trip", {2026, 10, 20, 0, 0, 0}, {2026, 10, 23, 0, 0, 0}, true);   // 20, 21, 22
    MonthModel::Config mc;
    mc.store = e.store.get();
    mc.tz = e.tz.get();
    mc.clock = &e.clock;
    MonthModel m(mc);
    m.refresh();
    PB_CHECK_EQ(m.title().toStdString(), std::string("October 2026"));
    PB_CHECK_EQ(m.mode().toStdString(), std::string("month"));
    PB_CHECK_EQ(m.weeks().size(), 5);                         // 28 Sep .. 1 Nov
    QStringList marked;
    QString today, selected;
    for (const QVariant& w : m.weeks())
        for (const QVariant& c : w.toList()) {
            const QVariantMap cell = c.toMap();
            if (cell["marked"].toBool()) marked << cell["date"].toString();
            if (cell["today"].toBool()) today = cell["date"].toString();
            if (cell["selected"].toBool()) selected = cell["date"].toString();
        }
    PB_CHECK_EQ(marked.join(",").toStdString(), std::string("2026-10-15,2026-10-20,2026-10-21,2026-10-22"));
    PB_CHECK_EQ(today.toStdString(), std::string("2026-10-14"));
    PB_CHECK_EQ(selected.toStdString(), std::string("2026-10-14"));
    const QVariantMap first = m.weeks().first().toList().first().toMap();
    PB_CHECK_EQ(first["date"].toString().toStdString(), std::string("2026-09-28"));            // a Monday
    PB_CHECK(!first["inMonth"].toBool());
    PB_CHECK_EQ(m.weekdayNames().first().toStdString(), std::string("Mon"));
    m.next();
    PB_CHECK_EQ(m.title().toStdString(), std::string("November 2026"));
    m.previous();
    m.previous();
    PB_CHECK_EQ(m.title().toStdString(), std::string("September 2026"));
    m.selectDate("2026-12-31");
    PB_CHECK_EQ(m.title().toStdString(), std::string("December 2026"));
    m.next();
    PB_CHECK_EQ(m.title().toStdString(), std::string("January 2027"));
    m.goToday();
    PB_CHECK_EQ(m.selectedDate().toStdString(), std::string("2026-10-14"));
}

PB_TEST(the_week_mode_shows_one_row_and_moves_by_weeks) {
    Env e;
    MonthModel::Config mc;
    mc.store = e.store.get();
    mc.tz = e.tz.get();
    mc.clock = &e.clock;
    MonthModel m(mc);
    m.setMode("week");
    PB_CHECK_EQ(m.weeks().size(), 1);
    PB_CHECK_EQ(m.title().toStdString(), std::string("12 - 18 October 2026"));
    m.next();
    PB_CHECK_EQ(m.title().toStdString(), std::string("19 - 25 October 2026"));
    m.next();
    PB_CHECK_EQ(m.title().toStdString(), std::string("26 Oct - 1 Nov 2026"));
    m.setMode("nonsense");
    PB_CHECK_EQ(m.mode().toStdString(), std::string("week"));
    m.setMode("month");
    PB_CHECK_EQ(m.mode().toStdString(), std::string("month"));
}

PB_TEST(the_event_view_shows_the_time_in_the_users_zone_and_the_event_zone_when_it_differs) {
    Env e;
    e.add(e.cal, "ny", "Call", {2026, 10, 15, 9, 0, 0}, {2026, 10, 15, 10, 0, 0}, false, "America/New_York", "FREQ=WEEKLY;BYDAY=TH;UNTIL=20270630");
    EventBridge::Config c;
    c.store = e.store.get();
    c.tz = e.tz.get();
    c.clock = &e.clock;
    EventBridge b(c);
    const int64_t id = std::atoll(e.store->debug_query("SELECT id FROM event").c_str());
    b.open(id);
    PB_CHECK(b.isOpen());
    PB_CHECK_EQ(b.summary().toStdString(), std::string("Call"));
    PB_CHECK_EQ(b.whenText().toStdString(), std::string("Thursday, 15 October 2026, 15:00 – 16:00"));      // 09:00 New York = 15:00 Berlin
    PB_CHECK_EQ(b.zoneText().toStdString(), std::string("Thu 15 Oct 09:00 – 10:00 (America/New_York)"));
    PB_CHECK_EQ(b.recurrenceText().toStdString(), std::string("every week on Thursday until 30 June 2027"));
    PB_CHECK_EQ(b.calendarName().toStdString(), std::string("Work"));
    PB_CHECK(!b.readOnly());
    b.close();
    PB_CHECK(!b.isOpen());
}

PB_TEST(the_event_view_marks_feeds_and_read_only_calendars_and_shows_all_day_ranges) {
    Env e;
    e.add(e.feed, "h", "Holiday", {2026, 12, 24, 0, 0, 0}, {2026, 12, 27, 0, 0, 0}, true);
    EventBridge::Config c;
    c.store = e.store.get();
    c.tz = e.tz.get();
    c.clock = &e.clock;
    EventBridge b(c);
    b.open(std::atoll(e.store->debug_query("SELECT id FROM event").c_str()));
    PB_CHECK(b.readOnly());
    PB_CHECK_EQ(b.readOnlyReason().toStdString(), std::string("feed"));
    PB_CHECK_EQ(b.whenText().toStdString(), std::string("Thursday, 24 December 2026 – Saturday, 26 December 2026 (all day)"));
    PB_CHECK(b.zoneText().isEmpty());
    PB_CHECK(b.recurrenceText().isEmpty());
    b.open(424242);
    PB_CHECK(!b.isOpen());
}

PB_TEST(the_zone_picker_pages_the_places_of_a_group_and_rejects_unknown_names) {
    Env e;
    e.store->set_setting("tz_override", "");
    TimeZoneBridge::Config c;
    c.store = e.store.get();
    c.tz = e.tz.get();
    c.clock = &e.clock;
    c.worker = &e.worker;
    c.environment = no_zone;
    TimeZoneBridge b(c);
    PB_CHECK(b.needsZoneChoice());
    PB_CHECK(b.zoneName().isEmpty());
    PB_CHECK(b.groups().contains("Europe"));
    PB_CHECK(!b.tzVersion().isEmpty());
    b.setPageSize(8);
    b.setGroup("Europe");
    PB_CHECK_EQ(b.rows().size(), 8);
    PB_CHECK(b.pageCount() >= 6);
    PB_CHECK_EQ(b.group().toStdString(), std::string("Europe"));
    bool saw_berlin = false;
    for (int p = 0; p < b.pageCount(); ++p) {
        b.goToPage(p);
        for (const QVariant& r : b.rows()) {
            const QVariantMap m = r.toMap();
            PB_CHECK(m["name"].toString().startsWith("Europe/"));
            if (m["name"].toString() == "Europe/Berlin") {
                saw_berlin = true;
                PB_CHECK_EQ(m["label"].toString().toStdString(), std::string("Berlin"));
            }
        }
    }
    PB_CHECK(saw_berlin);
    b.setGroup("UTC");
    PB_CHECK_EQ(b.rows().size(), 1);

    QSignalSpy set(&b, &TimeZoneBridge::zoneSet);
    b.setTimeZone("Mars/Olympus");
    PB_CHECK_EQ(set.count(), 1);
    PB_CHECK(!set.at(0).at(0).toBool());
    PB_CHECK_EQ(set.at(0).at(1).toString().toStdString(), std::string("invalid_zone"));
    PB_CHECK(b.needsZoneChoice());
    PB_CHECK_EQ(e.store->setting("tz_override"), std::string(""));       // never stored
    b.setTimeZone("../../etc/passwd");
    PB_CHECK_EQ(e.store->setting("tz_override"), std::string(""));

    b.setTimeZone("europe/berlin");
    QTRY_COMPARE_WITH_TIMEOUT(set.count(), 3, 5000);
    PB_CHECK(set.at(2).at(0).toBool());
    PB_CHECK(!b.needsZoneChoice());
    PB_CHECK_EQ(b.zoneName().toStdString(), std::string("Europe/Berlin"));
    PB_CHECK_EQ(e.store->setting("tz_override"), std::string("Europe/Berlin"));       // the canonical spelling
}

PB_TEST(changing_the_zone_expands_the_occurrences_again) {
    Env e;
    e.worker.post([] {});   // the worker is up
    // an event stored with its text; a zone change must move its date
    pbo::EventUpsert u;
    u.href = "/cal/late.ics";
    u.uid = "late";
    u.ics = "BEGIN:VCALENDAR\r\nBEGIN:VEVENT\r\nUID:late\r\nDTSTART;TZID=Europe/Berlin:20261015T233000\r\nDTEND;TZID=Europe/Berlin:20261016T003000\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n";
    u.summary = "Late";
    u.start_local = "2026-10-15T23:30:00";
    pbo::OccurrenceRow o;
    o.start_utc = e.tz->local_to_utc("Europe/Berlin", {2026, 10, 15, 23, 30, 0});
    o.end_utc = o.start_utc + 3600;
    o.local_date = "2026-10-15";
    u.occurrences.push_back(o);
    pbo::CalendarPullBatch b;
    b.upserts.push_back(u);
    PB_CHECK(e.store->apply_calendar_pull(e.cal, b).ok());
    TimeZoneBridge::Config c;
    c.store = e.store.get();
    c.tz = e.tz.get();
    c.clock = &e.clock;
    c.worker = &e.worker;
    TimeZoneBridge zb(c);
    QSignalSpy set(&zb, &TimeZoneBridge::zoneSet);
    zb.setTimeZone("Asia/Tokyo");
    QTRY_COMPARE_WITH_TIMEOUT(set.count(), 1, 5000);
    PB_CHECK(set.at(0).at(0).toBool());
    PB_CHECK_EQ(e.store->debug_query("SELECT local_date FROM occurrence"), std::string("2026-10-16"));
}

PB_TEST(the_calendar_tab_validates_tests_and_saves_with_the_password_apart) {
    pbtest::TmpDir dir, pki_dir;
    pbtest::TestPki pki(pki_dir);
    if (!pki.available()) PB_SKIP("openssl CLI not available");
    auto leaf = pki.issue("srv", "DNS:localhost,IP:127.0.0.1");
    PB_CHECK(leaf.ok);
    Clock clock;
    auto store = std::move(pbo::Store::open(dir.path(), clock).value);
    Worker worker;
    pbtest::FakeCalDavServer::Config sc;
    sc.cert = leaf.cert;
    sc.key = leaf.key;
    pbtest::FakeCalDavServer server(sc);
    pbtest::FakeCalDavServer::CalendarOptions work;
    work.name = "Work";
    server.add_calendar("work", work);
    AccountBridge::Config ac;
    ac.store = store.get();
    ac.data_dir = dir.path();
    ac.ca_file = pki.ca_cert();
    ac.worker = &worker;
    AccountBridge account(ac);
    PB_CHECK(!account.calendarConfigured());

    QSignalSpy save(&account, &AccountBridge::saveResult);
    QSignalSpy test(&account, &AccountBridge::testResult);
    QSignalSpy found(&account, &AccountBridge::calendarsFound);
    QVariantMap bad;
    bad["caldavUrl"] = "http://insecure.example.org/dav";
    bad["username"] = "alice";
    bad["password"] = "x";
    account.testCalendar(bad);
    PB_CHECK_EQ(test.count(), 1);
    PB_CHECK_EQ(test.at(0).at(1).toString().toStdString(), std::string("invalid_input"));
    PB_CHECK_EQ(test.at(0).at(2).toString().toStdString(), std::string("caldav_url:url_not_https"));
    account.saveCalendar(bad);
    PB_CHECK(!save.at(0).at(0).toBool());
    PB_CHECK(!account.calendarConfigured());

    QVariantMap good;
    good["caldavUrl"] = QString::fromStdString(server.origin());
    good["username"] = "alice";
    good["password"] = "pw-for-tests";
    account.testCalendar(good);
    QTRY_COMPARE_WITH_TIMEOUT(test.count(), 2, 20000);
    PB_CHECK_EQ(test.at(1).at(1).toString().toStdString(), std::string("ok"));
    PB_CHECK_EQ(found.count(), 1);
    PB_CHECK_EQ(found.at(0).at(0).toList().size(), 1);
    PB_CHECK_EQ(found.at(0).at(0).toList().at(0).toMap()["name"].toString().toStdString(), std::string("Work"));

    good["password"] = "wrong";
    account.testCalendar(good);
    QTRY_COMPARE_WITH_TIMEOUT(test.count(), 3, 20000);
    PB_CHECK_EQ(test.at(2).at(1).toString().toStdString(), std::string("auth_failed"));
    PB_CHECK_EQ(test.at(2).at(2).toString().toStdString(), std::string("caldav"));

    good["password"] = "pw-for-tests";
    account.saveCalendar(good);
    PB_CHECK(save.back().at(0).toBool());
    PB_CHECK(account.calendarConfigured());
    const auto creds = pbo::load_credentials(dir.path()).value;
    PB_CHECK_EQ(creds.calendar_password, std::string("pw-for-tests"));
    PB_CHECK(creds.mail_password.empty());
    const QVariantMap form = account.calendarForm();
    PB_CHECK_EQ(form["username"].toString().toStdString(), std::string("alice"));
    PB_CHECK(!form.contains("password"));
    PB_CHECK(store->debug_query("SELECT count(*) FROM account_calendar") == "1");
}

PB_TEST(calendars_can_be_hidden_from_the_list_of_known_calendars) {
    Env e;
    AccountBridge::Config ac;
    ac.store = e.store.get();
    ac.data_dir = e.dir.path();
    ac.worker = &e.worker;
    AccountBridge account(ac);
    QSignalSpy changed(&account, &AccountBridge::calendarsChanged);
    PB_CHECK_EQ(account.calendars().size(), 2);
    account.setCalendarShown(e.feed, false);
    PB_CHECK_EQ(changed.count(), 1);
    for (const QVariant& v : account.calendars())
        PB_CHECK(v.toMap()["shown"].toBool() == (v.toMap()["kind"].toString() == "caldav"));
    account.setCalendarShown(99999, false);
    PB_CHECK_EQ(changed.count(), 1);
}

PB_TEST_MAIN()
