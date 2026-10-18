# Research: Server Contacts

**Feature**: [spec.md](spec.md) | **Plan**: [plan.md](plan.md) | **Date**: 2026-10-18

Decisions taken before the design was fixed. Evidence labels (constitution Principle IX):

- **[REPO]** read from this repository (code, specs, docs) on 2026-10-18.
- **[RFC]** taken from a public specification (RFC 6352 CardDAV, RFC 6350 vCard 4.0, RFC 2426 vCard 3.0,
  RFC 6578 sync-collection, RFC 4918 WebDAV). Proves what the standard says, not how a server behaves.
- **[UNVERIFIED]** assumed; needs a real server (Nextcloud first, OpenCloud second) or the device. Listed again
  in [Open items](#open-items-to-verify).

Nothing in this file was run against a real server or the reader.

---

## R-01 Protocol: CardDAV over the existing HTTP seam

**Decision**: read and write contacts with CardDAV (RFC 6352) through the `HttpTransport` seam that CalDAV
already uses. **No new library, no new transport, no new TLS code.**

**Rationale**: the spec says "contacts from server" next to a Nextcloud/OpenCloud calendar account; both servers
speak CardDAV on the same origin as CalDAV [UNVERIFIED]. CardDAV is the same WebDAV dialect as the CalDAV code
the app already contains: `PROPFIND` for discovery, `REPORT` `sync-collection` / `multiget`, conditional `PUT` /
`DELETE` with `ETag` [REPO `core/include/pbo/caldav.h`, `contracts/caldav-ical-subset.md` of spec 001; RFC].
`CurlTransport` already verifies TLS, refuses non-https URLs and does not send credentials to another host
[REPO `http_transport.h`, test `test_redirect_credentials`].

**Alternatives considered**
- *Google People API / Microsoft Graph*: vendor APIs with OAuth; rejected (the project signs in with passwords
  only, spec 001 Assumptions; constitution V forbids a new dependency class without need).
- *Import of `.vcf` files from the reader's storage*: not "from server"; listed in the spec's Out of Scope.
- *A separate CardDAV library*: none is small and MIT; the existing client structure is enough.

## R-02 Server address, discovery and credentials

**Decision**: reuse the calendar server account: the address `account_calendar.caldav_url`, the same user name
and the same stored calendar password. No new field in the account form and no second password. Discovery:
`current-user-principal` -> `addressbook-home-set` -> `PROPFIND Depth: 1` for collections whose `resourcetype`
holds `card:addressbook`. Start addresses as for CalDAV: the address as entered and `/.well-known/carddav`
(first when the address is a bare server). An account with a feed only, or without a calendar server account,
has no contacts and the view says so (spec US5-4).

**Rationale**: the spec's Assumptions say contacts live on the account already configured. The principal lookup
works from any DAV address the user may have entered (server, `/dav/`, a calendar home), so no new input is
needed [UNVERIFIED for each target server]. A new address field is more setup for the owner and more code
(YAGNI, constitution V).

**If discovery finds no address book** the result is the key `no_addressbook`; it is not a sync failure (the
Contacts view explains it, the sync is reported as a success for that step).

**Alternatives considered**: a separate "contacts server address" field (rejected for now; becomes a small
follow-up if a live run shows a server whose address book cannot be found from the calendar address, open item
O-2).

## R-03 Share the DAV plumbing instead of copying it

**Decision**: first task of the feature, done under green tests: move the generic parts out of
`core/src/caldav.cpp` into `core/include/pbo/dav_common.h` + `core/src/dav_common.cpp` and let `CalDavClient`
and the new `CardDavClient` use them: origin and `url_for` (the rule that an address on another host is ignored),
`send` (credentials, response cap, server time stamp), `status_error`, `parse_multistatus`,
`prop_in_ok_propstat`, `dav_headers`, `escape`. No behaviour change; the existing CalDAV tests
(`test_caldav_discovery`, `test_calendar_sync_pull`, `test_event_push`, `test_redirect_credentials`, ...) must
stay green before and after (refactor step of Red-Green-Refactor).

**Rationale**: about 150 lines of security-relevant code (host check, status mapping) would otherwise exist twice
and drift. **Alternative**: copy into `carddav.cpp` (faster, but two places to fix; rejected).

## R-04 Change detection and download

**Decision**: per address book, the same ladder as for calendars:
1. `REPORT sync-collection` (RFC 6578) with the stored sync token; `403`/`409 valid-sync-token` -> full list.
2. Fallback: `PROPFIND Depth: 0 getctag`; when changed, a full list of `href/getetag` (`PROPFIND Depth: 1`).
3. Changed resources are fetched with `REPORT addressbook-multiget` in batches of 30, at most 4 MiB per answer
   (a batch that is too large is split), exactly like events [REPO `CalDavClient::fetch`].

There is **no time window** (contacts are not dated); an address book is downloaded completely. The cursor
(token or ctag) and the changed contacts are committed in one transaction, so an interrupted run is repeated
without damage (FR-004).

## R-05 Photos and other heavy data (spec FR-002, FR-015)

**Problem**: a vCard carries `PHOTO` (often 20-200 KB), also `LOGO`, `SOUND`, `KEY`. They must not be stored or
shown (budget SC-012: 20 MB for 2,000 contacts), yet an edit must not delete them on the server (FR-015, data
safety).

**Decision**:
1. Ask the server to leave them out: `address-data` with an explicit property list (`FN N NICKNAME ORG TITLE
   TEL EMAIL ADR NOTE UID VERSION REV` and the grouped `X-ABLabel` of Apple cards) [RFC 6352 section 10.4.2;
   server support UNVERIFIED].
2. Whether the server honours it or not, the ingest step **removes `PHOTO`, `LOGO`, `SOUND`, `KEY` and any
   property value longer than 64 KiB** before storing. **Every card downloaded from the server is stored with
   `contact.partial = 1`**: because the request names a property list, a server that honours it leaves the photo out
   and the app cannot tell that one exists.
3. **A write of a `partial` contact never sends the stored text.** The push does `GET <href>` (full card and its
   `ETag`), compares that `ETag` with the stored one (different -> conflict, nothing is written), applies the
   user's field changes to the full text (see R-06) and sends `PUT` with `If-Match: <that etag>`. Photos and every
   other property the app does not edit stay byte for byte as the server delivered them.
4. A new contact (created on the reader) has nothing to preserve and is sent as written.

**Rationale**: the only alternatives either store photos (budget, RAM, e-ink list speed) or lose data on edit
(constitution III). The cost is one extra `GET` per edited contact at push time, which is acceptable (user edits
are rare).

**Consequence for the spec**: FR-002 was reworded on 2026-10-18 from "photos MUST NOT be downloaded" to "MUST NOT
be stored or shown, the app MUST ask the server to leave them out" because a server that ignores the property
list still sends them.

## R-06 vCard: a small, total, bounded, lossless reader/writer

**Decision**: new `core/include/pbo/vcard.h` + `core/src/vcard.cpp`, Qt-free, in-house (like `ical.cpp`), from
RFC 2426 / 6350 (public specifications, constitution VII). Reads vCard 2.1-style leftovers, 3.0 and 4.0 as far as
the fields below; never throws, never recurses, bounded.

| Aspect | Rule |
|--------|------|
| Lines | CRLF/LF/CR tolerant; folding (continuation lines start with space or tab); quoted-printable soft breaks of 2.1 cards treated as text; property and parameter names case-insensitive; groups (`item1.TEL`) kept |
| Escapes | `\n \N \, \; \\` in text values |
| Read | `UID`, `FN`, `N` (family, given, additional, prefix, suffix), `ORG` (first component), `TEL`, `EMAIL`, `ADR` (street, city, region, postcode, country), `NOTE`, `VERSION`, `REV`; labels from `TYPE=` (3.0 and 4.0 spellings: `TYPE=HOME,WORK`, `TYPE="home"`, bare `HOME`), custom labels `itemN.X-ABLabel` |
| Preserve | everything else stays in the text untouched (no interpretation): `BDAY`, `URL`, `TITLE`, `CATEGORIES`, `X-*`, `KIND`, `MEMBER`, `PHOTO` (see R-05) |
| Limits | card 256 KiB (above: `too_large`, shown as unreadable), 5,000 lines, line 64 KiB, 100 entries per kind, text fields cut for display at 64 KiB while the raw text keeps them |
| Unreadable | not a `BEGIN:VCARD ... END:VCARD`, binary garbage, no `UID` after repair: row kept with `unreadable = 1` (listed as "unreadable contact"), never edited, never crashes the sync (FR-026) |
| Display name | `FN`; else "given family"; else `ORG`; else first `EMAIL`; else first `TEL`; else "(no name)" |
| New cards | **vCard 3.0** (`VERSION:3.0`, `PRODID:-//office-pocketbook//EN`, `UID` = random 128-bit + `@pboffice.invalid`, `FN`, `N`, `ORG`, `TEL;TYPE=`, `EMAIL;TYPE=`, `ADR;TYPE=`, `NOTE`, `REV`); 3.0 is accepted by Nextcloud/Sabre, Radicale, Apple and Google clients [UNVERIFIED for OpenCloud] |
| Edited cards | keep the card's own `VERSION`; rewrite only the properties the user changed (below) |

**Lossless edit** (`apply_contact_edit`): the editor starts from `contact_edit_of(parsed)` and ends with a
`ContactEdit` (given, family, org, phones[], emails[], addresses[], note, each entry with a label). The writer
compares per kind: an entry that is unchanged keeps its original content lines **byte for byte**; a changed entry
is rewritten as one `TEL`/`EMAIL`/`ADR` line; a removed entry removes its line **and every line of its group**
(`item1.X-ABLabel`, `item1.X-ABADR`); an added entry is appended. `FN` is rewritten only when the user changed
name or organization (then it becomes "given family" or the organization); `N` replaces only the family and given
components (prefix, suffix, additional name stay); `REV` is set to the save time. Round-trip tests compare the
content-line multiset before and after for fixtures from several client styles (all `.unverified`).

**Alternatives considered**: re-serialize the whole card from the parsed fields (rejected: loses `BDAY`, `URL`,
`X-*`, grouped labels, FR-015); keep a vCard 4.0 writer only (rejected: older servers).

## R-07 Storage: schema 2 and a real migration

**Decision**: raise `Store::kSchemaVersion` to **2** and add the first built-in migration (1 -> 2) in the new
`core/src/store_migrations.cpp`, which the policy of spec 001 `data-model.md` (Migration policy) names but which
does not exist yet [REPO: `Store::open` only runs `StoreOptions::extra_migrations`]. The migration creates
`addressbook`, `contact`, `contact_email`, `contact_conflict` and rebuilds `sync_state` so that `problem_area`
may be `'contacts'` (a CHECK constraint cannot be altered in place). Details: [data-model.md](data-model.md).

**Duplicate or missing UIDs** (analysis F1): a downloaded card without a usable `UID`, or with a `UID` already used in the
book, is stored as `unreadable` under the synthetic uid `href:<href>`, so the `UNIQUE(addressbook_id, uid)` constraint holds.

**Rationale**: the data is first-class (pending changes that must survive), so it is modelled like events, with
constraints. A fresh database is created as schema 1 and then migrated, so there is only one path to test.
`Store::open` already: backs the file up before an upgrade, runs the steps in one transaction, checks integrity
and refuses a database of a newer version [REPO `core/src/store.cpp`].

**Consequence (owner decision to note)**: after the first start of the new build the database is schema 2 and
**the previous build (0.2.0) refuses it** ("written by a newer version") by design. Going back means restoring
the pre-upgrade copy: the existing code deletes `pboffice.db.bak` after a successful upgrade; this plan keeps it
until the next start has opened the database successfully once more (one extra cleanup step), so a reader that is
rolled back at once loses nothing. This is the only user-visible irreversible step of the feature.

**Alternatives considered**
- *`CREATE TABLE IF NOT EXISTS` at open, as `message_image` was added* [REPO]: no version bump and the old build
  keeps opening the file. Rejected for this feature because it cannot change the `sync_state` CHECK, it hides
  the schema from the version number, and the constitution wants tested one-way upgrades. It remains the
  fallback if the owner prefers a downgrade-safe database (tell the plan; it then drops the `'contacts'` problem
  area in favour of `'calendar'`).

## R-08 Contact state machine (mirrors events)

States of `contact.sync_state`: `synced`, `pending_create`, `pending_update`, `pending_delete`, `conflict`,
`server_deleted`. A server refusal is not a state of its own: the row stays `pending_*` and `error_key` is set
(`read_only`, `too_large`, `refused`, `storage_full_server`); the list shows "failed: reason" and offers retry
(clears `error_key`), edit and discard (FR-024). Transitions, identical in spirit to `Store::save_event_local`,
`request_event_delete`, `event_pushed`, `event_conflicted`, `event_gone_on_server` [REPO `store.h`], are written out
in [data-model.md](data-model.md). Points that differ from events:

- **Undo of a deletion** (FR-019): `request_contact_delete` on a `synced` row sets `pending_delete` and
  `undo_state = 'synced'`; on a `pending_update` row `undo_state = 'pending_update'` (the user's edit is kept);
  `undo_contact_delete` restores it. A `pending_create` is simply removed (never reached the server).
- **Create retry** (analysis F2): the resource path of a new card is chosen and stored with the row before the first `PUT`
  (`href NOT NULL`, `etag` NULL until confirmed). A lost answer is recognised by `412` on the retry plus a `GET` of that path
  (own `UID`), never by searching the book; a pull that delivers the app's own card for a `pending_create` row adopts its
  `ETag` (data-model rules 2 and 4).
- **Discard of a failed update**: sets `refetch = 1`, state `synced`; the next pull downloads the server version
  of that href regardless of the cursor and overwrites the row.
- **Delete of a changed contact** (US4-4): `DELETE` is sent with `If-Match: <stored etag>`; `412` -> `conflict`
  with the server version kept and `deleting = true` for the user's side (exactly `event_conflicted`).

## R-09 Conflicts and "deleted on the server" (FR-020 to FR-022)

**Decision**: the same resolution model as events: `contact_conflict(contact_id, server_vcf, server_etag,
fetched_at)` keeps the server version beside the local row; choices `KeepMine` (push again against the new etag),
`KeepServer` (replace the local row), `KeepBoth` (the user's version becomes a new contact with a new `UID`).
Detection: (a) `412` on `PUT`/`DELETE`; (b) the pre-write `GET` of a `partial` contact shows a different `ETag`
(R-05), in which case nothing is written. `404` on update -> `server_deleted` (edited version kept; the user
re-creates it or discards it after confirmation). An address book removed on the server marks its contacts with
local work `server_deleted` and keeps the book row (`present_on_server = 0`); the rest are removed.

The "To decide" page (`ConflictsPage`) gets a contacts section next to messages and events [REPO
`bridge/src/conflicts_model.cpp`, `qml/pages/ConflictsPage.qml`].

## R-10 Sync integration

**Decision**: two new steps after the calendar steps: `contacts_push`, `contacts_pull` (names added to
`sync_step_names()`); `SyncOptions.contacts` (default true); `SyncDeps` unchanged (the HTTP seam is already there);
`SyncReport.contacts_counts`; `problem_area = "contacts"`. Order: contacts after calendar so that a contact
problem never delays mail or events. Failure of one area does not stop the others (existing `partial` logic
extended). `counts_json` gets `contacts`, `contacts_removed`, `contacts_sent`, `contact_conflicts`.

Triggers are unchanged (FR-003): `SyncBridge` already starts a sync a few seconds after the user's own change and
switches Wi-Fi on for it; the "has local work" test (`SyncBridge::hasPending`) additionally asks
`Store::pending_contacts()`. The start-up sync still runs only with Wi-Fi already connected [REPO
`bridge/src/sync_bridge.cpp`, owner decisions]. A pull of address books runs only for **shown** books; a push
always runs for every book with pending contacts, shown or not.

Idle behaviour (SC-009): no timer, no network from the Contacts view; a test extends `test_idle_no_network`.

## R-11 Search and sort

**Decision**: columns computed in C++ at write time with the existing folding function (`sort_key`, SQL function
`pbo_fold` [REPO `utf8.h`, `store.cpp`]): `sort_key` (family, given, then organization/email/phone for nameless
cards) and `search_text` (folded name, organization, email addresses, phone numbers reduced to digits and `+`).
The list is `ORDER BY sort_key LIMIT n OFFSET m`; search is `search_text LIKE '%word%'` for every word (all words
must match), after escaping `% _ \`. For 2,000 rows this is a scan of a few hundred KB; no FTS5 table is needed
(YAGNI). Search starts when the user confirms the field (keyboard "done" / the Search button), not on every key,
to keep e-ink refreshes down (constitution VI); the result is the same paged list.

## R-12 Recipient suggestions (clarification of 2026-10-18)

**Decision**: extend `Store::address_suggestions(prefix, limit)`: the result type gains `label` and
`from_contact`; contacts come first (rows of `contact_email` whose folded address, or whose contact's folded
name/organization words, start with the typed prefix, from shown address books, state not `pending_delete`),
then the existing cached-mail suggestions that are not already present (compare lower-cased addresses). The
bound of 8 stays. `contact_email(contact_id, addr, addr_key, label, idx)` is rewritten with every contact write
(it is a derived index; the vCard stays the source). `ComposeBridge` shows "Name <address> (work)" for
contacts. No change to sending or to `address_suggestion` (the cached-mail table is untouched).

## R-13 UI

**Decision**: three pages built from the existing widgets (`TopBar`, `InkRow`, `PageButtons`, `InkField`,
`ChoiceDialog`, `ConfirmDialog`, `FooterButton`); no new QML widget, no `Flickable`:
- `ContactsPage` (list, paged; search field; "New" and sync status line; marks "not synced" / "failed");
- `ContactPage` (details, paged when long; Edit, Delete; read-only and conflict banners);
- `ContactEditPage` (name, organization, repeated rows for phone / email / address with a label chooser and a
  remove button, "add" buttons, note; address book chooser when creating and more than one is writable).
Home gets a "Contacts" button next to Agenda, shown whenever a calendar **server** account is configured (analysis F3: the
page itself explains "no address book was found" or "empty"); the create form offers only writable address books that are shown (F11); Settings gets "Address books" with an on/off switch each (as the
calendar visibility). Bridges: `ContactsModel` (page, count, search), `ContactBridge` (open, new, save, remove,
undo, resolve, retry, discard), `SettingsBridge` extended. Strings are English `qsTr` with German entries in
`qml/strings_de.tsv` (`check_translations`). Details: [contracts/bridge-qml.md](contracts/bridge-qml.md).

## R-14 Validation of user input

Limits apply only to values the user adds or changes (analysis F4): an entry or note that is already longer than the limit
is passed through byte for byte, shown read-only in the edit form ("too long to edit here, kept unchanged") and can be removed
but not changed (each such entry and the note carry a `locked` flag in `ContactEdit`; a submitted change to one is refused with
`locked_changed`; removing one removes its line and its group); text fields are never cut before they reach the editor. Constants in one header (`contact_edit.h`): name parts 200 chars, organization 200, phone 64, email 254, each
address part 200, note 8 KiB, at most 20 entries per kind, whole card 64 KiB when written. Errors (keys): `empty`
(no name, organization, phone or email: FR-013), `email_invalid` (detail = entry index; rule: one `@`, non-empty
local part and domain, no spaces/control characters, domain has a `.` or is a bracketed literal is **not**
required), `too_long`, `too_many`, `card_too_large`. Control characters are removed; a newline in a single-line
field becomes a space; the note keeps newlines. All checks live in the core with core tests; the bridge only
passes the strings (constitution II).

## R-15 Budgets (constitution V), all **unverified** until measured on the InkPad One

| Budget | Value | Check |
|--------|-------|-------|
| Storage for 2,000 contacts without photos | <= 20 MB (SC-012); expected about 5 MB (about 1.5 KB stored card + derived columns) | host test `test_contacts_perf` measures the database growth; device checklist item |
| Memory | Contacts view <= +10 MB over the Agenda (page query, rows of one page only; no model of all contacts) | device checklist (RSS) |
| Cold start | unchanged (< 5 s, SC-003 of spec 001); contacts are not read at start | `test_perf_budgets` extended: start with 2,000 contacts |
| List | page turn < 1 s, search < 2 s with 2,000 contacts (SC-004) | host proxy `test_contacts_perf` (runs on the host; device unverified) |
| Suggestions | < 1 s with 2,000 contacts (SC-011) | host proxy |
| Package size | + < 100 KB | `package-device.sh` size check |

Eviction never touches contacts (they are not part of the mail window); the storage-limit logic counts them in
the database size but cannot delete them. If the protected data alone exceed the limit the existing "storage
full" warning applies. A contact with local work is protected in every case (FR-023).

## R-16 Logging and mechanical checks

New event codes in `core/src/log_codes.h`: counts only (`contacts.pull`, `contacts.push`, `contacts.conflict`,
`contacts.rejected`, `contacts.unreadable`). `scripts/check-constitution.sh` rule R8 gets more forbidden log-key
words (`contact`, `phone`, `email`, `book`, `org`, `note`) with a self-test that proves detection. `check-ui-style.sh`
needs no new rule (no new widget kinds). `check-fixtures.sh`: new fixtures under `tests/fixtures/carddav/` and
`tests/fixtures/vcard/` use example domains and fictional people and carry `.unverified`.

## R-17 Test strategy (Principle I)

Tests are written first and seen failing; the order is in `tasks.md` (not created here).

| Layer | Tests (new files) |
|-------|-------------------|
| Unit | `test_vcard_parse`, `test_vcard_edit_roundtrip` (lossless, groups, 2.1/3.0/4.0, several client styles), `test_malformed_vcard` (corpus of 20+: truncated, binary, 5,000 lines, huge field, bad charset, missing `END`), `test_contact_edit_validation`, `test_contact_sort_search`, `test_address_suggestions` (extended: contacts first, dedupe, hidden book, pending delete), `test_store_migration_2` (fixture schema-1 database -> 2, older build refuses 2, crash during the migration) |
| Contract | `test_carddav_discovery` (principal, home, books, privileges, well-known, no address book), `test_carddav_requests` (conditional headers, address-data property list, batches, host check) |
| Integration | `test_contacts_sync_pull` (full, sync-collection, ctag fallback, removal, unreadable card skipped, interrupted repeat), `test_contact_push` (create / update / delete exactly one conditional request each; partial -> GET-merge-PUT; photo kept), `test_contact_conflict` (412, changed etag at pre-GET, both versions kept), `test_contact_server_deleted`, `test_contact_failed` (403 / 413 / 507 -> error_key, retry, discard), `test_contacts_kill` (kill matrix: 100 random kills, 0 lost, 0 duplicates), `test_contacts_readonly`, `test_contacts_perf`, `test_sync_with_contacts` (step order, partial outcomes), `test_idle_no_network` (extended) |
| Bridge | `test_contacts_bridge`, `test_contact_edit_bridge`, `test_conflicts_model` (extended), `test_settings_bridge` (extended), `test_compose_bridge` (extended: suggestion labels) |
| QML | `tst_contacts_flow` (list, search, open, edit, delete with confirmation, offline marks), stubs extended, `test_qml_pages` (new pages load) |

Fakes: `tests/support/fake_carddav_server.h` on the same TLS scaffolding as `fake_caldav_server.h` (scripted
address books, cards, ETags, tokens, privileges, failure injection, a hook that edits the server copy between the
app's read and write, a switch "ignores the property list"). Every test that uses it prints the "UNVERIFIED against
real servers" banner (`PBO_FAKE_SERVER_TEST()`, label `fake-server`) as `check-fake-markers.sh` demands.

## R-18 Licensing and third-party code

No new component. The vCard code is written from RFC 2426/6350/6352 (behaviour only). Android's Contacts, DAVx5,
K-9 and Etar are reference-only and nothing is copied (constitution VII). `NOTICE`, `licenses/manifest.txt` and the
About box do not change; `check_license_consistency` must stay green.

## R-19 Release

MINOR change: `VERSION` 0.2.0 -> **0.3.0**, a `CHANGELOG.md` entry, README (features, privacy notes: contacts live in
the database, no photos), both live-server and device checklists get contact rows; the version bump is the last
task (constitution, Development Workflow). Tag `v0.3.0` after the merge (owner).

---

## Open items to verify

All **unverified** until a live run against the named server or the device. They map to new rows in
`docs/live-server-checklist.md` (LV-K*) and `docs/device-test-checklist.md` (DT-CT*).

| Id | Item |
|----|------|
| O-1 | Nextcloud and OpenCloud expose address books (`addressbook-home-set`, `card:addressbook` collections, privileges) at the discovered address; `/.well-known/carddav` redirects as `/.well-known/caldav` does |
| O-2 | The principal lookup works from every form of calendar address the owner may have entered (server, `/dav/`, calendar home). If not: add the separate "contacts address" field (R-02) |
| O-3 | `sync-collection` works on address books; the ctag fallback otherwise |
| O-4 | The server honours the `address-data` property list; otherwise photos still arrive and are stripped on ingest (works either way, costs bandwidth) |
| O-5 | The server accepts a new vCard 3.0 with `If-None-Match: *`, returns an `ETag`, keeps unknown properties and `PHOTO` of an edited card, answers `412` / `404` / `403` / `507` as mapped |
| O-6 | Cards written by other clients (Apple grouped labels, Android, Thunderbird, Nextcloud web) survive the edit round trip |
| O-7 | A 2,000-contact address book: first sync time, list page turn, search time and storage on the device |
| O-8 | The schema 1 -> 2 upgrade of the owner's real reader database (backup kept, previous build refuses the new file); the data of the reader is never copied to the host and the host app is never run against it (owner rule) |
