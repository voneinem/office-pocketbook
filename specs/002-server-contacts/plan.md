# Implementation Plan: Server Contacts

**Branch**: `002-server-contacts` (`feature/002-server-contacts`) | **Date**: 2026-10-18 | **Spec**: [spec.md](spec.md)

**Input**: Feature specification from `/specs/002-server-contacts/spec.md` (1 clarification, session 2026-10-18)

## Summary

Contacts from the user's cloud server on the PocketBook InkPad One: a paged, searchable Contacts view, a detail
view, and create / edit / delete that work offline and are sent to the server at the next sync; recipient suggestions
in the mail composer use the contacts too. Nothing the user typed is lost or silently overwritten.

Technical approach: add a **CardDAV** client next to the CalDAV client and reuse everything else of the stack
(Qt-free `core/`, Qt `bridge/`, QML pages from the existing widgets, SQLite, libcurl through the `HttpTransport`
seam, the sync engine and its triggers). **No new library.**

- **Protocol**: CardDAV (RFC 6352) over `HttpTransport`; same server account, user name and password as the
  calendar; discovery principal -> `addressbook-home-set` -> address books; `sync-collection` with a ctag fallback;
  `addressbook-multiget` in batches of 30; every write conditional (`If-None-Match: *`, `If-Match`). The generic DAV
  plumbing is extracted from `caldav.cpp` into `dav_common` first, under green tests (R-01 to R-04).
- **vCard**: a small in-house, bounded, total reader/writer (`vcard.h`); new cards are vCard 3.0; **edits are
  lossless** (only changed entries are rewritten, everything else stays byte for byte). Photos and other heavy
  properties are neither stored nor shown; a contact that lost them on ingest is written by *GET full card ->
  merge -> conditional PUT*, so the server keeps its photo (R-05, R-06).
- **Storage**: schema **2** with the first built-in migration (`addressbook`, `contact`, `contact_email`,
  `contact_conflict`, and `sync_state` rebuilt for the problem area `contacts`); the state machine mirrors events,
  including both-versions-kept conflicts and "deleted on the server"; a refusal keeps the contact as *failed*;
  a deletion can be undone until the server confirms it (R-07 to R-09, [data-model.md](data-model.md)).
- **Sync**: two new steps `contacts_push`, `contacts_pull` after the calendar steps; same triggers, Auto Sync and
  Wi-Fi rules; no background activity (R-10).
- **UI**: `ContactsPage`, `ContactPage`, `ContactEditPage`, a contacts section in "To decide", an "Address books"
  section in Settings, a Home button; the search runs when the field is confirmed (R-11, R-13,
  [contracts/bridge-qml.md](contracts/bridge-qml.md)).
- **Suggestions**: `Store::address_suggestions` returns contacts first, labelled, deduplicated against cached mail
  addresses (R-12).
- **Tests**: test-first; fake CardDAV server on the same TLS scaffolding as the fake CalDAV server; malformed-card
  corpus; kill matrix; migration test; host proxies for the budgets; live-server and device checks listed as
  **unverified** (R-17, [quickstart.md](quickstart.md)).

All design alternatives, with the recommendation taken, are in [research.md](research.md) (R-01 to R-19, open items
O-1 to O-8).

## Technical Context

**Language/Version**: C++17 (core, bridge, app glue); QML for Qt 6.8 (SDK Qt 6.8.2); CMake >= 3.16; GCC 15.2 cross-compiler from the SDK (unchanged)

**Primary Dependencies**: libcurl, OpenSSL 3.x, SQLite 3.54.0 (vendored, FTS5), Qt 6.8 Core/Gui/Qml/Quick (UI only). **No new dependency, no new data asset.**

**Storage**: the existing SQLite database `pboffice.db`, schema 1 -> **2** (one-way migration, backup kept until the next start); no new file; credentials file unchanged (the calendar password is reused); see [data-model.md](data-model.md)

**Testing**: the existing header-only test support + CTest (labels unit, contract, integration, bridge, qml); new in-process fake CardDAV server over TLS; malformed-card corpus; kill-matrix; ASan/UBSan option; manual device and live-server checklists (**unverified** until run)

**Target Platform**: PocketBook InkPad One (RK3566, 32-bit ARM hard-float Linux userland, QPA `pocketbook2egl`); host Linux x86-64 for the core, tests and the Qt app (unchanged)

**Project Type**: single repository: Qt-free library + Qt bridge + QML app (device app with host-testable core), unchanged

**Performance Goals**: with 2,000 contacts: page turn < 1 s and search < 2 s (SC-004), suggestions < 1 s (SC-011), cold start still < 5 s (SC-003 of spec 001). All **unverified** on the device; host proxies in `test_contacts_perf`

**Constraints**: offline-first; no background network; TLS verification always on, `https` only, credentials never sent to another host; no web engine, no remote content; no data loss on kill / battery / Wi-Fi drop; no photos stored or shown; storage <= 20 MB and RAM <= +10 MB for 2,000 contacts (R-15, **unverified**)

**Scale/Scope**: one server account, a few address books, up to about 2,000 contacts in total; 3 new pages (plus extensions of Home, Settings, To decide, Compose); 6 user stories (3 priority levels)

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-checked after Phase 1 design.*

| Principle | Before research | After design | Evidence / notes |
|-----------|-----------------|--------------|------------------|
| I. Test-First | PASS (obligation) | PASS | Strategy in research R-17: unit, contract, integration, bridge, QML tests for every wire subset and interface change ([contracts/](contracts/)); malformed / truncated / oversized corpus for vCard and XML; fake servers only, no real account. `tasks.md` must order each test task before its implementation task and require the red run; the DAV extraction (R-03) is a refactor under the existing green tests |
| II. Host-testable core, layers | PASS | PASS | `vcard`, `carddav`, `dav_common`, `contact_edit`, `contacts_sync` live in `core/` (Qt-free, InkView-free); network, clock and random are the existing interfaces; the bridge only translates ([contracts/core-interfaces.md](contracts/core-interfaces.md)). Host and ARM targets unchanged |
| III. No silent data loss | PASS (obligation) | PASS | Contacts persisted before any network operation; pull never touches a row with local work; conditional `PUT` / `DELETE` only; `412`, a changed ETag at the pre-write `GET`, or a deleted card -> both versions kept or the edited version kept (R-08, R-09); a refusal keeps the contact as failed; deletion needs confirmation and is undoable until confirmed (FR-016, FR-019); photos survive edits by GET-merge-PUT (R-05); contacts are never evicted; schema 2 upgrade keeps a backup until the next start (R-07); interrupted sync repeatable and idempotent, kill matrix of 100 runs |
| IV. Security & privacy | PASS (obligation) | PASS | Same `CurlTransport`: TLS verified, `https` only, no downgrade, no credential to another host; the calendar credential is reused, no new secret and no new storage (the credential-storage decision R-13 of spec 001 stands); no remote content, no links opened, no call / mail actions from a contact (FR-011); photos and binary data removed at ingest; vCard input parsed defensively with limits; logs hold counts and codes only, rule R8 extended with a self-test (R-16) |
| V. Simplicity & bounded footprint | PASS | PASS | No new library. In-house vCard code replaces a dependency (justified in Complexity Tracking); search by `LIKE` on a derived column instead of a new FTS table; no new widget kind. Budgets stated (R-15) and checked by host proxy tests, the package-size check and the device checklist. Contacts are bounded by the server's address book; their storage is a budget item (SC-012) |
| VI. E-ink UX & battery | PASS | PASS | Paged lists, no animation, large targets, on-screen keyboard, offline / unsynced / last-sync always visible; search on confirm, not per key; network only on start / request / after a save or delete (Auto Sync); idle test extended (SC-009) |
| VII. Licensing hygiene | PASS | PASS | MIT project; no new component, so `NOTICE`, `licenses/manifest.txt`, About unchanged; vCard written from RFCs; DAVx5 / Android Contacts reference-only, nothing copied (R-18) |
| VIII. Quality gates | PASS | PASS | `pre-merge.sh` kept; R8 extended with a self-test; new fixtures `.unverified` and anonymized; no new SDK path; ARM build unchanged. Version bump to 0.3.0 and CHANGELOG are the last task (R-19) |
| IX. Specs-driven, honest verification | PASS | PASS | Spec -> clarify -> plan done; every server and device behaviour is labelled unverified (O-1 to O-8) and appears in `docs/live-server-checklist.md` / `docs/device-test-checklist.md` as `U`; fake-server tests carry the banner and the `fake-server` label |
| X. English documentation | PASS | PASS | All files of this feature are English; German appears only in `qml/strings_de.tsv` and in test data (umlaut names) explained by English test names |

**Technology Stack & Build Constraints**: stack unchanged (C++17, Qt 6.8, CMake, libcurl, OpenSSL, SQLite): no
amendment. **Development Workflow**: "Database schema upgrades are one-way and MUST be tested; an older build MUST
refuse a newer database" is satisfied by `test_store_migration_2` and the existing `schema_too_new` path (R-07).
**Release**: MINOR bump (0.3.0).

**Gate result**: no unjustified violation. Spec 001 listed an address book as out of scope (apart from suggesting
addresses from cached mail); this feature supersedes that entry for server contacts. Spec 001 is not rewritten: a
note in its Out-of-Scope paragraph points to this spec (a task).

## Project Structure

### Documentation (this feature)

```text
specs/002-server-contacts/
├── spec.md                      # feature specification (clarified)
├── plan.md                      # this file
├── research.md                  # Phase 0: decisions, alternatives, budgets, test strategy, open items
├── data-model.md                # Phase 1: schema 2, state machine, migration
├── quickstart.md                # Phase 1: build, validation scenarios, device / live steps
├── contracts/
│   ├── carddav-vcard-subset.md  # CardDAV requests, status mapping, fake server, vCard subset, lossless edit
│   ├── core-interfaces.md       # new and changed C++ seams
│   └── bridge-qml.md            # bridge objects, pages, navigation, strings
├── checklists/requirements.md   # spec quality checklist (existing)
└── tasks.md                     # Phase 2 (/speckit-tasks) - not created here
```

### Source Code (repository root)

New files are marked `(new)`, changed files `(changed)`; everything else is untouched.

```text
core/
├── include/pbo/
│   ├── dav_common.h (new)           # extracted DAV plumbing (R-03)
│   ├── carddav.h (new)              # CardDavClient
│   ├── vcard.h (new)                # parse, derive, strip, new card, lossless edit
│   ├── contact_types.h (new)        # AddressBook, Contact*, ContactEdit, ContactProblem, ...
│   ├── contact_edit.h (new)         # validation and user actions
│   ├── contacts_sync.h (new)        # pull_contacts, push_contacts
│   ├── caldav.h (changed)           # uses dav_common; interface unchanged
│   ├── store.h (changed)            # kSchemaVersion 2, contact / address-book API, suggestions with contacts
│   ├── sync.h (changed)             # two steps, options, report, problem area
│   └── mail_types.h (changed)       # problem_area comment
├── src/
│   ├── dav_common.cpp, carddav.cpp, vcard.cpp, contact_edit.cpp, contacts_sync.cpp (new)
│   ├── store_contacts.cpp (new)     # the contact part of Store
│   ├── store_migrations.cpp (new)   # built-in migrations, step 1 -> 2
│   ├── caldav.cpp, store.cpp, store_organize.cpp (address_suggestions), sync.cpp, log_codes.h (changed)
│   └── CMakeLists.txt (changed)

bridge/
├── include/ + src/  contacts_model, contact_bridge (new)
│                    sync_bridge, conflicts_model, settings_bridge, compose_bridge, home_bridge (changed)

qml/
├── pages/ContactsPage.qml, ContactPage.qml, ContactEditPage.qml (new)
│        HomePage.qml, SettingsPage.qml, ConflictsPage.qml, ComposePage.qml (changed)
├── Main.qml / qml.qrc / qmldir (changed: registration of the pages and bridges)
└── strings_de.tsv (changed)

app/                               # main.cpp (changed): create the two bridges, pass the store

tests/
├── support/fake_carddav_server.h (new), mail_env / event_env helpers (changed: contact_env.h new)
├── unit/        test_vcard_parse, test_vcard_edit_roundtrip, test_malformed_vcard, test_contact_edit_validation,
│                test_contact_sort_search, test_store_migration_2 (new); test_address_suggestions (changed)
├── contract/    test_carddav_discovery, test_carddav_requests (new)
├── integration/ test_contacts_sync_pull, test_contact_push, test_contact_conflict, test_contact_server_deleted,
│                test_contact_failed, test_contacts_kill, test_contacts_readonly, test_contacts_perf,
│                test_sync_with_contacts (new); test_idle_no_network, test_perf_budgets, test_eviction_sync,
│                test_store_threads, test_redirect_credentials, test_tls_cert_failures (changed)
├── bridge/      test_contacts_bridge, test_contact_edit_bridge (new); test_compose_bridge, test_settings_bridge,
│                test_conflicts_model, test_home_status (changed)
├── qml/         tst_contacts_flow.{cpp,qml} (new); stubs.qml, test_qml_pages.cpp (changed)
└── fixtures/    carddav/, vcard/ (new, anonymized, `.unverified`)

scripts/ check-constitution.sh (R8 words + self-test), run-host-demo.sh (address book demo data), pre-merge.sh (no change expected)
docs/    live-server-checklist.md, device-test-checklist.md, reference/security-review.md (rows for contacts)
README.md, CHANGELOG.md, VERSION (0.3.0), specs/001-.../spec.md (pointer note in Out of Scope)
```

**Structure Decision**: keep the layered layout of spec 001 (`core/` -> `bridge/` -> `qml/` -> `app/`). The contact code is
added beside the calendar code with the same shape (client, sync, store part, edit module, bridges, pages) so that a
reader who knows the calendar knows the contacts. The only change to existing behaviour is the extraction of shared DAV
code (refactor under green tests), the schema bump with the first built-in migration, and the address suggestion order.

### Increments (the plan may deliver in this order; each ends green and is usable)

1. **Foundation**: `dav_common` extraction (refactor, CalDAV tests green); schema 2 migration + backup handling
   (`test_store_migration_2`); `vcard.h` parse / derive / strip with the malformed corpus.
2. **Read (US1, US5-3/4)**: `carddav` discovery / changes / fetch; `contacts_sync` pull; store read API; sync steps;
   `ContactsModel`, `ContactsPage`, `ContactPage` (read-only), Home button, Settings address books.
3. **Write (US2, US3, US4)**: `contact_edit`, lossless `apply_contact_edit`, push with GET-merge-PUT, conflicts,
   `server_deleted`, failed / retry / discard, undo; `ContactBridge`, `ContactEditPage`, conflict section.
4. **Suggestions and polish (US6, US5-1/2)**: `address_suggestions` with contacts; auto-sync trigger and pending
   counts; performance and idle tests; log rule R8; docs, checklists, README, version bump (last).

## Complexity Tracking

> Filled because the plan adds code in place of a dependency and changes the schema version; there is no violation.

| Item | Why needed | Simpler alternative rejected because |
|------|------------|--------------------------------------|
| In-house vCard reader / writer (about 600 lines) instead of a library | No small MIT vCard library cross-compiles for the reader without new dependencies; lossless editing needs control of every line (FR-015); license, size and ARM feasibility checked in R-01 / R-06 | A vCard library: adds a dependency (constitution V, justification would be longer than the code); re-serializing a parsed model loses unknown properties (data loss, Principle III) |
| Schema 2 + first built-in migration | Contacts are first-class synced data with constraints, and `sync_state.problem_area` needs a new value | `CREATE TABLE IF NOT EXISTS` at open: downgrade-safe but cannot change the CHECK, hides the schema from the version, and skips the tested one-way upgrade the constitution asks for (kept as fallback if the owner prefers it, R-07) |
| `dav_common` extraction | Avoids two copies of the host-check and status-mapping code | Copying into `carddav.cpp`: two places to fix a security bug |
| GET-merge-PUT for edited contacts | Keeps server-side photos and unknown data without storing them (R-05) | Storing full cards: breaks the storage and RAM budgets; sending the stripped card: deletes photos on the server |
