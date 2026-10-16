# Quickstart: build and validate on the host

**Feature**: [spec.md](spec.md) | **Plan**: [plan.md](plan.md)

A runnable guide to build the core on Linux x86-64 and check the feature end to end **without the device and
without any real account**. Device and live-server checks are listed at the end and stay "unverified" until run.
The data model is in [data-model.md](data-model.md), the interfaces in [contracts/](contracts/).

## 1. Prerequisites (Ubuntu/Debian-style host)

```bash
sudo apt install build-essential cmake ninja-build libcurl4-openssl-dev libssl-dev openssl
# optional, for the bridge and QML tests (Qt 6.8+; the development host has 6.10):
sudo apt install qt6-base-dev qt6-base-dev-tools qt6-declarative-dev qt6-declarative-dev-tools qml6-module-qtquick qml6-module-qttest
# optional, only for scripts/update-tzdata.sh:
sudo apt install tzdata   # provides zic (/usr/sbin/zic) and the source tz data
```

The vendored pieces (SQLite with FTS5, Mozilla CA bundle, Noto Emoji, tz data) need no installation.
The `openssl` command-line tool is required by the TLS tests (they create a throw-away CA); tests skip with a
message when it is missing.

## 2. Configure, build, test

```bash
cmake -S . -B build-host -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build-host -j
ctest --test-dir build-host --output-on-failure            # all host tests
ctest --test-dir build-host -L unit                         # a label: unit | contract | integration | bridge | qml
```

Sanitizer run for the parsers and the interruption tests (Principle I, SC-010):

```bash
cmake -S . -B build-san -G Ninja -DCMAKE_BUILD_TYPE=Debug -DPBO_SANITIZE=ON
cmake --build build-san -j && ctest --test-dir build-san -L "unit|contract|integration" --output-on-failure
```

The full local gate before any merge (Principle VIII): `scripts/pre-merge.sh` (build, tests, secret scan,
`check-constitution.sh`, `check-fixtures.sh`, `check-ui-style.sh`, `check-version-bump.sh`).

Expected result today of a fresh clone of this branch: the baseline's notes tests still build and pass until
the rename/removal tasks delete them; new tests are added test-first and are **red** until their code exists.

## 3. Validation scenarios (what proves the spec)

**Try the app by hand (host demo).** `scripts/run-host-demo.sh` starts `pbo_fake_imap` (a fake IMAP server with a
throw-away certificate authority and 40 demo messages; built from `tests/tools/pbo_fake_imap.cpp`) and the desktop app with
a throw-away data folder; enter the printed server, port, user name and password in the account form, tap Test, Save,
and the inbox appears after the first sync. The server is **fake**: it shows that the app works against this harness, not
against a real provider. `pbo_fake_imap --listen 0.0.0.0 --host <address> --ca-out ca.pem` serves a device on the LAN
(still TLS with its own CA) for `pbo-probe --imap-selftest` (INSTALL.md, "IMAP behaviour check").

Each scenario names the test that automates it. Run them by name with `ctest -R <name>`.

| # | Story / requirement | Scenario | Automated by | Expected |
|---|--------------------|----------|--------------|----------|
| 1 | US1-1, FR-001, FR-003 | Connection test against fake IMAP/SMTP: good, wrong password, expired / wrong-host / unknown-CA certificate, refused STARTTLS, server down | `test_connection_test_mail`, `test_tls_cert_failures`, `test_imap_tls_failures` | specific cause key each time; **zero** credential bytes seen by the server on every TLS failure |
| 2 | US1-2/3, FR-005..007, FR-011, SC-002 | First sync of a 30-day window (fixtures), then open every message with the transports removed | `test_mail_sync_pull`, `test_offline_read` | every windowed message readable; `older_on_server` set |
| 3 | US1-4, FR-008, FR-009 | HTML-only mail | `test_html_text`, `test_reader_no_remote` | readable text, links as `[n]` text, no connection attempted |
| 4 | US1-5, FR-010 | Attachment list, download under/over the 10 MiB limit, offline | `test_attachment_fetch` | limit message, no partial file left |
| 5 | US1-6, FR-006 | Older mail on demand | `test_fetch_older` | extra messages appear; window unchanged |
| 6 | US2-1..3, FR-002, FR-021 | CalDAV discovery, calendar choice, feed, agenda paging | `test_caldav_discovery`, `test_feed`, `test_agenda` | calendars listed with writability; feed read-only |
| 7 | US2-4..6, FR-022..024, SC-009 | 30+ time-zone/recurrence cases incl. DST crossings | `test_tz_dst`, `test_rrule`, `test_occurrences_golden` | 100% expected local dates/times |
| 8 | US3-1/2, FR-012, FR-013 | Reply/reply-all/forward drafts; kill at random points; restart | `test_compose`, `test_draft_autosave_kill` | draft intact (<= 2 s of typing lost) |
| 9 | US3-3..6, FR-014..017, SC-007 | Send with failures injected at every step, repeated taps and retries | `test_outbox_state_machine`, `test_smtp_injection`, `test_no_double_send` | fake server accepts a Message-ID at most once; unknown outcome shown as `uncertain` |
| 10 | FR-016, clarification | Sent copy: switch off, switch on, APPEND fails, APPEND reply lost | `test_sent_copy` | exactly one copy; no second SMTP session |
| 11 | FR-013 mirror | Draft mirror: edit, send, delete, interruption between APPEND and delete, both-sides change | `test_draft_mirror` | never zero copies; conflict = two drafts |
| 12 | US4, FR-025..028, SC-008 | Create/edit/delete offline then sync; server edit in between; server delete; calendar removed | `test_event_push`, `test_event_conflict`, `test_event_server_deleted` | conditional requests; both versions kept (100%) |
| 13 | US5, FR-018..020, clarifications | Flags/move/delete offline; flag merge; message gone on server; no Trash folder; search 5,000 headers | `test_mail_push`, `test_store_organize`, `test_delete_no_trash`, `test_search_perf` | merge rule holds; "could not apply" notes; search < 3 s on the host (device figure unverified) |
| 14 | US6, FR-030..038, SC-011 | Home status, Auto Sync decisions, idle network, window change, custom CA, About offline | `test_sync_scheduler`, `test_auto_sync`, `test_idle_no_network`, `test_private_ca`, `test_about_files`, `test_about_bridge`, `test_store_eviction`, `test_window_change` | no connection while idle; protected rows never evicted |
| 15 | FR-040, SC-010 | 50 malformed mails + 20 malformed calendar items + generated mutations | `test_malformed_mail`, `test_malformed_ical`, `test_malformed_imap`, `test_malformed_xml` | no crash/hang under ASan/UBSan; each item readable-as-far-as-possible or unreadable |
| 16 | SC-006 | Kill at a random point 100 times per scenario (compose, send, edit, sync) | `test_draft_autosave_kill`, `test_event_edit_kill`, `test_kill_matrix` | 0 lost drafts/outbox/edited events; the sync is killed 100 times too, and the next sync finishes what was left once |
| 17 | FR-041 | Logs contain no personal data | `test_log`, `test_error_mapping`, `check-constitution.sh` (R4, R6, R8, R10) | clean; no libcurl tracing, no host/credential text in errors or logs |
| 18 | Principle VIII | Rename completeness and licenses | `check-constitution.sh`, `test_license_consistency` | no `pbnotes` left; manifest = NOTICE = About = CMake dependencies |
| 19 | FR-016, K4 | Provider presets and budgets | `test_provider_presets`, `test_calendar_account_input`, `test_perf_budgets` | presets exact-host only; calendar form refuses `http://`; host proxies for RSS/time budgets pass (device figures unverified) |
| 20 | FR-023, I8, E3 | Zone picker, UIDVALIDITY guard, clock warning | `test_user_zone`, `test_uidvalidity_guard`, `test_clock_skew`, `test_clock_skew_caldav`, `test_clock_skew_imap` | `Unresolved` zone asks once and never assumes one; no UID-addressed write after a UIDVALIDITY change or a failed `STATUS`; skew above 600 s warns, no time stamp means no warning |

Optional once, to cover the SDK's libcurl version: build curl 8.16.0 from source into a scratch prefix and run the
`unit|contract|integration` labels against it (`-DCMAKE_PREFIX_PATH=<prefix>`); record the result in
`docs/device-findings.md` next to the device self-test (`pbo-probe --imap-selftest`).

## 4. Device build and checks (unverified until run on the InkPad One)

```bash
export POCKETBOOK_SDK=/path/to/arm-buildroot-linux-gnueabihf_sdk-buildroot
cmake -S . -B build-arm -DTARGET_TYPE=ARM -DCMAKE_BUILD_TYPE=Release
cmake --build build-arm -j
scripts/package-device.sh build-arm/app/pboffice dist
```

Then follow `INSTALL.md` (copy `applications/`, register `U_PbOffice` in `view.json`, reboot), run
`scripts/probe-device.sh` (records `docs/device-findings.md`), and work through
`docs/device-test-checklist.md`. Every item stays **unverified** until a date and a result are written next
to it. The live-server checklist (`docs/live-server-checklist.md`) does the same for Dovecot, Gmail,
Outlook.com, the author's provider, and Nextcloud/OpenCloud, using app-specific passwords and throw-away
test accounts.
