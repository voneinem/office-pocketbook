// T095 (part): the iCalendar parser: the properties of contracts/caldav-ical-subset.md section 3, tolerance and limits.
#include <algorithm>
#include <string>

#include "pbo/ical.h"
#include "test_support.h"

namespace {

std::unique_ptr<pbo::TzDatabase> db() {
    auto d = pbo::TzDatabase::open(std::string(PBO_SOURCE_DIR) + "/third_party/tzdata/zoneinfo");
    if (!d) PB_FAIL_AT("the bundled zone data cannot be opened");
    return d;
}

std::string wrap(const std::string& event_lines) { return "BEGIN:VCALENDAR\r\nVERSION:2.0\r\nPRODID:-//example.org//test//EN\r\n" + event_lines + "END:VCALENDAR\r\n"; }

std::string event(const std::string& props) { return "BEGIN:VEVENT\r\nUID:u1@example.org\r\nDTSTAMP:20260101T000000Z\r\n" + props + "END:VEVENT\r\n"; }

int64_t utc(int y, int mo, int d, int h = 0, int mi = 0, int s = 0) { return pbo::civil::naive_epoch({y, mo, d, h, mi, s}); }

}  // namespace

PB_TEST(a_timed_event_in_a_zone_is_read_with_all_its_text_fields) {
    auto d = db();
    const auto p = pbo::parse_ical(wrap(event("SUMMARY:Team meeting\r\nLOCATION:Room 4\r\nDESCRIPTION:Agenda\\nitems\\, two\\; three\\\\ done\r\n"
                                              "DTSTART;TZID=Europe/Berlin:20261014T093000\r\nDTEND;TZID=Europe/Berlin:20261014T103000\r\n"
                                              "STATUS:confirmed\r\nTRANSP:OPAQUE\r\nCLASS:PUBLIC\r\nSEQUENCE:3\r\n")), *d);
    PB_CHECK(p.ok);
    PB_CHECK_EQ(p.events.size(), size_t(1));
    const auto& e = p.events[0];
    PB_CHECK_EQ(e.uid, std::string("u1@example.org"));
    PB_CHECK_EQ(e.summary, std::string("Team meeting"));
    PB_CHECK_EQ(e.location, std::string("Room 4"));
    PB_CHECK_EQ(e.description, std::string("Agenda\nitems, two; three\\ done"));
    PB_CHECK_EQ(e.status, std::string("CONFIRMED"));
    PB_CHECK_EQ(e.sequence, 3);
    PB_CHECK_EQ(e.tzid, std::string("Europe/Berlin"));
    PB_CHECK(!e.all_day && !e.floating);
    PB_CHECK(*e.start == (pbo::LocalDateTime{2026, 10, 14, 9, 30, 0}));
    PB_CHECK_EQ(*e.start_utc, utc(2026, 10, 14, 7, 30));
    PB_CHECK_EQ(*e.end_utc, utc(2026, 10, 14, 8, 30));
    PB_CHECK_EQ(e.note, std::string(""));
}

PB_TEST(utc_floating_and_windows_zone_names_are_resolved) {
    auto d = db();
    auto p = pbo::parse_ical(wrap(event("DTSTART:20261014T073000Z\r\nDTEND:20261014T083000Z\r\n")), *d);
    PB_CHECK_EQ(p.events[0].tzid, std::string("UTC"));
    PB_CHECK_EQ(*p.events[0].start_utc, utc(2026, 10, 14, 7, 30));
    p = pbo::parse_ical(wrap(event("DTSTART:20261014T093000\r\nDTEND:20261014T103000\r\n")), *d);
    PB_CHECK(p.events[0].floating);
    PB_CHECK_EQ(p.events[0].tzid, std::string(""));
    PB_CHECK(!p.events[0].start_utc.has_value());
    PB_CHECK(*p.events[0].end == (pbo::LocalDateTime{2026, 10, 14, 10, 30, 0}));
    p = pbo::parse_ical(wrap(event("DTSTART;TZID=W. Europe Standard Time:20261014T093000\r\nDTEND;TZID=W. Europe Standard Time:20261014T103000\r\n")), *d);
    PB_CHECK_EQ(p.events[0].tzid, std::string("Europe/Berlin"));
    PB_CHECK_EQ(*p.events[0].start_utc, utc(2026, 10, 14, 7, 30));
    p = pbo::parse_ical(wrap(event("DTSTART;TZID=\"/softwarestudio.org/Tzfile/Europe/Berlin\":20261014T093000\r\nDTEND;TZID=/softwarestudio.org/Tzfile/Europe/Berlin:20261014T103000\r\n")), *d);
    PB_CHECK_EQ(p.events[0].tzid, std::string("Europe/Berlin"));
}

PB_TEST(an_unknown_zone_is_shown_as_floating_with_a_note) {
    auto d = db();
    const auto p = pbo::parse_ical(wrap(event("DTSTART;TZID=Mars/Olympus:20261014T093000\r\nDTEND;TZID=Mars/Olympus:20261014T103000\r\n")), *d);
    PB_CHECK_EQ(p.events.size(), size_t(1));
    PB_CHECK(p.events[0].floating);
    PB_CHECK_EQ(p.events[0].note, std::string("tz_unknown"));
    PB_CHECK(*p.events[0].start == (pbo::LocalDateTime{2026, 10, 14, 9, 30, 0}));
}

PB_TEST(all_day_events_have_an_exclusive_end_and_a_default_of_one_day) {
    auto d = db();
    auto p = pbo::parse_ical(wrap(event("DTSTART;VALUE=DATE:20261014\r\nDTEND;VALUE=DATE:20261016\r\n")), *d);
    PB_CHECK(p.events[0].all_day);
    PB_CHECK(*p.events[0].end == (pbo::LocalDateTime{2026, 10, 16, 0, 0, 0}));
    p = pbo::parse_ical(wrap(event("DTSTART;VALUE=DATE:20261014\r\n")), *d);
    PB_CHECK(*p.events[0].end == (pbo::LocalDateTime{2026, 10, 15, 0, 0, 0}));
    PB_CHECK_EQ(p.events[0].note, std::string("no_end"));
    p = pbo::parse_ical(wrap(event("DTSTART;VALUE=DATE:20261014\r\nDURATION:P3D\r\n")), *d);
    PB_CHECK(*p.events[0].end == (pbo::LocalDateTime{2026, 10, 17, 0, 0, 0}));
    p = pbo::parse_ical(wrap(event("DTSTART:20261014\r\n")), *d);                           // a date without VALUE=DATE
    PB_CHECK(p.events[0].all_day);
}

PB_TEST(a_timed_event_without_an_end_has_zero_length_and_duration_is_added) {
    auto d = db();
    auto p = pbo::parse_ical(wrap(event("DTSTART;TZID=Europe/Berlin:20261014T093000\r\n")), *d);
    PB_CHECK_EQ(*p.events[0].end_utc, *p.events[0].start_utc);
    PB_CHECK_EQ(p.events[0].note, std::string("no_end"));
    p = pbo::parse_ical(wrap(event("DTSTART;TZID=Europe/Berlin:20261014T093000\r\nDURATION:PT1H30M\r\n")), *d);
    PB_CHECK_EQ(*p.events[0].end_utc - *p.events[0].start_utc, 5400);
    PB_CHECK(*p.events[0].end == (pbo::LocalDateTime{2026, 10, 14, 11, 0, 0}));
    PB_CHECK_EQ(p.events[0].note, std::string(""));
    p = pbo::parse_ical(wrap(event("DTSTART;TZID=Europe/Berlin:20261014T093000\r\nDTEND;TZID=Europe/Berlin:20261014T083000\r\n")), *d);   // end before start
    PB_CHECK_EQ(*p.events[0].end_utc, *p.events[0].start_utc);
}

PB_TEST(an_end_in_another_zone_is_converted_into_the_zone_of_the_start) {
    auto d = db();
    const auto p = pbo::parse_ical(wrap(event("DTSTART;TZID=Europe/Berlin:20261014T093000\r\nDTEND:20261014T090000Z\r\n")), *d);   // 11:00 Berlin
    PB_CHECK(*p.events[0].end == (pbo::LocalDateTime{2026, 10, 14, 11, 0, 0}));
}

PB_TEST(folding_line_endings_parameter_quoting_and_case_are_tolerated) {
    auto d = db();
    const std::string crlf = wrap(event("summary:A very long\r\n  summary that is\r\n\t folded\r\ndtstart;tzid=Europe/Berlin:20261014T093000\r\n"
                                        "ATTENDEE;CN=\"Doe, Jane\";ROLE=REQ-PARTICIPANT:mailto:jane@example.org\r\nATTENDEE:mailto:bob@example.org\r\n"
                                        "ORGANIZER;CN=Boss:mailto:boss@example.org\r\n"));
    auto p = pbo::parse_ical(crlf, *d);
    PB_CHECK_EQ(p.events.size(), size_t(1));
    PB_CHECK_EQ(p.events[0].summary, std::string("A very long summary that is folded"));
    PB_CHECK_EQ(p.events[0].attendees.size(), size_t(2));
    PB_CHECK_EQ(p.events[0].attendees[0], std::string("Doe, Jane <jane@example.org>"));
    PB_CHECK_EQ(p.events[0].attendees[1], std::string("bob@example.org"));
    PB_CHECK_EQ(p.events[0].organizer, std::string("Boss <boss@example.org>"));
    std::string lf, cr;
    for (char c : crlf) {
        if (c != '\r') lf += c;
        cr += (c == '\n') ? '\r' : (c == '\r' ? '\0' : c);
    }
    cr.erase(std::remove(cr.begin(), cr.end(), '\0'), cr.end());
    PB_CHECK_EQ(pbo::parse_ical(lf, *d).events.size(), size_t(1));
    PB_CHECK_EQ(pbo::parse_ical(lf, *d).events[0].summary, std::string("A very long summary that is folded"));
    PB_CHECK_EQ(pbo::parse_ical(cr, *d).events.size(), size_t(1));
}

PB_TEST(alarms_and_timezone_definitions_are_parsed_past_and_not_mistaken_for_the_event) {
    auto d = db();
    const std::string ics =
        "BEGIN:VCALENDAR\r\nVERSION:2.0\r\n"
        "BEGIN:VTIMEZONE\r\nTZID:Europe/Berlin\r\nBEGIN:STANDARD\r\nDTSTART:19701025T030000\r\nTZOFFSETFROM:+0200\r\nTZOFFSETTO:+0100\r\nEND:STANDARD\r\nEND:VTIMEZONE\r\n"
        "BEGIN:VEVENT\r\nUID:u1@example.org\r\nSUMMARY:Real\r\nDTSTART;TZID=Europe/Berlin:20261014T093000\r\nDTEND;TZID=Europe/Berlin:20261014T103000\r\n"
        "BEGIN:VALARM\r\nACTION:DISPLAY\r\nTRIGGER:-PT15M\r\nDESCRIPTION:Alarm text\r\nSUMMARY:Alarm summary\r\nEND:VALARM\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n";
    const auto p = pbo::parse_ical(ics, *d);
    PB_CHECK_EQ(p.events.size(), size_t(1));
    PB_CHECK_EQ(p.events[0].summary, std::string("Real"));
    PB_CHECK_EQ(p.events[0].description, std::string(""));
    PB_CHECK(*p.events[0].start == (pbo::LocalDateTime{2026, 10, 14, 9, 30, 0}));
}

PB_TEST(recurrence_properties_are_collected_in_the_zone_of_the_event) {
    auto d = db();
    const auto p = pbo::parse_ical(wrap(event("DTSTART;TZID=Europe/Berlin:20261014T093000\r\nDTEND;TZID=Europe/Berlin:20261014T103000\r\nRRULE:FREQ=DAILY;COUNT=5\r\n"
                                              "EXDATE;TZID=Europe/Berlin:20261015T093000,20261016T093000\r\nEXDATE:20261017T073000Z\r\nRDATE;TZID=Europe/Berlin:20261101T150000\r\n")), *d);
    const auto& e = p.events[0];
    PB_CHECK_EQ(e.rrule, std::string("FREQ=DAILY;COUNT=5"));
    PB_CHECK(e.exdates == (std::vector<std::string>{"20261015T093000", "20261016T093000", "20261017T093000"}));   // the Z form converted: 07:30Z = 09:30 CEST
    PB_CHECK(e.rdates == (std::vector<std::string>{"20261101T150000"}));
    PB_CHECK_EQ(e.note, std::string(""));
}

PB_TEST(an_override_names_the_instance_it_replaces) {
    auto d = db();
    const auto p = pbo::parse_ical(wrap(event("DTSTART;TZID=Europe/Berlin:20261014T093000\r\nDTEND;TZID=Europe/Berlin:20261014T103000\r\nRRULE:FREQ=DAILY;COUNT=5\r\n") +
                                        "BEGIN:VEVENT\r\nUID:u1@example.org\r\nRECURRENCE-ID;TZID=Europe/Berlin:20261015T093000\r\nDTSTART;TZID=Europe/Berlin:20261015T140000\r\n"
                                        "DTEND;TZID=Europe/Berlin:20261015T150000\r\nEND:VEVENT\r\n"
                                        "BEGIN:VEVENT\r\nUID:u1@example.org\r\nRECURRENCE-ID;RANGE=THISANDFUTURE;TZID=Europe/Berlin:20261017T093000\r\nDTSTART;TZID=Europe/Berlin:20261017T100000\r\n"
                                        "DTEND;TZID=Europe/Berlin:20261017T110000\r\nEND:VEVENT\r\n"), *d);
    PB_CHECK_EQ(p.events.size(), size_t(3));
    PB_CHECK_EQ(p.events[1].recurrence_id, std::string("20261015T093000"));
    PB_CHECK_EQ(p.events[2].recurrence_range, std::string("THISANDFUTURE"));
    PB_CHECK_EQ(p.events[2].note, std::string("range_unsupported"));
}

PB_TEST(an_unsupported_rule_is_noted_and_the_event_stays_readable) {
    auto d = db();
    const auto p = pbo::parse_ical(wrap(event("DTSTART;TZID=Europe/Berlin:20261014T093000\r\nDTEND;TZID=Europe/Berlin:20261014T103000\r\nRRULE:FREQ=YEARLY;BYWEEKNO=20\r\n")), *d);
    PB_CHECK_EQ(p.events.size(), size_t(1));
    PB_CHECK_EQ(p.events[0].note, std::string("rrule_unsupported"));
}

PB_TEST(events_that_cannot_be_used_are_skipped_and_counted_and_the_rest_is_kept) {
    auto d = db();
    const std::string bad_uid = "BEGIN:VEVENT\r\nDTSTART:20261014T093000Z\r\nEND:VEVENT\r\n";
    const std::string bad_start = "BEGIN:VEVENT\r\nUID:x\r\nDTSTART:20261314T093000Z\r\nEND:VEVENT\r\n";
    const std::string no_start = "BEGIN:VEVENT\r\nUID:y\r\nSUMMARY:none\r\nEND:VEVENT\r\n";
    const std::string bad_feb = "BEGIN:VEVENT\r\nUID:z\r\nDTSTART:20260230T093000Z\r\nEND:VEVENT\r\n";
    const auto p = pbo::parse_ical(wrap(bad_uid + bad_start + no_start + bad_feb + event("DTSTART:20261014T093000Z\r\n")), *d);
    PB_CHECK(p.ok);
    PB_CHECK_EQ(p.events.size(), size_t(1));
    PB_CHECK_EQ(p.skipped, 4);
}

PB_TEST(long_text_is_cut_on_a_character_boundary_and_noted) {
    auto d = db();
    std::string summary = "SUMMARY:" + std::string(70000, 'a') + "\r\n";
    auto p = pbo::parse_ical(wrap(event(summary + "DTSTART:20261014T093000Z\r\nDTEND:20261014T103000Z\r\n")), *d);
    PB_CHECK_EQ(p.events[0].summary.size(), size_t(65536));
    PB_CHECK_EQ(p.events[0].note, std::string("truncated"));
    std::string emoji;
    for (int i = 0; i < 20000; ++i) emoji += "\xF0\x9F\x93\x85";
    p = pbo::parse_ical(wrap(event("DESCRIPTION:" + emoji + "\r\nDTSTART:20261014T093000Z\r\nDTEND:20261014T103000Z\r\n")), *d);
    PB_CHECK_EQ(p.events[0].description.size() % 4, size_t(0));
}

PB_TEST(invalid_utf8_in_text_is_replaced_never_kept) {
    auto d = db();
    const auto p = pbo::parse_ical(wrap(event("SUMMARY:Caf\xE9 \xFF\xFE ok\r\nDTSTART:20261014T093000Z\r\n")), *d);
    PB_CHECK_EQ(p.events.size(), size_t(1));
    for (size_t i = 0; i < p.events[0].summary.size(); ++i) PB_CHECK(static_cast<unsigned char>(p.events[0].summary[i]) < 0xF8);
    PB_CHECK(p.events[0].summary.find("ok") != std::string::npos);
}

PB_TEST(attendees_are_kept_up_to_200) {
    auto d = db();
    std::string a;
    for (int i = 0; i < 250; ++i) a += "ATTENDEE:mailto:p" + std::to_string(i) + "@example.org\r\n";
    const auto p = pbo::parse_ical(wrap(event(a + "DTSTART:20261014T093000Z\r\n")), *d);
    PB_CHECK_EQ(p.events[0].attendees.size(), size_t(200));
}

PB_TEST(limits_stop_the_parser_without_losing_what_was_read) {
    auto d = db();
    pbo::IcalLimits tiny;
    tiny.max_bytes = 100;
    auto p = pbo::parse_ical(wrap(event("DTSTART:20261014T093000Z\r\n")), *d, tiny);
    PB_CHECK(!p.ok);
    PB_CHECK_EQ(p.note, std::string("too_large"));
    PB_CHECK(p.events.empty());

    pbo::IcalLimits lines;
    lines.max_lines = 12;
    std::string many = "SUMMARY:s\r\n";
    for (int i = 0; i < 50; ++i) many += "X-FOO:" + std::to_string(i) + "\r\n";
    p = pbo::parse_ical(wrap(event(many + "DTSTART:20261014T093000Z\r\n")), *d, lines);
    PB_CHECK(!p.ok);
    PB_CHECK_EQ(p.note, std::string("too_many_lines"));

    pbo::IcalLimits comps;
    comps.max_components = 5;
    std::string events;
    for (int i = 0; i < 20; ++i) events += event("DTSTART:20261014T093000Z\r\n");
    p = pbo::parse_ical(wrap(events), *d, comps);
    PB_CHECK(!p.ok);
    PB_CHECK_EQ(p.note, std::string("too_many_components"));
    PB_CHECK(p.events.size() <= 5);

    pbo::IcalLimits longline;
    longline.max_line_bytes = 100;
    p = pbo::parse_ical(wrap(event("SUMMARY:" + std::string(500, 'x') + "\r\nDTSTART:20261014T093000Z\r\n")), *d, longline);
    PB_CHECK_EQ(p.events.size(), size_t(1));          // the overlong line is dropped, the event survives
    PB_CHECK_EQ(p.events[0].summary, std::string(""));
}

PB_TEST(input_that_is_not_a_calendar_says_so) {
    auto d = db();
    for (const char* junk : {"", "hello", "<html></html>", "BEGIN:VCARD\r\nFN:x\r\nEND:VCARD\r\n"}) {
        const auto p = pbo::parse_ical(junk, *d);
        PB_CHECK(!p.ok);
        PB_CHECK_EQ(p.note, std::string("not_a_calendar"));
        PB_CHECK(p.events.empty());
    }
}

PB_TEST(a_resource_with_many_events_is_split_by_uid_keeping_overrides_and_dropping_timezone_blocks) {
    auto d = db();
    const std::string feed =
        "BEGIN:VCALENDAR\r\nVERSION:2.0\r\nPRODID:-//example.org//feed//EN\r\n"
        "BEGIN:VTIMEZONE\r\nTZID:Europe/Berlin\r\nEND:VTIMEZONE\r\n"
        "BEGIN:VEVENT\r\nUID:a@example.org\r\nSUMMARY:A\r\nDTSTART:20261014T093000Z\r\nEND:VEVENT\r\n"
        "BEGIN:VEVENT\r\nUID:b@example.org\r\nSUMMARY:B\r\nDTSTART:20261015T093000Z\r\nBEGIN:VALARM\r\nTRIGGER:-PT5M\r\nEND:VALARM\r\nEND:VEVENT\r\n"
        "BEGIN:VEVENT\r\nUID:a@example.org\r\nRECURRENCE-ID:20261014T093000Z\r\nSUMMARY:A moved\r\nDTSTART:20261014T100000Z\r\nEND:VEVENT\r\n"
        "BEGIN:VEVENT\r\nSUMMARY:no uid\r\nDTSTART:20261016T093000Z\r\nEND:VEVENT\r\n"
        "END:VCALENDAR\r\n";
    const auto parts = pbo::split_ical_by_uid(feed);
    PB_CHECK_EQ(parts.size(), size_t(2));
    PB_CHECK_EQ(parts[0].uid, std::string("a@example.org"));
    PB_CHECK_EQ(parts[1].uid, std::string("b@example.org"));
    PB_CHECK(parts[0].ics.find("VTIMEZONE") == std::string::npos);
    PB_CHECK(parts[0].ics.find("PRODID:-//example.org//feed//EN") != std::string::npos);
    const auto a = pbo::parse_ical(parts[0].ics, *d);
    PB_CHECK_EQ(a.events.size(), size_t(2));                         // the master and its override
    const auto b = pbo::parse_ical(parts[1].ics, *d);
    PB_CHECK_EQ(b.events.size(), size_t(1));
    PB_CHECK(parts[1].ics.find("VALARM") != std::string::npos);        // other components inside an event are kept
    PB_CHECK(pbo::split_ical_by_uid("garbage").empty());
}

PB_TEST(content_lines_are_folded_at_75_octets_without_cutting_characters) {
    std::string line = "SUMMARY:";
    for (int i = 0; i < 40; ++i) line += "\xC3\xA4\xC3\xB6";   // 160 bytes of umlauts
    const std::string folded = pbo::fold_ical_line(line);
    size_t pos = 0;
    while (pos < folded.size()) {
        const size_t e = folded.find("\r\n", pos);
        PB_CHECK(e != std::string::npos);
        PB_CHECK(e - pos <= 75);
        pos = e + 2;
    }
    auto d = db();
    // unfolding gives the original back
    const auto p = pbo::parse_ical("BEGIN:VCALENDAR\r\nBEGIN:VEVENT\r\nUID:u\r\n" + folded + "DTSTART:20261014T093000Z\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n", *d);
    PB_CHECK_EQ(p.events.size(), size_t(1));
    PB_CHECK_EQ(p.events[0].summary, line.substr(8));
}

PB_TEST(a_cancelled_event_has_no_occurrences) {
    auto d = db();
    const auto p = pbo::parse_ical(wrap(event("STATUS:CANCELLED\r\nDTSTART:20261014T093000Z\r\nDTEND:20261014T103000Z\r\n")), *d);
    PB_CHECK_EQ(p.events.size(), size_t(1));
    PB_CHECK(pbo::expand(p, *d, "Europe/Berlin", 0, int64_t(1) << 40).empty());
}

PB_TEST_MAIN()
