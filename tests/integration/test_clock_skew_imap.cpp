// T168/T174: the clock of the reader against the time the MAIL server stamps on a message the app has just appended (a mirrored draft, a copy in Sent).
// The APPEND carries no date, so the server stamps its own; one UID FETCH (INTERNALDATE) reads it back on the same connection. Above 600 seconds the app
// warns; with no APPEND in the sync there is no stamp and no warning. UNVERIFIED against real servers (fake servers).
#include <ctime>
#include <regex>

#include "outbox_env.h"
#include "pbo/clock_check.h"

PBO_FAKE_SERVER_TEST();

using pbtest::OutboxEnv;

namespace {

// the fake server stamps with the real clock + an offset; the store's clock is fixed: make the server show `server_minus_device` seconds more
void server_ahead_by(OutboxEnv& e, int64_t server_minus_device) {
    e.imap->clock_offset_s = (e.clock.t + server_minus_device) - static_cast<int64_t>(std::time(nullptr));
}
bool near(int64_t value, int64_t expected) { return value >= expected - 10 && value <= expected + 10; }

}  // namespace

PB_TEST(a_mirrored_draft_stamps_the_servers_time_and_a_reader_behind_the_server_is_noticed) {
    OutboxEnv e;
    server_ahead_by(e, 3600);                       // the reader is an hour behind
    e.draft("Plan");
    PB_CHECK_EQ(e.mirror().mirrored, 1);
    const auto skew = e.store->sync_state().clock_skew_s;
    PB_CHECK(skew.has_value());
    PB_CHECK(near(*skew, -3600));
    PB_CHECK(pbo::clock_skew_is_large(*skew));
}

PB_TEST(a_reader_ahead_of_the_server_is_noticed_too) {
    OutboxEnv e;
    server_ahead_by(e, -7200);
    e.draft("Plan");
    e.mirror();
    const auto skew = e.store->sync_state().clock_skew_s;
    PB_CHECK(skew.has_value() && near(*skew, 7200) && pbo::clock_skew_is_large(*skew));
}

PB_TEST(a_difference_of_ten_minutes_or_less_does_not_warn) {
    OutboxEnv e;
    server_ahead_by(e, 540);
    e.draft("Plan");
    e.mirror();
    const auto skew = e.store->sync_state().clock_skew_s;
    PB_CHECK(skew.has_value() && near(*skew, -540));
    PB_CHECK(!pbo::clock_skew_is_large(*skew));
}

PB_TEST(the_copy_in_sent_is_a_stamp_too) {
    OutboxEnv e;
    server_ahead_by(e, -1800);
    const int64_t id = e.queue("Hello");
    e.run();
    PB_CHECK_EQ(e.imap->message_count("Sent"), 1);
    (void)id;
    const auto skew = e.store->sync_state().clock_skew_s;
    PB_CHECK(skew.has_value() && near(*skew, 1800));
}

PB_TEST(the_append_carries_no_date_so_that_the_server_stamps_it) {
    OutboxEnv e;
    e.draft("Plan");
    e.mirror();
    const std::regex date(R"(\d{1,2}-[A-Z][a-z]{2}-\d{4})");
    for (const std::string& c : e.imap->commands())
        if (c.find("APPEND") != std::string::npos) PB_CHECK(!std::regex_search(c, date));
}

PB_TEST(without_an_append_there_is_no_stamp_and_no_warning) {
    OutboxEnv e;
    server_ahead_by(e, 7200);
    e.mirror();                                     // no draft: nothing is appended
    PB_CHECK(!e.store->sync_state().clock_skew_s.has_value());
}

PB_TEST_MAIN()
