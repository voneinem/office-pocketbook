// T147: EventEditBridge (new / change / delete an event as the pages do) and ConflictsModel (the list of events that need the user), with the real core
// and a throw-away store. The network side is covered by the core tests (fake CalDAV server).
#include <QCoreApplication>
#include <QSignalSpy>
#include <QTest>
#include <QVariantMap>

#include "conflicts_model.h"
#include "event_edit_bridge.h"
#include "pbo/calendar_sync.h"
#include "test_support.h"
#include "tmp_dir.h"

namespace {

int g_argc = 1;
char g_arg0[] = "test_event_edit_bridge";
char* g_argv[] = {g_arg0, nullptr};
QCoreApplication g_app(g_argc, g_argv);

constexpr int64_t kNow = 1791979200;   // 2026-10-14 12:00:00 UTC = 14:00 CEST

struct Clock : pbo::Clock {
    int64_t now() const override { return kNow; }
};
struct Rnd : pbo::Random {
    uint8_t n = 3;
    void bytes(uint8_t* out, size_t len) override {
        for (size_t i = 0; i < len; ++i) out[i] = n++;
    }
};

struct Env {
    pbtest::TmpDir dir;
    Clock clock;
    Rnd random;
    std::unique_ptr<pbo::Store> store;
    std::unique_ptr<pbo::TzDatabase> tz;
    int64_t cal = 0, ro = 0, feed = 0;
    Env() {
        store = std::move(pbo::Store::open(dir.path(), clock).value);
        tz = pbo::TzDatabase::open(std::string(PBO_SOURCE_DIR) + "/third_party/tzdata/zoneinfo");
        PB_CHECK(tz != nullptr);
        store->set_setting("tz_override", "Europe/Berlin");
        cal = add_calendar("/cal/work/", "Work", true, "caldav");
        ro = add_calendar("/cal/shared/", "Shared", false, "caldav");
        feed = add_calendar("https://example.org/h.ics", "Holidays", false, "feed");
    }
    int64_t add_calendar(const std::string& href, const std::string& name, bool writable, const char* kind) {
        pbo::Calendar c;
        c.kind = kind;
        c.href = href;
        c.name = name;
        c.writable = writable;
        return store->upsert_calendar(c).value;
    }
    EventEditBridge::Config config() {
        EventEditBridge::Config c;
        c.store = store.get();
        c.tz = tz.get();
        c.clock = &clock;
        c.random = &random;
        return c;
    }
    // a synced event as a pull leaves it
    int64_t pulled(int64_t calendar, const std::string& uid, const std::string& title, const std::string& extra = "") {
        const std::string ics = "BEGIN:VCALENDAR\r\nVERSION:2.0\r\nPRODID:-//x//y//EN\r\nBEGIN:VEVENT\r\nUID:" + uid + "\r\nDTSTAMP:20260101T000000Z\r\nSUMMARY:" + title +
                                "\r\nDTSTART;TZID=Europe/Berlin:20261015T093000\r\nDTEND;TZID=Europe/Berlin:20261015T103000\r\n" + extra + "END:VEVENT\r\nEND:VCALENDAR\r\n";
        pbo::EventUpsert u;
        PB_CHECK(pbo::build_event_upsert("/cal/" + uid + ".ics", "\"e1\"", ics, *tz, "Europe/Berlin", kNow, &u));
        pbo::CalendarPullBatch b;
        b.upserts.push_back(u);
        PB_CHECK(store->apply_calendar_pull(calendar, b).ok());
        return std::atoll(store->debug_query("SELECT id FROM event WHERE uid = '" + uid + "'").c_str());
    }
    std::string q(const std::string& sql) { return store->debug_query(sql); }
};

}  // namespace

PB_TEST(a_new_event_starts_at_the_next_full_hour_in_the_users_zone_and_lists_the_writable_calendars) {
    Env e;
    EventEditBridge b(e.config());
    b.beginNew(QString());
    PB_CHECK(b.loaded());
    PB_CHECK(b.isNew());
    PB_CHECK_EQ(b.blockReason().toStdString(), std::string(""));
    PB_CHECK_EQ(b.startDate().toStdString(), std::string("2026-10-14"));
    PB_CHECK_EQ(b.startTime().toStdString(), std::string("15:00"));      // now is 14:00 in Berlin
    PB_CHECK_EQ(b.endTime().toStdString(), std::string("16:00"));
    PB_CHECK_EQ(b.zoneName().toStdString(), std::string("Europe/Berlin"));
    PB_CHECK_EQ(b.calendars().size(), 1);                                   // only the writable calendar
    PB_CHECK_EQ(b.calendarId(), static_cast<qlonglong>(e.cal));
    b.beginNew(QStringLiteral("2026-12-24"));
    PB_CHECK_EQ(b.startDate().toStdString(), std::string("2026-12-24"));
}

PB_TEST(without_a_writable_calendar_or_a_zone_a_new_event_says_why_it_cannot_start) {
    Env e;
    e.store->debug_exec("UPDATE calendar SET writable = 0");
    EventEditBridge b(e.config());
    b.beginNew(QString());
    PB_CHECK_EQ(b.blockReason().toStdString(), std::string("no_calendar"));
    Env z;
    z.store->set_setting("tz_override", "");
    auto c = z.config();
    c.environment = [] { return pbo::ZoneEnvironment{"", ""}; };
    EventEditBridge nz(c);
    nz.beginNew(QString());
    PB_CHECK_EQ(nz.blockReason().toStdString(), std::string("zone_unresolved"));
}

PB_TEST(saving_a_new_event_writes_it_pending_create_and_reports_its_id) {
    Env e;
    EventEditBridge b(e.config());
    QSignalSpy saved(&b, &EventEditBridge::saved);
    b.beginNew(QStringLiteral("2026-10-20"));
    b.setTitle(QStringLiteral("Dentist"));
    b.setLocation(QStringLiteral("Main street"));
    b.setStartTime(QStringLiteral("9:30"));
    b.setEndTime(QStringLiteral("10:15"));
    PB_CHECK(b.save());
    PB_CHECK_EQ(saved.count(), 1);
    const qlonglong id = saved.at(0).at(0).toLongLong();
    PB_CHECK_EQ(e.q("SELECT sync_state || '|' || summary || '|' || location FROM event WHERE id = " + std::to_string(id)), std::string("pending_create|Dentist|Main street"));
    PB_CHECK_EQ(e.q("SELECT count(*) FROM occurrence WHERE event_id = " + std::to_string(id)), std::string("1"));
    PB_CHECK_EQ(e.q("SELECT local_date FROM occurrence WHERE event_id = " + std::to_string(id)), std::string("2026-10-20"));
    PB_CHECK_EQ(e.q("SELECT start_utc FROM occurrence WHERE event_id = " + std::to_string(id)), std::to_string(e.tz->local_to_utc("Europe/Berlin", {2026, 10, 20, 9, 30, 0})));
}

PB_TEST(validation_names_the_field_and_the_reason_and_writes_nothing) {
    Env e;
    EventEditBridge b(e.config());
    b.beginNew(QStringLiteral("2026-10-20"));
    PB_CHECK(!b.save());
    PB_CHECK_EQ(b.errorKey().toStdString(), std::string("title_missing"));
    PB_CHECK_EQ(b.errorField().toStdString(), std::string("title"));
    b.setTitle(QStringLiteral("x"));
    PB_CHECK(b.errorKey().isEmpty());                                       // typing clears the message
    b.setStartDate(QStringLiteral("2026-13-40"));
    PB_CHECK(!b.save());
    PB_CHECK_EQ(b.errorKey().toStdString(), std::string("date_invalid"));
    PB_CHECK_EQ(b.errorField().toStdString(), std::string("startDate"));
    b.setStartDate(QStringLiteral("2026-10-20"));
    b.setStartTime(QStringLiteral("25:00"));
    PB_CHECK(!b.save());
    PB_CHECK_EQ(b.errorKey().toStdString(), std::string("time_invalid"));
    PB_CHECK_EQ(b.errorField().toStdString(), std::string("startTime"));
    b.setStartTime(QStringLiteral("11:00"));
    b.setEndTime(QStringLiteral("10:00"));
    PB_CHECK(!b.save());
    PB_CHECK_EQ(b.errorKey().toStdString(), std::string("end_before_start"));
    PB_CHECK_EQ(b.errorField().toStdString(), std::string("endTime"));
    PB_CHECK_EQ(e.q("SELECT count(*) FROM event"), std::string("0"));
}

PB_TEST(an_all_day_event_has_an_inclusive_last_day_in_the_form_and_an_exclusive_end_in_the_store) {
    Env e;
    EventEditBridge b(e.config());
    b.beginNew(QStringLiteral("2026-12-24"));
    b.setTitle(QStringLiteral("Holiday"));
    b.setAllDay(true);
    b.setEndDate(QStringLiteral("2026-12-26"));
    PB_CHECK(b.save());
    PB_CHECK_EQ(e.q("SELECT count(*) FROM occurrence WHERE all_day = 1"), std::string("1"));
    PB_CHECK(e.q("SELECT ics FROM event").find("DTEND;VALUE=DATE:20261227") != std::string::npos);
    // and read back for editing: the last day is the 26th again
    const int64_t id = std::atoll(e.q("SELECT id FROM event").c_str());
    EventEditBridge r(e.config());
    r.begin(id);
    PB_CHECK(r.allDay());
    PB_CHECK_EQ(r.startDate().toStdString(), std::string("2026-12-24"));
    PB_CHECK_EQ(r.endDate().toStdString(), std::string("2026-12-26"));
    b.beginNew(QStringLiteral("2026-12-24"));
    b.setTitle(QStringLiteral("Backwards"));
    b.setAllDay(true);
    b.setEndDate(QStringLiteral("2026-12-20"));
    PB_CHECK(!b.save());
    PB_CHECK_EQ(b.errorKey().toStdString(), std::string("end_before_start"));
}

PB_TEST(an_existing_event_opens_with_its_values_and_a_change_marks_it_pending_update) {
    Env e;
    const int64_t id = e.pulled(e.cal, "p1@x", "Planning", "LOCATION:Room 1\r\n");
    EventEditBridge b(e.config());
    QSignalSpy saved(&b, &EventEditBridge::saved);
    b.begin(id);
    PB_CHECK(b.loaded());
    PB_CHECK(!b.isNew());
    PB_CHECK_EQ(b.title().toStdString(), std::string("Planning"));
    PB_CHECK_EQ(b.location().toStdString(), std::string("Room 1"));
    PB_CHECK_EQ(b.startDate().toStdString(), std::string("2026-10-15"));
    PB_CHECK_EQ(b.startTime().toStdString(), std::string("09:30"));
    PB_CHECK_EQ(b.endTime().toStdString(), std::string("10:30"));
    PB_CHECK(b.canSave());
    PB_CHECK(b.canDelete());
    b.setTitle(QStringLiteral("Planning 2"));
    PB_CHECK(b.save());
    PB_CHECK_EQ(saved.at(0).at(0).toLongLong(), static_cast<qlonglong>(id));
    PB_CHECK_EQ(e.q("SELECT sync_state || '|' || summary FROM event WHERE id = " + std::to_string(id)), std::string("pending_update|Planning 2"));
    PB_CHECK(e.q("SELECT ics FROM event WHERE id = " + std::to_string(id)).find("LOCATION:Room 1") != std::string::npos);   // untouched
}

PB_TEST(events_that_cannot_be_changed_open_read_only_with_the_reason_but_a_series_can_be_deleted) {
    Env e;
    const int64_t shared = e.pulled(e.ro, "s1@x", "Shared event");
    const int64_t series = e.pulled(e.cal, "s2@x", "Weekly", "RRULE:FREQ=WEEKLY\r\n");
    EventEditBridge b(e.config());
    b.begin(shared);
    PB_CHECK_EQ(b.blockReason().toStdString(), std::string("read_only"));
    PB_CHECK(!b.canSave());
    PB_CHECK(!b.canDelete());
    PB_CHECK(!b.save());
    b.begin(series);
    PB_CHECK_EQ(b.blockReason().toStdString(), std::string("series_read_only"));
    PB_CHECK(!b.canSave());
    PB_CHECK(b.canDelete());                                                  // the whole series, after the page asked
    QSignalSpy removed(&b, &EventEditBridge::removed);
    PB_CHECK(b.remove());
    PB_CHECK_EQ(removed.count(), 1);
    PB_CHECK_EQ(e.q("SELECT sync_state FROM event WHERE id = " + std::to_string(series)), std::string("pending_delete"));
    PB_CHECK_EQ(e.q("SELECT sync_state FROM event WHERE id = " + std::to_string(shared)), std::string("synced"));
}

PB_TEST(deleting_an_event_that_never_left_the_reader_removes_it_at_once) {
    Env e;
    EventEditBridge b(e.config());
    b.beginNew(QStringLiteral("2026-10-20"));
    b.setTitle(QStringLiteral("Oops"));
    PB_CHECK(b.save());
    const int64_t id = std::atoll(e.q("SELECT id FROM event").c_str());
    b.begin(id);
    PB_CHECK(b.remove());
    PB_CHECK_EQ(e.q("SELECT count(*) FROM event"), std::string("0"));
}

PB_TEST(the_conflicts_model_lists_both_versions_and_applies_the_three_choices) {
    Env e;
    const int64_t id = e.pulled(e.cal, "c1@x", "Original");
    EventEditBridge b(e.config());
    b.begin(id);
    b.setTitle(QStringLiteral("Mine"));
    PB_CHECK(b.save());
    const std::string theirs = "BEGIN:VCALENDAR\r\nVERSION:2.0\r\nPRODID:-//x//y//EN\r\nBEGIN:VEVENT\r\nUID:c1@x\r\nDTSTAMP:20260101T000000Z\r\nSUMMARY:Theirs\r\n"
                               "DTSTART;TZID=Europe/Berlin:20261015T110000\r\nDTEND;TZID=Europe/Berlin:20261015T120000\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n";
    PB_CHECK(e.store->event_conflicted(id, theirs, "\"e2\"", "/cal/c1@x.ics").ok());
    ConflictsModel::Config cc;
    cc.store = e.store.get();
    cc.tz = e.tz.get();
    cc.clock = &e.clock;
    cc.random = &e.random;
    ConflictsModel m(cc);
    m.refresh();
    PB_CHECK_EQ(m.total(), 1);
    const QVariantMap row = m.rows().at(0).toMap();
    PB_CHECK_EQ(row["state"].toString().toStdString(), std::string("conflict"));
    PB_CHECK_EQ(row["mineTitle"].toString().toStdString(), std::string("Mine"));
    PB_CHECK_EQ(row["serverTitle"].toString().toStdString(), std::string("Theirs"));
    PB_CHECK_EQ(row["serverWhen"].toString().toStdString(), std::string("2026-10-15 11:00"));
    PB_CHECK_EQ(row["calendarName"].toString().toStdString(), std::string("Work"));
    PB_CHECK(m.resolve(id, QStringLiteral("both")));
    PB_CHECK_EQ(m.total(), 0);
    PB_CHECK_EQ(e.q("SELECT count(*) FROM event"), std::string("2"));
    PB_CHECK(!m.resolve(id, QStringLiteral("nonsense")));
}

PB_TEST(an_event_deleted_on_the_server_can_be_re_created_or_discarded) {
    Env e;
    const int64_t id = e.pulled(e.cal, "g1@x", "Original");
    EventEditBridge b(e.config());
    b.begin(id);
    b.setTitle(QStringLiteral("Edited"));
    PB_CHECK(b.save());
    PB_CHECK(e.store->event_gone_on_server(id).ok());
    ConflictsModel::Config cc;
    cc.store = e.store.get();
    cc.tz = e.tz.get();
    cc.clock = &e.clock;
    cc.random = &e.random;
    ConflictsModel m(cc);
    m.refresh();
    PB_CHECK_EQ(m.total(), 1);
    PB_CHECK_EQ(m.rows().at(0).toMap()["state"].toString().toStdString(), std::string("server_deleted"));
    PB_CHECK(m.recreate(id));
    PB_CHECK_EQ(e.q("SELECT sync_state FROM event WHERE id = " + std::to_string(id)), std::string("pending_create"));
    PB_CHECK(e.store->event_gone_on_server(id).ok());
    m.refresh();
    PB_CHECK(m.discard(id));
    PB_CHECK_EQ(e.q("SELECT count(*) FROM event"), std::string("0"));
}

PB_TEST_MAIN()
