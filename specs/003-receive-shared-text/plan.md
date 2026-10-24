# Implementation Plan: Receive Shared Text

**Branch**: `003-receive-shared-text` (`feature/003-receive-shared-text`) | **Date**: 2026-10-23 | **Spec**: [spec.md](spec.md)

**Input**: Feature specification from `/specs/003-receive-shared-text/spec.md` (2 clarifications, session 2026-10-23; the
decision on the second one is the fallback of FR-005, see Open decision O-1 below)

## Summary

Another app starts PB Office with a text (and optionally a subject and one recipient); PB Office opens a new message with them,
saved as a draft from the first moment, and never lets the text leak into a log, the screen or a long-lived command line.

Technical approach: **no new dependency and no schema change**; a hand-over becomes an ordinary draft.

- **Cold start**: the launcher `pboffice.app` takes the arguments in its first lines into a private pending entry
  (`system/pboffice/handoff/<id>/`, one raw file per argument, renamed when complete) and **executes itself again without
  arguments**, so the text is not in the command line of the process that lives for the whole session (R-02). A mechanical check
  (new rule R11) keeps the launcher from expanding its arguments anywhere else.
- **Receiving**: a Qt-free parser (`core/handoff`) with fixed rules (limits, cleaning, `textlen` check against truncation, one plain
  recipient address, no header injection) and a folder reader (`core/handoff_inbox`, read at start, at foreground and on a wake
  signal only; entries expire after 10 minutes) (R-04, R-07).
- **Message**: `HandoffBridge` saves the draft in the store **before** any screen exists, then asks the UI to open
  `ComposePage { draftId }`; an open message is saved first; one draft per hand-over; no sync call (R-06).
- **Running instance** (the open risk): the SDK strings suggest that the system routes the arguments of a second start to the running
  task as a `REQ_OPENBOOK2` request, which a request listener can receive. This is **unverified**. The plan builds the common core
  first (increment 1) and gates the running-instance mechanism on two device experiments (E1 stand-in target, E2 inside PB Office):
  B1 listener, B2 wake signal or restart, B3 contract v2 (the sender writes the pending entry) (R-05).
- **Tests**: test-first; parser, folder reader, bridge, header-injection, launcher (shell) and QML tests on the host; rule R11 with
  a self-test; device experiments E1 to E8 recorded as `U` rows until observed (R-11, [quickstart.md](quickstart.md)).

All alternatives, with the decision taken, are in [research.md](research.md) (R-01 to R-13, experiments E1 to E8, open items
O-1 to O-8). The sender-facing interface is [contracts/handoff-contract.md](contracts/handoff-contract.md).

## Technical Context

**Language/Version**: C++17 (core, bridge, app glue); QML for Qt 6.8 (SDK Qt 6.8.2); POSIX `sh` for the launcher; CMake >= 3.16; GCC 15.2 cross-compiler from the SDK (unchanged)

**Primary Dependencies**: libcurl, OpenSSL 3.x, SQLite 3.54.0, Qt 6.8 Core/Gui/Qml/Quick, InkView (already linked; `SetRequestListener`, `UnpackParameters`, `SetActiveTask` come from `libinkview`). **No new dependency, no new data asset; `libcommon_utilities` is not linked.**

**Storage**: the existing SQLite database `pboffice.db`, **schema 2 unchanged** (the draft table); one new transient folder `system/pboffice/handoff/` (pending entries that live for seconds, at most 10 minutes); see [data-model.md](data-model.md)

**Testing**: the existing header-only test support + CTest (labels unit, bridge, integration, qml, launcher); new: parser and folder-reader unit tests, bridge test with a captured log sink, header-injection integration test, launcher shell test, QML flow test, constitution rule R11 with self-test; device experiments E1 to E8 (**unverified** until run)

**Target Platform**: PocketBook InkPad One (RK3566, 32-bit ARM hard-float Linux userland, QPA `pocketbook2egl`); host Linux x86-64 for core, tests and the Qt app (unchanged)

**Project Type**: single repository: Qt-free library + Qt bridge + QML app (device app with host-testable core), unchanged

**Performance Goals**: editor with the text in under 10 s from the tap in the sender when PB Office was not running, under 3 s when it was (SC-001); ordinary cold start still under 5 s (SC-007); launcher overhead under 100 ms. All **unverified** on the device (E8)

**Constraints**: offline-first; no polling, no watcher, no background activity, no network from a hand-over; no secret in any log, error, diagnostic or long-lived command line; no data loss (draft saved before any screen; entry removed only after the draft exists); untrusted input bounded and cleaned; only the three values travel; any app may send

**Scale/Scope**: one launcher block, two core files, one bridge, three small QML changes, one probe program; at most 8 queued hand-overs of at most 8.3 KiB; 4 user stories (P1, P1, P1, P2)

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-checked after Phase 1 design.*

| Principle | Before research | After design | Evidence / notes |
|-----------|-----------------|--------------|------------------|
| I. Test-First | PASS (obligation) | PASS | Strategy in research R-11: parser, folder reader, bridge, header-injection, launcher and QML tests for every rule of the contract; malformed, truncated, oversized and hostile input (injection strings, control characters, invalid UTF-8, duplicate keys) for the parser; the launcher is tested against a fake device layout. `tasks.md` must order each test before its implementation and require the red run. Device behaviour is not claimed by any host test |
| II. Host-testable core, layers | PASS | PASS | `handoff` and `handoff_inbox` are Qt-free and InkView-free in `core/`; the bridge translates; `app/` holds only the InkView listener, the signal handler and the probe ([contracts/core-interfaces.md](contracts/core-interfaces.md)). Host and ARM targets unchanged |
| III. No silent data loss | PASS (obligation) | PASS | The draft is saved in the store before any screen and before the pending entry is removed; a crash in between yields a duplicate draft, never zero (data-model); an open message is saved first; hand-overs are never merged or replaced; a refused hand-over says so and creates nothing; a text is never cut short silently (`textlen`, limit refusal); the no-account case tells the user the text was not kept. A drop by the system itself (B3) is detected only with contract v2 (O-1) and is stated honestly |
| IV. Security & privacy | PASS (obligation) | PASS | Text, subject and recipient are untrusted: cleaned, bounded, never interpreted, links never opened; the recipient is one validated plain address, line breaks are removed before a draft exists, header injection is tested; logs hold counts and codes only (R-09), notices never repeat a value; the launcher never expands arguments outside the capture block (R11) and executes itself again so the text is not left in a long-lived command line; pending entries live seconds and expire; the residual exposure (milliseconds in the system's start of the launcher; vfat has no `chmod`; flash is not shreddable) is stated in the docs and the About text, not hidden. No credential involved, no network, no new permission |
| V. Simplicity & bounded footprint | PASS | PASS | No new dependency (`libcommon_utilities` rejected: the two needed functions are in `libinkview`). Smallest receiving core first; the three running-instance branches are 15 to 40 lines each and only the selected one is built. Budgets stated (research R-13) and checked by host tests, the package-size check and the device checklist |
| VI. E-ink UX & battery | PASS | PASS | No polling, no watcher: the folder is read at start, at foreground and on a signal; no background network; no animation; the existing `push`/`pop`; one one-button dialog for notices; the idle test is extended to prove no network from a hand-over |
| VII. Licensing hygiene | PASS | PASS | MIT project; no new component, so `NOTICE`, `licenses/manifest.txt` and About stay; the probe adapts patterns of the sibling project's hand-over probe (same author, MIT), attributed in `NOTICE` if code is copied |
| VIII. Quality gates | PASS | PASS | `pre-merge.sh` kept; rule R11 with a self-test added; the launcher test joins CTest; fixtures that imitate system arguments carry `.unverified`; no SDK path in any file (the listener uses `inkview.h` only). Version bump to 0.4.0 and CHANGELOG are the last task |
| IX. Specs-driven, honest verification | PASS | PASS | Spec -> clarify -> plan done; every statement about the reader is labelled (R-01 evidence labels) and appears as `U` rows DT-HO-1..8; the SDK-string reading is labelled "suggests", not "shows" |
| X. English documentation | PASS | PASS | All files of this feature are English; German appears only in `qml/strings_de.tsv` and in test data (umlaut texts) explained by English test names |

**Technology Stack & Build Constraints**: stack unchanged (C++17, Qt 6.8, CMake, libcurl, OpenSSL, SQLite): no amendment.
The rule that the app must never overwrite or share data with notes-pocketbook is unaffected: the
`handoff/` folder is PB Office's own; only the conditional contract v2 would let a sibling app write into it, and that is an
explicit interface (Complexity Tracking). **Development Workflow**: no schema change, so the one-way upgrade rule is not touched.
**Release**: MINOR bump (0.4.0).

**Gate result**: no unjustified violation.

## Project Structure

### Documentation (this feature)

```text
specs/003-receive-shared-text/
├── spec.md                      # feature specification (clarified)
├── plan.md                      # this file
├── research.md                  # Phase 0: decisions, alternatives, device experiments, budgets, test strategy, open items
├── data-model.md                # Phase 1: hand-over, pending entry, draft mapping, state transitions (no schema change)
├── quickstart.md                # Phase 1: host validation scenarios, the proof on the reader
├── contracts/
│   ├── handoff-contract.md      # the interface for any sending app (v1, and v2 if needed)
│   ├── core-interfaces.md       # parser, folder reader, bridge, app glue, launcher block
│   └── bridge-qml.md            # QML wiring, notices, UI tests
├── checklists/requirements.md   # spec quality checklist (existing)
└── tasks.md                     # Phase 2 (/speckit-tasks) - not created here
```

### Source Code (repository root)

New files are marked `(new)`, changed files `(changed)`; everything else is untouched.

```text
core/
├── include/pbo/handoff.h (new)             # parse_handoff, cleaning, limits
├── include/pbo/handoff_inbox.h (new)       # HandoffInbox: take_ready, remove
├── src/handoff.cpp, handoff_inbox.cpp (new)
├── src/log_codes.h (changed)               # handoff.received / refused / expired / drafted
└── src/CMakeLists.txt (changed)

bridge/
├── include/handoff_bridge.h, src/handoff_bridge.cpp (new)
└── CMakeLists.txt (changed)

qml/
├── HandoffNavigator.qml, DialogState.qml (new)   # show / notice connections, held show, notices, account hook; the open-dialog counter (singleton)
├── Main.qml (changed)                      # places HandoffNavigator
├── pages/ComposePage.qml (changed)         # objectName, focus on the first empty field (again once the page is current)
├── pages/AccountSetupPage.qml (changed)    # objectName, setupActive / setupLeft
├── DialogFrame.qml (changed)               # counts itself in DialogState
├── qmldir, qml.qrc (changed)
└── strings_de.tsv (changed)

app/
├── main.cpp (changed)                      # create and expose HandoffBridge, checkInbox() after load
├── handoff_listener.h, handoff_listener.cpp (new)   # device: request listener (B1), SIGUSR1 (B2), foreground; host: no-op. Built after E1/E2
├── probe/pbo_handoff_probe.cpp (new)       # test sender + stand-in target for E1 to E8; counts only
└── CMakeLists.txt, probe/CMakeLists.txt (changed)

platform/pocketbook/pboffice.app (changed)  # capture block (R-02), `run-handoff-probe` flag like run-probe

tests/
├── unit/        test_handoff_parse, test_handoff_inbox (new)
├── bridge/      test_handoff_bridge (new)
├── integration/ test_handoff_header_injection (new); test_idle_no_network (changed)
├── launcher/    test_handoff_launcher.sh (new); test_launcher.sh (unchanged, must stay green)
├── qml/         tst_handoff_flow.{cpp,qml} (new); stubs.qml, test_qml_pages.cpp (changed)
└── fixtures/    handoff/ (new, anonymized texts with umlauts / injection strings, `.unverified` where they imitate system arguments)

scripts/ check-constitution.sh (R11 + self-test), package-device.sh (probe and launcher in the package; no new asset)
docs/    device-test-checklist.md (DT-HO-1..8), device-findings.md (results), reference/security-review.md (rows), handoff-for-app-authors note
README.md, INSTALL.md ("Hand-over probe"), CHANGELOG.md, VERSION (0.4.0)
```

**Structure Decision**: keep the layered layout of spec 001 (`core/` -> `bridge/` -> `qml/` -> `app/`). The hand-over is a small
vertical slice: one parser and one folder reader in `core/`, one bridge, three QML touches, and the device glue in `app/` and the
launcher. A hand-over reuses the draft, the compose page and the Drafts list unchanged, so nothing about a hand-over can drift from
how a message is written, saved, mirrored and sent.

### Increments (each ends green and is usable; 2 and 3 are gated by device sessions)

1. **Cold start, end to end on the host and the first device test (US1, US3, US4; FR-001..004, 006..018, 020)**: parser,
   folder reader, `HandoffBridge`, QML wiring, launcher capture block, rule R11, launcher test, idle test; log events. Installable
   on the reader: a text handed over to a closed PB Office becomes an editor with the text and nothing leaks. This also ships
   the **E1 stand-in target** in the probe so the next session can already run.
2. **Running instance (US2; FR-005)**, after **E1/E2**: build the branch the result selects (R-05): B1 request listener
   (`app/handoff_listener`), or B2 wake signal / restart in the launcher and the app, or B3 contract v2 (reader side is the same folder
   reader). Includes E5 to E7. Gate: spec SC-002 and SC-004 on the reader; until then User Story 2 is **unverified** and says so
   in the docs and the CHANGELOG.
3. **Proof and polish**: E3, E4 (final byte limit), E8 (timing); entries in `docs/device-test-checklist.md` and
   `docs/device-findings.md`; the contract page for app authors; README, INSTALL ("Hand-over probe"), About/security text for the
   residual exposure; version bump (last).

## Complexity Tracking

> Filled because the launcher changes structure (it executes itself again) and one branch of increment 2 would let another app write
> into PB Office's folder; there is no constitution violation.

| Item | Why needed | Simpler alternative rejected because |
|------|------------|--------------------------------------|
| Launcher re-executes itself without arguments | A shell cannot overwrite its own `argv`; only `exec` ends the process that holds the text, and the launcher otherwise lives for the whole session (FR-016) | Passing the text on as an argument or environment variable: visible for the whole session. Piping into the app: lost if the app dies before reading it (Principle III) |
| Pending entries as one raw file per argument | No escaping, no length prefix, no parser in the launcher; binary-safe; the app reads exactly what the system delivered | One file with delimiters: needs quoting and a parser in `sh`, and invites injection through the delimiter |
| `textlen` field in the contract | The receiver cannot otherwise tell a text cut by the system from a short one; a wrong text in a mail is worse than a refusal (FR-009) | Trusting the system to deliver whole: unmeasured above 1500 bytes |
| A second object (`HandoffBridge`) next to `ComposeBridge` | The compose object is shared state; an arriving text must not disturb a message being typed, and the draft must exist before any screen | Reusing `ComposeBridge::start`: would overwrite the open message unless saved and reset, and saves only on change (FR-003) |
| Conditional: contract v2 (sender writes `handoff/`), only if experiments show B3 | In B3 the arguments never reach any PB Office process, so the clarified fallback cannot work (O-1) | Doing nothing: the feature would silently lose texts handed to a running app, which the spec forbids; building v2 now: unneeded if B1 or B2 holds |

## Open decision for the owner

**O-1 (affects FR-005 of the spec)**: the spec says that, when the system cannot deliver the text to a running PB Office, PB Office
restarts or exits so that the next start receives the text. That works only when the text reaches *some* PB Office process (cases
B1, B2). If the experiment shows B3 (the system drops the arguments), nothing can be kept or restarted, and the only way is contract
v2: the sender writes the text into PB Office's `handoff/` folder and starts PB Office with the marker only. The plan builds nothing
for B3 until E1/E2 show it; if they do, the owner is asked to confirm contract v2 and FR-005 is amended to say so.
