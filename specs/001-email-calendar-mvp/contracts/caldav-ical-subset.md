# Contract: CalDAV and iCalendar subset

**Feature**: [../spec.md](../spec.md) | **Research**: [../research.md](../research.md) R-14, R-15, R-16, R-18

What the app sends to, and expects from, a calendar server or feed, and exactly which parts of iCalendar it
understands. Server behavior is **unverified** (Nextcloud first, OpenCloud second) until
`docs/live-server-checklist.md` records a run; fixtures imitating servers carry `.unverified`.

## 1. Requests (HTTPS only; Basic authentication; TLS rules of the core)

| Step | Request | Expectation |
|------|---------|-------------|
| Principal | `PROPFIND <entered URL or /.well-known/caldav> Depth: 0` body `current-user-principal` | `207`; redirects stay on `https` and same host for credentials |
| Home set | `PROPFIND <principal> Depth: 0` body `c:calendar-home-set` | `207` |
| Calendars | `PROPFIND <home> Depth: 1` body `resourcetype, displayname, c:supported-calendar-component-set, cs:getctag, d:sync-token, d:current-user-privilege-set, d:supported-report-set` | `207`; a collection is a calendar when `resourcetype` has `c:calendar` and the component set contains `VEVENT`; **writable** when privileges include `d:write-content` (or `d:all`) |
| Changes (preferred) | `REPORT <cal> Depth: 1` `d:sync-collection` with the stored `sync-token` | `207` with changed `href/getetag` and removed `href` (status 404); a `403/409 valid-sync-token` answer => full resync |
| Changes (fallback) | `PROPFIND <cal> Depth: 0` `getctag`; if changed: `REPORT` `c:calendar-query` with time-range returning `getetag` | compare with stored hrefs/etags |
| Window | `REPORT` `c:calendar-query` filter `VEVENT` `c:time-range start=<today-30d UTC> end=<today+365d UTC>` | server returns every resource with an instance in the window (including recurring masters) |
| Fetch | `REPORT` `c:calendar-multiget` for changed hrefs in batches of 30 (size cap per batch 4 MiB) | `207` with `calendar-data` |
| Create | `PUT <home>/<new-name>.ics` `If-None-Match: *`, `Content-Type: text/calendar; charset=utf-8` | `201`/`204`; `ETag` header stored, else one `PROPFIND getetag` |
| Update | `PUT <href>` `If-Match: "<etag>"` | `204`/`200` => new `ETag`; `412` => conflict; `404` => `server_deleted` |
| Delete | `DELETE <href>` `If-Match: "<etag>"` | `204` => removed; `412` => conflict; `404` => already gone (treated as done) |
| Feed | `GET <https feed URL>` with `If-None-Match` / `If-Modified-Since` | `200` full; `304` unchanged; size cap 8 MiB; `webcal://` rewritten to `https://` |

Rules: writes are **always conditional** (Principle III); `Depth` and `Content-Type` are set explicitly; a
server answering with an absolute `href` on another host is ignored; user names and passwords are never in
URLs; response bodies are capped (8 MiB in memory); every XML body goes through `xml_min` (research R-15).

### Status mapping

| HTTP / situation | Core result |
|------------------|-------------|
| `401` | `Error{Auth,"auth_failed"}` |
| `403` on write | `Refused` => event stays `pending_*`, user told "read-only or refused" |
| `404` on update | `server_deleted` |
| `409`, `412` | `PreconditionFailed` => `conflict` |
| `413`, `507` | `Refused` (`too_large` / `storage_full_server`) |
| `5xx`, timeouts | `Error{Server|Network}`, retry at next sync; local state unchanged |
| `207` with per-resource `5xx` | those resources skipped and retried; the others applied |
| malformed XML / ICS | that resource skipped as unreadable; the sync continues (FR-040) |

## 2. Fake CalDAV server (test contract)

Scriptable: principals/homes/calendars with ctag and sync-token, events (href, etag, ICS), privileges,
`supported-report-set` (with/without `sync-collection`), per-request failure injection (drop, delay, `412`,
`404`, `403`, `507`, `5xx`, malformed XML, redirect to another host), concurrent server-side edit between
the app's read and write. Required scenarios:

| Scenario | Expectation (maps to) |
|----------|------------------------|
| discovery, several calendars, one read-only | calendars listed, writability correct; user selection stored (US2-1) |
| feed only | events shown read-only (US2-2) |
| window | only events with instances in [-30 d, +365 d] stored; older ones removed on the next sync unless pending (FR-021) |
| create/update/delete offline then sync | exactly one `PUT`/`DELETE` each, conditional headers present |
| same event edited on both sides | `412` => both versions kept (`event_conflict`), neither lost (US4-4, SC-008) |
| event deleted on server, edited locally | `server_deleted`, edited version kept (spec clarification) |
| calendar removed on server | its events with unsynced changes kept as `server_deleted` |
| interruption after `PUT` but before the reply is processed | next sync finds the etag/content equal and does not create a duplicate (idempotent by `UID`) |
| redirect to another host | credentials not sent there (baseline `test_redirect_credentials` analog) |

## 3. iCalendar understood by the parser (RFC 5545 subset)

Content lines: CRLF/LF/CR tolerant, folding, parameter quoting, text escapes (`\n \, \; \\`), property names
case-insensitive. Limits: input 8 MiB per resource, 200,000 lines, line 1 MiB, 2,000 components,
`ATTENDEE` entries kept up to 200.

| Property | Handling |
|----------|----------|
| `UID`, `SUMMARY`, `DESCRIPTION`, `LOCATION`, `STATUS`, `TRANSP`, `CLASS`, `SEQUENCE`, `LAST-MODIFIED`, `DTSTAMP` | read; summary/description/location length-capped for display (64 KiB), the raw text keeps everything |
| `DTSTART`, `DTEND`, `DURATION` | `DATE` (all-day, end exclusive), `DATE-TIME` UTC (`Z`), `DATE-TIME;TZID=<IANA or Windows name>`, floating; no end => `no_end` note, zero length for timed, one day for all-day |
| `RRULE` | see §4 |
| `EXDATE`, `RDATE` | `DATE` and `DATE-TIME` forms, multiple values per line, TZID honored |
| `RECURRENCE-ID` | overrides (moved/edited instance); `STATUS:CANCELLED` override removes the instance; `RANGE=THISANDFUTURE` => treated as unsupported => note |
| `ATTENDEE`, `ORGANIZER` | names and addresses shown; no reply handling (out of scope) |
| `VALARM`, `VTIMEZONE`, `X-*`, unknown | parsed past, **preserved** in the raw text, not interpreted |

### Time zones

`TZID` resolved through the bundled IANA database (names, plus ~40 Windows names mapped). Unknown `TZID` =>
the value is shown as floating and the event carries the note `tz_unknown`. `VTIMEZONE` blocks are ignored in v1.

| Case | Rule |
|------|------|
| Nonexistent local time (spring-forward gap) | interpreted with the UTC offset in force before the gap (RFC 5545 §3.3.5) |
| Repeated local time (fall-back) | first occurrence (before the change) |
| Floating | wall-clock time kept as written, displayed in the user's zone as the same wall-clock time |
| All-day | date only, no zone, appears on the date(s) shown |
| Recurring across DST | generated in wall-clock time of the event's zone and converted per instance => same local time before and after (US2-6) |
| Display | times converted to the user's zone (research R-16 precedence); when the event zone differs, the event view also shows the event's zone (FR-022) |

## 4. RRULE subset

| Part | Support |
|------|---------|
| `FREQ` | DAILY, WEEKLY, MONTHLY, YEARLY |
| `INTERVAL`, `COUNT`, `UNTIL` (date or date-time, UTC or local), `WKST` | yes |
| `BYDAY` | weekly lists; monthly/yearly with ordinals (`1MO`, `-1FR`) |
| `BYMONTHDAY` (including negative), `BYMONTH`, `BYSETPOS` | yes |
| everything else (`SECONDLY/MINUTELY/HOURLY`, `BYWEEKNO`, `BYYEARDAY`, `BYHOUR/BYMINUTE/BYSECOND`, `RSCALE`, `SKIP`) | single occurrence at `DTSTART` + note `rrule_unsupported` |
| invalid dates (Feb 30), contradictory parts | skipped per RFC 5545 §3.3.10 (instance dropped), never an error |

Expansion limits: window only; at most 5,000 instances per event; iteration guard (100,000 candidate steps)
so a hostile rule cannot hang the sync. `describe_rrule` produces the words for the event view ("every week on
Monday and Thursday until 30 June 2027") for the supported subset, and "repeats (details not shown)" otherwise.

## 5. Writing events (lossless edit)

- `new_event_ics`: `VCALENDAR` (`PRODID:-//office-pocketbook//EN`, `VERSION:2.0`) + one `VEVENT` with `UID`
  (random 128-bit + `@pboffice.invalid`), `DTSTAMP`, `DTSTART/DTEND` (`TZID` = the user's IANA zone for timed
  events; `VALUE=DATE` for all-day), `SUMMARY`, `LOCATION`, `DESCRIPTION`; text escaped; lines folded at 75 octets.
- `apply_edit`: changes only the edited properties inside the existing `VEVENT`; bumps `SEQUENCE`, sets
  `DTSTAMP`/`LAST-MODIFIED`; leaves `VALARM`, `ATTENDEE`, unknown properties, other components untouched.
  Editing one instance of a series and series-wide `RRULE` editing are **out of scope** (FR-025 and the
  clarification of 2026-10-13): events with `RRULE` open as read-only series in the editor with the message
  "repeating events can be changed in your calendar app"; the user may delete the whole series (with
  confirmation).
- Round-trip tests: parse -> edit title -> serialize keeps every other byte-significant property
  (compared with the content-line multiset), for fixtures from several client styles (`.unverified`).
