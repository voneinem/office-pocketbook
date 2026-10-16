#pragma once
// Shared setup of the event-editing tests (US4): a fake CalDAV server, a store with the user's zone, and the pull / push / edit calls.
// Everything runs against the fake server: UNVERIFIED against real servers.
#include <string>

#include "fake_caldav_server.h"
#include "pbo/calendar_sync.h"
#include "pbo/curl_transport.h"
#include "pbo/event_edit.h"
#include "test_support.h"
#include "tmp_dir.h"

namespace pbtest {

constexpr int64_t kEventNow = 1791979200;   // 2026-10-14 12:00:00 UTC

struct EventClock : pbo::Clock {
    int64_t now() const override { return kEventNow; }
};
struct EventNoCancel : pbo::Cancel {
    bool requested() const override { return false; }
};
struct EventCounterRandom : pbo::Random {
    uint8_t n = 1;
    void bytes(uint8_t* out, size_t len) override {
        for (size_t i = 0; i < len; ++i) out[i] = n++;
    }
};

inline std::string event_ics(const std::string& uid, const std::string& summary, const std::string& start, const std::string& end, const std::string& extra = "",
                             const std::string& tz = "Europe/Berlin") {
    return "BEGIN:VCALENDAR\r\nVERSION:2.0\r\nPRODID:-//example.org//test//EN\r\nBEGIN:VEVENT\r\nUID:" + uid + "@example.org\r\nDTSTAMP:20260101T000000Z\r\nSUMMARY:" + summary +
           "\r\nDTSTART;TZID=" + tz + ":" + start + "\r\nDTEND;TZID=" + tz + ":" + end + "\r\n" + extra + "END:VEVENT\r\nEND:VCALENDAR\r\n";
}

struct EventEnv {
    TmpDir data, pki_dir;
    TestPki pki{pki_dir};
    TestPki::Leaf leaf;
    EventClock clock;
    EventNoCancel cancel;
    EventCounterRandom random;
    std::unique_ptr<FakeCalDavServer> server;
    std::unique_ptr<pbo::Store> store;
    std::unique_ptr<pbo::TzDatabase> tz;
    std::unique_ptr<pbo::CurlTransport> http;
    int64_t work = 0;   // the writable calendar "Work"

    explicit EventEnv(bool with_read_only = false) {
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
        FakeCalDavServer::CalendarOptions o;
        o.name = "Work";
        server->add_calendar("work", o);
        if (with_read_only) {
            FakeCalDavServer::CalendarOptions ro;
            ro.name = "Shared";
            ro.writable = false;
            server->add_calendar("shared", ro);
        }
        pbo::CalendarAccount a;
        a.caldav_url = server->origin();
        a.username = "alice";
        PB_CHECK(store->save_calendar_account(a).ok());
        PB_CHECK(pull().ok());
        for (const pbo::Calendar& cal : store->calendars())
            if (cal.name == "Work") work = cal.id;
        PB_CHECK(work != 0);
    }
    pbo::CalendarSyncDeps sync_deps() { return pbo::CalendarSyncDeps{*store, *http, *tz, clock, cancel}; }
    pbo::Error pull(pbo::CalendarCounts* counts = nullptr) {
        auto d = sync_deps();
        return pbo::pull_calendars(d, "pw-for-tests", counts);
    }
    pbo::Error push(pbo::PushCounts* counts = nullptr) {
        auto d = sync_deps();
        return pbo::push_events(d, "pw-for-tests", counts);
    }
    pbo::EventEditDeps edit_deps() { return pbo::EventEditDeps{*store, *tz, clock, random}; }
    std::string q(const std::string& sql) { return store->debug_query(sql); }
    int count(const std::string& sql) { return std::atoi(q(sql).c_str()); }
    int64_t id_of(const std::string& uid_like) { return std::atoll(q("SELECT id FROM event WHERE uid LIKE '" + uid_like + "'").c_str()); }
    std::string state_of(int64_t id) { return q("SELECT sync_state FROM event WHERE id = " + std::to_string(id)); }

    // a synced event from the server, as the user sees it after a pull
    int64_t server_event(const std::string& name, const std::string& uid, const std::string& summary, const std::string& start = "20261015T093000",
                         const std::string& end = "20261015T100000") {
        server->put_event("work", name, event_ics(uid, summary, start, end), kEventNow, kEventNow + 86400);
        PB_CHECK(pull().ok());
        return id_of(uid + "@example.org");
    }
    static pbo::EventEdit edit(const std::string& title, int hour = 9, const std::string& tzid = "Europe/Berlin") {
        pbo::EventEdit e;
        e.title = title;
        e.tzid = tzid;
        e.start = {2026, 10, 16, hour, 0, 0};
        e.end = {2026, 10, 16, hour + 1, 0, 0};
        return e;
    }
};

}  // namespace pbtest
