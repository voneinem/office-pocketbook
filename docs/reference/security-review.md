# Security review of PB Office (mail, calendar and contacts)

Date: 2026-10-16. Scope: the core library (`core/`), the Qt bridge (`bridge/`), the QML pages (`qml/`), the
app (`app/`), the build and test tooling and the repository, for the features `001-email-calendar-mvp` and `002-server-contacts` (section 8).
Every claim names the automated test or the repeatable command that backs it. Run everything with
`scripts/pre-merge.sh`.

**What this review is not.** All servers in the tests are fakes (`tests/support/fake_*_server.h`).
Nothing here has been checked against a real mail or calendar provider, and the device behaviour (file
permissions on the FAT storage, Wi-Fi API, battery pull) is listed under "Open items". A claim that rests
on a fake server is labelled "fake server".

## 1. Transport security (constitution Principle III)

| Requirement | Evidence |
|-------------|----------|
| TLS is verified (peer and host name) on every connection; no switch turns it off | `core/src/curl_session.cpp` sets `VERIFYPEER=1`, `VERIFYHOST=2`; `scripts/check-constitution.sh` rule R1 fails on `curl -k`, `--insecure`, `--no-check-certificate`, on any `VERIFY*` option set to 0/false or `VERIFYHOST` set to 1; its `--self-test` (CTest `check_constitution_selftest`) proves each pattern is detected |
| No connection without TLS: IMAP/SMTP only with implicit TLS or an enforced STARTTLS; no plain-text fallback | rule R7 (`CURLOPT_USE_SSL` only `CURLUSESSL_ALL`; the literal `imap://` and `smtp://` only in `curl_session.cpp`); `test_imap_tls_failures` (STARTTLS refused by the server: error, and no password is sent in the clear; a server that disables LOGIN without TLS gets nothing; implicit TLS against a plain-text server: error without a password); `test_smtp_injection` (`tls_problems_are_certain_failures_and_no_credentials_leave_the_device`) — fake servers |
| An expired certificate, a wrong host name, an unknown or a self-signed authority are refused before any credential is sent | IMAP: `test_imap_tls_failures`; HTTP (CalDAV, feed): `test_tls_verification`, `test_tls_cert_failures`; SMTP: `test_smtp_injection` — each asserts that the server saw no `LOGIN`, `AUTH` or `Authorization` |
| A redirect never forwards credentials to another host and never goes to plain `http://` | `test_redirect_credentials`; `CURLOPT_PROTOCOLS_STR` / `REDIR_PROTOCOLS_STR` allow `https` only |
| Trust anchors: the device store when usable, else the bundled CA file; a user-supplied `custom-ca.pem` only extends them; Settings says whether it is loaded | `test_ca_selector`, `test_effective_ca` (a broken, empty, oversized or non-PEM file is ignored), `test_private_ca` (a private CA works for HTTP and for IMAP only with the file; a different CA does not help; a wrong host name is still refused with the custom CA loaded; a broken custom file opens no way around the check), `test_settings_bridge` (`settings_report_the_custom_certificate_authority`) |
| libcurl tracing is never on (it would write `LOGIN` and `AUTH PLAIN` passwords to the log) | rule R10 |
| The mail client never opens a link and loads no remote content by itself; the pictures of ONE message are loaded only on the user's tap (constitution 1.1.0, FR-009) | `test_reader_no_remote` (an HTML message with remote content opens as text and nothing is fetched), `test_image_load` (`remote_pictures_are_not_loaded_by_the_sync_and_only_by_the_tap`: the sync makes no request; a tap makes one plain GET per picture over https, no credentials, cookie or referrer; what is not a picture by its first bytes is not kept; size and number are bounded; a failed one is asked for again only by another tap), `test_html_text` (only `cid:` and `https:` sources are listed; `http:`, `data:`, `file:`, relative and script addresses, addresses with credentials and tracking pixels are not), `test_reader_bridge` (opening a message loads nothing); rules U5 and U6 of `scripts/check-ui-style.sh` (no WebView/WebEngine import, no `Qt.openUrlExternally`). The page says before the tap that the sender is told |

Residual points: (a) libcurl's compiled-in default CA directory is not disabled; on the device this is
the SDK's `/etc/ssl/certs`, whose content was not inspected (O1). (b) The minimum TLS version is 1.2.
(c) libcurl 8.18 is bundled for the ARM build (`scripts/build-curl-arm.sh`); its updates are the owner's
to follow.

## 2. Credentials, secrets and personal data

| Requirement | Evidence |
|-------------|----------|
| Passwords are kept as plain text in `credentials` with the strictest permissions the storage allows, written atomically; the protection is MEASURED after saving and shown on the setup page and in About (FR-004) | `test_credentials_file` (private whatever the umask; no temp file left; a group-readable file is reported as `not_enforced`; atomic replace; `forget` overwrites then unlinks), `test_about_bridge` (`the_password_file_state_is_measured_and_always_comes_with_the_advice`), `AccountSetupPage.qml` / `AboutPage.qml` texts |
| The storage on the reader (FAT) probably cannot enforce permissions | UNVERIFIED on the device (O2); the app says so plainly and recommends app-specific passwords in both states |
| Passwords never reach the log, an error message or the database | `test_credentials_file` (`passwords_never_reach_the_log`), `test_log`; rule R4; `Store` has no password column |
| Logs hold no addresses, subjects, folder names, host names, file names or paths | `test_log` (name-based redaction, newline injection, the list of event codes equals the sources), rules R4, R6, R8; log codes are listed in `core/src/log_codes.h` |
| No secrets or real user data in the repository | `scripts/pre-merge.sh --secrets` (private keys, Basic auth values, hard-coded password/token assignments; `gitleaks` when installed), `scripts/check-fixtures.sh` (only `example.com`, `example.org`, `localhost`) |
| No absolute local paths in tracked files | rule R5 |
| Nothing leaves the device except to the servers the user configured; no telemetry, no update check | no code path other than the IMAP, SMTP, CalDAV and feed transports opens a socket; `test_idle_no_network` (after the automatic sync of a change the servers see nothing more; with Auto Sync off nothing is sent until the user syncs) — fake servers |

## 3. Untrusted input

All of it comes from the network, from other people's mail and calendars.

| Input | Evidence |
|-------|----------|
| IMAP responses | `test_malformed_imap` (cut at every offset, byte flips, random bytes, deep nesting, huge counts and literals, a hostile mailbox name cannot inject a command) |
| Mail (MIME, headers, charsets, HTML) | `test_malformed_mail` (at least 50 malformed files, truncation at every offset, size and nesting bombs), `test_mime_parse`, `test_mime_headers`, `test_charset`, `test_html_text` (HTML becomes text: script and style are skipped, images are never loaded, only their alt text is shown, links become numbered text) |
| Calendar data (iCalendar) | `test_malformed_ical` (at least 20 malformed files, mutations, huge input bounded), `test_ical_parse`, `test_rrule`, `test_occurrences_golden` |
| CalDAV XML | `test_malformed_xml` (hostile constructs such as entities are refused; huge input refused quickly; mutations of a real answer) |
| Header and command injection in what the app sends | `test_smtp_injection`, `test_compose` (line breaks in subject, names and addresses cannot add headers or recipients), `test_malformed_imap` (mailbox names) |
| Attachment names and sizes | `test_attachment_fetch`: names from the network are made safe, an existing file is never overwritten (the new one gets a number), a size over the limit is refused without fetching, a part larger than announced is aborted and leaves no file |
| SQL | every statement is prepared with bound parameters; search input is literal (`test_search`) |

Attachments are saved as they are, with their real name, in `/mnt/ext1/Downloads`; the app never opens or
runs them and never deletes them.

## 4. Data safety (constitution Principle VI)

| Requirement | Evidence |
|-------------|----------|
| A message of unknown send outcome is never sent again by the app | `test_no_double_send` (`an_unknown_outcome_becomes_uncertain_and_is_never_resent_by_a_sync`, `the_matrix_of_injection_points_never_produces_a_duplicate`), `test_draft_autosave_kill` (killed while sending: never accepted twice), `test_kill_matrix` |
| A draft is deleted on the server only after the new copy is confirmed; no bare `EXPUNGE` ever | `test_draft_mirror`, `test_mail_push`, `test_move_variants` (the command log of the fake server contains `UID EXPUNGE <uid>` only; in `core/` the word EXPUNGE appears in the response parser and nowhere as a command: `grep -rn EXPUNGE core/src`) |
| A server folder that was reset (`UIDVALIDITY`) gets no UID-addressed command | `test_uidvalidity_reset`, `test_draft_mirror`, `test_mail_push` |
| Nothing the user changed is lost to a sync, an eviction or a shorter window | `test_store_eviction`, `test_eviction_sync`, `test_window_change` (messages with a pending change or an error, drafts, the outbox and events with local work are never removed), `test_store_organize` |
| Event edits: `If-Match` on every write; a delete never removes a version that changed on the server; a conflict keeps both | `test_event_push`, `test_event_conflict`, `test_event_server_deleted`, `test_readonly_calendar` (fake CalDAV server) |
| Killing the app (SIGKILL, like a battery pull) at random moments loses nothing and corrupts nothing | `test_draft_autosave_kill` (autosave; the outbox; sending), `test_event_edit_kill`, `test_kill_matrix` (one whole sync: send, Sent copy, draft mirror, flag push, pull; 100 kills; afterwards the store passes its integrity check, no draft or queued message is lost, none is accepted twice, the next sync finishes the rest once). The real battery pull on the reader is a device item (T137, T186) |
| Storage nearly full never corrupts data | `test_store_eviction`, `test_eviction_sync`; writes that fail return an error and keep the earlier state (`Store` transactions) |
| Local changes that wait for the server are visible | `message_list`/`reader` marks "not synced", event marks, the "To decide" page; QML tests in the skeleton and calendar flows |

## 5. Things that need the owner's eyes

- Links are text only (FR-009); the app has no way to open one.
- The clock check (FR "device clock wrong") compares the reader's clock with the HTTP `Date` header of the
  calendar server and with the `INTERNALDATE` the mail server stamps on a message the app has just
  appended (`test_clock_skew_caldav`, `test_clock_skew_imap`). Both are fake servers.
- The language of the interface can be German; the text comes from `qml/strings_de.tsv`. A wrong or missing
  entry shows the English text; no text is ever executed or interpreted as markup (`scripts/extract-strings.py
  --check`, CTest `check_translations`).

## 6. Findings during implementation

| # | Finding | Resolution |
|---|---------|------------|
| 1 | Sending a message started a sync at once even with Auto Sync off, and never switched Wi-Fi on | Found 2026-10-16 while reviewing the Auto Sync rules; fixed: the Compose page calls `sync.noteChange`; `test_send_with_auto_sync_off_waits_for_the_sync_button`, `test_saving_a_draft_never_starts_a_sync` |
| 2 | Tapping a day in the Month view did not open the day (the grid is rebuilt by the first call and its delegate loses the context) | Found on the reader's own log (`ReferenceError: dayModel is not defined`); fixed and covered by `test_tapping_a_day_in_the_month_selects_it_for_the_day_page` |
| 3 | A shorter "days of mail kept" removed nothing | Spec US6 scenario 4 asks for removal; `Store::trim_to_window` after the pull, only when the window was shortened, never for local work; `test_window_change` |
| 4 | The newsletter of the owner's screenshot (HTML with pictures on the sender's server) showed as text with `[alt]` marks only | Owner decision 2026-10-16: show `cid:` pictures at once, remote ones on a tap per message, long links shortened; constitution 1.1.0, FR-009 amended; `test_html_text`, `test_mime_images`, `test_store_images`, `test_image_load`, `test_reader_bridge`, QML `test_html_picture_mail_…` |
| 5 | The host app was run once against a copy of the reader's database and contacted the real mail server with an empty password (one refused login) | Not repeated; the rule is in the working notes: never run the host app on a copy of the reader's data |

## 7. Open items

- **O1** The device CA store and the Qt runtime of the reader were probed only partly (`docs/device-findings.md`).
- **O2** Whether the reader's storage honours file permissions for `credentials` (the app measures it and
  says what it finds; the finding on the real device is in `docs/device-test-checklist.md`, item DT-SEC-1).
- **O3** Everything marked "fake server" is unverified against real Gmail, Outlook.com, iCloud, generic
  IMAP/SMTP, Nextcloud and OpenCloud, including CardDAV address books (`docs/live-server-checklist.md`).
- **O4** Commits made before the clean-up of the baseline may contain local absolute paths in their
  history. Rewrite the history before the first public push if that matters.
- **O5** `gitleaks` is not installed on the development machine; the built-in scan is weaker.
- **O6** The performance budgets (cold start, page turn, first sync of 500 messages) are measured on the
  host only; the reader figures are device items.

## 8. Contacts (feature 002-server-contacts)

All servers in these tests are fakes (`tests/support/fake_carddav_server.h`): every claim is "fake server" until the
rows LV-K1 ... of `docs/live-server-checklist.md` are done.

| Requirement | Evidence |
|-------------|----------|
| CardDAV uses the same verified TLS as everything else; an expired, wrong-host or untrusted certificate is refused before any credential is sent | `test_tls_cert_failures` (`carddav_discovery_refuses_an_expired_certificate_and_sends_no_credentials`, `..._a_certificate_for_another_host_...`, `carddav_refuses_a_certificate_from_an_authority_the_client_does_not_trust`); CardDAV goes through `DavSession`/`CurlTransport`, the only place that sets `VERIFYPEER`/`VERIFYHOST` (rule R1) |
| A redirect never forwards credentials to another host and never goes to plain `http://` | `test_redirect_credentials` (`carddav_discovery_does_not_forward_credentials_...`, `carddav_discovery_refuses_a_redirect_to_plain_http`); `DavSession` ignores a resource named on another host (`test_dav_common`, `test_carddav_discovery`) |
| The account is the calendar server account: no second password, nothing new is stored; a hidden address book is not contacted | `test_contacts_sync_pull` (`a_hidden_book_is_not_downloaded`, `without_a_calendar_server_account_the_pull_does_nothing`); the credentials file has no new field |
| Cards and XML are untrusted input: bounded, total, no recursion | `test_vcard_parse`, `test_malformed_vcard` (truncation at every offset, byte flips, nesting, huge lines, 5,000 lines, 100 entries per kind; `too_large` / `malformed` / `multiple_cards` / `no_uid` keys), `test_malformed_xml` and `test_carddav_discovery` (CardDAV answers) |
| Photos and other heavy properties are never stored; the server's copy keeps them | `test_contacts_sync_pull` (`photos_are_not_stored_whether_the_server_honours_the_property_list_or_not`), `test_contact_push` (`editing_a_contact_from_the_server_reads_the_full_card_first_and_keeps_what_the_app_does_not_show`, `editing_the_cards_of_the_corpus_loses_none_of_their_other_properties_on_the_server`: 24+ cards of the corpus, 100 % of the unedited lines kept) |
| Contact text is plain text; nothing starts a call, a mail or a navigation | `tst_contacts_flow` (`test_the_detail_offers_no_call_mail_or_link_action`, `plainTextEverywhere`); rules U5 / U6 of `scripts/check-ui-style.sh` |
| Logs hold no names, numbers, addresses, notes, host names or paths of contacts | `test_contact_push` (`the_log_of_a_push_...`, `the_log_of_an_update_...`, `the_log_of_a_delete_...`), `test_contacts_sync_pull` (`the_log_of_a_pull_...`), `test_contacts_bridge` (`opening_the_list_and_a_contact_writes_nothing_personal_to_the_log`); rule R8 (event keys `contact`, `phone`, `email`, `book`, `org`, `note` are refused; self-test in `check_constitution_selftest`); codes `contacts.pull`, `contacts.push`, `contacts.conflict` in `core/src/log_codes.h` |
| Every write is conditional: create `If-None-Match: *`, update `If-Match: <etag>` (of the look at the full card for a card the reader holds in part), delete `If-Match: <etag>`; a card that changed on the server is never overwritten or deleted by an older version | `test_contact_push`, `test_contact_conflict` (three ways a conflict comes about, for update and for delete; both versions kept; `KeepMine`/`KeepServer`/`KeepBoth`), `test_contact_server_deleted`, `test_carddav_requests` |
| A lost answer never makes a second card; a lost answer to an update or a delete converges | `test_contact_push` (`an_answer_lost_after_the_server_took_the_card_never_makes_a_second_card`, `the_card_of_an_update_whose_answer_was_lost_is_recognised_at_the_next_pull`, `an_answer_lost_after_the_server_deleted_the_card_ends_with_the_next_push`); the resource path is chosen and stored before the first request |
| Killing the app at random moments loses and doubles nothing | `test_contacts_kill` (create/push/pull, edit/push with the photo kept on the server, delete/push; 100 kills each) |
| Local work is never evicted, overwritten by a pull, or deleted by hiding a book or by a shortened mail window | `test_eviction_sync` (`eviction_never_selects_a_contact_and_never_touches_one_with_local_work`), `test_contacts_sync_pull`, `test_contact_push` (`a_hidden_book_still_sends_what_waits_...`), `test_store_threads` |
| The upgrade of the database is one-way and loses nothing: a copy is kept until the next successful start; the previous build refuses the file untouched | `test_store_migration_2` (every row survives; the backup is kept until the next successful start; a failing step rolls the whole upgrade back; an older build refuses the file and leaves it untouched; `sync_state` rebuilt); the ARM build and the reader are device items (DT-CT-1) |
| Nothing leaves the reader except to the configured server; an idle app with open Contacts pages makes no request | `test_idle_no_network` (`open_contact_pages_make_no_request_and_a_change_waits_for_the_sync_when_auto_sync_is_off`) |
| Recipient suggestions read the local store only and send nothing | `test_address_suggestions`, `test_compose_bridge` (`picking_a_contact_suggestion_adds_the_recipient_without_the_label_and_changes_nothing_else`) |

Residual points: (a) vCards of real clients differ; the in-house parser is tested against fixtures written from the RFCs and
from memory of other clients, all `.unverified`. (b) A server that ignores `If-Match` on a `PUT` or `DELETE` could overwrite a
change made between the look and the write; the app cannot detect that (LV-K8). (c) A card kept for "keep both" loses the
properties the reader never held (photo, birthday): the server's own card, which is kept, still has them.

## 9. Hand-over from another app (feature 003-receive-shared-text)

Another app starts PB Office with a text, a subject and one recipient. Everything is untrusted input and may hold a secret (a share link). The host tests use the real
core and bridges; the reader's own behaviour (how the system delivers the arguments, what stays visible to other programs and for how long) is **unverified** until the rows
DT-HO-1 to DT-HO-9 of `docs/device-test-checklist.md` are done.

| Requirement | Evidence |
|-------------|----------|
| The text, subject and recipient are bounded and cleaned before anything is made of them: invalid UTF-8 and control characters become U+FFFD, line ends become a line feed, the subject is one line, the recipient is exactly one plain address | `test_handoff_parse` (every limit, 8192/8193 bytes, 300/301 characters, 254/255 bytes, the control and C1 ranges, overlong and surrogate forms, a megabyte, ten thousand arguments) |
| A cut or changed text is never taken for a complete one | `test_handoff_parse` (`textlen` missing, shorter, longer, not a number, overflow); the notice `incomplete` (`tst_handoff_flow`) |
| A subject or recipient can add no header: no hidden second recipient, no hidden blind copy | `test_handoff_header_injection` (the real path: parse, store, build the outgoing message; one `To`, no `Bcc` or `Cc`, one `Subject`, the same header names as an ordinary message, the envelope holds only the typed address; the builder also refuses a hostile address if one ever reached a draft) |
| The text is never run, expanded or interpreted: the launcher writes each argument with `printf '%s'` into a file of its own | `tests/launcher/test_handoff_launcher.sh` (`$(...)`, backticks, `;`, `&&`, glob characters, `$1`, `-n`, quotes, a newline, umlauts, 1500 and 8192 bytes: the file holds the bytes, no command ran) |
| The text is not left in the command line of the long-lived launcher: it executes itself again without arguments | `test_handoff_launcher.sh` (the app is started once with no arguments; no process of the host holds the text while the app runs; the log and the log before it hold none; the only files holding it are those of the pending entry). The few milliseconds between the system's start of the launcher and its `exec` are **unverified** on the reader (DT-HO-8) |
| The launcher never prints, logs or evals the arguments, and never traces | rule R11 of `scripts/check-constitution.sh` (arguments only inside the capture block; no `echo`, `logger`, `eval`, `set -x`; 12 self-test cases in `check_constitution_selftest`) |
| Logs hold counts and codes only | `test_handoff_bridge` (`no_log_line_and_no_notice_holds_the_text_...`: no 8-character part of a value in any log line or notice, also after a store error; `only_the_four_codes_...`); codes `handoff.received`, `.drafted`, `.refused`, `.expired` in `core/src/log_codes.h`; the key `fields` is a bit mask because rule R8 refuses keys ending in `recipient`, `subject` and similar words |
| A notice never repeats a value | `tst_handoff_flow` (`test_no_notice_shows_a_value_of_the_handoff`) |
| The pending entry lives seconds, is removed after the draft exists, expires after 10 minutes, and is read without following links | `test_handoff_inbox` (expiry at 600 s, unfinished `.tmp` entries removed too, symbolic links not followed, `remove` refuses ids that leave the folder), `test_handoff_bridge` (entry removed after the draft; a failing store keeps it). The card is vfat: the entry cannot be made private there, and flash cannot be shredded; the README and INSTALL say so |
| Nothing is lost: the draft is saved before any screen; an open message is saved first; a crash between draft and removal makes a second draft, never none; no text is cut silently | `test_handoff_bridge`, `tst_handoff_flow` (`test_a_message_being_typed_is_saved_first_...`) |
| A hand-over sends nothing, starts no sync, switches no Wi-Fi on | `test_handoff_bridge` (no outbox row), `test_idle_no_network` (no connection, no Wi-Fi request), `tst_handoff_flow` (`test_a_handoff_during_a_sync_does_not_touch_it`) |
| Without a mail account the text waits in memory only and is discarded with a notice when the set-up is left | `test_handoff_bridge` (the data folder holds no copy), `tst_handoff_flow` |
| Any app may hand over text; PB Office cannot tell who | by design (spec 003 Clarifications): the only effect is a draft the user must send |

