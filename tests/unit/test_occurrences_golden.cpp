// T095 (part): >= 30 recurring and time-zone cases with golden expected local dates and times. The expectations in
// tests/fixtures/ical/golden_occurrences.txt come from an independent implementation (python-dateutil + zoneinfo, see
// scripts/gen-golden-occurrences.py), including DST crossings, EXDATE/RDATE/overrides and invalid dates dropped.
#include <algorithm>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "pbo/ical.h"
#include "test_support.h"

namespace {

struct Case {
    std::string name, user, from, to, ics;
    std::vector<std::string> expect;
};

std::vector<Case> load() {
    std::vector<Case> cases;
    std::ifstream f(std::string(PBO_FIXTURE_DIR) + "/ical/golden_occurrences.txt");
    std::string line, section;
    Case cur;
    bool open = false;
    while (std::getline(f, line)) {
        if (line.rfind("### ", 0) == 0) { cur = Case(); cur.name = line.substr(4); open = true; section = "head"; continue; }
        if (!open) continue;
        if (line == "--- ics") { section = "ics"; continue; }
        if (line == "--- expect") { section = "expect"; continue; }
        if (line == "--- end") { cases.push_back(cur); open = false; continue; }
        if (section == "head") {
            if (line.rfind("user: ", 0) == 0) cur.user = line.substr(6);
            if (line.rfind("window: ", 0) == 0) { std::istringstream w(line.substr(8)); w >> cur.from >> cur.to; }
        } else if (section == "ics") {
            cur.ics += line + "\r\n";
        } else if (!line.empty()) {
            cur.expect.push_back(line);
        }
    }
    return cases;
}

int64_t parse_day(const std::string& ymd) {
    return pbo::civil::naive_epoch({std::stoi(ymd.substr(0, 4)), std::stoi(ymd.substr(5, 2)), std::stoi(ymd.substr(8, 2)), 0, 0, 0});
}

std::string minutes(const pbo::LocalDateTime& t) {
    char b[32];
    snprintf(b, sizeof b, "%04d-%02d-%02dT%02d:%02d", t.year, t.month, t.day, t.hour, t.minute);
    return b;
}

}  // namespace

PB_TEST(every_golden_case_gives_exactly_the_expected_occurrences) {
    auto db = pbo::TzDatabase::open(std::string(PBO_SOURCE_DIR) + "/third_party/tzdata/zoneinfo");
    PB_CHECK(db != nullptr);
    const auto cases = load();
    PB_CHECK(cases.size() >= 30);
    for (const Case& c : cases) {
        const auto parsed = pbo::parse_ical(c.ics, *db);
        if (!parsed.ok || parsed.events.empty()) PB_FAIL_AT(c.name + ": the calendar did not parse");
        const auto occ = pbo::expand(parsed, *db, c.user, parse_day(c.from), parse_day(c.to));
        std::vector<std::string> got;
        for (const auto& o : occ) {
            if (o.all_day) {
                // all-day: the date and the exclusive end date in the user's zone
                const pbo::LocalDateTime s = db->utc_to_local(c.user, o.start_utc), en = db->utc_to_local(c.user, o.end_utc);
                char b[64];
                snprintf(b, sizeof b, "D%04d-%02d-%02d|%04d-%02d-%02d", s.year, s.month, s.day, en.year, en.month, en.day);
                got.push_back(b);
            } else {
                got.push_back(minutes(db->utc_to_local(c.user, o.start_utc)) + "|" + minutes(db->utc_to_local(c.user, o.end_utc)));
            }
        }
        std::sort(got.begin(), got.end());
        std::vector<std::string> want = c.expect;
        std::sort(want.begin(), want.end());
        if (got != want) {
            std::string msg = c.name + ": expected " + std::to_string(want.size()) + " occurrences, got " + std::to_string(got.size());
            for (size_t i = 0; i < std::max(got.size(), want.size()); ++i) {
                const std::string g = i < got.size() ? got[i] : "-", w = i < want.size() ? want[i] : "-";
                if (g != w) { msg += "; first difference at " + std::to_string(i) + ": got " + g + " want " + w; break; }
            }
            PB_FAIL_AT(msg);
        }
    }
}

PB_TEST(local_dates_are_the_dates_in_the_users_zone) {
    auto db = pbo::TzDatabase::open(std::string(PBO_SOURCE_DIR) + "/third_party/tzdata/zoneinfo");
    PB_CHECK(db != nullptr);
    // 23:30 in Berlin is already the next day in Tokyo; an all-day event stays on its own date
    const std::string ics = "BEGIN:VCALENDAR\r\nBEGIN:VEVENT\r\nUID:a\r\nDTSTART;TZID=Europe/Berlin:20261014T233000\r\nDTEND;TZID=Europe/Berlin:20261015T003000\r\nEND:VEVENT\r\n"
                            "BEGIN:VEVENT\r\nUID:b\r\nDTSTART;VALUE=DATE:20261014\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n";
    // the two events are different resources: parse them apart
    const auto first = pbo::parse_ical(ics.substr(0, ics.find("BEGIN:VEVENT\r\nUID:b")) + "END:VCALENDAR\r\n", *db);
    const auto tokyo = pbo::expand(first, *db, "Asia/Tokyo", parse_day("2026-10-01"), parse_day("2026-11-01"));
    PB_CHECK_EQ(tokyo.size(), size_t(1));
    PB_CHECK_EQ(tokyo[0].local_date, std::string("2026-10-15"));
    const auto berlin = pbo::expand(first, *db, "Europe/Berlin", parse_day("2026-10-01"), parse_day("2026-11-01"));
    PB_CHECK_EQ(berlin[0].local_date, std::string("2026-10-14"));
    const auto allday = pbo::parse_ical("BEGIN:VCALENDAR\r\nBEGIN:VEVENT\r\nUID:b\r\nDTSTART;VALUE=DATE:20261014\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n", *db);
    for (const char* z : {"Asia/Tokyo", "America/New_York", "Pacific/Auckland"}) {
        const auto o = pbo::expand(allday, *db, z, parse_day("2026-10-01"), parse_day("2026-11-01"));
        PB_CHECK_EQ(o.size(), size_t(1));
        PB_CHECK_EQ(o[0].local_date, std::string("2026-10-14"));
        PB_CHECK(o[0].all_day);
    }
}

PB_TEST(an_unresolved_or_unknown_user_zone_gives_no_occurrences_instead_of_an_assumed_zone) {
    auto db = pbo::TzDatabase::open(std::string(PBO_SOURCE_DIR) + "/third_party/tzdata/zoneinfo");
    const auto p = pbo::parse_ical("BEGIN:VCALENDAR\r\nBEGIN:VEVENT\r\nUID:a\r\nDTSTART:20261014T100000Z\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n", *db);
    PB_CHECK(pbo::expand(p, *db, "", 0, int64_t(1) << 40).empty());
    PB_CHECK(pbo::expand(p, *db, "Mars/Olympus", 0, int64_t(1) << 40).empty());
}

PB_TEST(an_unsupported_rule_gives_the_single_occurrence_at_dtstart) {
    auto db = pbo::TzDatabase::open(std::string(PBO_SOURCE_DIR) + "/third_party/tzdata/zoneinfo");
    const auto p = pbo::parse_ical("BEGIN:VCALENDAR\r\nBEGIN:VEVENT\r\nUID:a\r\nDTSTART;TZID=Europe/Berlin:20261014T100000\r\nDTEND;TZID=Europe/Berlin:20261014T110000\r\n"
                                   "RRULE:FREQ=YEARLY;BYWEEKNO=20\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n", *db);
    const auto o = pbo::expand(p, *db, "Europe/Berlin", 0, int64_t(1) << 40);
    PB_CHECK_EQ(o.size(), size_t(1));
    PB_CHECK_EQ(o[0].local_date, std::string("2026-10-14"));
}

PB_TEST(at_most_5000_occurrences_per_series_and_a_window_outside_the_series_is_empty) {
    auto db = pbo::TzDatabase::open(std::string(PBO_SOURCE_DIR) + "/third_party/tzdata/zoneinfo");
    const auto p = pbo::parse_ical("BEGIN:VCALENDAR\r\nBEGIN:VEVENT\r\nUID:a\r\nDTSTART;TZID=Europe/Berlin:20260101T100000\r\nDTEND;TZID=Europe/Berlin:20260101T110000\r\n"
                                   "RRULE:FREQ=DAILY\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n", *db);
    PB_CHECK_EQ(pbo::expand(p, *db, "Europe/Berlin", 0, int64_t(1) << 40).size(), size_t(5000));
    PB_CHECK(pbo::expand(p, *db, "Europe/Berlin", 0, pbo::civil::naive_epoch({2025, 12, 31, 0, 0, 0})).empty());
}

PB_TEST_MAIN()
