# Implementation Plan: Email and Calendar MVP (office-pocketbook)

**Branch**: `001-email-calendar-mvp` (`feature/001-email-calendar-mvp`) | **Date**: 2026-10-13 | **Spec**: [spec.md](spec.md)

**Input**: Feature specification from `/specs/001-email-calendar-mvp/spec.md` (10 clarifications, session 2026-10-13)

## Summary

One mail account (IMAP/SMTP) and one calendar source (CalDAV and/or a read-only feed) on the PocketBook
InkPad One, mostly offline, syncing only on app start, on request, and shortly after a send or save.

Technical approach: keep the stack and layout of notes-pocketbook (C++17 Qt-free `core/`, Qt `bridge/`,
Qt 6 Quick `qml/`, device glue in `app/` and `platform/pocketbook/`, CMake host build + ARM cross-build
with the SDK via `POCKETBOOK_SDK`, SQLite, libcurl + OpenSSL, bundled Mozilla CA bundle,
`scripts/pre-merge.sh`) and replace the notes-specific code with mail and calendar modules:

- **Mail transport**: libcurl IMAPS/SMTPS (verified present in the SDK's libcurl 8.16.0; behavior proven on the
  host with a spike), behind `ImapTransport`/`SmtpTransport` seams; own IMAP response parser; UID +
  UIDVALIDITY (+ optional CONDSTORE) incremental sync; no IDLE, no background activity.
- **Mail content**: in-house bounded MIME parser, built-in charset tables (+ optional iconv), in-house
  HTML-to-text with visible links; only readable text is stored; FTS5 search.
- **Sending**: outbox state machine (`queued -> sending -> sent_copy_pending | failed | uncertain`) committed
  before any SMTP byte; "outcome unknown" is never retried automatically; Sent copy by APPEND unless the
  account's switch says the server does it; drafts mirrored to the server Drafts folder.
- **Calendar**: CalDAV through libcurl/HTTPS (PROPFIND/REPORT/PUT/DELETE, ETag, ctag/sync-token),
  in-house XML reader, iCalendar and RRULE subset in the core, **bundled IANA time-zone data** (TZif) for DST
  correctness, lossless event editing, both-versions-kept conflicts.
- **Storage**: one SQLite database, one-way migrations, protected rows never evicted; credentials in a
  separate plain-text `0600` file with an honest, runtime-checked warning.
- **UI**: e-ink pages built from the baseline's generic QML widgets; offline/unsynced/last-sync always visible.
- **Tests**: test-first; fake IMAP/SMTP/CalDAV servers in-process over real TLS so the production
  transports run end to end; kill-matrix and malformed-input corpus; device and live-server checks listed
  and marked **unverified**.

All design alternatives, with the recommendation taken, are in [research.md](research.md) (R-01 to R-21).

## Technical Context

**Language/Version**: C++17 (core, bridge, app glue); QML for Qt 6.8 (SDK Qt 6.8.2); CMake >= 3.16 (host has 4.2); GCC 15.2 cross-compiler from the SDK

**Primary Dependencies**: libcurl (IMAP, SMTP, HTTPS; SDK 8.16.0), OpenSSL 3.x (TLS via libcurl, random ids, tests), SQLite 3.54.0 vendored with FTS5, Qt 6.8 Core/Gui/Qml/Quick (UI only). Data assets: Mozilla CA bundle, Noto Emoji, IANA tz data (new). **No new library.**

**Storage**: SQLite `pboffice.db` (schema 1, one-way migrations), `credentials` file (0600 where honored), downloaded attachments in the Downloads folder (real names); see [data-model.md](data-model.md)

**Testing**: the baseline's header-only test support + CTest (labels unit, contract, integration, bridge, qml); in-process fake IMAP/SMTP/CalDAV servers over TLS; malformed-input corpus; kill-matrix; ASan/UBSan build option; manual device and live-server checklists (**unverified** until run)

**Target Platform**: PocketBook InkPad One (RK3566, 32-bit ARM hard-float Linux userland, Qt via QPA `pocketbook2egl`); host Linux x86-64 for the core, tests and the Qt app

**Project Type**: single repository: Qt-free library + Qt bridge + QML app (device app with host-testable core)

**Performance Goals**: cold start to home < 5 s (SC-003); page turn in a 5,000-header list < 1 s and search < 3 s (SC-004); open a 1 MB message < 2 s (SC-005); first sync of 500 messages < 10 min on home Wi-Fi (SC-014). All **unverified** until measured on the device

**Constraints**: offline-first; no background network (SC-011); TLS verification always on, enforced STARTTLS only; no web engine, no remote content except the pictures of one message on the user's tap (constitution 1.1.0, FR-009); no data loss on kill/battery/Wi-Fi drop; RAM <= 150 MB browsing / <= 250 MB peak sync, package <= 25 MB, data <= 200 MB default (budgets in research R-19, **unverified**); no QRESYNC, no IDLE

**Scale/Scope**: one mail account, one calendar source; sync window 30 days of mail (default) and 30 days back / 365 days ahead of calendar; folders with tens of thousands of messages tolerated by windowing; 16 screens, 6 user stories (3 priority levels)

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-checked after Phase 1 design.*

| Principle | Before research | After design | Evidence / notes |
|-----------|-----------------|--------------|------------------|
| I. Test-First | PASS (obligation) | PASS | Strategy in research R-20; contract tests for every wire subset and interface change ([contracts/](contracts/)); malformed/truncated/oversized corpus for MIME, iCalendar, IMAP, XML; fakes only, no real accounts. `tasks.md` must order each test task before its implementation task and require the red run |
| II. Host-testable core, layers | PASS | PASS | `core/` stays Qt-free and InkView-free; the only system library beyond libcurl/OpenSSL/SQLite is libc `iconv`, optional at run time. Every external dependency (network, clock, random, files) is an interface ([contracts/core-interfaces.md](contracts/core-interfaces.md)). Host and ARM targets both kept |
| III. No silent data loss | PASS (obligation) | PASS | Drafts/outbox/events persisted before network (data-model); `sending` committed before SMTP; unknown outcome => `uncertain`, never auto-resent (R-08); conditional PUT/DELETE, `412` => both versions (R-18); UIDVALIDITY rebuild carries pending work (R-04); no bare `EXPUNGE`; every delete confirmed, Trash by default, extra warning when no Trash exists. Every UID-addressed write is preceded by a `STATUS`/`UIDVALIDITY` check (research R-18); `UID EXPUNGE` is limited to UIDs the app itself superseded (move source after a successful COPY, replaced own draft after a successful APPEND), recorded as the reading of "expunge requires confirmation" in research R-18. **Unverified**: move/delete behavior on servers without `UIDPLUS`/`MOVE` (open item O-11) |
| IV. Security & privacy | PASS (obligation) | PASS | TLS verify always, `imaps/smtps/https` or enforced STARTTLS, no plaintext fallback (R-12); credentials: better-than-plaintext options evaluated and the decision recorded (R-13, user decision A); honest runtime statement because `chmod` may not work on ext1; HTML converted in-house, no remote content; attachments on demand with limit, never executed; logs exclude addresses, subjects, hosts, folders, titles ([contracts/core-interfaces.md](contracts/core-interfaces.md) §9) |
| V. Simplicity & bounded footprint | PASS | PASS with justifications | No new library. New code instead of dependencies (XML reader, MIME, iCalendar/RRULE, IMAP parser) and one new data asset (IANA tz data) are justified in Complexity Tracking. Budgets stated (R-19) and checked by host proxy tests (`test_perf_budgets`), a package-size check in `package-device.sh`, and RSS capture in the device checklist. Mailbox bounded by window; eviction never touches protected rows |
| VI. E-ink UX & battery | PASS | PASS | Paged lists, no animation, offline/unsynced/last-sync always shown ([contracts/bridge-qml.md](contracts/bridge-qml.md)); network only on start/request/after save; idle test (SC-011) |
| VII. Licensing hygiene | PASS | PASS | MIT project; components in R-21; IANA tz data is public domain; Windows zone-name mapping facts attributed in NOTICE; no GPL/LGPL-incompatible code; K-9/Etar/DAVx5 reference-only (nothing copied); InkFeed/notes patterns keep attribution. `NOTICE`, `licenses/manifest.txt`, About box updated in the same change that adds tz data |
| VIII. Quality gates | PASS | PASS | `pre-merge.sh` kept; new mechanical rules R7 (`USE_SSL` other than ALL, plaintext schemes outside the factory), R8 (more forbidden log-field words), R9 rename-completeness (`pbnotes`), R10 (no libcurl tracing), UI-style additions (no remote Image/WebView/openUrlExternally), each with a self-test. ARM build reproducible, SDK only from `POCKETBOOK_SDK`/`-DSDK_PATH` (R2 stays) |
| IX. Specs-driven, honest verification | PASS | PASS | Spec -> clarify -> plan done; unverified labels on every device/live-server item (research "Open items", O-1..O-12); hand-written server transcripts get `.unverified`. Tests that use a fake IMAP/SMTP/CalDAV server print an "UNVERIFIED against real servers" banner (`PBO_FAKE_SERVER_TEST()`) and carry the ctest label `fake-server`; `scripts/check-fake-markers.sh` enforces it. |

**Technology Stack & Build Constraints**: stack unchanged (C++17, Qt 6.8, CMake, libcurl, OpenSSL, SQLite): no
amendment needed. "Verify the device libcurl supports IMAP/SMTP before the email design is fixed":
done for the **SDK** libcurl (research R-01, evidence [SDK]/[SPIKE]); the **device** libcurl is covered by the
probe (R-02, open item O-1) with a documented fallback seam (`ImapTransport`). "Rename before the first
device build": first increment (below).

**Gate result**: no unjustified violation. The scope question of Complexity row 6 was decided by the clarification of 2026-10-13.

## Project Structure

### Documentation (this feature)

```text
specs/001-email-calendar-mvp/
├── spec.md                      # feature specification (clarified)
├── plan.md                      # this file
├── research.md                  # Phase 0: SDK/libcurl findings, decisions, alternatives, open items
├── data-model.md                # Phase 1: SQLite schema, state machines, eviction
├── quickstart.md                # Phase 1: host build, validation scenarios, device/live steps
├── contracts/
│   ├── core-interfaces.md       # C++ seams of the core and bridge
│   ├── imap-smtp-subset.md      # wire subset, outcome rules, fake-server scenarios
│   ├── caldav-ical-subset.md    # DAV requests, iCalendar/RRULE/time-zone subset
│   ├── bridge-qml.md            # bridge objects, pages, navigation, e-ink rules
│   └── device-package.md        # install layout, runtime data, build and probe contract
├── checklists/requirements.md   # spec quality checklist (existing)
└── tasks.md                     # Phase 2 (/speckit-tasks) - not created here
```

### Source Code (repository root)

The tree below is the target. Files marked `(baseline)` already exist and are renamed/adapted; files marked
`(remove)` are notes-specific and are deleted in the first increment; everything else is new.

```text
CMakeLists.txt, VERSION, LICENSE, NOTICE, README.md, INSTALL.md, CHANGELOG.md (new)
cmake/                       sdk.cmake, qt_manual.cmake (baseline; names pbnotes_* -> pbo_*)

core/                        # Qt-free, InkView-free (library `office_core`, namespace pbo)
├── CMakeLists.txt
├── include/pbo/
│   ├── types.h, log.h, utf8.h, paging.h, version.h, about.h, ca_selector.h        (baseline, adapted)
│   ├── http_transport.h, curl_transport.h                                         (baseline, +PROPFIND/REPORT)
│   ├── sync_scheduler.h, sync_status.h                                            (baseline, adapted inputs)
│   ├── imap_transport.h, smtp_transport.h, curl_session.h                         # libcurl IMAP/SMTP, shared TLS policy
│   ├── imap_parse.h, mime.h, html_text.h, charset.h, mutf7.h                      # parsers (total, bounded)
│   ├── uidvalidity_guard.h, provider_presets.h, account_input.h, calendar_account_input.h, clock_check.h   # guards, presets, validation
│   ├── compose.h                                                                  # build_message, quoting, addresses
│   ├── xml_min.h, caldav.h, feed.h, ical.h, rrule.h, tz.h                         # calendar stack
│   ├── store.h, store_types.h, credentials_file.h                                 # SQLite + credentials file
│   ├── mail_sync.h, outbox.h, draft_mirror.h, calendar_sync.h, sync.h              # sync engine
│   └── connection_test.h                                                          # (baseline, adapted: mail + calendar)
├── src/                     # one .cpp per header + store_sql.h (baseline), store_migrations.cpp, log_codes.h
│   └── (remove) api_client, attachment_*, edit_history, json, links, markdown*, note_store*, slash,
│       sync_engine, sync_attachments, text_clean, title
└── CMakeLists.txt builds vendored SQLite with SQLITE_ENABLE_FTS5

bridge/                      # Qt Core only; QObject facades (library `pbo_bridge`)
├── include/ + src/          sync_bridge, account_bridge, folder_model, message_list_model, reader_bridge,
│                            compose_bridge, outbox_model, agenda_model, day_model, month_model,
│                            event_bridge, settings_bridge, about_bridge (baseline, adapted), worker thread helper
│   └── (remove) notes_bridge, edit_bridge, reader_blocks, attachments_bridge, attachment_image_provider,
│                file_preview_provider, image_texture_factory

qml/                         # Qt Quick UI
├── Style.qml, TopBar, BottomPanel, PageButtons, ConfirmDialog, DialogBox/Frame, ChoiceDialog, TextEntryDialog,
│   InkField, InkSwitch, InkRow, InkSlider, RowDivider, IconButton, Icon, FooterButton, ToolButton,
│   DateTimePicker, EmojiPicker, SelectableText + Selection*, PopupBubble          (baseline, reused)
├── Main.qml (navigation), HomePage, FoldersPage, MessageListPage, ReaderPage, ComposePage, OutboxPage,
│   SearchPage, AgendaPage, DayPage, MonthPage, EventPage, EventEditPage, ConflictsPage,
│   AccountSetupPage, SettingsPage, TimeZonePage, AboutPage, Strings_de.js, qml.qrc
└── (remove) AttachmentView, BlockText, TableRowView, TitleBlock, SlashMenu, SelectionMenu (notes-specific)

app/                         # InkView glue, main.cpp, device_bridge (baseline, renamed)
platform/pocketbook/pboffice.app        # launcher (baseline pbnotes.app renamed)
app/probe/pbo_probe.cpp      # device probe (+ --imap-selftest); tests/tools/pbo_fake_imap.cpp + scripts/run-host-demo.sh: host demo
scripts/                     # pre-merge.sh, check-*.sh (extended, + check-fake-markers.sh), package-device.sh, probe-device.sh (+pbo-probe),
                             # update-tzdata.sh (new), make-icons.sh
third_party/                 # sqlite (FTS5), cacert, noto-emoji (baseline), tzdata/ (new: compact TZif + VERSION)
licenses/, NOTICE            # manifest updated with tz data and the zone-name mapping
res/icons/                   # pboffice*.bmp
docs/                        # reference/ (baseline), device-findings.md, device-test-checklist.md,
                             # live-server-checklist.md (new, created when first filled)

tests/
├── unit/        parsers, tz/rrule, compose, store, outbox, merge, eviction, scheduler, status, about
├── contract/    IMAP/SMTP/CalDAV exchanges against the fakes; interface contract tests
├── integration/ end-to-end sync with real transports over TLS to the fakes; kill-matrix; idle network; TLS failures
├── bridge/, qml/, launcher/
├── fixtures/    anonymized mails, ICS, server transcripts (*.unverified), malformed/ corpora
└── support/     test_support.h, tmp_dir.h, tls_test_server.h (baseline); fake_imap_server.h, fake_smtp_server.h,
                 fake_caldav_server.h, fake_transport.h (adapted), kill_harness.h (new)
```

**Structure Decision**: the layered single-repository layout of notes-pocketbook is kept unchanged
(Constitution II). Namespace and targets are renamed `pbnotes` -> `pbo`/`office_core`, binary and data names
`pboffice` (data directory `/mnt/ext1/system/pboffice`, package `pboffice-inkpad-one-<version>.zip`, view.json
id `U_PbOffice`), so the app can never overwrite or share data with notes-pocketbook.

## Delivery increments

The spec is one release-sized feature; delivery is split along story priority so each increment builds,
passes its tests and is independently demonstrable (spec Assumptions). Details become `tasks.md`.

| Increment | Content | Stories | Exit criterion |
|-----------|---------|---------|----------------|
| I0 Foundations | Rename to `pboffice`; delete notes code and tests; new CMake targets; store skeleton + migrations framework; fake-server harness; tz data import (`update-tzdata.sh`); extended checks R7/R8; `pbo-probe` | - | host tests green with only baseline-generic tests; `check-constitution` self-tests for new rules; ARM build links (device run unverified) |
| I1 Read mail offline | account setup + connection test, IMAP transport/parsers, folder list, window sync, MIME/HTML text, reader, attachments on demand, older-on-demand, minimal settings/home status | US1 (P1) | scenarios 1-5, 15 (mail part) of the quickstart |
| I2 See my agenda | XML reader, CalDAV discovery/sync, feed, iCalendar, RRULE, tz, occurrence cache, agenda/day/month/event views | US2 (P1) | scenarios 6-7, 15 (calendar part) |
| I3 Write and send | compose, quoting, drafts + autosave + mirror, outbox state machine, SMTP, Sent copy, kill-matrix for send | US3 (P2) | scenarios 8-11, 16 (mail part) |
| I4 Edit events | lossless edit, conditional writes, conflicts, server-deleted, read-only handling | US4 (P2) | scenario 12 |
| I5 Organize and find | flags, move, delete, Trash rules, merge, "could not apply", sort, FTS5 search, address suggestions | US5 (P3) | scenario 13 |
| I6 Home, sync control, settings, finish | home page, Auto Sync, sync window/limits/eviction, custom CA status, About with licenses, packaging, docs, version bump and changelog (last phase of `tasks.md`) | US6 (P3) | scenarios 14, 17-18; device checklist drafted |

## Complexity Tracking

> Required by Principle V (a further dependency needs a written justification) and Governance (deviations).

| # | Item | Why needed | Simpler alternative rejected because | License / size / ARM |
|---|------|------------|--------------------------------------|----------------------|
| 1 | **IANA tz data** bundled (`third_party/tzdata`, compact TZif, ~0.4 MB) - a data dependency | Spec clarification: DST-correct events independent of the device (SC-009); device time-zone data is unknown and old data would shift events | Using the device's `/usr/share/zoneinfo`: not verified to exist or be current; host and device results would differ, making SC-009 untestable on the host | Public domain; no build step needed (files committed, regenerated by `update-tzdata.sh` with `zic -b slim`); pure data, no cross-compile issue; manifest/NOTICE/About updated |
| 2 | In-house parsers (IMAP responses, MIME, HTML-to-text, XML reader, iCalendar/RRULE, TZif) instead of libraries | Each library candidate adds a dependency to justify (GMime/GLib, libxml2, libical, expat) and a hostile-input surface, while only a small, fixed subset is needed; own code is bounded by explicit limits and fuzz-style corpora | libxml2/expat/libical/GMime: considered in research R-05, R-07, R-15, R-16; rejected for size, dependency count, cross-build friction and because they do not remove the need to bound and test untrusted input | n/a (own code, MIT); estimated 4,000-5,000 lines in total; every parser has a malformed-input test |
| 3 | SQLite FTS5 enabled | Meets SC-004 (search < 3 s over 5,000 headers with body text) offline | Application-side scan (as in notes) is too slow for bodies up to the 200 MB cap; `LIKE` has the same cost | Already in the vendored amalgamation (compile flag only); index adds ~30-60% of text size, counted against the storage limit |
| 4 | Draft mirroring to the server Drafts folder (user clarification) | Spec FR-013 | Local-only drafts were the simpler option and were declined | extra sync step with APPEND-then-delete ordering (R-11) |
| 5 | Windows time-zone name table (~40 entries) | Outlook-style `TZID` values from Microsoft-origin events would otherwise show as unknown zones | Treat all non-IANA names as unknown (events at wrong local time on common accounts) | facts from CLDR `windowsZones` (Unicode license) credited in NOTICE |
| 6 | **Decided (clarification 2026-10-13)**: repeating events are read-only in this release - view and delete the whole series only; single occurrences and whole-series edits are out of scope | FR-025 now says so; changing a series needs `RECURRENCE-ID`/`THISANDFUTURE` handling with high data-loss risk on servers not yet verified (O-5) | Full series editing: large, and conflicts with "no silent data loss" until real-server behavior is verified | none; a later increment if wanted |

## Decisions the author should confirm (defaults chosen in this plan)

1. **Names**: app/binary/data `pboffice`, namespace `pbo`, library `office_core`, view.json id `U_PbOffice`, title "PB Office".
2. *(decided by clarification 2026-10-13)* Repeating events are read-only; whole-series delete only (FR-025, Complexity row 6).
3. **`VTIMEZONE` blocks are ignored**; time zones come only from the bundled IANA data and the small Windows-name table; unknown names show as floating with a note (research R-16).
4. **Search is word/prefix-based** (FTS5), not arbitrary substring search (research R-17).
5. **No QRESYNC/IDLE**; CONDSTORE optional (research R-03).
6. **Credentials file on the ext1 partition** with a runtime-checked honest statement; a protected location is only used if the device probe finds one (research R-13).
7. **User time zone** from the Settings choice, then the reader's setting (`TZ`, then `/etc/localtime`); if neither yields a zone the app asks once from a list (FR-023, clarification 2026-10-13); there is no InkView API (research R-16).
8. **German translations** compiled in as a QML string table because `lrelease` does not run with the SDK ([contracts/bridge-qml.md](contracts/bridge-qml.md) §4); English remains the fallback.

## Post-design Constitution re-check

Re-evaluated after the design artifacts above: all nine principles PASS (table above, "After design"
column). No item is open: Complexity row 6 is decided (clarification 2026-10-13). Open verification work is explicit and labelled **unverified** in
[research.md](research.md#open-items-to-verify) (O-1 to O-12).
