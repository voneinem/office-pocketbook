// T140: writing events (contracts/caldav-ical-subset.md section 5): new_event_ics, and the lossless apply_edit.
#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "pbo/ical.h"
#include "test_support.h"

namespace {

std::unique_ptr<pbo::TzDatabase> db() {
    auto d = pbo::TzDatabase::open(std::string(PBO_SOURCE_DIR) + "/third_party/tzdata/zoneinfo");
    if (!d) PB_FAIL_AT("the bundled zone data cannot be opened");
    return d;
}

constexpr int64_t kNow = 1792000000;   // 2026-10-14T...Z

// the logical content lines (unfolded), line endings of any kind
std::vector<std::string> logical(const std::string& ics) {
    std::vector<std::string> out;
    std::string cur;
    bool have = false;
    size_t i = 0;
    while (i < ics.size()) {
        size_t e = i;
        while (e < ics.size() && ics[e] != '\n' && ics[e] != '\r') ++e;
        const std::string line = ics.substr(i, e - i);
        if (!line.empty() && (line[0] == ' ' || line[0] == '\t') && have) cur += line.substr(1);
        else {
            if (have) out.push_back(cur);
            cur = line;
            have = true;
        }
        if (e < ics.size() && ics[e] == '\r' && e + 1 < ics.size() && ics[e + 1] == '\n') e += 2;
        else if (e < ics.size()) ++e;
        i = e;
    }
    if (have && !cur.empty()) out.push_back(cur);
    return out;
}

std::multiset<std::string> as_set(const std::string& ics) {
    const auto l = logical(ics);
    return std::multiset<std::string>(l.begin(), l.end());
}

// lines in `a` but not in `b` (multiset difference)
std::vector<std::string> minus(const std::multiset<std::string>& a, const std::multiset<std::string>& b) {
    std::multiset<std::string> rest = a;
    for (const auto& x : b) {
        auto it = rest.find(x);
        if (it != rest.end()) rest.erase(it);
    }
    return std::vector<std::string>(rest.begin(), rest.end());
}

bool starts_with(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }

pbo::EventEdit timed(const std::string& title) {
    pbo::EventEdit e;
    e.title = title;
    e.tzid = "Europe/Berlin";
    e.start = {2026, 10, 20, 9, 30, 0};
    e.end = {2026, 10, 20, 10, 30, 0};
    return e;
}

// ---- styles of other clients (unverified: written from the format, not captured) ----
const char* kNextcloudStyle =
    "BEGIN:VCALENDAR\r\nVERSION:2.0\r\nPRODID:-//Nextcloud calendar v5.0.0\r\nCALSCALE:GREGORIAN\r\n"
    "BEGIN:VEVENT\r\nCREATED:20260101T100000Z\r\nDTSTAMP:20260101T100000Z\r\nLAST-MODIFIED:20260101T100000Z\r\nSEQUENCE:2\r\n"
    "UID:nc-1@example.org\r\nDTSTART;TZID=Europe/Berlin:20261020T090000\r\nDTEND;TZID=Europe/Berlin:20261020T100000\r\n"
    "SUMMARY:Planning\r\nLOCATION:Room 1\r\nDESCRIPTION:Bring the notes\r\nATTENDEE;CN=Bob;PARTSTAT=ACCEPTED:mailto:bob@example.org\r\n"
    "ORGANIZER:mailto:alice@example.org\r\nX-CUSTOM-FLAG:keep-me\r\n"
    "BEGIN:VALARM\r\nACTION:DISPLAY\r\nTRIGGER:-PT15M\r\nDESCRIPTION:Alarm text\r\nSUMMARY:Alarm summary\r\nEND:VALARM\r\n"
    "END:VEVENT\r\nEND:VCALENDAR\r\n";

const char* kOutlookStyle =
    "BEGIN:VCALENDAR\r\nMETHOD:PUBLISH\r\nPRODID:Microsoft Exchange Server 2010\r\nVERSION:2.0\r\n"
    "BEGIN:VTIMEZONE\r\nTZID:W. Europe Standard Time\r\nBEGIN:STANDARD\r\nDTSTART:16010101T030000\r\nTZOFFSETFROM:+0200\r\nTZOFFSETTO:+0100\r\n"
    "RRULE:FREQ=YEARLY;INTERVAL=1;BYDAY=-1SU;BYMONTH=10\r\nEND:STANDARD\r\n"
    "BEGIN:DAYLIGHT\r\nDTSTART:16010101T020000\r\nTZOFFSETFROM:+0100\r\nTZOFFSETTO:+0200\r\nRRULE:FREQ=YEARLY;INTERVAL=1;BYDAY=-1SU;BYMONTH=3\r\n"
    "END:DAYLIGHT\r\nEND:VTIMEZONE\r\n"
    "BEGIN:VEVENT\r\nUID:040000008200E00074C5B7101A82E008@example.org\r\nSUMMARY:Review\r\nDTSTART;TZID=W. Europe Standard Time:20261021T140000\r\n"
    "DTEND;TZID=W. Europe Standard Time:20261021T150000\r\nCLASS:PUBLIC\r\nPRIORITY:5\r\nDTSTAMP:20260102T080000Z\r\nTRANSP:OPAQUE\r\nSTATUS:CONFIRMED\r\n"
    "SEQUENCE:0\r\nX-MICROSOFT-CDO-BUSYSTATUS:BUSY\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n";

const char* kAppleStyle =   // LF line endings, folded long property, an all-day event with DURATION
    "BEGIN:VCALENDAR\nVERSION:2.0\nPRODID:-//Apple Inc.//macOS 15//EN\nBEGIN:VEVENT\nCREATED:20260105T120000Z\nUID:AP-1\nDTSTAMP:20260105T120000Z\n"
    "DTSTART;VALUE=DATE:20261022\nDURATION:P2D\nSUMMARY:Conference\nDESCRIPTION:A very long description that is folded by the other client in the\n"
    "  middle of the text to see that unfolding and refolding keep the words\nX-APPLE-TRAVEL-ADVISORY-BEHAVIOR:AUTOMATIC\nEND:VEVENT\nEND:VCALENDAR\n";

const char* kSeries =
    "BEGIN:VCALENDAR\r\nVERSION:2.0\r\nPRODID:-//x//y//EN\r\nBEGIN:VEVENT\r\nUID:s1@example.org\r\nDTSTAMP:20260101T000000Z\r\n"
    "DTSTART;TZID=Europe/Berlin:20261019T090000\r\nDTEND;TZID=Europe/Berlin:20261019T100000\r\nRRULE:FREQ=WEEKLY;BYDAY=MO\r\nSUMMARY:Weekly\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n";

pbo::EventEdit edit_of(const char* ics) {
    auto d = db();
    const auto p = pbo::parse_ical(ics, *d);
    if (p.events.empty()) PB_FAIL_AT("fixture has no event");
    return pbo::event_edit_of(p.events[0]);
}

}  // namespace

PB_TEST(a_new_timed_event_is_written_in_the_users_zone_and_read_back) {
    auto d = db();
    pbo::EventEdit e = timed("Dentist");
    e.location = "Main street 5";
    e.description = "Bring the card";
    const std::string ics = pbo::new_event_ics(e, "abc123@pboffice.invalid", kNow);
    PB_CHECK(ics.find("PRODID:-//office-pocketbook//EN") != std::string::npos);
    PB_CHECK(ics.find("VERSION:2.0") != std::string::npos);
    PB_CHECK(ics.find("UID:abc123@pboffice.invalid") != std::string::npos);
    PB_CHECK(ics.find("DTSTART;TZID=Europe/Berlin:20261020T093000") != std::string::npos);
    PB_CHECK(ics.find("DTEND;TZID=Europe/Berlin:20261020T103000") != std::string::npos);
    PB_CHECK(ics.find("DTSTAMP:") != std::string::npos);
    const auto p = pbo::parse_ical(ics, *d);
    PB_CHECK(p.ok);
    PB_CHECK_EQ(p.events.size(), size_t(1));
    PB_CHECK_EQ(p.events[0].summary, std::string("Dentist"));
    PB_CHECK_EQ(p.events[0].location, std::string("Main street 5"));
    PB_CHECK_EQ(p.events[0].description, std::string("Bring the card"));
    PB_CHECK_EQ(p.events[0].tzid, std::string("Europe/Berlin"));
    PB_CHECK(*p.events[0].start == e.start && *p.events[0].end == e.end);
}

PB_TEST(all_day_utc_and_floating_events_use_their_own_forms) {
    auto d = db();
    pbo::EventEdit day;
    day.title = "Holiday";
    day.all_day = true;
    day.start = {2026, 12, 24, 0, 0, 0};
    day.end = {2026, 12, 27, 0, 0, 0};   // exclusive
    std::string ics = pbo::new_event_ics(day, "d1@pboffice.invalid", kNow);
    PB_CHECK(ics.find("DTSTART;VALUE=DATE:20261224") != std::string::npos);
    PB_CHECK(ics.find("DTEND;VALUE=DATE:20261227") != std::string::npos);
    auto p = pbo::parse_ical(ics, *d);
    PB_CHECK(p.events[0].all_day);
    PB_CHECK((*p.events[0].end == pbo::LocalDateTime{2026, 12, 27, 0, 0, 0}));

    pbo::EventEdit utc = timed("Call");
    utc.tzid = "UTC";
    ics = pbo::new_event_ics(utc, "u1@pboffice.invalid", kNow);
    PB_CHECK(ics.find("DTSTART:20261020T093000Z") != std::string::npos);
    PB_CHECK_EQ(pbo::parse_ical(ics, *d).events[0].tzid, std::string("UTC"));

    pbo::EventEdit floating = timed("Walk");
    floating.tzid = "";
    ics = pbo::new_event_ics(floating, "f1@pboffice.invalid", kNow);
    PB_CHECK(ics.find("DTSTART:20261020T093000\r\n") != std::string::npos);
    PB_CHECK(pbo::parse_ical(ics, *d).events[0].floating);
}

PB_TEST(text_is_escaped_and_lines_are_folded_at_75_octets_without_cutting_characters) {
    auto d = db();
    pbo::EventEdit e = timed("a, b; c\\ d");
    e.description = "line one\nline two with ümläuts and 日本語 " + std::string(300, 'x') + " end";
    const std::string ics = pbo::new_event_ics(e, "t1@pboffice.invalid", kNow);
    PB_CHECK(ics.find("SUMMARY:a\\, b\\; c\\\\ d") != std::string::npos);
    size_t i = 0;
    while (i < ics.size()) {
        const size_t e2 = ics.find("\r\n", i);
        PB_CHECK(e2 != std::string::npos);                  // CRLF everywhere
        PB_CHECK(e2 - i <= 75);                             // no physical line over 75 octets
        i = e2 + 2;
    }
    const auto p = pbo::parse_ical(ics, *d);
    PB_CHECK_EQ(p.events[0].summary, e.title);
    PB_CHECK_EQ(p.events[0].description, e.description);    // folding and escaping are lossless
}

PB_TEST(validation_names_what_is_wrong) {
    auto d = db();
    pbo::EventEdit e = timed("ok");
    PB_CHECK_EQ(pbo::validate_event_edit(e, *d), std::string(""));
    e.title = "   ";
    PB_CHECK_EQ(pbo::validate_event_edit(e, *d), std::string("title_missing"));
    e = timed("x");
    e.end = {2026, 10, 20, 9, 0, 0};
    PB_CHECK_EQ(pbo::validate_event_edit(e, *d), std::string("end_before_start"));
    e = timed("x");
    e.end = e.start;   // an event of no length is allowed
    PB_CHECK_EQ(pbo::validate_event_edit(e, *d), std::string(""));
    e = timed("x");
    e.tzid = "Mars/Olympus";
    PB_CHECK_EQ(pbo::validate_event_edit(e, *d), std::string("zone_unknown"));
    e = timed("x");
    e.all_day = true;
    e.start = {2026, 10, 20, 0, 0, 0};
    e.end = {2026, 10, 20, 0, 0, 0};   // all-day: the end date is exclusive, so it must be after the start
    PB_CHECK_EQ(pbo::validate_event_edit(e, *d), std::string("end_before_start"));
    e = timed("x");
    e.start = {2026, 2, 30, 9, 0, 0};
    PB_CHECK_EQ(pbo::validate_event_edit(e, *d), std::string("date_invalid"));
    e = timed(std::string(1100, 'a'));
    PB_CHECK_EQ(pbo::validate_event_edit(e, *d), std::string("too_long"));
}

PB_TEST(editing_the_title_changes_only_the_title_and_the_bookkeeping_lines) {
    auto d = db();
    for (const char* fixture : {kNextcloudStyle, kOutlookStyle, kAppleStyle}) {
        pbo::EventEdit e = edit_of(fixture);
        e.title += " (moved)";
        const auto r = pbo::apply_edit(fixture, e, *d, kNow);
        PB_CHECK(r.ok);
        PB_CHECK(r.changed);
        const auto before = as_set(fixture), after = as_set(r.ics);
        std::vector<std::string> removed = minus(before, after), added = minus(after, before);
        for (const auto& l : removed)
            PB_CHECK(starts_with(l, "SUMMARY:") || starts_with(l, "SEQUENCE:") || starts_with(l, "DTSTAMP:") || starts_with(l, "LAST-MODIFIED:"));
        for (const auto& l : added)
            PB_CHECK(starts_with(l, "SUMMARY:") || starts_with(l, "SEQUENCE:") || starts_with(l, "DTSTAMP:") || starts_with(l, "LAST-MODIFIED:"));
        PB_CHECK(std::find_if(added.begin(), added.end(), [](const std::string& l) { return starts_with(l, "SUMMARY:") && l.find("(moved)") != std::string::npos; }) != added.end());
        PB_CHECK(std::find_if(added.begin(), added.end(), [](const std::string& l) { return starts_with(l, "SEQUENCE:"); }) != added.end());
        // still a calendar that parses to the edited title
        PB_CHECK_EQ(pbo::parse_ical(r.ics, *d).events[0].summary, e.title);
    }
}

PB_TEST(alarms_attendees_unknown_properties_and_other_components_survive) {
    auto d = db();
    pbo::EventEdit e = edit_of(kNextcloudStyle);
    e.title = "Planning 2";
    e.location = "";   // removing a property
    const auto r = pbo::apply_edit(kNextcloudStyle, e, *d, kNow);
    PB_CHECK(r.ok);
    const auto after = as_set(r.ics);
    PB_CHECK(after.count("BEGIN:VALARM") == 1);
    PB_CHECK(after.count("SUMMARY:Alarm summary") == 1);       // the alarm's own SUMMARY is not the event's
    PB_CHECK(after.count("DESCRIPTION:Alarm text") == 1);
    PB_CHECK(after.count("ATTENDEE;CN=Bob;PARTSTAT=ACCEPTED:mailto:bob@example.org") == 1);
    PB_CHECK(after.count("ORGANIZER:mailto:alice@example.org") == 1);
    PB_CHECK(after.count("X-CUSTOM-FLAG:keep-me") == 1);
    PB_CHECK(after.count("SUMMARY:Planning 2") == 1);
    PB_CHECK(after.count("LOCATION:Room 1") == 0);              // gone
    const auto o = as_set(kOutlookStyle);
    const auto r2 = pbo::apply_edit(kOutlookStyle, [&] { auto x = edit_of(kOutlookStyle); x.description = "added"; return x; }(), *d, kNow);
    PB_CHECK(r2.ok);
    const auto a2 = as_set(r2.ics);
    for (const char* kept : {"BEGIN:VTIMEZONE", "TZID:W. Europe Standard Time", "X-MICROSOFT-CDO-BUSYSTATUS:BUSY", "PRIORITY:5", "METHOD:PUBLISH"})
        PB_CHECK(a2.count(kept) == 1);
    PB_CHECK(a2.count("DESCRIPTION:added") == 1);
}

PB_TEST(sequence_dtstamp_and_last_modified_are_bumped_and_set) {
    auto d = db();
    pbo::EventEdit e = edit_of(kNextcloudStyle);
    e.title = "Changed";
    const auto r = pbo::apply_edit(kNextcloudStyle, e, *d, kNow);
    PB_CHECK(r.ok);
    const auto s = as_set(r.ics);
    PB_CHECK(s.count("SEQUENCE:3") == 1);                       // was 2
    PB_CHECK(s.count("SEQUENCE:2") == 0);
    PB_CHECK(s.count("DTSTAMP:20260101T100000Z") == 0);
    PB_CHECK(s.count("LAST-MODIFIED:20260101T100000Z") == 0);
    // a resource without SEQUENCE gets 1
    const auto r2 = pbo::apply_edit(kAppleStyle, [&] { auto x = edit_of(kAppleStyle); x.title = "C2"; return x; }(), *d, kNow);
    PB_CHECK(as_set(r2.ics).count("SEQUENCE:1") == 1);
}

PB_TEST(an_edit_that_changes_nothing_leaves_the_text_alone) {
    auto d = db();
    const auto r = pbo::apply_edit(kNextcloudStyle, edit_of(kNextcloudStyle), *d, kNow);
    PB_CHECK(r.ok);
    PB_CHECK(!r.changed);
    PB_CHECK_EQ(r.ics, std::string(kNextcloudStyle));
}

PB_TEST(changing_the_time_replaces_start_and_end_and_drops_a_duration) {
    auto d = db();
    pbo::EventEdit e = edit_of(kAppleStyle);       // all-day, DURATION:P2D
    PB_CHECK(e.all_day);
    e.end = {2026, 10, 25, 0, 0, 0};                // three days
    auto r = pbo::apply_edit(kAppleStyle, e, *d, kNow);
    PB_CHECK(r.ok);
    auto s = as_set(r.ics);
    PB_CHECK(s.count("DURATION:P2D") == 0);
    PB_CHECK(s.count("DTSTART;VALUE=DATE:20261022") == 1);
    PB_CHECK(s.count("DTEND;VALUE=DATE:20261025") == 1);

    e = edit_of(kNextcloudStyle);                   // timed -> all-day
    e.all_day = true;
    e.start = {2026, 10, 20, 0, 0, 0};
    e.end = {2026, 10, 21, 0, 0, 0};
    r = pbo::apply_edit(kNextcloudStyle, e, *d, kNow);
    PB_CHECK(r.ok);
    s = as_set(r.ics);
    PB_CHECK(s.count("DTSTART;VALUE=DATE:20261020") == 1);
    PB_CHECK(s.count("DTEND;VALUE=DATE:20261021") == 1);
    PB_CHECK(s.count("DTSTART;TZID=Europe/Berlin:20261020T090000") == 0);
    const auto p = pbo::parse_ical(r.ics, *d);
    PB_CHECK(p.events[0].all_day);

    e = edit_of(kNextcloudStyle);                   // later start keeps the zone
    e.start = {2026, 10, 20, 11, 0, 0};
    e.end = {2026, 10, 20, 12, 0, 0};
    r = pbo::apply_edit(kNextcloudStyle, e, *d, kNow);
    s = as_set(r.ics);
    PB_CHECK(s.count("DTSTART;TZID=Europe/Berlin:20261020T110000") == 1);
    PB_CHECK(s.count("DTEND;TZID=Europe/Berlin:20261020T120000") == 1);
}

PB_TEST(a_windows_zone_name_is_kept_when_the_time_is_not_touched_and_iana_when_it_is) {
    auto d = db();
    pbo::EventEdit e = edit_of(kOutlookStyle);
    PB_CHECK_EQ(e.tzid, std::string("Europe/Berlin"));
    e.title = "Review 2";                            // times untouched: the original lines stay
    auto r = pbo::apply_edit(kOutlookStyle, e, *d, kNow);
    PB_CHECK(as_set(r.ics).count("DTSTART;TZID=W. Europe Standard Time:20261021T140000") == 1);
    e.start = {2026, 10, 21, 15, 0, 0};
    e.end = {2026, 10, 21, 16, 0, 0};
    r = pbo::apply_edit(kOutlookStyle, e, *d, kNow);
    PB_CHECK(as_set(r.ics).count("DTSTART;TZID=Europe/Berlin:20261021T150000") == 1);
    PB_CHECK_EQ(*pbo::parse_ical(r.ics, *d).events[0].start_utc, pbo::civil::naive_epoch({2026, 10, 21, 13, 0, 0}));
}

PB_TEST(a_repeating_event_is_reported_as_a_read_only_series) {
    auto d = db();
    pbo::EventEdit e = edit_of(kSeries);
    e.title = "Changed";
    const auto r = pbo::apply_edit(kSeries, e, *d, kNow);
    PB_CHECK(!r.ok);
    PB_CHECK_EQ(r.error, std::string("series_read_only"));
    PB_CHECK(pbo::is_series(kSeries, *d));
    PB_CHECK(!pbo::is_series(kNextcloudStyle, *d));
}

PB_TEST(a_text_without_an_event_is_not_editable) {
    auto d = db();
    pbo::EventEdit e = timed("x");
    const auto r = pbo::apply_edit("BEGIN:VCALENDAR\r\nVERSION:2.0\r\nEND:VCALENDAR\r\n", e, *d, kNow);
    PB_CHECK(!r.ok);
    PB_CHECK_EQ(r.error, std::string("not_editable"));
}

PB_TEST(event_uids_are_random_and_end_with_the_reserved_domain) {
    struct Counter : pbo::Random {
        uint8_t n = 0;
        void bytes(uint8_t* out, size_t len) override { for (size_t i = 0; i < len; ++i) out[i] = n++; }
    } rnd;
    const std::string a = pbo::make_event_uid(rnd), b = pbo::make_event_uid(rnd);
    PB_CHECK(a != b);
    PB_CHECK(a.size() > 32 + 1 + 14);
    PB_CHECK(a.size() >= 18 && a.substr(a.size() - 17) == "@pboffice.invalid");
}

PB_TEST_MAIN()
