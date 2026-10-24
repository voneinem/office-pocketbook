# Device test checklist (InkPad One)

Every item starts as **unverified**. An item changes only when someone has done it on the reader and written
the date and the result next to it. Measured values go in the "Value" column with their unit; if an item
cannot be done, write why. Nothing on this page may be copied from a host run: the host tests use fake servers
and a desktop CPU (see `docs/reference/security-review.md`).

How to run: build the package (`scripts/package-device.sh`, see `INSTALL.md`), install it, register
`U_PbOffice`, and keep `system/pboffice/pboffice.log` (it holds counts and codes only, no personal data) for
the evidence. Peak memory: `grep VmHWM /proc/<pid>/status` while the app is open (`pidof pboffice.bin`, over
SSH/telnet if the reader offers it; otherwise "not measured").

Legend: **U** unverified · **O** observed informally by the owner (no measured value) · **V** verified (date,
value) · **F** failed (what happened).

## Installation and start

| ID | Item | Expected | Status | Date | Value / note |
|----|------|----------|--------|------|--------------|
| DT-INS-1 | Install per `INSTALL.md`, register `U_PbOffice`, start the app | The icon appears; the app opens on the account setup page (no account) or on the start page | O | 2026-10-16 | The owner has installed several builds this way; no timing |
| DT-INS-2 | Cold start to the first usable screen (SC-003) | under 5 s | U | | |
| DT-INS-3 | Package size | at most 25 MB (`scripts/package-device.sh` refuses more) | V | 2026-10-16 | 4.3 MB (host check of the zip, not a device measurement) |
| DT-INS-4 | Upgrade over an older build keeps the data folder `system/pboffice/` and the account | Account and mail are still there | O | 2026-10-16 | Several upgrades by the owner; schema upgrades are covered by `test_store_migrations` |
| DT-INS-5 | notes-pocketbook and InkFeed are untouched by installing and starting PB Office | Their files are unchanged | U | | |

## Account and security (US1, FR-004)

| ID | Item | Expected | Status | Date | Value / note |
|----|------|----------|--------|------|--------------|
| DT-ACC-1 | Set up a mail account with the on-screen keyboard (SC-001) | under 5 minutes, connection test passes, first mail readable | U | | |
| DT-ACC-2 | The credential statement matches the measured protection: after saving a password, `ls -l system/pboffice/credentials` on the reader | The page and About say "only this app can read" only if the mode is 600; on FAT they must say that the storage cannot protect the file | U | | |
| DT-SEC-1 | Same check from a PC: can another user read `credentials` after mounting the reader? | Expected yes on FAT (documented); the text must say so | U | | |
| DT-ACC-3 | A wrong password, a wrong server name and no network each give their own message | Specific text, no password in the log | U | | |
| DT-ACC-4 | A private certificate authority: put `custom-ca.pem` in `system/pboffice/`; Settings > Security says "loaded"; a server with an unknown certificate is refused without it | Settings shows the state; no way to skip the check | U | | |

## Reading mail (US1)

| ID | Item | Expected | Status | Date | Value / note |
|----|------|----------|--------|------|--------------|
| DT-RD-1 | First sync of about 500 messages of 30 days (SC-014) | under 10 minutes | U | | The host needs about 52 s against the fake server |
| DT-RD-2 | Open a downloaded message of about 1 MB (SC-005) | under 2 s | U | | |
| DT-RD-3 | Turn pages in a list and in a message (SC-004) | under 1 s each | U | | |
| DT-RD-4 | German and Eastern-European letters in subject and text (ä ö ü ß, ł ž č) | shown correctly | U | | |
| DT-RD-5 | An HTML-only message shows as text, links as `[1]` text, nothing is loaded | no network use while reading | U | | |
| DT-RD-6 | Attachment: download under the limit; over the limit refused; saved in `/mnt/ext1/Downloads` under its real name; a second download does not overwrite | as listed | U | | |
| DT-RD-7 | Peak memory while browsing, and during a full sync (K4) | at most 150 MB browsing, 250 MB syncing | U | | |
| DT-RD-9 | Pictures of an HTML mail (a newsletter): the pictures that are parts of the mail show on the page; "Load pictures" says that the sender is told; after the tap with Wi-Fi on the remote pictures appear; each page turn keeps whole lines | pictures readable in gray; no cut lines; JPEG and PNG decode | O | 2026-10-17 | Owner: "Everything was OK!" (one newsletter; log: `image.loaded loaded=4/1 failed=0` at sync, `loaded=11 failed=0 remote=1` after "Load pictures", no `image.undecodable`). Which decoder was used (reader's Qt or the shipped plugins) and the JPEG/GIF/PNG split are not recorded. The probe lists `/usr/plugins/imageformats` (JPEG and GIF need plugins; PNG is built in); a picture that cannot be decoded shows "The picture cannot be shown" and the log gets `image.undecodable` |
| DT-RD-10 | Long links in the list are shortened; a tap shows the whole address; nothing is opened | as described | U | | |
| DT-RD-8 | Airplane mode: every downloaded message opens | all windowed mail readable | U | | |

## Calendar (US2, US4)

| ID | Item | Expected | Status | Date | Value / note |
|----|------|----------|--------|------|--------------|
| DT-CAL-1 | Time zone: the picker asks once when the reader's `/etc/localtime` is not usable; Settings shows the chosen zone | no zone is ever assumed | U | | |
| DT-CAL-2 | Agenda paging, month view readability, day view | readable, whole pages | U | | |
| DT-CAL-3 | A day with a daylight-saving change shows the events at the right hour (last Sunday of March / October) | correct local times | U | | |
| DT-CAL-4 | Create, change and delete an event offline; sync; the server has it (see the live checklist) | marked "not synced" until the sync | U | | |
| DT-CAL-5 | Repeating events: shown with their words; cannot be changed, can be deleted as a whole | as in the app text | U | | |

## Organizing mail and Auto Sync (US5, US6)

| ID | Item | Expected | Status | Date | Value / note |
|----|------|----------|--------|------|--------------|
| DT-ORG-1 | Star and read/unread offline: the date line shows "starred" / "unread" and the list shows the star at once | immediate, with "not synced" | O | 2026-10-16 | Owner: "That worked!" after the marks were added |
| DT-ORG-2 | With Wi-Fi off, change a message: after about 4 s the reader connects, syncs and the mark disappears | Wi-Fi comes on once per change; sync runs | O | 2026-10-16 | Log: three `network.connect result=failed` (the owner may have dismissed the system dialog), then `sync.finished outcome=success flags_sent=1`; not timed |
| DT-ORG-3 | Move to a folder; delete (moves to Trash); delete in Trash asks twice | as described in the app text | U | | |
| DT-ORG-4 | Search over 5,000 downloaded headers and list page turns (SC-004, T162) | search under 3 s | U | | |
| DT-ORG-5 | Sort by date, sender, subject | list order changes, search keeps its results | U | | |
| DT-US6-1 | Start page at cold start: unread count, today and tomorrow, sync state | as `HomePage` text | O | 2026-10-16 | Owner: "Start page as expected for now" |
| DT-US6-2 | Wi-Fi reconnect, sleep and wake during a sync (O-12) | the sync ends in a state the user sees; nothing is lost | U | | |
| DT-US6-3 | Idle battery: app open, Auto Sync finished, 1 hour (SC-011) | no network use; battery drop noted | U | | |
| DT-US6-4 | About: version, every license text offline, time zone data release, password statement | all texts open without network | U | | |
| DT-US6-5 | Settings: days kept (shorter removes older mail after the next sync; drafts and unsynced changes stay), attachment limit, space for mail, special folders, language | each takes effect | U | | |
| DT-US6-6 | German interface (Settings > Language): all pages readable, no text cut off, dates in German | layout holds with the longer texts | U | | German texts exist for all 417 texts; layout is untested on the reader |
| DT-US6-7 | Wrong clock (set the reader's date 1 hour off, sync): "Check the clock" appears | warning above 10 minutes | U | | |

## Writing mail (US3)

| ID | Item | Expected | Status | Date | Value / note |
|----|------|----------|--------|------|--------------|
| DT-WR-1 | On-screen keyboard while writing: the field stays visible | usable | O | 2026-10-16 | Copy/paste menus and the keyboard were used by the owner; no timing |
| DT-WR-2 | Autosave survives a battery pull / hard power-off while typing (SC-006) | at most the last 2 s of typing lost | U | | |
| DT-WR-3 | Send with Wi-Fi off: the message waits in the outbox, Wi-Fi comes on, it is sent once | one copy at the recipient; one in Sent (or none if the provider files it) | U | | |
| DT-WR-4 | Wi-Fi drops during sending | the message shows "may have been sent"; the app never sends it again by itself | U | | |
| DT-WR-5 | Copy and paste inside the app (long press); text copied in PB Notes cannot be pasted (no system clipboard) | as described | O | 2026-10-16 | Owner tested the menus; the missing system clipboard is accepted |

## Contacts (spec 002)

| ID | Item | Expected | State | Date | Value / note |
|----|------|----------|-------|------|--------------|
| DT-CT-1 | Upgrade the database from schema 1 to 2 on the reader: install over an existing build with mail and calendar; start; check `pboffice.db.bak` exists; start again; the backup is gone; then try the previous build (it must refuse the file with a clear message and leave it untouched) | all mail and events still there; contacts empty until the first sync | O | 2026-10-18 | First start migrated and synced (log); `.bak` written at launch. Not yet tried: second start removes it, previous build refuses the file |
| DT-CT-2 | List and search with a large address book (about 2,000 contacts) | page turn under 1 s, search under 2 s | U | | |
| DT-CT-3 | The on-screen keyboard in the contact form: the field being typed in stays visible, phone and email fields use the matching keyboard | usable | O | 2026-10-18 | Owner: the note field was covered by the keyboard in the first builds; after three fixes the field and the cursor line stay above the keyboard ("viel besser"). Phone and email keyboards not itemised |
| DT-CT-4 | Edit a contact with Wi-Fi off, then sync | marked "not synced" until the sync; the server has the change; photo and birthday of the card are still on the server | U | | |
| DT-CT-5 | Memory and storage with 2,000 contacts | database grown by under 20 MB; peak memory within the limits of DT-RD-7 | U | | |
| DT-CT-6 | Find a contact by typing part of its name and read its number (SC-002) | under 20 s | U | | |
| DT-CT-7 | Create a contact with a name, a number and an email address using only the on-screen keyboard (SC-003) | under 2 minutes | U | | |

## Receiving shared text (spec 003)

Another app starts PB Office with a text (and optionally a subject and one recipient); PB Office opens a new draft. The host tests prove the rules against the
real core and bridges; everything about the reader's own behaviour is **unverified** until observed here. Experiments E1 to E8 are described in
`specs/003-receive-shared-text/research.md`; the test sender and the stand-in app are in the package (`INSTALL.md`, "Hand-over probe"). Use fixed test texts only.

| ID | Item | Expected | State | Date | Value / note |
|----|------|----------|-------|------|--------------|
| DT-HO-1 | Cold start: PB Office closed, the test sender hands over boxes A (text, subject, recipient), B (text only), C (1500 bytes, umlauts, line breaks), D (marker only); kill the app after A | A: editor with the text, subject and recipient, cursor in the body; B: cursor in the recipient field; C: the 1500 bytes complete; D: nothing created; the draft survives the kill; Back returns to the sender (US1, SC-003) | U | | |
| DT-HO-2 | Running instance, E1 (stand-in) and E2 (inside PB Office, `run-listener-probe`): what does the system do for a second start while the app runs: listener called (B1), launcher run again (B2), or only brought forward (B3)? Which flag is needed? In which thread does the listener run, who frees `argv`, is the window in front afterwards, does Qt see the app become active? | recorded in `docs/device-findings.md`; selects the design of research R-05 (tasks T033 to T035) | U | | |
| DT-HO-3 | The fallback of FR-005 for the selected case; a start with the marker and no payload (contract v2 only) shows "Nothing was handed over" | the text arrives, or the user is told (US2, FR-005, FR-009); the owner's answer to open decision O-1 if the case is B3 | U | | |
| DT-HO-4 | Size: 1500, 4000, 6000, 8192, 16000, 32000, 65000 bytes (E4): what arrives complete, what is refused, the return value of `OpenBook2` | at least 6000 bytes complete (the spec minimum of 1500 characters); `kHandoffMaxTextBytes` set to the measured limit if it is below 8192 | U | | |
| DT-HO-5 | After a hand-over to the running app, leave PB Office (E5) | the sender is shown again (US1-5) | U | | |
| DT-HO-6 | Two hand-overs one second apart (E6) | two drafts, the newest shown, nothing lost or merged (FR-007) | U | | |
| DT-HO-7 | Hand over while a sync runs, while a dialog is open, while the keyboard is shown, and while a message is half typed (E7) | same folder, same sync state; the half-typed message is a draft, unchanged; no dialog answered (US2, SC-005) | U | | |
| DT-HO-8 | Privacy (E3): search `/proc/*/cmdline`, `ps`, `/var/run/task`, the system log, `pboffice.log` and `.prev`, `handoff-probe.txt` and `system/pboffice/handoff/` for the marker word of a text, a subject and a recipient | found only in the draft (and in the entry for seconds); the exposure window and whether the system itself keeps the arguments are recorded (SC-004, FR-015, FR-016; a hit anywhere else blocks the release, FR-017) | U | | |
| DT-HO-9 | Timing and the exit criteria (E8): 20 consecutive hand-overs, 10 with PB Office closed and 10 running in different states | tap to usable editor under 10 s closed and under 3 s running (SC-001); 20 of 20 show the complete text or tell the user that nothing arrived (SC-002); ordinary cold start still under 5 s (SC-007) | U | | |

## Recording

For each measured value write the build version (About) and the date. Findings that change a design decision
go into `docs/device-findings.md`.
