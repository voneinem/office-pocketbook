#pragma once
// Calendar data as the store keeps and hands it out (data-model.md: account_calendar, calendar, event, occurrence).

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace pbo {

struct CalendarAccount {
    std::string caldav_url;     // "" = no server account
    std::string username;       // the password lives in the credentials file
    std::string feed_url;       // "" = no feed
    std::string feed_name;
    int64_t created = 0;        // set by the store on the first save
};

struct Calendar {
    int64_t id = 0;
    std::string kind;           // "caldav" | "feed"
    std::string href;           // path on the server, or the feed URL
    std::string name;
    bool writable = false;
    bool shown = true;          // the user's choice (the calendar selection)
    bool supports_vevent = true;
    std::string ctag, sync_token, feed_etag, feed_last_modified;
    bool present_on_server = true;
    int64_t last_sync_at = 0;
};

struct OccurrenceRow {
    int64_t start_utc = 0, end_utc = 0;
    std::string local_date;     // "YYYY-MM-DD" in the user's zone
    bool all_day = false;
    std::string recurrence_id;  // "" for a single event
};

// One event as it is written after a pull (everything the lists need, plus the raw text)
struct EventUpsert {
    std::string href, etag, uid, ics;
    std::string summary, location, description, attendees;
    bool all_day = false;
    std::string tzid;                           // IANA name, "" for floating/UTC/all-day
    std::optional<int64_t> start_utc, end_utc;
    std::string start_local, end_local;         // wall clock, "YYYY-MM-DDTHH:MM:SS" ("YYYY-MM-DD" for all-day)
    std::string rrule, note;
    std::vector<OccurrenceRow> occurrences;     // inside the window; none = the event is not stored (and an old copy is removed)
};

struct CalendarPullBatch {
    std::vector<EventUpsert> upserts;
    std::vector<std::string> removed_hrefs;
    // A complete list: every synced event of the calendar whose href is not in `listed_hrefs` is removed
    bool full = false;
    std::set<std::string> listed_hrefs;
    // The cursor of the calendar, written in the same transaction
    std::string ctag, sync_token, feed_etag, feed_last_modified;
    int64_t synced_at = 0;
};

struct StoredEventRef {
    int64_t id = 0;
    std::string href, etag, uid, sync_state;
};

struct AgendaItem {
    int64_t event_id = 0;
    std::string recurrence_id;
    int64_t start_utc = 0, end_utc = 0;
    std::string local_date;
    bool all_day = false;
    std::string summary, location;
    int64_t calendar_id = 0;
    std::string calendar_name;
    bool writable = false;
    std::string sync_state;     // 'synced' or one of the pending / conflict states
};

struct EventView {
    int64_t id = 0;
    int64_t calendar_id = 0;
    std::string calendar_name, calendar_kind;
    bool calendar_writable = false;
    std::string href, etag, uid, ics;
    std::string summary, location, description, attendees;
    bool all_day = false;
    std::string tzid;
    std::optional<int64_t> start_utc, end_utc;
    std::string start_local, end_local, rrule, note, sync_state;
};

// An event with work that the server has not seen yet, as the push needs it.
struct PendingEvent {
    int64_t id = 0, calendar_id = 0;
    std::string calendar_href, calendar_kind;
    bool calendar_writable = false, calendar_present = true;
    std::string href, etag, uid, ics, summary;
    std::string sync_state;     // 'pending_create' | 'pending_update' | 'pending_delete'
};

// An event that needs the user's decision: a conflict (both versions kept) or one that was deleted on the server.
struct EventProblem {
    int64_t event_id = 0;
    std::string state;          // 'conflict' | 'server_deleted'
    std::string summary, calendar_name;
    bool deleting = false;      // conflict: the user's side is a request to delete
    std::string server_ics, server_etag;   // conflict only
};

enum class ConflictChoice { KeepMine, KeepServer, KeepBoth };

}  // namespace pbo
