# Live-server checklist

Every item starts as **unverified**: the tests of this project run against fake servers
(`tests/support/fake_imap_server.h`, `fake_smtp_server.h`, `fake_caldav_server.h`) that implement what the
specifications say, not what a particular provider does. An item changes only when someone has run it against a
real server and written the date and the result next to it. Use **app-specific passwords** and throw-away
test accounts; never paste real mail, addresses or passwords into this file or into the repository. Anonymized
exchanges that were seen live become fixtures: drop the `.unverified` suffix only for those.

Targets (spec Assumptions): a Dovecot test account, Gmail and Outlook.com with app-specific passwords, the
owner's own provider, Nextcloud first and OpenCloud second for CalDAV, and one iCalendar feed.

Legend: **U** unverified · **V** verified (date, target, note) · **F** failed (what happened) · **n/a** the
target does not offer it.

Columns for every table: Dovecot / Gmail / Outlook.com / own provider (mail), Nextcloud / OpenCloud / feed
(calendar). Write the result per target, for example `V 2026-11-02`.

## Mail: IMAP and SMTP (T086)

| ID | Item | Expected | Dovecot | Gmail | Outlook.com | Own |
|----|------|----------|---------|-------|-------------|-----|
| LV-M1 | Connection test (implicit TLS 993/465, STARTTLS 143/587) with the right and a wrong password | Success; wrong password gives the password message and no other detail | U | U | U | U |
| LV-M2 | Provider preset: host names fill ports, security and the "server keeps sent mail itself" switch (on for Gmail and Outlook.com, off for Dovecot) | exact-host presets only | U | U | U | U |
| LV-M3 | Folder roles: Inbox, Sent, Drafts, Trash, Archive found by attribute or by name (including German names) | roles right; a missing one is asked for in Settings | U | U | U | U |
| LV-M4 | First sync of a 30-day window; `UIDVALIDITY`, `SINCE`, `UID FETCH` of headers and `BODYSTRUCTURE` accepted by the server | window downloaded, `older_on_server` set | U | U | U | U |
| LV-M5 | Body fetch: text/plain, text/html only, multipart/alternative, a message with an unusual charset | readable text; charset note when unknown | U | U | U | U |
| LV-M6 | Attachment download under and over the limit, offline afterwards | file in Downloads under its name; limit message | U | U | U | U |
| LV-M7 | Special-folder names and Gmail labels: All Mail and Spam are not synced by default | as in the app text | U | n/a | n/a | U |
| LV-M8 | `CONDSTORE` / `CHANGEDSINCE`: flags changed in another client arrive; note whether the server offers it | flags merged by the rule | U | U | U | U |
| LV-M9 | Capabilities seen: `UIDPLUS`, `MOVE`, `LITERAL+`, `ID`, `ESEARCH` (write down which are present) | used or fallen back to without error | U | U | U | U |

| LV-M10 | A newsletter with pictures: parts of the mail (`cid:`) are loaded with the body; the remote pictures only after "Load pictures"; the sender's server sees one GET per picture without cookies or referrer (check in the sender's own logs if you can, else with a local proxy) | as described | U | U | U | U |

## Mail: sending, copies, drafts (T136)

| ID | Item | Expected | Dovecot | Gmail | Outlook.com | Own |
|----|------|----------|---------|-------|-------------|-----|
| LV-S1 | Send to yourself; the message arrives once | exactly one message received | U | U | U | U |
| LV-S2 | Sent copy: switch as preset | exactly one copy in Sent (Dovecot: filed by the app; Gmail and Outlook.com: by the provider) | U | U | U | U |
| LV-S3 | A refused recipient (a made-up address at the provider's domain) | "a recipient address was refused", nothing sent | U | U | U | U |
| LV-S4 | Cut the connection (Wi-Fi off) in the middle of sending | the row shows "may have been sent"; the app never sends it again by itself; check the recipient's inbox and Sent | U | U | U | U |
| LV-S5 | Draft mirror: write a draft on the reader; it appears in the Drafts folder of another client; edit it in the reader; one copy remains | one copy, the old one removed only after the new one exists | U | U | U | U |
| LV-S6 | Delete a draft on the reader | the copy on the server is removed | U | U | U | U |
| LV-S7 | A draft changed on both sides | two drafts, nothing lost | U | U | U | U |
| LV-S8 | Reply, reply to all, forward: headers (`In-Reply-To`, `References`) and quoted text correct in the recipient's client | threads correctly | U | U | U | U |

## Mail: organizing (T161)

| ID | Item | Expected | Dovecot | Gmail | Outlook.com | Own |
|----|------|----------|---------|-------|-------------|-----|
| LV-O1 | Read/unread and star offline, then sync; check in another client | flags equal on both sides | U | U | U | U |
| LV-O2 | Flag changed in another client and on the reader before a sync | merge rule: the reader's change wins for that flag only; nothing else touched | U | U | U | U |
| LV-O3 | Move to a folder (note `MOVE` or `COPY` + delete + `UID EXPUNGE`) | one copy in the target, none in the source | U | U | U | U |
| LV-O4 | Delete with a Trash folder; delete inside Trash (asks twice); delete without a Trash folder | as described in the app text; never a bare `EXPUNGE` | U | U | U | U |
| LV-O5 | A message deleted in another client while a change is waiting on the reader | "could not apply" in "To decide"; the user chooses | U | U | U | U |
| LV-O6 | Gmail: a move to a folder is a label change; All Mail and Spam stay off | no duplicates in the list | n/a | U | n/a | n/a |

## Calendar: reading (T115)

| ID | Item | Expected | Nextcloud | OpenCloud | Feed |
|----|------|----------|-----------|-----------|------|
| LV-C1 | Discovery from the entered address: principal, calendar home set, calendars with names, colors ignored, writability | calendars listed; read-only ones marked | U | U | n/a |
| LV-C2 | `sync-collection` supported? If not, window query fallback | events of 30 days back to 365 ahead | U | U | n/a |
| LV-C3 | The server's time (HTTP `Date`) is read; set the reader's clock 1 hour off and sync | "Check the clock" warning | U | U | U |
| LV-C4 | Time zones and repeating events from other clients (Thunderbird, phone): same hour on the reader | correct local times; DST crossings | U | U | U |
| LV-C5 | All-day events and events without a zone (floating) | shown as all day / as written | U | U | U |
| LV-C6 | An iCalendar feed address (https or webcal) | read only; no edit buttons | n/a | n/a | U |
| LV-C7 | A redirect at the server (http to https, or to another path) | followed on https only; credentials not forwarded | U | U | U |

## Calendar: writing (T149)

| ID | Item | Expected | Nextcloud | OpenCloud |
|----|------|----------|-----------|-----------|
| LV-W1 | Create an event on the reader, sync | appears in another client; the `If-None-Match: *` create works | U | U |
| LV-W2 | Edit an event created in another client (with fields the app does not show: alarms, categories, attendees) | those fields are still there after the edit (lossless) | U | U |
| LV-W3 | Edit on both sides before a sync | conflict page; both versions kept when asked | U | U |
| LV-W4 | Delete on the reader; delete on the server while a change waits on the reader | delete succeeds; or "deleted on the server" asks | U | U |
| LV-W5 | `If-Match` with a stale ETag is refused with 412 and handled as a conflict | no overwrite | U | U |
| LV-W6 | A calendar that the server marks read-only | the app offers no edit | U | U |
| LV-W7 | A repeating event: shown, not editable, deletable as a whole | as in the app text | U | U |
| LV-W8 | The calendar is removed on the server | the app says so; its events stay until the user decides | U | U |

## Recording

For every `V` or `F` add the date and one line (no addresses, no host names that identify the owner). Put
anonymized transcripts in `tests/fixtures/` with the suffix `.unverified` until a live run has produced
them; then rename. Findings that change a decision go into `docs/device-findings.md`.
