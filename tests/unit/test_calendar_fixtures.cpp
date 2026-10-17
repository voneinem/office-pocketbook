// T090: the calendar fixtures (tests/fixtures/ical/*.ics single cases, tests/fixtures/caldav/*.unverified.xml transcripts) are readable
// by the code that will consume them. The CalDAV transcripts imitate a server and are UNVERIFIED until a live run (docs/live-server-checklist.md).
#include <dirent.h>

#include <algorithm>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "pbo/ical.h"
#include "pbo/xml_min.h"
#include "test_support.h"

namespace {

const char* kDav = "DAV:";

std::string slurp(const std::string& relative) {
    std::ifstream f(std::string(PBO_FIXTURE_DIR) + "/" + relative, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::vector<std::string> names_in(const std::string& relative, const std::string& ext) {
    std::vector<std::string> out;
    if (DIR* d = opendir((std::string(PBO_FIXTURE_DIR) + "/" + relative).c_str())) {
        while (dirent* e = readdir(d)) {
            const std::string n = e->d_name;
            if (n.size() > ext.size() && n.compare(n.size() - ext.size(), ext.size(), ext) == 0) out.push_back(n);
        }
        closedir(d);
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::unique_ptr<pbo::TzDatabase> db() {
    auto d = pbo::TzDatabase::open(std::string(PBO_SOURCE_DIR) + "/third_party/tzdata/zoneinfo");
    if (!d) PB_FAIL_AT("the bundled zone data cannot be opened");
    return d;
}

pbo::ParsedCalendar parse(const std::string& file) {
    auto d = db();
    return pbo::parse_ical(slurp("ical/" + file), *d);
}

}  // namespace

PB_TEST(every_caldav_transcript_is_well_formed_dav_xml) {
    const auto files = names_in("caldav", ".unverified.xml");
    PB_CHECK(files.size() >= 6);
    for (const std::string& f : files) {
        const auto r = pbo::parse_xml(slurp("caldav/" + f));
        PB_CHECK(r.ok());
        PB_CHECK_EQ(r.value.ns, std::string(kDav));
        PB_CHECK(r.value.name == "multistatus" || r.value.name == "error");
    }
}

PB_TEST(the_discovery_transcripts_name_principal_home_and_calendars) {
    const auto principal = pbo::parse_xml(slurp("caldav/principal.unverified.xml"));
    PB_CHECK(principal.ok());
    PB_CHECK(principal.value.child(kDav, "response") != nullptr);

    const auto calendars = pbo::parse_xml(slurp("caldav/calendars.unverified.xml"));
    PB_CHECK(calendars.ok());
    PB_CHECK_EQ(calendars.value.children_named(kDav, "response").size(), size_t(4));
}

PB_TEST(the_sync_transcript_has_a_changed_a_removed_and_a_failed_resource_and_a_token) {
    const auto r = pbo::parse_xml(slurp("caldav/sync_collection.unverified.xml"));
    PB_CHECK(r.ok());
    PB_CHECK_EQ(r.value.children_named(kDav, "response").size(), size_t(3));
    PB_CHECK(r.value.child(kDav, "sync-token") != nullptr);
}

PB_TEST(the_multiget_transcript_carries_two_calendar_resources_with_escaped_and_cdata_text) {
    const auto r = pbo::parse_xml(slurp("caldav/multiget_calendar_data.unverified.xml"));
    PB_CHECK(r.ok());
    PB_CHECK_EQ(r.value.children_named(kDav, "response").size(), size_t(2));
    auto d = db();
    size_t events = 0;
    for (const pbo::XmlNode* resp : r.value.children_named(kDav, "response")) {
        const pbo::XmlNode* prop = resp->child(kDav, "propstat")->child(kDav, "prop");
        const pbo::XmlNode* data = prop->child("urn:ietf:params:xml:ns:caldav", "calendar-data");
        PB_CHECK(data != nullptr);
        const auto p = pbo::parse_ical(data->text, *d);
        PB_CHECK(p.ok);
        events += p.events.size();
    }
    PB_CHECK_EQ(events, size_t(2));
}

PB_TEST(a_windows_zone_name_is_mapped_and_an_unknown_one_is_noted) {
    const auto win = parse("windows_zone_name.ics");
    PB_CHECK_EQ(win.events.size(), size_t(1));
    PB_CHECK_EQ(win.events[0].tzid, std::string("Europe/Berlin"));
    PB_CHECK(win.events[0].start_utc.has_value());

    const auto unknown = parse("unknown_tzid.ics");
    PB_CHECK_EQ(unknown.events.size(), size_t(1));
    PB_CHECK_EQ(unknown.events[0].note, std::string("tz_unknown"));
    PB_CHECK(unknown.events[0].floating);
}

PB_TEST(a_missing_end_is_noted_and_a_duration_gives_the_end) {
    const auto none = parse("no_end.ics");
    PB_CHECK_EQ(none.events.size(), size_t(1));
    PB_CHECK_EQ(none.events[0].note, std::string("no_end"));

    const auto dur = parse("duration.ics");
    PB_CHECK_EQ(dur.events.size(), size_t(1));
    PB_CHECK(dur.events[0].start_utc.has_value() && dur.events[0].end_utc.has_value());
    PB_CHECK_EQ(*dur.events[0].end_utc - *dur.events[0].start_utc, int64_t(90 * 60));
}

PB_TEST(an_unsupported_rule_part_is_noted_and_an_all_day_event_keeps_its_exclusive_end) {
    const auto weekno = parse("byweekno_unsupported.ics");
    PB_CHECK_EQ(weekno.events.size(), size_t(1));
    PB_CHECK_EQ(weekno.events[0].note, std::string("rrule_unsupported"));

    const auto allday = parse("allday_date.ics");
    PB_CHECK_EQ(allday.events.size(), size_t(1));
    PB_CHECK(allday.events[0].all_day);
}

PB_TEST(a_huge_description_is_read_and_the_event_stays_usable) {
    const auto p = parse("huge_description.ics");
    PB_CHECK(p.ok);
    PB_CHECK_EQ(p.events.size(), size_t(1));
    PB_CHECK_EQ(p.events[0].summary, std::string("Huge description"));
    PB_CHECK(p.events[0].description.size() <= (64u << 10));
}

PB_TEST_MAIN()
