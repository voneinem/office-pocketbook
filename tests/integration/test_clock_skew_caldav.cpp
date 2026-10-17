// T168/T174 (part): a pull from the calendar server measures the device clock against the server's Date header and remembers the difference; no
// extra connection is made. UNVERIFIED against real servers (fake server).
#include "event_env.h"
#include "pbo/clock_check.h"

PBO_FAKE_SERVER_TEST();

using pbtest::EventEnv;
using pbtest::kEventNow;

PB_TEST(a_server_ten_minutes_ahead_of_the_reader_is_noticed) {
    EventEnv e;
    e.server->set_server_time(kEventNow + 1200);      // the reader's clock is 20 minutes behind
    const size_t requests = e.server->log().size();
    PB_CHECK(e.pull().ok());
    PB_CHECK_EQ(e.store->sync_state().clock_skew_s.value_or(0), int64_t(-1200));
    PB_CHECK(pbo::clock_skew_is_large(*e.store->sync_state().clock_skew_s));
    PB_CHECK(e.server->log().size() > requests);       // the measurement came with the normal requests
}

PB_TEST(a_small_difference_is_remembered_but_does_not_warn) {
    EventEnv e;
    e.server->set_server_time(kEventNow - 45);
    PB_CHECK(e.pull().ok());
    PB_CHECK_EQ(e.store->sync_state().clock_skew_s.value_or(0), int64_t(45));
    PB_CHECK(!pbo::clock_skew_is_large(*e.store->sync_state().clock_skew_s));
}

PB_TEST(the_latest_measurement_replaces_the_old_one_so_a_corrected_clock_stops_the_warning) {
    EventEnv e;
    e.server->set_server_time(kEventNow + 7200);
    PB_CHECK(e.pull().ok());
    PB_CHECK(pbo::clock_skew_is_large(*e.store->sync_state().clock_skew_s));
    e.server->set_server_time(kEventNow);
    PB_CHECK(e.pull().ok());
    PB_CHECK(!pbo::clock_skew_is_large(*e.store->sync_state().clock_skew_s));
}

PB_TEST_MAIN()
