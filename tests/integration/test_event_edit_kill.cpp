// T146 (SC-006, Principle III): the app is killed (SIGKILL, like a battery pull) at seeded random moments while events are created, edited,
// pushed and deleted. After every kill the store opens, passes its integrity check, holds every event the child reported as saved (once, whole,
// never a mixture of two versions), and an event always has its occurrence. The device battery pull itself is a device item (T137/T186).
#include <map>
#include <set>
#include <sstream>

#include "kill_harness.h"
#include "test_support.h"
#include "tmp_dir.h"
#include "pbo/store.h"

using pbtest::KillRun;

namespace {

struct FixedClock : pbo::Clock {
    int64_t now() const override { return 1791979200; }
};

pbo::EventUpsert rec(const std::string& uid, const std::string& summary) {
    pbo::EventUpsert e;
    e.uid = uid;
    e.ics = "BEGIN:VCALENDAR\r\nBEGIN:VEVENT\r\nUID:" + uid + "\r\nSUMMARY:" + summary + "\r\n" + std::string(2000, 'x').insert(0, "DESCRIPTION:") + "\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n";
    e.summary = summary;
    e.start_utc = 1791979200 + 3600;
    e.end_utc = *e.start_utc + 3600;
    e.start_local = "2026-10-14T14:00:00";
    pbo::OccurrenceRow o;
    o.start_utc = *e.start_utc;
    o.end_utc = *e.end_utc;
    o.local_date = "2026-10-14";
    e.occurrences.push_back(o);
    return e;
}

}  // namespace

PB_TEST(killing_the_app_while_events_are_edited_pushed_and_deleted_never_loses_or_splits_an_event) {
    const int n = KillRun::iterations(100);
    for (int i = 0; i < n; ++i) {
        pbtest::TmpDir dir;
        FixedClock clock;
        const std::string path = dir.path();
        const auto reports = KillRun::run(
            [&](int fd) {
                auto opened = pbo::Store::open(path, clock);
                pbo::Store& s = *opened.value;
                pbo::Calendar c;
                c.kind = "caldav";
                c.href = "/dav/work/";
                c.name = "Work";
                c.writable = true;
                const int64_t cal = s.upsert_calendar(c).value;
                KillRun::report(fd, "calendar 0");
                for (int k = 1;; ++k) {
                    const std::string uid = "k" + std::to_string(k) + "@x";
                    const auto made = s.save_event_local(cal, 0, rec(uid, "v0"));
                    if (!made.ok()) continue;
                    KillRun::report(fd, "created " + std::to_string(k) + " 0");
                    for (int v = 1; v <= 3; ++v) {
                        if (s.save_event_local(cal, made.value, rec(uid, "v" + std::to_string(v))).ok()) KillRun::report(fd, "edited " + std::to_string(k) + " " + std::to_string(v));
                    }
                    const std::string text = s.event(made.value)->ics;
                    if (s.event_pushed(made.value, text, "/dav/work/" + uid + ".ics", "\"e" + std::to_string(k) + "\"").ok()) KillRun::report(fd, "pushed " + std::to_string(k) + " 3");
                    if (k % 2 == 0) {
                        if (s.request_event_delete(made.value).ok()) KillRun::report(fd, "deleting " + std::to_string(k) + " 3");
                        if (s.event_removed_on_server(made.value).ok()) KillRun::report(fd, "removed " + std::to_string(k) + " 3");
                    }
                }
            },
            1, KillRun::delay_for(i, 60000));
        auto reopened = pbo::Store::open(path, clock);
        if (!reopened.ok()) PB_FAIL_AT("iteration " + std::to_string(i) + ": the store did not open after the kill: " + reopened.error.key + " / " + reopened.error.detail);
        pbo::Store& s = *reopened.value;
        PB_CHECK(s.check_integrity().ok());
        std::map<int, std::pair<std::string, int>> last;   // k -> last reported step and version
        for (const std::string& r : reports) {
            if (r.rfind("calendar", 0) == 0) continue;
            std::istringstream is(r);
            std::string step;
            int k = 0, v = 0;
            is >> step >> k >> v;
            last[k] = {step, v};
        }
        for (const auto& kv : last) {
            const std::string uid = "k" + std::to_string(kv.first) + "@x";
            const int rows = std::atoi(s.debug_query("SELECT count(*) FROM event WHERE uid = '" + uid + "'").c_str());
            PB_CHECK(rows <= 1);                                                   // never twice
            const std::string step = kv.second.first;
            if (step == "removed") {
                PB_CHECK_EQ(rows, 0);
                continue;
            }
            if (step == "deleting") {
                if (rows == 1) PB_CHECK_EQ(s.debug_query("SELECT sync_state FROM event WHERE uid = '" + uid + "'"), std::string("pending_delete"));
                continue;
            }
            // an event that is deleted after it was pushed (even k) may be on its way out: the next steps can be committed although their
            // lines were not written yet
            const bool leaving = kv.first % 2 == 0 && step == "pushed";
            if (leaving && rows == 0) continue;
            PB_CHECK_EQ(rows, 1);                                                  // reported as saved: it is there
            if (leaving && s.debug_query("SELECT sync_state FROM event WHERE uid = '" + uid + "'") == "pending_delete") continue;
            const std::string summary = s.debug_query("SELECT summary FROM event WHERE uid = '" + uid + "'");
            PB_CHECK(summary.size() == 2 && summary[0] == 'v');                    // a whole version
            const int v = summary[1] - '0';
            PB_CHECK(v >= kv.second.second && v <= kv.second.second + 1);          // none that was reported is lost, at most one more
            const std::string ics = s.debug_query("SELECT ics FROM event WHERE uid = '" + uid + "'");
            PB_CHECK(ics.find("SUMMARY:" + summary + "\r\n") != std::string::npos);   // text and fields agree
            PB_CHECK_EQ(s.debug_query("SELECT count(*) FROM occurrence o JOIN event e ON e.id = o.event_id WHERE e.uid = '" + uid + "'"), std::string("1"));
        }
        // no half-written event anywhere
        PB_CHECK_EQ(s.debug_query("SELECT count(*) FROM event WHERE sync_state NOT IN ('pending_create','pending_update','synced','pending_delete')"), std::string("0"));
        PB_CHECK_EQ(s.debug_query("SELECT count(*) FROM event e WHERE e.sync_state <> 'pending_delete' AND NOT EXISTS (SELECT 1 FROM occurrence o WHERE o.event_id = e.id)"), std::string("0"));
    }
}

PB_TEST_MAIN()
