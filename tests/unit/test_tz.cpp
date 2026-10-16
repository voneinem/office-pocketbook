// T093 (part): the TZif reader and the conversions between UTC and local time with the bundled IANA data.
#include <fstream>
#include <sstream>
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

std::string release_of_version_file() {
    std::ifstream f(std::string(PBO_SOURCE_DIR) + "/third_party/tzdata/VERSION");
    std::string line;
    while (std::getline(f, line))
        if (line.rfind("Release: ", 0) == 0) return line.substr(9);
    return "";
}

}  // namespace

PB_TEST(the_directory_must_hold_usable_zones) {
    PB_CHECK(pbo::TzDatabase::open("/nonexistent/zoneinfo") == nullptr);
    PB_CHECK(pbo::TzDatabase::open(std::string(PBO_SOURCE_DIR) + "/tests") == nullptr);   // exists, no zones
}

PB_TEST(the_version_is_the_release_recorded_in_third_party_tzdata_VERSION) {
    auto d = db();
    PB_CHECK(!release_of_version_file().empty());
    PB_CHECK_EQ(d->version(), release_of_version_file());
}

PB_TEST(names_are_found_case_insensitively_and_unknown_or_hostile_names_are_not) {
    auto d = db();
    PB_CHECK(d->has_zone("Europe/Berlin"));
    PB_CHECK_EQ(d->canonical_name("europe/berlin"), std::string("Europe/Berlin"));
    PB_CHECK(d->has_zone("UTC"));
    PB_CHECK(d->has_zone("Asia/Calcutta"));              // a link name of the database
    for (const char* bad : {"", "Mars/Olympus", "../../etc/passwd", "/etc/passwd", "Europe/", "Europe", "Europe/Berlin/../Paris", "Europe\\Berlin"})
        PB_CHECK(!d->has_zone(bad));
    PB_CHECK(d->zone_names().size() > 400);
}

PB_TEST(offsets_follow_the_summer_time_rules_in_both_hemispheres_and_at_fixed_zones) {
    auto d = db();
    PB_CHECK_EQ(d->offset_at("Europe/Berlin", utc(2026, 1, 15, 12)), 3600);
    PB_CHECK_EQ(d->offset_at("Europe/Berlin", utc(2026, 7, 1, 12)), 7200);
    PB_CHECK_EQ(d->offset_at("America/New_York", utc(2026, 1, 15, 12)), -18000);
    PB_CHECK_EQ(d->offset_at("America/New_York", utc(2026, 7, 1, 12)), -14400);
    PB_CHECK_EQ(d->offset_at("Australia/Sydney", utc(2026, 1, 15, 12)), 39600);   // southern summer
    PB_CHECK_EQ(d->offset_at("Australia/Sydney", utc(2026, 7, 1, 12)), 36000);
    PB_CHECK_EQ(d->offset_at("Asia/Kolkata", utc(2026, 7, 1)), 19800);
    PB_CHECK_EQ(d->offset_at("Asia/Kathmandu", utc(2026, 7, 1)), 20700);
    PB_CHECK_EQ(d->offset_at("UTC", utc(2026, 7, 1)), 0);
    PB_CHECK_EQ(d->offset_at("Asia/Tokyo", utc(2026, 7, 1)), 32400);
}

PB_TEST(the_rule_after_the_last_listed_change_comes_from_the_posix_footer) {
    auto d = db();
    // 2060 and 2100 are far beyond the transitions that a compact file lists
    PB_CHECK_EQ(d->offset_at("Europe/Berlin", utc(2060, 7, 1, 12)), 7200);
    PB_CHECK_EQ(d->offset_at("Europe/Berlin", utc(2060, 1, 1, 12)), 3600);
    PB_CHECK_EQ(d->offset_at("America/New_York", utc(2100, 7, 1, 12)), -14400);
    PB_CHECK_EQ(d->offset_at("Australia/Sydney", utc(2100, 1, 1, 12)), 39600);
    PB_CHECK_EQ(d->offset_at("Australia/Sydney", utc(2100, 7, 1, 12)), 36000);
}

PB_TEST(history_before_the_first_transition_and_far_in_the_past_does_not_crash) {
    auto d = db();
    (void)d->offset_at("Europe/Berlin", utc(1800, 1, 1));
    (void)d->offset_at("Europe/Berlin", utc(1, 1, 1));
    (void)d->offset_at("Europe/Berlin", -(int64_t(1) << 60));
    (void)d->offset_at("Europe/Berlin", int64_t(1) << 60);
    PB_CHECK_EQ(d->offset_at("Europe/Berlin", utc(1950, 7, 1)), 3600);   // standard time, no summer time in 1950 in Germany
}

PB_TEST(utc_to_local_gives_the_wall_clock_of_the_zone) {
    auto d = db();
    const pbo::LocalDateTime l = d->utc_to_local("Europe/Berlin", utc(2026, 10, 14, 22, 30, 15));
    PB_CHECK(l == (pbo::LocalDateTime{2026, 10, 15, 0, 30, 15}));
    const pbo::LocalDateTime n = d->utc_to_local("America/New_York", utc(2026, 1, 1, 3, 0, 0));
    PB_CHECK(n == (pbo::LocalDateTime{2025, 12, 31, 22, 0, 0}));
}

PB_TEST(local_to_utc_inverts_it_for_ordinary_times) {
    auto d = db();
    PB_CHECK_EQ(d->local_to_utc("Europe/Berlin", {2026, 10, 14, 12, 0, 0}), utc(2026, 10, 14, 10));
    PB_CHECK_EQ(d->local_to_utc("Europe/Berlin", {2026, 1, 15, 12, 0, 0}), utc(2026, 1, 15, 11));
    PB_CHECK_EQ(d->local_to_utc("America/New_York", {2026, 7, 1, 9, 0, 0}), utc(2026, 7, 1, 13));
    PB_CHECK_EQ(d->local_to_utc("Asia/Kolkata", {2026, 7, 1, 9, 0, 0}), utc(2026, 7, 1, 3, 30));
    for (int64_t t = utc(2026, 1, 1); t < utc(2027, 1, 1); t += 3 * 3600 + 17 * 60) {   // outside the changes the round trip is exact
        const pbo::LocalDateTime l = d->utc_to_local("Europe/Berlin", t);
        const int64_t back = d->local_to_utc("Europe/Berlin", l);
        const bool near_change = (t > utc(2026, 3, 28) && t < utc(2026, 3, 30)) || (t > utc(2026, 10, 24) && t < utc(2026, 10, 26));
        if (!near_change) PB_CHECK_EQ(back, t);
    }
}

PB_TEST(an_unknown_zone_name_is_read_as_utc_and_must_be_checked_before) {
    auto d = db();
    PB_CHECK_EQ(d->offset_at("Nowhere/Land", utc(2026, 7, 1)), 0);
}

PB_TEST_MAIN()
