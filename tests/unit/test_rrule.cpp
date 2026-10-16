// T095 (part): parsing of RRULE values, what is outside the subset, and the expansion limits.
#include <string>

#include "pbo/rrule.h"
#include "test_support.h"

namespace {

int64_t at(int y, int mo, int d, int h = 0, int mi = 0, int s = 0) { return pbo::civil::naive_epoch({y, mo, d, h, mi, s}); }
const int64_t kFar = at(2100, 1, 1);
const int64_t kNever = at(1900, 1, 1);

std::vector<std::string> dates(const pbo::RRuleResult& r) {
    std::vector<std::string> out;
    for (const auto& t : r.starts) {
        char b[32];
        snprintf(b, sizeof b, "%04d-%02d-%02d %02d:%02d", t.year, t.month, t.day, t.hour, t.minute);
        out.push_back(b);
    }
    return out;
}

}  // namespace

PB_TEST(a_simple_rule_is_parsed_into_its_parts) {
    const auto r = pbo::parse_rrule("FREQ=WEEKLY;INTERVAL=2;BYDAY=MO,TH;COUNT=10;WKST=SU");
    PB_CHECK(r.supported);
    PB_CHECK(r.freq == pbo::RRule::Freq::Weekly);
    PB_CHECK_EQ(r.interval, 2);
    PB_CHECK_EQ(r.count, 10);
    PB_CHECK_EQ(r.wkst, 0);
    PB_CHECK_EQ(r.by_day.size(), size_t(2));
    PB_CHECK_EQ(r.by_day[0].weekday, 1);
    PB_CHECK_EQ(r.by_day[1].weekday, 4);
}

PB_TEST(ordinals_negative_values_and_until_forms_are_parsed) {
    auto r = pbo::parse_rrule("FREQ=MONTHLY;BYDAY=-1FR,2TU;BYMONTHDAY=-1,15;BYSETPOS=-1;UNTIL=20270630");
    PB_CHECK(r.supported);
    PB_CHECK_EQ(r.by_day[0].ordinal, -1);
    PB_CHECK_EQ(r.by_day[0].weekday, 5);
    PB_CHECK_EQ(r.by_day[1].ordinal, 2);
    PB_CHECK_EQ(r.by_month_day.size(), size_t(2));
    PB_CHECK(r.until.has_value() && r.until->date_only && !r.until->utc);
    PB_CHECK_EQ(r.until->local.year, 2027);
    r = pbo::parse_rrule("freq=daily;until=20270630T120000Z");
    PB_CHECK(r.supported);
    PB_CHECK(r.until.has_value() && !r.until->date_only && r.until->utc);
    PB_CHECK_EQ(r.until->local.hour, 12);
}

PB_TEST(parts_outside_the_subset_make_the_rule_unsupported) {
    for (const char* bad : {"FREQ=SECONDLY", "FREQ=MINUTELY;INTERVAL=5", "FREQ=HOURLY", "FREQ=YEARLY;BYWEEKNO=20", "FREQ=YEARLY;BYYEARDAY=100",
                            "FREQ=DAILY;BYHOUR=9,17", "FREQ=DAILY;BYMINUTE=30", "FREQ=DAILY;BYSECOND=1", "FREQ=YEARLY;RSCALE=HEBREW", "FREQ=MONTHLY;SKIP=FORWARD",
                            "FREQ=WEEKLY;X-FOO=1"}) {
        const auto r = pbo::parse_rrule(bad);
        if (r.supported) PB_FAIL_AT(std::string("accepted: ") + bad);
    }
}

PB_TEST(malformed_rules_are_unsupported_not_errors) {
    for (const char* bad : {"", "FREQ", "FREQ=", "INTERVAL=2", "FREQ=DAILY;INTERVAL=0", "FREQ=DAILY;INTERVAL=-1", "FREQ=DAILY;INTERVAL=x", "FREQ=DAILY;INTERVAL=99999999999",
                            "FREQ=DAILY;COUNT=0", "FREQ=DAILY;COUNT=-3", "FREQ=DAILY;COUNT=2;UNTIL=20270101", "FREQ=WEEKLY;BYDAY=XX", "FREQ=WEEKLY;BYDAY=0MO", "FREQ=WEEKLY;BYDAY=54MO",
                            "FREQ=MONTHLY;BYMONTHDAY=0", "FREQ=MONTHLY;BYMONTHDAY=32", "FREQ=YEARLY;BYMONTH=13", "FREQ=YEARLY;BYMONTH=0", "FREQ=MONTHLY;BYSETPOS=0",
                            "FREQ=DAILY;UNTIL=nonsense", "FREQ=DAILY;UNTIL=20261301", "FREQ=DAILY;WKST=XX", "FREQ=DAILY;;;", "====", "FREQ=DAILY;FREQ=WEEKLY",
                            "FREQ=WEEKLY;BYMONTHDAY=3", "FREQ=WEEKLY;BYDAY=1MO", "FREQ=DAILY;BYDAY=2TU"}) {
        const auto r = pbo::parse_rrule(bad);
        if (r.supported) PB_FAIL_AT(std::string("accepted: ") + bad);
        const auto e = pbo::expand_rrule(r, {2026, 1, 1, 9, 0, 0}, std::nullopt, kNever, kFar);
        PB_CHECK(e.starts.empty());                 // an unsupported rule yields nothing here: the caller adds the single occurrence
    }
}

PB_TEST(expansion_follows_count_interval_and_until) {
    auto r = pbo::parse_rrule("FREQ=DAILY;INTERVAL=2;COUNT=4");
    auto e = pbo::expand_rrule(r, {2026, 1, 1, 9, 30, 0}, std::nullopt, kNever, kFar);
    PB_CHECK(dates(e) == (std::vector<std::string>{"2026-01-01 09:30", "2026-01-03 09:30", "2026-01-05 09:30", "2026-01-07 09:30"}));
    r = pbo::parse_rrule("FREQ=WEEKLY;BYDAY=MO,FR");
    e = pbo::expand_rrule(r, {2026, 1, 5, 8, 0, 0}, at(2026, 1, 16, 8, 0, 0), kNever, kFar);   // UNTIL is inclusive
    PB_CHECK(dates(e) == (std::vector<std::string>{"2026-01-05 08:00", "2026-01-09 08:00", "2026-01-12 08:00", "2026-01-16 08:00"}));
}

PB_TEST(instances_before_emit_from_are_counted_but_not_returned) {
    const auto r = pbo::parse_rrule("FREQ=DAILY;COUNT=10");
    const auto e = pbo::expand_rrule(r, {2026, 1, 1, 9, 0, 0}, std::nullopt, at(2026, 1, 8), kFar);
    PB_CHECK(dates(e) == (std::vector<std::string>{"2026-01-08 09:00", "2026-01-09 09:00", "2026-01-10 09:00"}));
}

PB_TEST(a_rule_that_started_long_ago_does_not_hit_the_guard) {
    const auto r = pbo::parse_rrule("FREQ=DAILY");
    const auto e = pbo::expand_rrule(r, {1990, 1, 1, 9, 0, 0}, std::nullopt, at(2026, 6, 1), at(2026, 6, 4));
    PB_CHECK(!e.guard_hit);
    PB_CHECK_EQ(e.starts.size(), size_t(3));
    const auto y = pbo::parse_rrule("FREQ=YEARLY;INTERVAL=3");
    const auto ey = pbo::expand_rrule(y, {1900, 5, 5, 9, 0, 0}, std::nullopt, at(2026, 1, 1), at(2040, 1, 1));
    PB_CHECK(!ey.guard_hit);
    PB_CHECK(dates(ey) == (std::vector<std::string>{"2026-05-05 09:00", "2029-05-05 09:00", "2032-05-05 09:00", "2035-05-05 09:00", "2038-05-05 09:00"}));
}

PB_TEST(at_most_5000_instances_are_returned) {
    const auto r = pbo::parse_rrule("FREQ=DAILY");
    const auto e = pbo::expand_rrule(r, {2026, 1, 1, 9, 0, 0}, std::nullopt, kNever, at(2100, 1, 1));
    PB_CHECK(e.capped);
    PB_CHECK_EQ(e.starts.size(), size_t(5000));
}

PB_TEST(a_rule_that_never_matches_stops_at_the_guard_quickly) {
    for (const char* rule : {"FREQ=YEARLY;BYMONTH=2;BYMONTHDAY=30", "FREQ=MONTHLY;BYMONTHDAY=31;BYMONTH=2,4", "FREQ=DAILY;BYMONTH=2;BYMONTHDAY=30"}) {
        const auto r = pbo::parse_rrule(rule);
        PB_CHECK(r.supported);
        const auto e = pbo::expand_rrule(r, {2026, 1, 1, 9, 0, 0}, std::nullopt, kNever, kFar);
        PB_CHECK(e.starts.empty());
        PB_CHECK(e.guard_hit || at(2026, 1, 1) > 0);   // either ends by the horizon or by the guard: it returned
    }
    pbo::RRuleLimits tight;
    tight.max_steps = 10;
    const auto r = pbo::parse_rrule("FREQ=YEARLY;BYMONTH=2;BYMONTHDAY=30");
    const auto e = pbo::expand_rrule(r, {2026, 1, 1, 9, 0, 0}, std::nullopt, kNever, kFar, tight);
    PB_CHECK(e.guard_hit);
}

PB_TEST(huge_values_do_not_overflow_or_hang) {
    for (const char* rule : {"FREQ=DAILY;INTERVAL=1000000", "FREQ=YEARLY;INTERVAL=999999", "FREQ=MONTHLY;COUNT=2147483647", "FREQ=WEEKLY;INTERVAL=1000000;BYDAY=MO,TU,WE,TH,FR,SA,SU"}) {
        const auto r = pbo::parse_rrule(rule);
        const auto e = pbo::expand_rrule(r, {2026, 12, 31, 23, 59, 59}, std::nullopt, kNever, kFar);
        PB_CHECK(e.starts.size() <= 5000);
    }
    const auto r = pbo::parse_rrule("FREQ=DAILY");
    (void)pbo::expand_rrule(r, {9999, 12, 31, 23, 59, 59}, std::nullopt, kNever, int64_t(1) << 50);
    (void)pbo::expand_rrule(r, {1, 1, 1, 0, 0, 0}, std::nullopt, kNever, at(30, 1, 1));
}

PB_TEST_MAIN()
