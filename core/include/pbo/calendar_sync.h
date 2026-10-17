#pragma once
// The calendar side of a synchronisation (research R-18, contracts/caldav-ical-subset.md): discovery, change detection, fetching, the window
// [today - 30 days, today + 365 days], the occurrence cache in the user's zone, and the read-only feed. Every step commits data and cursor
// together; an interrupted run converges when it is run again. Local work (events with sync_state not 'synced') is never touched by the pull.

#include <string>
#include <vector>

#include "pbo/caldav.h"
#include "pbo/calendar_types.h"
#include "pbo/http_transport.h"
#include "pbo/store.h"
#include "pbo/types.h"
#include "pbo/tz.h"

namespace pbo {

constexpr int kCalendarWindowPastDays = 30;
constexpr int kCalendarWindowFutureDays = 365;

struct CalendarSyncDeps {
    Store& store;
    HttpTransport& http;
    const TzDatabase& tz;
    Clock& clock;
    Cancel& cancel;
};

struct CalendarCounts {
    int calendars = 0;        // calendars looked at
    int changed = 0;          // events written
    int removed = 0;          // events removed because the server removed them
    int unreadable = 0;       // resources skipped because they could not be read (FR-040)
    int feed_events = 0;
};

// The user's zone as the settings, TZ and /etc/localtime give it ("" = Unresolved). Setting key: tz_override.
std::string user_zone_name(const Store& store, const TzDatabase& tz);
// The same with the environment given (tests); nullptr reads the real TZ and /etc/localtime.
std::string user_zone_name(const Store& store, const TzDatabase& tz, const ZoneEnvironment* environment);

// Everything the occurrence cache depends on is recorded in the settings (zone, tz data release, the day of the window); when one of them
// changed, the occurrences of all events are expanded again from their stored text. No-op otherwise. Error "zone_unresolved" when no zone is known.
Error refresh_occurrences(Store& store, const TzDatabase& tz, Clock& clock);

// Occurrences of one stored resource inside the window of `now`, in the zone `zone`.
std::vector<OccurrenceRow> occurrences_of(const std::string& ics, const TzDatabase& tz, const std::string& zone, int64_t now);

// The event row for one resource: its fields and the occurrences inside the window of `now` in `zone`. False if it holds no usable event.
bool build_event_upsert(const std::string& href, const std::string& etag, const std::string& ics, const TzDatabase& tz, const std::string& zone, int64_t now, EventUpsert* out);

struct PushCounts {
    int created = 0, updated = 0, deleted = 0;
    int conflicts = 0;         // 412: both versions kept, the user decides
    int server_deleted = 0;    // the event or its calendar is gone on the server: the edited version is kept and marked
    int refused = 0;           // 403 / 413 / 507: the change stays waiting
};

// The push: every event with local work (Store::pending_events) goes to its calendar with a conditional request (If-None-Match: * for a new
// event, If-Match for a change or a deletion). The mark clears only when the server confirmed. A 412 keeps both versions (never an
// overwrite); a request that may have been executed but whose answer was lost is recognised at the next push by the content (no duplicate).
// Errors: the first problem ("auth_failed", "server_unreachable", "read_only" for a refusal, ...); a connection or login problem stops the push.
Error push_events(CalendarSyncDeps& deps, const std::string& calendar_password, PushCounts* counts);

// The pull: calendars of the server account and the feed. Without a calendar account it does nothing. Errors: "zone_unresolved" (no step ran: the
// user is asked for the zone first), or the first problem of a calendar ("auth_failed", "server_unreachable", ...); a problem in one calendar does
// not stop the others unless the connection or the login is the problem.
Error pull_calendars(CalendarSyncDeps& deps, const std::string& calendar_password, CalendarCounts* counts);

}  // namespace pbo
