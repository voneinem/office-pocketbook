# Quickstart: build and validate the hand-over

**Feature**: [spec.md](spec.md) | **Plan**: [plan.md](plan.md) | Builds on the
[quickstart of spec 001](../001-email-calendar-mvp/quickstart.md) (prerequisites, configure, build are identical).

Part A runs on the host without the reader and without any real account. Part B is the proof on the reader that the spec asks
for; **nothing in part B has been run yet**, every result is "unverified" until it is entered in
`docs/device-test-checklist.md` (rows DT-HO-1 to DT-HO-8) with its date.

## A. Host

```bash
cmake -S . -B build-host -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build-host -j
ctest --test-dir build-host --output-on-failure                         # everything
ctest --test-dir build-host -R 'handoff'                                # this feature only
tests/launcher/test_handoff_launcher.sh                                 # the launcher against a fake device layout
scripts/check-constitution.sh --self-test && scripts/check-constitution.sh   # includes rule R11
scripts/pre-merge.sh                                                    # the gate
```

Each test is written first and seen failing before the code it covers (Principle I); `tasks.md` orders them that way.

| # | Scenario | Test | Spec |
|---|----------|------|------|
| 1 | Marker, text, subject, recipient parse; 1, 100, 1500, 6000, 8192 bytes; umlauts, euro, emoji, line breaks | `test_handoff_parse` | US1-1/4, FR-002 |
| 2 | Over the limit, missing or wrong `textlen`, duplicate key, no marker | `test_handoff_parse` | US4-2, FR-009/012 |
| 3 | Control characters and invalid UTF-8 become U+FFFD; `\r\n` becomes `\n`; subject is one line | `test_handoff_parse` | US4-3, FR-013 |
| 4 | Recipient list, display name, angle brackets, hidden header: not prefilled; text and subject arrive | `test_handoff_parse`, `test_handoff_header_injection` | FR-021/022 |
| 5 | All fields empty: no message; only a subject: a message with an empty body | `test_handoff_parse`, `test_handoff_bridge` | FR-014 |
| 6 | Draft exists before `show`; one draft per hand-over; two at once; no sync call | `test_handoff_bridge` | FR-003/004/007 |
| 7 | Open message saved first and unchanged | `test_handoff_bridge`, `tst_handoff_flow` | FR-006 |
| 8 | No account: held, continues after set-up, discarded with the notice when set-up is left | `test_handoff_bridge`, `tst_handoff_flow` | FR-008 |
| 9 | Pending entries: half-written ignored, expiry 10 min, removal after the draft, crash gives a duplicate not a loss | `test_handoff_inbox`, `test_handoff_bridge` | FR-016, Principle III |
| 10 | The marker is in no log line and no notice | `test_handoff_bridge` (log sink) | FR-015 |
| 11 | Launcher: files byte for byte, injection attempts do nothing, no marker in log or diagnostics, no arguments in the long-lived process | `test_handoff_launcher.sh` | FR-016/017 |
| 12 | Launcher expands arguments only in the capture block | `check-constitution.sh` R11 + self-test | FR-017 |
| 13 | No connection and no Wi-Fi request from a hand-over | `test_idle_no_network` (extended) | FR-004, FR-018 |
| 14 | Pages, focus, notices, Back | `tst_handoff_flow` | US1, US2, US4 |

## B. The reader (the proof the spec asks for)

Procedure of the owner (memory: install only after "mounted"; the owner ejects, tests, says "mounted" again; fixed test strings,
no personal content; PB Notes and InkFeed files are never touched):

1. Build and package: `cmake -S . -B build-arm -G Ninja -DTARGET_TYPE=ARM -DCMAKE_BUILD_TYPE=Release && cmake --build build-arm`, then
   `scripts/package-device.sh build-arm/app/pboffice.app "" build-arm/dist build-arm/app/probe/pbo-probe.bin` with `POCKETBOOK_SDK`
   exported (the package also holds the hand-over probe).
2. **E1** (stand-in target, no change of PB Office behaviour): create `system/pboffice/run-handoff-probe`, start PB Office, tap the
   boxes, read `system/pboffice/handoff-probe.txt`. Result selects B1, B2 or B3 ([research R-05](research.md)).
3. **E2 to E8** in the order of the table in [research.md](research.md#device-experiments-the-proof-the-spec-asks-for), after the
   first increment is installed; the sender is the probe's test sender; the text carries a unique marker so that E3 can search for it.
4. Enter each result in `docs/device-test-checklist.md` (U -> V/F with date and value) and in `docs/device-findings.md`.

Exit criteria for the running-instance increment (spec SC-002, SC-004): 20 of 20 hand-overs either show the complete text or tell the
user that nothing arrived; the marker is found 0 times outside the draft, the handed-over text is not in `/proc/*/cmdline` of any process
after the launcher's `exec`, and an unsent message that was open is found complete in the drafts.
