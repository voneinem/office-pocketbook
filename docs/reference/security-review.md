# Security review of PB Office (mail and calendar)

Date: 2026-10-16. Scope: the core library (`core/`), the Qt bridge (`bridge/`), the QML pages (`qml/`), the
app (`app/`), the build and test tooling and the repository, for the feature `001-email-calendar-mvp`.
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
  IMAP/SMTP, Nextcloud and OpenCloud (`docs/live-server-checklist.md`).
- **O4** Commits made before the clean-up of the baseline may contain local absolute paths in their
  history. Rewrite the history before the first public push if that matters.
- **O5** `gitleaks` is not installed on the development machine; the built-in scan is weaker.
- **O6** The performance budgets (cold start, page turn, first sync of 500 messages) are measured on the
  host only; the reader figures are device items.
