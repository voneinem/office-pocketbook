# Research: Receive Shared Text

**Feature**: [spec.md](spec.md) | **Plan**: [plan.md](plan.md) | **Date**: 2026-10-23

Decisions taken before the design was fixed. Evidence labels (constitution Principle IX):

- **[REPO]** read from this repository on 2026-10-23.
- **[SDK]** read from the PocketBook SDK 6.11 headers or from strings of its libraries (`libinkview.so`). Proves that an
  interface exists and what its name and signature say, not how the reader behaves.
- **[DEVICE-OTHER]** observed on 2026-10-23 on the InkPad One with the hand-over probe of the sibling project
  `nextcloud-pocketbook` (`docs/device-findings.md`, section "Starting one app from another with text"). The target there
  was a stand-in script, **not PB Office**.
- **[UNVERIFIED]** assumed; needs the reader with PB Office itself. Each one has an experiment in
  [Device experiments](#device-experiments-the-proof-the-spec-asks-for) and is listed again in
  [Open items](#open-items).

Nothing in this file was run with PB Office on the reader.

---

## R-01 What is known, and what the SDK adds

**[DEVICE-OTHER]** `OpenBook2(<path of an .app>, argc, argv, 0)` starts the file; each argument arrives unchanged (tested to
1500 bytes, non-ASCII letters, quotes, line breaks); the caller keeps running and gets EVT_BACKGROUND, then EVT_FOREGROUND when
the target ends; the system's own log lines name only the path of the started app.

**[REPO]** The launcher `platform/pocketbook/pboffice.app` is a shell script. The system starts *the script*, so the text arrives
as `$1` of a shell that stays alive for the whole session (the launcher runs `pboffice.bin` as a child, not with `exec`).
Today the launcher would therefore keep the text in its own command line for as long as PB Office runs, and its diagnostics line
`ps | grep -E 'pboffice\.bin|inkfeed\.bin'` could print it if the text contains such a word. Both are exactly the leaks the
spec forbids (FR-015, FR-016) and are the first things the design must close.

**[SDK]** Three facts about the running-instance case, which is the open risk of the spec:

1. `libinkview.so` contains the strings `hw_task_findbyappname`, `sendParametersToTaskNoWait: REQ_OPENBOOK2` and
   `SendRequestTo: REQ_OPENBOOK2` inside `iv_OpenBook`. Read together with the 2026-10-23 log lines (`task_findbyappname`), this
   says: when `OpenBook2` finds a task of the same app name, the *caller's* library sends the arguments to that running task as a
   `REQ_OPENBOOK2` request instead of starting the file again.
2. `inkview.h` has `SetRequestListener(int request, int flags, iv_requestlistener hproc)` and
   `UnpackParameters(void *data, int len, int *argc, char ***argv)`; `libcommon_utilities` wraps them as
   `TaskTable::setOpenBook2RequestListener(ArgsListener)` with a `main`-like `(argc, argv)` signature. So a running app can
   register to receive such a request. No SDK sample of this was found; ownership of the `argv` returned by `UnpackParameters`
   is not documented.
3. `inkview.h` has the flags `OB_NONEWTASK (0x08)`, `OB_WITHRETURN (0x02)` and the task flag `TASK_SINGLEINSTANCE`, and
   `SetActiveTask(task, subtask)`, `GetCurrentTask()`, `SendEventTo(...)`. What the flags change is not documented.

So there are three possible behaviours of the system for a second start, each with a different design (R-05):

| Case | What the system does | Who receives the text |
|------|----------------------|-----------------------|
| **B1** | routes the arguments to the running task (`REQ_OPENBOOK2`) | the running PB Office, through a request listener |
| **B2** | starts the launcher again although PB Office runs | a second launcher process |
| **B3** | only brings the running task forward, arguments are dropped | nobody: the text is lost |

The SDK strings make **B1** the most likely [SDK], but that is a reading of strings, not an observation: **[UNVERIFIED]**.

## R-02 Mechanism, cold start: the launcher takes the text into a private hand-over folder and starts itself again without it

**Decision**: the first lines of `pboffice.app`, before any `echo`, any diagnostics and the log redirection:

- if there are arguments and the first one is the marker `pbo-mail-v1`, write every following argument, byte for byte, into its own
  small file in a new folder `system/pboffice/handoff/<id>.tmp/` (`0`, `1`, `2`, ...), then `mv` the folder to
  `handoff/<id>/` (so the app never sees a half-written one), then
  **`exec sh "$0"` with no arguments**: the process image is replaced, so the text is gone from the command line of the
  long-lived launcher; the second run is an ordinary start;
- if the arguments do not start with the marker, they are dropped (the launcher may log that N arguments were dropped, never
  which); the start goes on as an ordinary start;
- nothing else in the launcher ever expands `$@`, `$*` or `$1`..`$9` (a mechanical check, R-12).

The files are written with `printf '%s'` from the shell variable, so there is no `eval`, no `$(...)` on the text, no quoting
problem, and the text cannot be mistaken for a command or an option. One file per argument means no length prefix, no escaping and
no parser for the launcher's output: the app reads each file as raw bytes.

**Rationale**: it is the only way to keep the text out of a long-lived command line without changing the structure of the
launcher (the launcher cannot overwrite its own `argv`; `exec` is what ends the process that holds it). The remaining exposure is the time between the
system's `exec` of the launcher and our `exec`: a few milliseconds, which the privacy experiment E3 measures and the About text
and `docs/` state honestly. The files live on vfat (`chmod` has no effect there, `device-findings` of spec 001); they hold the text
for at most the time between the launcher's write and the app's read (seconds), and the app removes them (R-04).

**Alternatives considered**
- *Pass the text to `pboffice.bin` as an argument*: rejected, same exposure for the whole session (FR-016).
- *Pass it in an environment variable*: visible in `/proc/<pid>/environ` of the same user for the whole session; rejected.
- *Pipe it into the app's standard input*: the app is started from a shell function with the log redirection; stdin is a
  possibility, but a pipe loses the text if the app dies before reading it, and cannot be retried; rejected for Principle III.
- *Write it into the existing launcher log folder*: the log folder is logged-to and rotated (`pboffice.log.prev`); a separate folder keeps
  secrets out of anything a user might attach to a bug report.

## R-03 What the sender hands over (contract v1)

**Decision**: [contracts/handoff-contract.md](contracts/handoff-contract.md). `OpenBook2(<path of pboffice.app>, argc, argv, 0)` with

```
argv[0] = "pbo-mail-v1"                    the marker and version
argv[n] = "text=<the text>"                optional
          "textlen=<bytes of the text>"    required when text= is present
          "subject=<one line>"             optional
          "to=<one plain address>"         optional
```

Keys are `key=value`, split at the first `=` only, so a text may contain `=`. Unknown keys are ignored (forward compatibility);
a key given twice makes the whole hand-over malformed (refused, FR-009), because a duplicate can only be a bug or an attack.
`textlen` lets the receiver notice a text that was cut or changed on the way: the byte length of the received text must equal it,
else the hand-over is refused as incomplete (FR-009). It costs the sender one `strlen`.

**Rationale**: keyed arguments avoid the question whether the system delivers *empty* arguments (not measured), allow
subject and recipient to be omitted without placeholders, and make the contract extensible. One argument per field was tested to carry line breaks and non-ASCII letters
([DEVICE-OTHER]); the text is never split or re-joined by the system or the launcher.

**Alternatives considered**
- *Positional arguments*: empty subject or recipient needs empty arguments, whose delivery is unmeasured.
- *One argument with a delimiter or JSON*: needs escaping in the sender, a parser in the launcher or the app, and invites
  injection through the delimiter.
- *`OpenBook` with a single string*: tested, works for one text, but cannot carry three fields without a delimiter.

## R-04 The hand-over folder, its readers and its life time

**Decision**: `system/pboffice/handoff/` is read by the app (`core/`, `HandoffInbox`) on three occasions only: at start, when the
app comes to the front (Qt `applicationStateChanged` to active), and when a wake signal arrives (R-05, case B2). It is **not**
polled and **not** watched. For each entry:

1. read the files (at most 8 files of at most 16 KiB each; more is refused), parse them like the arguments of a listener delivery;
2. **create the draft in the store first** (Principle III: persisted before anything else), then remove the entry;
3. an entry older than **10 minutes** (by its folder's modification time; a time in the future counts as fresh) is removed
   unread and counted in a `handoff.expired` log line: a text that nobody asked for must not wait on the card forever (FR-016);
4. a crash between 2 and the removal makes the next start create the draft a second time: a duplicate draft is the accepted price
   for never losing a text (Principle III; the user can delete one, with the usual confirmation).

**Rationale**: no polling and no background activity (Principle VI); events that already exist wake the app. The ten minutes
cover a slow start, an account set-up in between, and a restart (R-05) with a wide margin, and are short enough that a text
does not outlive the session that produced it.

**Alternatives considered**
- *`inotify`/`QFileSystemWatcher`*: no polling, but an always-on watcher is more code and one more thing to test for a case that the
  foreground event already covers; kept as a possible refinement only if E2 shows the foreground event is not delivered.
- *Keep entries until consumed*: rejected, FR-016 (abandoned hand-over must not leave the text behind).
- *Shred before delete*: not possible on vfat flash with wear levelling; the honest statement goes in the docs instead.

## R-05 Mechanism, running instance: a decision table decided by the device experiments

The spec's clarification (restart, or exit cleanly so that the next start receives the text; else tell the user) assumes that the text
reaches *some* PB Office process. Whether it does depends on B1 to B3 of R-01, which only the reader can tell. The plan therefore
builds one receiving core and chooses the last step after the experiments:

| Observed (experiments E1, E2) | Design | Spec fallback of FR-005 |
|---|---|---|
| **B1** the listener fires inside PB Office | `SetRequestListener(REQ_OPENBOOK2, ...)` registered after start; the listener copies the arguments at once and queues them to the Qt thread (`QMetaObject::invokeMethod`, queued); the same parser as for the files; the text never touches the disk; then bring the window to the front (`SetActiveTask(GetCurrentTask(), 0)`) if the system has not | not needed |
| **B2** a second launcher runs while PB Office runs | the launcher (after R-02) finds the live instance and signals it (`kill -USR1`), the app scans the folder on the signal. If the signal is not received (E2), the launcher asks the app to end (`kill -TERM`, which the app handles by saving drafts and leaving through `quit()`), waits up to 5 s, then starts a new instance, which finds the entry | restart, as clarified |
| **B3** nothing is delivered | **contract v2**: the sender writes the three values into `system/pboffice/handoff/<id>/` itself and starts PB Office with only the marker `pbo-mail-v1` (no secret in any argument); the app reads the folder when it comes to the front. A start with the marker and no entry says "nothing was handed over" (FR-009) | the restart fallback **cannot work**: there is no text in any process to keep; contract v2 is the only way (see O-1) |

Everything but the last column is code that exists in any case: parser (R-03), folder reader (R-04), draft creation (R-06). B1
adds about 40 lines (the listener), B2 adds about 15 lines of shell and 30 lines of signal handling, B3 adds a second writer, which
is the sender's job.

**Why the app does not decide by itself**: guessing which case applies would build three designs for one problem; the experiment
costs two reader sessions and the three branches are small, so the plan makes the experiment the first task of the running-instance
increment and gates the implementation on its result.

**Rationale for preferring B1/B2 over a restart**: a restart changes the state of PB Office (SC-005: same folder, same sync
state, nothing running interrupted); a delivered text in a live app changes nothing the user had open.

## R-06 Creating the message: a draft in the store, then show it

**Decision**: the new `HandoffBridge` does not use `ComposeBridge` (which is one editing context and would be disturbed when a
message is open). It builds a `pbo::Draft` itself (kind `new`, `to_text`, `subject`, `body`, message id from `pbo::make_message_id`)
and calls `Store::save_draft` (the same call `ComposeBridge::save()` makes), so the text is on disk before any screen exists and
before any question of timing arises (a dialog, a sync, a transition). The page then opens with `ComposePage { draftId: <id> }`
(which already calls `compose.edit(draftId)` and saves on destruction). Before a new page is shown over an open `ComposePage`,
that page is popped, whose `Component.onDestruction: compose.leave()` saves the earlier message first (FR-006).

Rules that follow:
- **One draft per hand-over** (FR-007), also for two that arrive together: the newest is shown, the others are in the Drafts list
  and the notice says how many wait there.
- **No account** (FR-008): a hand-over needs a *mail* account (a draft keeps the account's address in its Message-ID). While the
  account set-up page is the current page, the parsed hand-over is held in memory and continues when `mailConfigured` turns true.
  If the user leaves the set-up, or the page is anything other than the set-up, the user is told that the text was not kept. (It is
  not written to disk without an account; the sender still has it.)
- **Busy** (FR-006, US2-3): a hand-over is shown when no dialog is open (`DialogState`, a counter that every `DialogFrame` updates); a running
  sync is not touched (no bridge call stops it). The draft exists meanwhile.
- **Cursor** (US1-1): `ComposePage` focuses the first empty field of recipient, subject, body.
- **No network** (FR-004): `HandoffBridge` never calls `sync.noteChange`; only the user's own later change does (existing rule).

**Alternatives considered**
- *Prepare `ComposeBridge` via `start()` and a new setter*: the compose object is shared state; an arriving text would overwrite
  a message being typed unless that is saved and reset first; a second object is simpler and testable on its own.
- *Show first, save on the first change*: the existing rule ("nothing is created for a page left without a change") would lose the
  text if the app died before a change; FR-003 asks for the draft from the first moment.

## R-07 Parsing and cleaning the three values (core, host-tested)

**Decision**: `core/include/pbo/handoff.h`: `parse_handoff(const std::vector<std::string>& args) -> HandoffResult`, pure and Qt-free.

| Value | Rules | Failure |
|---|---|---|
| marker | the first argument that equals `pbo-mail-v1` (arguments before it, such as the app path the system may prepend, are skipped) | no marker: not a hand-over |
| text | UTF-8; invalid bytes and C0/C1 control characters except `\n` and `\t` become U+FFFD; `\r\n` and `\r` become `\n`; at most **8192 bytes** *before* cleaning; `textlen` must equal the byte length received | over the limit: refused whole, notice `too_long`. Length mismatch or `text=` without `textlen`: refused, `incomplete`. A text with only white space counts as empty |
| subject | as the text, then every line break and tab becomes one space, runs of blanks stay as typed, at most **300 characters** after cleaning | over the limit: subject dropped, the rest arrives, notice `subject_too_long` |
| to | exactly `pbo::is_valid_address` (one plain `local@domain`; no list, display name, angle brackets, blanks, quotes or control characters), at most 254 bytes | invalid: not prefilled, notice `recipient_ignored` (the address itself is never in the notice) |
| keys | `text`, `textlen`, `subject`, `to`; others ignored; a repeated key makes the hand-over malformed | refused, `malformed` |
| all empty | text, subject and to all empty or white space after cleaning | no message, no notice (FR-014) |

**Why 8192 bytes**: the spec fixes "at least 1500 characters" (FR-012). 1500 characters of any script are at most 6000 bytes
of UTF-8, so 8192 bytes guarantees that bound with room for a link and a password. The reader's own limit is not known above 1500 bytes
[DEVICE-OTHER]; experiment E4 measures it and the constant is **lowered to the measured limit if that is smaller than 8192** (and
never below 6000: if the system cannot carry 6000 bytes, the spec's minimum is unreachable and the spec must be revisited). The
limit is a contract constant of the sender as well.

**Why `textlen` counts bytes of the received text before cleaning**: it is what the sender can compute without knowing the cleaning
rules, and it detects both truncation and the case where an intermediate step changed the bytes.

**Subject and header safety** (FR-013, FR-021): the draft stores subject and address as plain text; the existing message builder
(`pbo::compose`) already rejects invalid addresses and builds headers from fields, so a line break in `subject` is removed
*before* the draft exists and the builder is never given one. A test builds the outgoing message from a draft made by a hand-over
with `\r\nBcc: x@example.org` in the subject and in the recipient and asserts one `To`, no `Bcc` header.

## R-08 The notices

**Decision**: one short dialog (the existing `ConfirmDialog` without a cancel button) from `HandoffNavigator.qml`, with a key from the bridge; English source
text, German in `qml/strings_de.tsv`; none repeats a handed-over value:

| Key | Text (English source) |
|---|---|
| `too_long` | "The text was too long to take over. Nothing was created. Hand it over again in a shorter form." |
| `incomplete` | "The text did not arrive completely. Nothing was created. Hand it over again." |
| `malformed` | "The hand-over could not be understood. Nothing was created." |
| `subject_too_long` | "The subject was too long and was left out." |
| `recipient_ignored` | "The recipient was not used because it is not a single mail address." |
| `no_account` | "To mail this text, set up a mail account first. The text was not kept; hand it over again afterwards." |
| `more_waiting` | "{n} more hand-overs wait in your drafts." |
| `nothing_handed` | "Nothing was handed over. Go back and try again." (shown only for a start with the marker and no payload, contract v2) |

## R-09 Logging

**Decision**: the logger already redacts by key name; the hand-over adds only counts and codes: `handoff.received`
(`text_chars`, `fields` as a bit mask 1 text / 2 subject / 4 recipient, `source` = `file` | `listener`), `handoff.refused`
(`reason` = a key of R-08), `handoff.expired` (`count`), `handoff.drafted` (`count`). The name `fields` is deliberate: rule R8
of `scripts/check-constitution.sh` forbids log keys ending in `recipient`, `subject`, `to` and similar words, so a key such as
`has_recipient` would be refused by the gate. A new rule **R11** (R-12) covers the launcher. The sources of text in the log
stay empty: no call passes the values (R4/R8 already refuse the key names; a bridge test captures the log sink with a unique
marker and asserts it is absent).

## R-10 What the plan does not build

- No second account, no choice of account, no Cc/Bcc/attachments/folder/action from the sender (spec Assumptions).
- No watcher, no polling, no new dependency (`libcommon_utilities` is **not** linked: `SetRequestListener` and `UnpackParameters`
  are in `libinkview`, which the app already uses).
- No sender code: the button in PB Cloud is the other project's feature; this repository ships the contract and a *test sender*
  (R-11).
- No confirmation dialog before showing a handed-over recipient: it appears in the editor like any other, and nothing is sent
  without the user's tap (spec Assumptions).

## R-11 Test strategy (Principle I)

Test-first for each item; tests marked **host** run on Linux without the reader; **device** items are `U` in
`docs/device-test-checklist.md` until observed.

| Layer | Test | Proves |
|---|---|---|
| unit (host) | `test_handoff_parse` | every row of R-07: 1, 100, 1500, 6000, 8192, 8193 bytes; umlauts, euro, emoji; `\n`, `\r\n`; invalid UTF-8; control characters; `textlen` match/mismatch/missing; duplicate key; unknown key; subject with `\r\nBcc:`; recipient list, display name, angle brackets, blank, 255 bytes; all-empty; whitespace-only; marker not first; no marker |
| unit (host) | `test_handoff_inbox` | folder entries: complete, half-written (`.tmp`) ignored, too many/too large files refused, expiry at 10 min (and future time), remove-after-use, two entries ordered by time, unreadable file |
| bridge (host) | `test_handoff_bridge` | draft exists in the store before `show` is emitted; body equals the text byte for byte; fields prefilled; one draft per hand-over; two at once; no account: held, continues after `mailConfigured`, discarded with notice; open `ComposePage` message saved first and unchanged; log sink and notice texts contain no marker; `sync.noteChange` never called (SC: FR-004); crash simulation between draft and removal gives a second draft, not zero |
| integration (host) | `test_handoff_header_injection` | the outgoing message built from a hand-over draft has one `To`, no `Bcc`, no extra header |
| integration (host) | `test_idle_no_network` (extended) | a hand-over causes no connection and no Wi-Fi request |
| launcher (host) | `tests/launcher/test_handoff_launcher.sh` | marker arguments become files byte for byte (newline, quotes, `$(...)`, backticks, `;`, leading `-`, 1500 and 8192 bytes); nothing in the log or diagnostics holds the marker, also when the text contains `pboffice.bin`; the fake binary's parent has no arguments (`ps -o args= -p $PPID`); no injection (a `touch` in the text creates no file); no arguments: unchanged start; wrong marker: dropped; the existing launcher tests stay green |
| gate (host) | `scripts/check-constitution.sh` R11 + self-test | the launcher expands arguments only between the capture markers |
| QML (host) | `tst_handoff_flow` | a hand-over from the home page, from the inbox list, from an open compose page and from the account set-up page shows the right page, the cursor in the first empty field, the notice dialog; Back returns to the page below |
| device | experiments E1 to E8 below | everything marked [UNVERIFIED] |

A **test sender** (`app/probe/pbo_handoff_probe.cpp`, device only, in the existing probe package) calls `OpenBook2` with
the contract and writes only counts and a marker check to its own file; its texts are fixed test strings, as in the sibling's
probe, and it has no network and no personal data.

## R-12 Mechanical check R11 for the launcher

`check-constitution.sh` gets rule **R11**: in `platform/pocketbook/pboffice.app`, `$@`, `$*`, `$1`..`$9` and `${1}`..`${9}` may
appear only between the comment lines `# handoff-capture-begin` and `# handoff-capture-end`, and a `set -x`, `echo`, `printf` or
`logger` of those variables is never allowed there either. A self-test adds a launcher with `echo "$1"` and expects a finding.
This is the "mechanical check that proves the rule detects a violation" the constitution asks for (Principle VIII).

## R-13 Budgets (Principle V)

All **unverified** until measured on the InkPad One; host proxies only where noted.

| Budget | Value | Check |
|---|---|---|
| RAM | at most +100 KiB transient; queue of at most 8 hand-overs of at most 8.3 KiB; nothing kept after the draft exists | `test_handoff_bridge` (queue bound), device note |
| Storage | at most 8 entries x about 9 KiB in `handoff/`, removed within seconds; nothing persistent; package +2 KiB (launcher, probe) | `test_handoff_inbox`, package size check |
| Cold start with a hand-over | editor with the text in under 10 s (SC-001); ordinary cold start unchanged, under 5 s (SC-007) | device experiment E8 |
| Running instance | editor in under 3 s | device experiment E8 |
| Launcher overhead | the capture and the `exec` add under 100 ms | host proxy in `test_handoff_launcher.sh`, device E8 |

## Device experiments (the proof the spec asks for)

Run through the existing owner procedure (memory: install only after "mounted"; the owner ejects, tests, says "mounted" again; the log is read;
no personal content is used, fixed test strings only). The probe package adds a stand-in target for E1 so that the first
session needs no change of PB Office.

| ID | Experiment | Reads | Decides |
|----|------------|-------|---------|
| E1 | A stand-in InkView target (long-running, registers `SetRequestListener(REQ_OPENBOOK2)`, logs listener calls and EVT_FOREGROUND/BACKGROUND, and logs when its launcher script is entered) is started, left open, and started again by the test sender with flags 0, `OB_NONEWTASK` and `OB_WITHRETURN` | the stand-in's file | B1 / B2 / B3 (R-01), and the right flag |
| E2 | The same listener, inside **PB Office** (a probe-mode switch like `run-probe`, no change of normal behaviour), with PB Office under Qt and the `pocketbook2egl` platform | the probe file | whether a request listener is served under Qt, in which thread, who frees `argv`; whether the foreground event reaches Qt; whether the window is in front after delivery |
| E3 | Hand over a text with a unique marker; during the session search `/proc/*/cmdline`, `ps`, `/var/run/task`, the system log, `pboffice.log`, `handoff/` | counts of the marker per place | R-02 holds; the window of exposure; whether the system itself keeps the arguments |
| E4 | Send 1500, 4000, 6000, 8192, 16000, 32000, 65000 bytes | what arrives, the return value of `OpenBook2` | the final byte limit (R-07); what the sender sees when the system refuses |
| E5 | After E1/E2: leave the target; is the caller shown again; with the target running before the hand-over | caller events | "Back returns to the app they came from" for the running case (US1-5) |
| E6 | Two hand-overs one second apart | both arrive, in order, as separate entries | FR-007 |
| E7 | Hand-over while a sync runs, while a dialog is open, while the keyboard is shown, while a message is half typed | the app state afterwards | US2 scenarios 2 and 3 |
| E8 | Timing: tap in the sender to a usable editor, cold and running; 20 runs | seconds | SC-001, SC-002, SC-007 |

Until a row is run its result is `U` in `docs/device-test-checklist.md` (new rows DT-HO-1 to DT-HO-8); a result changes the
row and, for E1/E2, selects the design of R-05 (a task of the plan's increment 2).

## Open items

| ID | Item | Why open | Closes when |
|----|------|----------|-------------|
| O-1 | **FR-005's wording presumes the text reaches a PB Office process.** In case B3 it does not, and "restart or exit so that the next start receives the text" is impossible; only contract v2 (the sender writes the folder) can deliver. The owner should confirm that contract v2 is acceptable if E1/E2 show B3 (a sibling app then writes into `system/pboffice/handoff/`, an explicit interface, never other data) | needs E1/E2 | after E1/E2; then amend FR-005 |
| O-2 | Ownership of `argv` returned by `UnpackParameters`; thread of the listener | undocumented | E2 |
| O-3 | Whether `OB_NONEWTASK` / `OB_WITHRETURN` change the routing | undocumented | E1 |
| O-4 | The byte limit above 1500 bytes | unmeasured | E4 |
| O-5 | The exposure window between the system's start of the launcher and its `exec` | unmeasured | E3 |
| O-6 | Whether the system itself writes the arguments to a place readable by other programs (`/var/run/task`, log) | unmeasured | E3 |
| O-7 | Whether the foreground event reaches the Qt app when brought forward | unmeasured | E2 |
| O-8 | Timing figures of SC-001/SC-007 | device | E8 |
