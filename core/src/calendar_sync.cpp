#include "pbo/calendar_sync.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <map>
#include <set>

#include "pbo/clock_check.h"
#include "pbo/ical.h"
#include "pbo/log.h"

namespace pbo {

namespace {

std::string local_text(const LocalDateTime& t, bool date_only) {
    char b[32];
    if (date_only) snprintf(b, sizeof b, "%04d-%02d-%02d", t.year, t.month, t.day);
    else snprintf(b, sizeof b, "%04d-%02d-%02dT%02d:%02d:%02d", t.year, t.month, t.day, t.hour, t.minute, t.second);
    return b;
}

void window_of(int64_t now, int64_t* from, int64_t* to) {
    *from = now - static_cast<int64_t>(kCalendarWindowPastDays) * 86400;
    *to = now + static_cast<int64_t>(kCalendarWindowFutureDays) * 86400;
}

std::vector<OccurrenceRow> rows_of(const std::vector<Occurrence>& occ) {
    std::vector<OccurrenceRow> rows;
    rows.reserve(occ.size());
    for (const Occurrence& o : occ) {
        OccurrenceRow r;
        r.start_utc = o.start_utc;
        r.end_utc = o.end_utc;
        r.local_date = o.local_date;
        r.all_day = o.all_day;
        r.recurrence_id = o.recurrence_id;
        rows.push_back(std::move(r));
    }
    return rows;
}

}  // namespace

// The event row for one stored resource; false if it cannot be read (no usable event).
bool build_event_upsert(const std::string& href, const std::string& etag, const std::string& ics, const TzDatabase& tz, const std::string& zone, int64_t now, EventUpsert* out) {
    const ParsedCalendar parsed = parse_ical(ics, tz);
    if (parsed.events.empty()) return false;
    // one event per resource: the first UID (the master, or an override if only that is present)
    const std::string uid = parsed.events.front().uid;
    ParsedCalendar one;
    one.ok = parsed.ok;
    for (const VEvent& e : parsed.events)
        if (e.uid == uid) one.events.push_back(e);
    const VEvent* shown = &one.events.front();
    for (const VEvent& e : one.events)
        if (e.recurrence_id.empty()) { shown = &e; break; }

    int64_t from, to;
    window_of(now, &from, &to);
    EventUpsert u;
    u.href = href;
    u.etag = etag;
    u.uid = uid;
    u.ics = ics;
    u.summary = shown->summary;
    u.location = shown->location;
    u.description = shown->description;
    for (size_t i = 0; i < shown->attendees.size(); ++i) {
        if (i) u.attendees += "\n";
        u.attendees += shown->attendees[i];
    }
    u.all_day = shown->all_day;
    u.tzid = shown->tzid == "UTC" ? std::string() : shown->tzid;
    u.start_utc = shown->start_utc;
    u.end_utc = shown->end_utc;
    if (shown->start) u.start_local = local_text(*shown->start, shown->all_day);
    if (shown->end) u.end_local = local_text(*shown->end, shown->all_day);
    u.rrule = shown->rrule;
    u.note = shown->note;
    u.occurrences = rows_of(expand(one, tz, zone, from, to));
    *out = std::move(u);
    return true;
}

std::string user_zone_name(const Store& store, const TzDatabase& tz, const ZoneEnvironment* environment) {
    const std::string stored = store.setting("tz_override");
    const ZoneChoice c = resolve_user_zone(stored.empty() ? std::nullopt : std::optional<std::string>(stored), tz, environment ? *environment : read_zone_environment());
    return c.state == ZoneChoice::State::Resolved ? c.name : std::string();
}

std::string user_zone_name(const Store& store, const TzDatabase& tz) { return user_zone_name(store, tz, nullptr); }

std::vector<OccurrenceRow> occurrences_of(const std::string& ics, const TzDatabase& tz, const std::string& zone, int64_t now) {
    const ParsedCalendar parsed = parse_ical(ics, tz);
    int64_t from, to;
    window_of(now, &from, &to);
    return rows_of(expand(parsed, tz, zone, from, to));
}

Error refresh_occurrences(Store& store, const TzDatabase& tz, Clock& clock) {
    const std::string zone = user_zone_name(store, tz);
    if (zone.empty()) return make_error(ErrorClass::Unsupported, "zone_unresolved", "the time zone of the user is not known yet");
    const int64_t now = clock.now();
    const std::string day = std::to_string(now / 86400);
    if (store.setting("occ_zone") == zone && store.setting("occ_tzdata") == tz.version() && store.setting("occ_day") == day) return {};
    const Error e = store.rebuild_occurrences([&](const std::string& ics) { return occurrences_of(ics, tz, zone, now); });
    if (!e.ok()) return e;
    store.set_setting("occ_zone", zone);
    store.set_setting("occ_tzdata", tz.version());
    store.set_setting("occ_day", day);
    return {};
}

namespace {

bool stop_class(const Error& e) {
    return e.cls == ErrorClass::Cancelled || e.cls == ErrorClass::Network || e.cls == ErrorClass::Tls || e.cls == ErrorClass::Auth || e.cls == ErrorClass::Storage;
}

Error pull_caldav(CalendarSyncDeps& d, const CalendarAccount& account, const std::string& password, const std::string& zone, CalendarCounts* counts) {
    CalDavClient client(d.http, account.caldav_url, account.username, password);
    auto found = client.discover();
    if (!found.ok()) return found.error;
    if (const auto t = client.last_server_time()) d.store.set_clock_skew(clock_skew(d.clock.now(), *t));   // a wrong clock is noticed here, for free

    std::set<std::string> present;
    for (const CalendarInfo& info : found.value) {
        Calendar c;
        c.kind = "caldav";
        c.href = info.href;
        c.name = info.name;
        c.writable = info.writable;
        c.supports_vevent = info.supports_vevent;
        auto id = d.store.upsert_calendar(c);
        if (!id.ok()) return id.error;
        present.insert(info.href);
    }
    if (Error e = d.store.mark_calendars_present("caldav", present); !e.ok()) return e;

    const int64_t now = d.clock.now();
    int64_t from, to;
    window_of(now, &from, &to);
    Error first;
    for (const CalendarInfo& info : found.value) {
        if (d.cancel.requested()) return make_error(ErrorClass::Cancelled, "cancelled", "the sync was cancelled");
        Calendar stored;
        for (const Calendar& c : d.store.calendars())
            if (c.kind == "caldav" && c.href == info.href) stored = c;
        if (stored.id == 0 || !stored.shown) continue;
        ++counts->calendars;

        const std::string cursor = info.supports_sync_report ? stored.sync_token : stored.ctag;
        auto changes = client.changes(info, cursor, from, to);
        if (!changes.ok()) {
            if (first.ok()) first = changes.error;
            if (stop_class(changes.error)) return first;
            continue;
        }
        CalendarPullBatch batch;
        batch.synced_at = now;
        if (changes.value.unchanged) {
            batch.ctag = changes.value.new_ctag;
            if (Error e = d.store.apply_calendar_pull(stored.id, batch); !e.ok()) return e;
            continue;
        }
        std::map<std::string, StoredEventRef> known;
        for (const StoredEventRef& r : d.store.event_refs(stored.id)) known[r.href] = r;

        std::vector<std::string> to_fetch;
        for (const EventRef& r : changes.value.changed) {
            batch.listed_hrefs.insert(r.href);
            auto k = known.find(r.href);
            if (k != known.end() && k->second.etag == r.etag && !r.etag.empty() && k->second.sync_state == "synced") continue;   // already current
            to_fetch.push_back(r.href);
        }
        batch.full = changes.value.full;
        batch.removed_hrefs = changes.value.removed;
        counts->removed += static_cast<int>(changes.value.removed.size());

        bool incomplete = false;
        if (!to_fetch.empty()) {
            auto fetched = client.fetch(info, to_fetch);
            if (!fetched.ok()) {
                if (first.ok()) first = fetched.error;
                if (stop_class(fetched.error)) return first;
                continue;
            }
            for (const EventResource& r : fetched.value.events) {
                EventUpsert u;
                if (!build_event_upsert(r.href, r.etag, r.ics, d.tz, zone, now, &u)) {
                    ++counts->unreadable;
                    continue;   // FR-040: this resource is skipped, the rest is applied
                }
                batch.upserts.push_back(std::move(u));
            }
            for (const std::string& g : fetched.value.gone) batch.removed_hrefs.push_back(g);
            for (const std::string& f : fetched.value.failed) {
                batch.listed_hrefs.insert(f);   // keep what is stored; it is fetched again at the next sync
                incomplete = true;
            }
        }
        for (const EventUpsert& u : batch.upserts)
            if (!u.occurrences.empty()) ++counts->changed;
        if (!incomplete) {   // the cursor only moves when everything it covers was fetched
            batch.ctag = changes.value.new_ctag;
            batch.sync_token = changes.value.new_sync_token;
        }
        if (Error e = d.store.apply_calendar_pull(stored.id, batch); !e.ok()) return e;
    }
    return first;
}

Error pull_feed(CalendarSyncDeps& d, const CalendarAccount& account, const std::string& zone, CalendarCounts* counts) {
    Calendar c;
    c.kind = "feed";
    c.href = account.feed_url;
    c.name = account.feed_name.empty() ? std::string("Feed") : account.feed_name;
    c.writable = false;
    auto id = d.store.upsert_calendar(c);
    if (!id.ok()) return id.error;
    Calendar stored;
    for (const Calendar& s : d.store.calendars())
        if (s.id == id.value) stored = s;
    if (!stored.shown) return {};
    ++counts->calendars;
    const int64_t now = d.clock.now();

    const FeedResult feed = fetch_feed(d.http, account.feed_url, stored.feed_etag, stored.feed_last_modified);
    CalendarPullBatch batch;
    batch.synced_at = now;
    if (feed.state == FeedResult::State::NotModified) return d.store.apply_calendar_pull(stored.id, batch);
    if (feed.state == FeedResult::State::Failed) return feed.error;

    batch.full = true;
    batch.feed_etag = feed.etag;
    batch.feed_last_modified = feed.last_modified;
    for (const IcalPart& part : split_ical_by_uid(feed.ics)) {
        EventUpsert u;
        if (!build_event_upsert(part.uid, std::string(), part.ics, d.tz, zone, now, &u)) {
            ++counts->unreadable;
            continue;
        }
        batch.listed_hrefs.insert(part.uid);
        batch.upserts.push_back(std::move(u));
    }
    for (const EventUpsert& u : batch.upserts)
        if (!u.occurrences.empty()) ++counts->feed_events;
    return d.store.apply_calendar_pull(stored.id, batch);
}

}  // namespace

namespace {

// The name of a new resource in its calendar: the UID with every character that is not safe in a path replaced
std::string resource_name(const std::string& uid) {
    std::string n;
    for (char c : uid) n += (std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-' || c == '_' || c == '@') ? c : '_';
    if (n.empty()) n = "event";
    if (n.size() > 120) n.resize(120);
    return n + ".ics";
}

bool same_when(const EventEdit& a, const EventEdit& b) {
    if (a.all_day != b.all_day) return false;
    auto day = [](const LocalDateTime& t) { return t.year * 10000 + t.month * 100 + t.day; };
    if (a.all_day) return day(a.start) == day(b.start) && day(a.end) == day(b.end);
    return a.start == b.start && a.end == b.end && a.tzid == b.tzid;
}

// The same event as far as the user can see: UID and the fields of the editor
bool same_event(const std::string& a, const std::string& b, const TzDatabase& tz) {
    const ParsedCalendar pa = parse_ical(a, tz), pb = parse_ical(b, tz);
    if (pa.events.size() != 1 || pb.events.size() != 1) return false;
    const VEvent &x = pa.events[0], &y = pb.events[0];
    if (x.uid != y.uid || x.rrule != y.rrule) return false;
    const EventEdit ex = event_edit_of(x), ey = event_edit_of(y);
    return ex.title == ey.title && ex.location == ey.location && ex.description == ey.description && same_when(ex, ey);
}

}  // namespace

Error push_events(CalendarSyncDeps& d, const std::string& calendar_password, PushCounts* counts) {
    PushCounts local;
    if (!counts) counts = &local;
    const std::vector<PendingEvent> pending = d.store.pending_events();
    if (pending.empty()) return {};
    const auto account = d.store.calendar_account();
    if (!account || account->caldav_url.empty()) return {};
    CalDavClient client(d.http, account->caldav_url, account->username, calendar_password);

    Error first;
    auto note = [&](const Error& e) {
        if (first.ok()) first = e;
    };
    // the server's current version of one resource: found / gone / error
    struct Server {
        bool ok = false, gone = false;
        EventResource res;
        Error error;
    };
    auto look = [&](const PendingEvent& pe, const std::string& href) {
        Server s;
        CalendarInfo info;
        info.href = pe.calendar_href;
        info.supports_vevent = true;
        auto f = client.fetch(info, {href});
        if (!f.ok()) {
            s.error = f.error;
            return s;
        }
        if (!f.value.events.empty()) {
            s.ok = true;
            s.res = f.value.events.front();
        } else if (!f.value.failed.empty()) {
            s.error = make_error(ErrorClass::Server, "server_error", "the server could not give the event");
        } else {
            s.gone = true;
        }
        return s;
    };

    for (const PendingEvent& pe : pending) {
        if (d.cancel.requested()) return make_error(ErrorClass::Cancelled, "cancelled", "the sync was cancelled");
        if (pe.calendar_kind != "caldav") continue;
        if (!pe.calendar_present) {   // the calendar is gone: the edited version stays, marked
            d.store.event_gone_on_server(pe.id);
            if (pe.sync_state != "pending_delete") ++counts->server_deleted;
            continue;
        }
        if (!pe.calendar_writable) {
            ++counts->refused;
            note(make_error(ErrorClass::Server, "read_only", "the calendar does not accept changes"));
            continue;
        }
        const std::string href = pe.href.empty() ? pe.calendar_href + resource_name(pe.uid) : pe.href;
        WriteResult w;
        bool need_look = false;   // the answer was 412 (or we have no etag): learn what the server has
        if (pe.sync_state == "pending_create") {
            w = client.put(href, pe.ics, nullptr);
        } else if (pe.etag.empty()) {
            need_look = true;
            w.kind = WriteResult::Kind::PreconditionFailed;   // without an etag nothing is sent blindly
        } else if (pe.sync_state == "pending_update") {
            w = client.put(href, pe.ics, &pe.etag);
        } else {
            w = client.remove(href, pe.etag);
        }
        (void)need_look;

        switch (w.kind) {
            case WriteResult::Kind::Created:
            case WriteResult::Kind::Updated:
                if (pe.sync_state == "pending_delete") {
                    d.store.event_removed_on_server(pe.id);
                    ++counts->deleted;
                } else {
                    d.store.event_pushed(pe.id, pe.ics, href, w.etag);
                    ++(pe.sync_state == "pending_create" ? counts->created : counts->updated);
                }
                break;
            case WriteResult::Kind::NotFound:
                if (pe.sync_state == "pending_delete") {
                    d.store.event_removed_on_server(pe.id);   // already gone
                } else {
                    d.store.event_gone_on_server(pe.id);
                    ++counts->server_deleted;
                }
                break;
            case WriteResult::Kind::PreconditionFailed: {
                const Server s = look(pe, href);
                if (!s.error.ok()) {
                    note(s.error);
                    if (stop_class(s.error)) return first;
                    break;
                }
                if (s.gone) {
                    if (pe.sync_state == "pending_delete") {
                        d.store.event_removed_on_server(pe.id);
                    } else {
                        d.store.event_gone_on_server(pe.id);
                        ++counts->server_deleted;
                    }
                    break;
                }
                // the server already holds what we wanted to send (the answer to an earlier request was lost): done, no duplicate
                if (pe.sync_state != "pending_delete" && same_event(s.res.ics, pe.ics, d.tz)) {
                    d.store.event_pushed(pe.id, pe.ics, href, s.res.etag);
                    ++(pe.sync_state == "pending_create" ? counts->created : counts->updated);
                    break;
                }
                d.store.event_conflicted(pe.id, s.res.ics, s.res.etag, href);
                ++counts->conflicts;
                break;
            }
            case WriteResult::Kind::Refused:
                ++counts->refused;
                note(w.error);
                break;
            case WriteResult::Kind::Failed:
                note(w.error);
                if (stop_class(w.error)) return first;
                break;
        }
    }
    return first;
}

Error pull_calendars(CalendarSyncDeps& d, const std::string& calendar_password, CalendarCounts* counts) {
    CalendarCounts local;
    if (!counts) counts = &local;
    const auto account = d.store.calendar_account();
    if (!account) return {};
    if (Error e = refresh_occurrences(d.store, d.tz, d.clock); !e.ok()) return e;
    const std::string zone = user_zone_name(d.store, d.tz);

    Error first;
    if (!account->caldav_url.empty()) {
        first = pull_caldav(d, *account, calendar_password, zone, counts);
        if (first.cls == ErrorClass::Cancelled || stop_class(first)) return first;
    }
    if (!account->feed_url.empty()) {
        const Error fe = pull_feed(d, *account, zone, counts);
        if (first.ok()) first = fe;
    }
    return first;
}

}  // namespace pbo
