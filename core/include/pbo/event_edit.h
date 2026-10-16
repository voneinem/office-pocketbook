#pragma once
// Editing events (US4): turns what the user entered into the event text and the store records, and resolves conflicts. Nothing here talks to the
// network; the push (calendar_sync.h) sends what is waiting. Repeating events are read only (FR-025, clarification of 2026-10-13).

#include <string>

#include "pbo/calendar_types.h"
#include "pbo/ical.h"
#include "pbo/store.h"
#include "pbo/types.h"
#include "pbo/tz.h"

namespace pbo {

struct EventEditDeps {
    Store& store;
    const TzDatabase& tz;
    Clock& clock;
    Random& random;
};

// "" when the event can be edited, else why not: "feed" / "read_only" (calendar), "series_read_only", "in_conflict", "delete_pending", "calendar_gone".
std::string event_edit_block_reason(const EventView& event, const TzDatabase& tz);

// The editor's starting point for a stored event.
Result<EventEdit> edit_of_event(Store& store, const TzDatabase& tz, int64_t event_id);

// A new event in a calendar. Errors: the keys of validate_event_edit, "zone_unresolved", "read_only", "not_found".
Result<int64_t> create_event(EventEditDeps& deps, int64_t calendar_id, const EventEdit& edit);
// An edited event; only the changed properties are rewritten. Errors: as above, "series_read_only", "in_conflict", "delete_pending". An edit that
// changes nothing is accepted and writes nothing.
Result<int64_t> change_event(EventEditDeps& deps, int64_t event_id, const EventEdit& edit);
// Delete (the caller asked the user first). A repeating event may be deleted as a whole.
Error delete_event(Store& store, int64_t event_id);

// The user's decision about a conflict (event_problems(), state 'conflict'). KeepBoth stores the user's version as a new event (new UID).
Error resolve_event_conflict(EventEditDeps& deps, int64_t event_id, ConflictChoice choice);

}  // namespace pbo
