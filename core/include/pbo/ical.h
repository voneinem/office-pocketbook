#pragma once
// iCalendar (RFC 5545 subset of contracts/caldav-ical-subset.md section 3): a total, bounded parser and the expansion of one resource
// (master + overrides) into occurrences. VTIMEZONE blocks are ignored (documented): TZID values are resolved through the bundled IANA
// database. Unknown properties, VALARM and the like are not interpreted here; the store keeps the raw text.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pbo/civil.h"
#include "pbo/tz.h"
#include "pbo/types.h"

namespace pbo {

struct IcalLimits {
    size_t max_bytes = 8u << 20;       // input per resource
    size_t max_lines = 200000;
    size_t max_line_bytes = 1u << 20;
    size_t max_components = 2000;
    size_t max_attendees = 200;
    size_t max_text = 64u << 10;       // summary, description, location (display copy; the raw text keeps everything)
};

struct VEvent {
    std::string uid, summary, description, location;
    std::string status;                // upper case: CONFIRMED, TENTATIVE, CANCELLED or ""
    std::string transp, klass, organizer;
    std::string rrule;                 // the RRULE value as written, "" if none
    std::string tzid;                  // canonical IANA name of the event's zone; "UTC" for a time written with Z; "" for floating and all-day
    // For an override of one instance of a series: which instance, as the wall clock of this event's own zone ("YYYYMMDD" or
    // "YYYYMMDDTHHMMSS"); "" for a master
    std::string recurrence_id;
    std::string recurrence_range;      // the RANGE parameter, e.g. "THISANDFUTURE" (not supported: noted)
    std::vector<std::string> attendees;        // "Name <address>" or "address"
    // Excluded and added instances in the wall clock of the event's zone ("YYYYMMDD" for a date, "YYYYMMDDTHHMMSS" for a time)
    std::vector<std::string> exdates, rdates;
    bool all_day = false;
    bool floating = false;             // timed, but no zone: the wall clock is kept as written
    std::optional<LocalDateTime> start, end;   // wall clock of the event's zone; for all-day the end date is exclusive
    std::optional<int64_t> start_utc, end_utc; // set for timed events with a zone (also "UTC")
    int sequence = 0;
    // '' | 'rrule_unsupported' | 'tz_unknown' | 'no_end' | 'truncated' | 'range_unsupported' (the first that applies)
    std::string note;
};

struct ParsedCalendar {
    std::vector<VEvent> events;
    bool ok = true;                    // false: the input exceeded a limit or has no calendar at all (events may still hold what was read)
    std::string note;                  // '' | 'too_large' | 'too_many_lines' | 'too_many_components' | 'not_a_calendar'
    int skipped = 0;                   // VEVENTs that could not be used (no UID, no or invalid DTSTART)
};

ParsedCalendar parse_ical(std::string_view ics, const TzDatabase& tz, const IcalLimits& limits = IcalLimits());

// The events of a resource apart: one text per UID ("BEGIN:VCALENDAR" + the original header lines + that UID's VEVENT blocks, overrides
// included, + "END:VCALENDAR"). VTIMEZONE blocks are dropped (TZIDs are resolved through the bundled database). Used for feeds, which hold
// many events in one file. Lines are folded at 75 octets.
struct IcalPart {
    std::string uid;
    std::string ics;
};
std::vector<IcalPart> split_ical_by_uid(std::string_view ics, const IcalLimits& limits = IcalLimits());

// One content line folded at 75 octets (CRLF-terminated; continuation lines start with a blank; UTF-8 characters are not cut).
std::string fold_ical_line(std::string_view line);

struct Occurrence {
    int64_t start_utc = 0, end_utc = 0;
    std::string local_date;            // "YYYY-MM-DD" in the user's zone (all-day: the date itself)
    bool all_day = false;
    std::string recurrence_id;         // "" for a single event; the wall-clock key of the instance for a series
    // which VEvent of the resource this came from (the master or an override), for display
    size_t event_index = 0;
};

// Occurrences of one resource that overlap [from_utc, to_utc), at most 5,000 per series. All-day events are placed at midnight of the
// user's zone, floating events are read in the user's zone. An unknown `user_zone` gives no occurrences (no assumed zone, FR-023).
std::vector<Occurrence> expand(const ParsedCalendar& resource, const TzDatabase& tz, const std::string& user_zone, int64_t from_utc, int64_t to_utc);

// ---- writing events (contracts/caldav-ical-subset.md section 5) ----

// What the user edits. Timed events: wall clock in `tzid` (an IANA name; "UTC" is written with Z; "" is a floating time). All-day
// events: dates only (the time of day is ignored) and `end` is the first day NOT included, like iCalendar.
struct EventEdit {
    std::string title, location, description;
    bool all_day = false;
    LocalDateTime start, end;
    std::string tzid;
};

// The fields of a parsed event as an edit (the starting point of the editor).
EventEdit event_edit_of(const VEvent& e);

// "" when the edit can be written, else a key: "title_missing", "too_long", "date_invalid", "end_before_start", "zone_unknown".
std::string validate_event_edit(const EventEdit& edit, const TzDatabase& tz);

// True when the resource is a repeating series (RRULE/RDATE, or overrides of single instances): such an event is read only here.
bool is_series(std::string_view raw_ics, const TzDatabase& tz);

// A random UID: 128 bits in hex + "@pboffice.invalid".
std::string make_event_uid(Random& random);

// The same resource with another UID (every VEVENT); used to keep the user's version as a new event beside the server's.
std::string replace_ical_uid(std::string_view raw_ics, const std::string& new_uid);

// A new resource (VCALENDAR with one VEVENT). The edit must have passed validate_event_edit. `now_utc` stamps DTSTAMP and CREATED.
std::string new_event_ics(const EventEdit& edit, const std::string& uid, int64_t now_utc);

struct EditResult {
    bool ok = false;
    bool changed = false;      // false: the edit equals what is there; `ics` is the input unchanged
    std::string ics;
    std::string error;         // when !ok: "series_read_only", "not_editable" or a key of validate_event_edit
};
// Lossless edit of the single VEVENT of `raw_ics`: only properties that differ from the event as parsed are rewritten; SEQUENCE is
// raised, DTSTAMP and LAST-MODIFIED are set to `now_utc`; VALARM, ATTENDEE, unknown properties and other components (VTIMEZONE ...)
// are kept as they are. Untouched content lines are re-folded at 75 octets and written with CRLF.
EditResult apply_edit(std::string_view raw_ics, const EventEdit& edit, const TzDatabase& tz, int64_t now_utc);

}  // namespace pbo
