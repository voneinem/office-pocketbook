#include "pbo/event_edit.h"

#include "pbo/calendar_sync.h"

namespace pbo {

namespace {

Result<int64_t> failure(const std::string& key, const std::string& detail = std::string()) {
    Result<int64_t> r;
    r.error = make_error(ErrorClass::Storage, key, detail.empty() ? key : detail);
    return r;
}

}  // namespace

std::string event_edit_block_reason(const EventView& e, const TzDatabase& tz) {
    if (e.calendar_kind == "feed") return "feed";
    if (!e.calendar_writable) return "read_only";
    if (e.sync_state == "conflict") return "in_conflict";
    if (e.sync_state == "pending_delete") return "delete_pending";
    if (is_series(e.ics, tz)) return "series_read_only";
    return "";
}

Result<EventEdit> edit_of_event(Store& store, const TzDatabase& tz, int64_t event_id) {
    Result<EventEdit> out;
    const auto view = store.event(event_id);
    if (!view) {
        out.error = make_error(ErrorClass::Storage, "not_found", "no such event");
        return out;
    }
    const ParsedCalendar parsed = parse_ical(view->ics, tz);
    if (parsed.events.empty()) {
        out.error = make_error(ErrorClass::Protocol, "not_editable", "the event cannot be read");
        return out;
    }
    const VEvent* shown = &parsed.events.front();
    for (const VEvent& e : parsed.events)
        if (e.recurrence_id.empty()) {
            shown = &e;
            break;
        }
    out.value = event_edit_of(*shown);
    return out;
}

Result<int64_t> create_event(EventEditDeps& deps, int64_t calendar_id, const EventEdit& edit) {
    if (const std::string bad = validate_event_edit(edit, deps.tz); !bad.empty()) return failure(bad);
    const std::string zone = user_zone_name(deps.store, deps.tz);
    if (zone.empty()) return failure("zone_unresolved", "the time zone of the user is not known yet");
    const int64_t now = deps.clock.now();
    const std::string ics = new_event_ics(edit, make_event_uid(deps.random), now);
    EventUpsert record;
    if (!build_event_upsert(std::string(), std::string(), ics, deps.tz, zone, now, &record)) return failure("invalid_event", "the event could not be built");
    return deps.store.save_event_local(calendar_id, 0, record);
}

Result<int64_t> change_event(EventEditDeps& deps, int64_t event_id, const EventEdit& edit) {
    const auto view = deps.store.event(event_id);
    if (!view) return failure("not_found", "no such event");
    if (const std::string why = event_edit_block_reason(*view, deps.tz); !why.empty()) return failure(why == "feed" ? "read_only" : why);
    const std::string zone = user_zone_name(deps.store, deps.tz);
    if (zone.empty()) return failure("zone_unresolved", "the time zone of the user is not known yet");
    const int64_t now = deps.clock.now();
    const EditResult r = apply_edit(view->ics, edit, deps.tz, now);
    if (!r.ok) return failure(r.error);
    Result<int64_t> same;
    same.value = event_id;
    if (!r.changed) return same;   // nothing differs: nothing is written, nothing is sent
    EventUpsert record;
    if (!build_event_upsert(view->href, view->etag, r.ics, deps.tz, zone, now, &record)) return failure("invalid_event", "the event could not be built");
    return deps.store.save_event_local(view->calendar_id, event_id, record);
}

Error delete_event(Store& store, int64_t event_id) { return store.request_event_delete(event_id); }

Error resolve_event_conflict(EventEditDeps& deps, int64_t event_id, ConflictChoice choice) {
    if (choice == ConflictChoice::KeepMine) return deps.store.keep_my_event(event_id);

    const auto view = deps.store.event(event_id);
    if (!view) return make_error(ErrorClass::Storage, "not_found", "no such event");
    std::string server_ics, server_etag;
    bool found = false;
    for (const EventProblem& p : deps.store.event_problems())
        if (p.event_id == event_id && p.state == "conflict") {
            server_ics = p.server_ics;
            server_etag = p.server_etag;
            found = true;
        }
    if (!found) return make_error(ErrorClass::Storage, "bad_state", "the event is not in conflict");
    const std::string zone = user_zone_name(deps.store, deps.tz);
    if (zone.empty()) return make_error(ErrorClass::Unsupported, "zone_unresolved", "the time zone of the user is not known yet");
    const int64_t now = deps.clock.now();
    EventUpsert server;
    if (!build_event_upsert(view->href, server_etag, server_ics, deps.tz, zone, now, &server))
        return make_error(ErrorClass::Protocol, "unreadable_server_version", "the server's version cannot be read");
    if (choice == ConflictChoice::KeepServer) return deps.store.keep_server_event(event_id, server);

    // keep both: the user's version becomes a new event with its own UID
    EventUpsert mine;
    const std::string ics = replace_ical_uid(view->ics, make_event_uid(deps.random));
    if (!build_event_upsert(std::string(), std::string(), ics, deps.tz, zone, now, &mine)) return make_error(ErrorClass::Protocol, "invalid_event", "the event could not be built");
    return deps.store.keep_both_events(event_id, server, mine);
}

}  // namespace pbo
