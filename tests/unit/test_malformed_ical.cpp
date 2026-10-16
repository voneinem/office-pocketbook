// T096 (SC-010): every malformed calendar file, mutations of the good ones and a few huge generated inputs go through parse_ical and
// expand without a crash or hang (run under PBO_SANITIZE for the memory checks). Results are bounded and valid UTF-8.
#include <dirent.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "mutate.h"
#include "pbo/charset.h"
#include "pbo/ical.h"
#include "test_support.h"

namespace {

std::vector<std::string> files_in(const std::string& relative, const std::string& ext) {
    std::vector<std::string> out;
    const std::string dir = std::string(PBO_FIXTURE_DIR) + "/" + relative;
    if (DIR* d = opendir(dir.c_str())) {
        while (dirent* e = readdir(d)) {
            const std::string n = e->d_name;
            if (n.size() > ext.size() && n.compare(n.size() - ext.size(), ext.size(), ext) == 0) out.push_back(dir + "/" + n);
        }
        closedir(d);
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::string slurp(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::unique_ptr<pbo::TzDatabase> db() {
    auto d = pbo::TzDatabase::open(std::string(PBO_SOURCE_DIR) + "/third_party/tzdata/zoneinfo");
    if (!d) PB_FAIL_AT("the bundled zone data cannot be opened");
    return d;
}

void check(const pbo::TzDatabase& tz, const std::string& data, const std::string& what) {
    const auto start = std::chrono::steady_clock::now();
    const pbo::ParsedCalendar p = pbo::parse_ical(data, tz);
    if (p.events.size() > 2000) PB_FAIL_AT(what + ": too many events");
    for (const auto& e : p.events) {
        for (const std::string* s : {&e.uid, &e.summary, &e.description, &e.location, &e.organizer})
            if (!pbo::is_valid_utf8(*s)) PB_FAIL_AT(what + ": text is not valid UTF-8");
        if (e.summary.size() > (64u << 10) || e.description.size() > (64u << 10) || e.location.size() > (64u << 10)) PB_FAIL_AT(what + ": text over the cap");
        if (e.attendees.size() > 200) PB_FAIL_AT(what + ": too many attendees");
        if (e.end && e.start && e.start_utc && e.end_utc && *e.end_utc < *e.start_utc) PB_FAIL_AT(what + ": end before start");
    }
    const auto occ = pbo::expand(p, tz, "Europe/Berlin", pbo::civil::naive_epoch({2020, 1, 1, 0, 0, 0}), pbo::civil::naive_epoch({2030, 1, 1, 0, 0, 0}));
    if (occ.size() > 5000u * 2000u) PB_FAIL_AT(what + ": unbounded occurrences");
    for (const auto& o : occ)
        if (o.end_utc < o.start_utc) PB_FAIL_AT(what + ": an occurrence ends before it starts");
    const auto secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    if (secs > 20.0) PB_FAIL_AT(what + ": took " + std::to_string(secs) + " s");
}

}  // namespace

PB_TEST(there_are_at_least_20_malformed_calendar_files) {
    PB_CHECK(files_in("malformed/ical", ".ics").size() >= 20);
}

PB_TEST(every_malformed_file_and_golden_calendar_is_handled) {
    auto tz = db();
    for (const std::string& f : files_in("malformed/ical", ".ics")) check(*tz, slurp(f), f);
}

PB_TEST(truncation_and_mutation_of_good_calendars_never_crash) {
    auto tz = db();
    const std::string good =
        "BEGIN:VCALENDAR\r\nVERSION:2.0\r\nBEGIN:VEVENT\r\nUID:g1@example.org\r\nSUMMARY:Standup\r\nDTSTART;TZID=Europe/Berlin:20261014T093000\r\n"
        "DTEND;TZID=Europe/Berlin:20261014T100000\r\nRRULE:FREQ=WEEKLY;BYDAY=MO,WE;UNTIL=20271231T000000Z\r\nEXDATE;TZID=Europe/Berlin:20261019T093000\r\n"
        "ATTENDEE;CN=\"Doe, Jane\":mailto:jane@example.org\r\nBEGIN:VALARM\r\nTRIGGER:-PT15M\r\nEND:VALARM\r\nEND:VEVENT\r\n"
        "BEGIN:VEVENT\r\nUID:g1@example.org\r\nRECURRENCE-ID;TZID=Europe/Berlin:20261021T093000\r\nDTSTART;TZID=Europe/Berlin:20261021T110000\r\nDTEND;TZID=Europe/Berlin:20261021T113000\r\nEND:VEVENT\r\n"
        "END:VCALENDAR\r\n";
    for (size_t at = 0; at <= good.size(); at += 3) check(*tz, pbtest::truncated(good, at), "truncated at " + std::to_string(at));
    for (uint64_t seed = 1; seed <= 400; ++seed) {
        check(*tz, pbtest::flipped(good, seed, 5), "flipped " + std::to_string(seed));
        check(*tz, pbtest::with_insertions(good, seed, 4), "inserted " + std::to_string(seed));
    }
}

PB_TEST(huge_generated_inputs_are_bounded) {
    auto tz = db();
    // a single line of 1 MiB + 1: dropped
    check(*tz, "BEGIN:VCALENDAR\r\nBEGIN:VEVENT\r\nUID:a\r\nSUMMARY:" + std::string((1u << 20) + 1, 'A') + "\r\nDTSTART:20261014T093000Z\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n", "1 MiB line");
    // many folded continuation lines forming one huge logical line
    std::string folded = "BEGIN:VCALENDAR\r\nBEGIN:VEVENT\r\nUID:a\r\nDESCRIPTION:x\r\n";
    for (int i = 0; i < 100000; ++i) folded += " yyyyyyyyyy\r\n";
    folded += "DTSTART:20261014T093000Z\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n";
    check(*tz, folded, "folded 1.1 MB");
    // 200,001 lines
    std::string lines = "BEGIN:VCALENDAR\r\nBEGIN:VEVENT\r\nUID:a\r\n";
    for (int i = 0; i < 200001; ++i) lines += "X-A:b\r\n";
    check(*tz, lines, "200,001 lines");
    // an input above 8 MiB
    check(*tz, std::string((8u << 20) + 1, 'x'), "8 MiB + 1");
    // 2,001 events
    std::string events = "BEGIN:VCALENDAR\r\n";
    for (int i = 0; i < 2001; ++i) events += "BEGIN:VEVENT\r\nUID:e" + std::to_string(i) + "\r\nDTSTART:20261014T093000Z\r\nEND:VEVENT\r\n";
    check(*tz, events, "2001 events");
    // a daily series from the year 1 to 9999 expands only the window
    check(*tz, "BEGIN:VCALENDAR\r\nBEGIN:VEVENT\r\nUID:a\r\nDTSTART:00010101T000000Z\r\nRRULE:FREQ=DAILY\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n", "since year 1");
}

PB_TEST_MAIN()
