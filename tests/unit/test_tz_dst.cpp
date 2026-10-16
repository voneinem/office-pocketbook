// T093 (part): daylight-saving corner cases with the RFC 5545 rules, and the Windows zone names.
#include <string>

#include "pbo/tz.h"
#include "test_support.h"

namespace {

std::unique_ptr<pbo::TzDatabase> db() {
    auto d = pbo::TzDatabase::open(std::string(PBO_SOURCE_DIR) + "/third_party/tzdata/zoneinfo");
    if (!d) PB_FAIL_AT("the bundled zone data cannot be opened");
    return d;
}

int64_t utc(int y, int mo, int d, int h = 0, int mi = 0, int s = 0) { return pbo::civil::naive_epoch({y, mo, d, h, mi, s}); }

}  // namespace

PB_TEST(the_exact_instant_of_the_change_switches_the_offset) {
    auto d = db();
    // Europe: last Sunday of March 01:00 UTC, last Sunday of October 01:00 UTC (2026-03-29, 2026-10-25)
    PB_CHECK_EQ(d->offset_at("Europe/Berlin", utc(2026, 3, 29, 0, 59, 59)), 3600);
    PB_CHECK_EQ(d->offset_at("Europe/Berlin", utc(2026, 3, 29, 1, 0, 0)), 7200);
    PB_CHECK_EQ(d->offset_at("Europe/Berlin", utc(2026, 10, 25, 0, 59, 59)), 7200);
    PB_CHECK_EQ(d->offset_at("Europe/Berlin", utc(2026, 10, 25, 1, 0, 0)), 3600);
    // United States: second Sunday of March 02:00 local, first Sunday of November (2026-03-08, 2026-11-01)
    PB_CHECK_EQ(d->offset_at("America/New_York", utc(2026, 3, 8, 6, 59, 59)), -18000);
    PB_CHECK_EQ(d->offset_at("America/New_York", utc(2026, 3, 8, 7, 0, 0)), -14400);
    PB_CHECK_EQ(d->offset_at("America/New_York", utc(2026, 11, 1, 5, 59, 59)), -14400);
    PB_CHECK_EQ(d->offset_at("America/New_York", utc(2026, 11, 1, 6, 0, 0)), -18000);
    // Australia/Sydney: first Sunday of October 02:00 standard (2026-10-04), first Sunday of April 03:00 daylight (2026-04-05)
    PB_CHECK_EQ(d->offset_at("Australia/Sydney", utc(2026, 10, 3, 15, 59, 59)), 36000);
    PB_CHECK_EQ(d->offset_at("Australia/Sydney", utc(2026, 10, 3, 16, 0, 0)), 39600);
    PB_CHECK_EQ(d->offset_at("Australia/Sydney", utc(2026, 4, 4, 15, 59, 59)), 39600);
    PB_CHECK_EQ(d->offset_at("Australia/Sydney", utc(2026, 4, 4, 16, 0, 0)), 36000);
}

PB_TEST(a_nonexistent_local_time_uses_the_offset_before_the_gap) {
    auto d = db();
    // 2026-03-29 02:30 does not exist in Berlin (02:00 -> 03:00): read with +01:00, which is 01:30 UTC
    PB_CHECK_EQ(d->local_to_utc("Europe/Berlin", {2026, 3, 29, 2, 30, 0}), utc(2026, 3, 29, 1, 30));
    PB_CHECK_EQ(d->local_to_utc("Europe/Berlin", {2026, 3, 29, 2, 0, 0}), utc(2026, 3, 29, 1, 0));
    PB_CHECK_EQ(d->local_to_utc("Europe/Berlin", {2026, 3, 29, 3, 0, 0}), utc(2026, 3, 29, 1, 0));
    // 2026-03-08 02:30 does not exist in New York: read with -05:00 = 07:30 UTC
    PB_CHECK_EQ(d->local_to_utc("America/New_York", {2026, 3, 8, 2, 30, 0}), utc(2026, 3, 8, 7, 30));
    // Sydney, 2026-10-04 02:30 does not exist: read with +10:00 = 16:30 UTC the day before
    PB_CHECK_EQ(d->local_to_utc("Australia/Sydney", {2026, 10, 4, 2, 30, 0}), utc(2026, 10, 3, 16, 30));
}

PB_TEST(a_repeated_local_time_means_its_first_occurrence) {
    auto d = db();
    // 2026-10-25 02:30 occurs twice in Berlin (CEST, then CET): the first is 00:30 UTC
    PB_CHECK_EQ(d->local_to_utc("Europe/Berlin", {2026, 10, 25, 2, 30, 0}), utc(2026, 10, 25, 0, 30));
    PB_CHECK_EQ(d->local_to_utc("Europe/Berlin", {2026, 10, 25, 3, 0, 0}), utc(2026, 10, 25, 2, 0));   // 03:00 CET: once
    // 2026-11-01 01:30 occurs twice in New York: the first (EDT, -04:00) is 05:30 UTC
    PB_CHECK_EQ(d->local_to_utc("America/New_York", {2026, 11, 1, 1, 30, 0}), utc(2026, 11, 1, 5, 30));
    // Sydney, 2026-04-05 02:30 occurs twice: the first (+11:00) is 15:30 UTC the day before
    PB_CHECK_EQ(d->local_to_utc("Australia/Sydney", {2026, 4, 5, 2, 30, 0}), utc(2026, 4, 4, 15, 30));
}

PB_TEST(the_same_local_time_before_and_after_the_change_differs_by_the_shift_in_utc) {
    auto d = db();
    const int64_t before = d->local_to_utc("Europe/Berlin", {2026, 3, 28, 10, 0, 0});
    const int64_t after = d->local_to_utc("Europe/Berlin", {2026, 3, 30, 10, 0, 0});
    PB_CHECK_EQ(after - before, 2 * 86400 - 3600);                    // a weekly 10:00 meeting moves one hour earlier in UTC
}

PB_TEST(windows_names_map_to_iana_zones_and_unknown_names_do_not) {
    auto d = db();
    PB_CHECK_EQ(d->canonical_name("W. Europe Standard Time"), std::string("Europe/Berlin"));
    PB_CHECK_EQ(d->canonical_name("Eastern Standard Time"), std::string("America/New_York"));
    PB_CHECK_EQ(d->canonical_name("AUS Eastern Standard Time"), std::string("Australia/Sydney"));
    PB_CHECK_EQ(d->canonical_name("Pacific Standard Time"), std::string("America/Los_Angeles"));
    PB_CHECK_EQ(d->canonical_name("GMT Standard Time"), std::string("Europe/London"));
    PB_CHECK_EQ(d->canonical_name("Romance Standard Time"), std::string("Europe/Paris"));
    PB_CHECK_EQ(d->canonical_name("Tokyo Standard Time"), std::string("Asia/Tokyo"));
    PB_CHECK_EQ(d->canonical_name("UTC"), std::string("UTC"));
    PB_CHECK_EQ(d->canonical_name("w. europe standard time"), std::string("Europe/Berlin"));   // case does not matter
    PB_CHECK_EQ(d->canonical_name("Unheard Standard Time"), std::string(""));
    // every mapped name must lead to a zone that exists in the database
    int mapped = 0;
    for (const char* w : {"Dateline Standard Time", "Hawaiian Standard Time", "Alaskan Standard Time", "Pacific Standard Time", "Mountain Standard Time",
                          "Central Standard Time", "Eastern Standard Time", "Atlantic Standard Time", "Newfoundland Standard Time", "E. South America Standard Time",
                          "Argentina Standard Time", "Greenwich Standard Time", "GMT Standard Time", "W. Europe Standard Time", "Central Europe Standard Time",
                          "Central European Standard Time", "Romance Standard Time", "E. Europe Standard Time", "FLE Standard Time", "GTB Standard Time",
                          "Turkey Standard Time", "Russian Standard Time", "Arab Standard Time", "Arabian Standard Time", "India Standard Time",
                          "China Standard Time", "Tokyo Standard Time", "Korea Standard Time", "Singapore Standard Time", "AUS Eastern Standard Time",
                          "E. Australia Standard Time", "W. Australia Standard Time", "New Zealand Standard Time", "South Africa Standard Time", "Egypt Standard Time"})
        if (!d->canonical_name(w).empty()) ++mapped;
    PB_CHECK_EQ(mapped, 35);
}

PB_TEST_MAIN()
