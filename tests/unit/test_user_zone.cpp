// T094 (E12, FR-023): the user's own zone. Stored choice, then TZ, then the /etc/localtime target; never a silent UTC.
#include <string>

#include "pbo/tz.h"
#include "test_support.h"

namespace {

std::unique_ptr<pbo::TzDatabase> db() {
    auto d = pbo::TzDatabase::open(std::string(PBO_SOURCE_DIR) + "/third_party/tzdata/zoneinfo");
    if (!d) PB_FAIL_AT("the bundled zone data cannot be opened");
    return d;
}

}  // namespace

PB_TEST(nothing_known_means_unresolved_with_no_utc_fallback) {
    auto d = db();
    const auto z = pbo::resolve_user_zone(std::nullopt, *d, {"", ""});
    PB_CHECK(z.state == pbo::ZoneChoice::State::Unresolved);
    PB_CHECK_EQ(z.name, std::string(""));
}

PB_TEST(a_stored_choice_wins_over_tz_and_the_localtime_link) {
    auto d = db();
    const auto z = pbo::resolve_user_zone(std::string("Asia/Tokyo"), *d, {"Europe/Paris", "/usr/share/zoneinfo/Europe/Berlin"});
    PB_CHECK(z.state == pbo::ZoneChoice::State::Resolved);
    PB_CHECK_EQ(z.name, std::string("Asia/Tokyo"));
}

PB_TEST(a_stored_name_that_is_not_bundled_is_ignored) {
    auto d = db();
    auto z = pbo::resolve_user_zone(std::string("Mars/Olympus"), *d, {"Europe/Paris", ""});
    PB_CHECK(z.state == pbo::ZoneChoice::State::Resolved);
    PB_CHECK_EQ(z.name, std::string("Europe/Paris"));
    z = pbo::resolve_user_zone(std::string("Mars/Olympus"), *d, {"", ""});
    PB_CHECK(z.state == pbo::ZoneChoice::State::Unresolved);
}

PB_TEST(tz_names_a_zone_with_or_without_a_leading_colon) {
    auto d = db();
    PB_CHECK_EQ(pbo::resolve_user_zone(std::nullopt, *d, {"Europe/Paris", ""}).name, std::string("Europe/Paris"));
    PB_CHECK_EQ(pbo::resolve_user_zone(std::nullopt, *d, {":Europe/Paris", ""}).name, std::string("Europe/Paris"));
}

PB_TEST(tz_values_that_are_not_a_bundled_zone_name_fall_through_to_the_link) {
    auto d = db();
    for (const char* tz : {"CET-1CEST,M3.5.0,M10.5.0/3", "garbage", ":/etc/localtime", "../../etc/passwd", "EST5EDTX"}) {
        auto z = pbo::resolve_user_zone(std::nullopt, *d, {tz, ""});
        PB_CHECK(z.state == pbo::ZoneChoice::State::Unresolved);
        z = pbo::resolve_user_zone(std::nullopt, *d, {tz, "/usr/share/zoneinfo/Europe/Berlin"});
        PB_CHECK_EQ(z.name, std::string("Europe/Berlin"));
    }
}

PB_TEST(the_localtime_link_target_is_read_after_the_zoneinfo_directory) {
    auto d = db();
    for (const char* target : {"/usr/share/zoneinfo/Europe/Berlin", "../usr/share/zoneinfo/Europe/Berlin", "/usr/share/zoneinfo/posix/Europe/Berlin",
                               "/usr/share/zoneinfo/right/Europe/Berlin", "/mnt/rootfs/usr/share/zoneinfo/Europe/Berlin"}) {
        const auto z = pbo::resolve_user_zone(std::nullopt, *d, {"", target});
        PB_CHECK(z.state == pbo::ZoneChoice::State::Resolved);
        PB_CHECK_EQ(z.name, std::string("Europe/Berlin"));
    }
    for (const char* target : {"/etc/localtime-berlin", "/usr/share/zoneinfo/", "/usr/share/zoneinfo/Mars/Olympus", "/tmp/Berlin", "zoneinfo/../../etc/passwd"})
        PB_CHECK(pbo::resolve_user_zone(std::nullopt, *d, {"", target}).state == pbo::ZoneChoice::State::Unresolved);
}

PB_TEST(an_explicit_utc_is_a_zone_like_any_other_but_is_never_assumed) {
    auto d = db();
    PB_CHECK_EQ(pbo::resolve_user_zone(std::nullopt, *d, {"UTC", ""}).name, std::string("UTC"));
    PB_CHECK(pbo::resolve_user_zone(std::nullopt, *d, {"", ""}).state == pbo::ZoneChoice::State::Unresolved);
}

PB_TEST_MAIN()
