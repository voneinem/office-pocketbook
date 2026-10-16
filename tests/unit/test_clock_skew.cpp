// T168 (part): the device clock against the time a server reports (the HTTP Date header of the calendar server). Above 600 seconds the app warns.
#include <string>

#include "pbo/clock_check.h"
#include "test_support.h"

PB_TEST(http_dates_are_parsed_and_garbage_is_ignored) {
    PB_CHECK_EQ(pbo::parse_http_date("Wed, 14 Oct 2026 12:00:00 GMT").value_or(-1), int64_t(1791979200));
    PB_CHECK_EQ(pbo::parse_http_date("Thu, 01 Jan 1970 00:00:00 GMT").value_or(-1), int64_t(0));
    PB_CHECK_EQ(pbo::parse_http_date("  Sun, 29 Feb 2028 23:59:59 GMT ").value_or(-1), int64_t(1835481599));
    for (const char* bad : {"", "yesterday", "Wed, 14 Oct 2026 12:00:00", "Wed, 14 Foo 2026 12:00:00 GMT", "Wed, 32 Oct 2026 12:00:00 GMT", "Wed, 14 Oct 2026 25:00:00 GMT",
                            "Wed, 14 Oct 2026 12:00:00 PST", "Wed, 14 Oct 1969 12:00:00 GMT"})
        PB_CHECK(!pbo::parse_http_date(bad).has_value());
}

PB_TEST(a_skew_is_the_device_time_minus_the_servers_and_only_more_than_ten_minutes_warns) {
    PB_CHECK_EQ(pbo::clock_skew(1000, 400), int64_t(600));
    PB_CHECK(!pbo::clock_skew_is_large(600));
    PB_CHECK(pbo::clock_skew_is_large(601));
    PB_CHECK(!pbo::clock_skew_is_large(-600));
    PB_CHECK(pbo::clock_skew_is_large(-601));
    PB_CHECK(!pbo::clock_skew_is_large(0));
    PB_CHECK_EQ(pbo::clock_skew_minutes(-3600), 60);      // for the message: how far off, in whole minutes
    PB_CHECK_EQ(pbo::clock_skew_minutes(7260), 121);
}

PB_TEST_MAIN()
