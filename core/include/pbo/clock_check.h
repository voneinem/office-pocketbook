#pragma once
// A wrong clock on the reader breaks more than the displayed time: events at the wrong hour, "today" in the wrong place, certificates that seem
// expired. The app therefore compares its clock with the time a server reports during a normal sync (the HTTP Date header of the calendar server)
// and warns when they differ by more than ten minutes. No connection of its own is made for this.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "pbo/imap_transport.h"
#include "pbo/store.h"

namespace pbo {

constexpr int64_t kClockWarnSeconds = 600;

// "Wed, 14 Oct 2026 12:00:00 GMT" (RFC 9110 IMF-fixdate) -> seconds since 1970-01-01 UTC; anything else is nullopt.
std::optional<int64_t> parse_http_date(std::string_view text);

// device time minus server time (positive: the reader's clock is ahead)
inline int64_t clock_skew(int64_t device_now, int64_t server_time) { return device_now - server_time; }
inline bool clock_skew_is_large(int64_t skew) { return skew > kClockWarnSeconds || skew < -kClockWarnSeconds; }
// how far off, in whole minutes (rounded up), for the message
inline int clock_skew_minutes(int64_t skew) {
    const int64_t a = skew < 0 ? -skew : skew;
    return static_cast<int>((a + 59) / 60);
}

// After a successful APPEND (a mirrored draft, a copy in Sent): the INTERNALDATE the server stamped on that message (the APPEND carried no date) is
// the server's time. One UID FETCH on the connection that is open already; the skew is stored. Nothing here can fail a sync: an answer that cannot be
// read changes nothing.
void note_server_stamp(ImapTransport& imap, Store& store, const Clock& clock, const std::string& mailbox_raw_name, uint32_t uid);

}  // namespace pbo
