# Feature Specification: Email and Calendar MVP (office-pocketbook)

**Feature Branch**: `feature/001-email-calendar-mvp`

**Created**: 2026-10-13

**Status**: Draft

**Input**: User description: "Build office-pocketbook (working title), an email and calendar app for the PocketBook InkPad One, based on the architecture and build setup of notes-pocketbook: one mail account and one calendar account, mostly offline on an e-ink reader, syncing over Wi-Fi when available."

## Overview

A single person reads and answers mail and checks their agenda on the InkPad One. The reader is
usually offline: the user syncs when Wi-Fi is available, then reads, writes and plans without a
connection, and the next sync delivers what they did. The app is for reading and light writing,
not for mailbox administration. It must never lose a draft, an unsent message or an edited event,
and it must be calm on e-ink: no animation, no background activity, readable pages.

## Clarifications

### Session 2026-10-13

- Q: How should the app make sure a sent message ends up in the Sent folder? → A: The app saves the copy itself on the mail server by default; a per-account switch "server files sent mail itself" is preset to on for Gmail and Outlook.com and can be changed in Settings.
- Q: How should the app treat Gmail's labels (one message in several "folders", "All Mail")? → A: Labels are ordinary folders with no Gmail-specific logic; "All Mail" and Spam are hidden from sync by default and can be turned on; move and delete act only on the folder the user is in.
- Q: How should the app store the mail and calendar password on the device? → A: As plain text in a file readable only by the app (restrictive permissions); no obfuscation, no PIN. The setup page and About page state this clearly.
- Q: Which time-zone data should the app use? → A: A copy of the IANA time-zone database bundled with the app, used exclusively and updated with app releases; the device's system time-zone data is not used.
- Q: How should a mail change made on the reader be settled when the server changed the same message meanwhile? → A: Flags are merged one by one (the local change wins for a flag the user changed, the server wins for the others); if the message was moved or deleted on the server, the local action is not applied and a "could not apply" note is shown. Nothing is dropped silently.
- Q: Where should drafts live: only on the reader, or also in the mail account's Drafts folder on the server? → A: Drafts are mirrored to the server's Drafts folder, so they can be continued in other mail apps; the reader always saves locally first.
- Q: How far back and ahead in time should the app download calendar events? → A: A fixed window from 30 days back to 365 days ahead of today, moving forward with each sync; not configurable.
- Q: How should the app find the server's Sent, Drafts and Trash folders? → A: Use the roles the server announces; if a role is missing, match common German and English names; if still unclear, ask the user once during account setup and remember the choice.
- Q: What should the app do when it can't find a Trash folder? → A: It creates none; before deleting it warns that deletion is permanent for this account and requires the same separate explicit confirmation as permanent deletion from Trash, each time.
- Q: What happens to a user's unsynced event edits when the event or its whole calendar was removed on the server? → A: It is a conflict: the edited version is kept and marked "deleted on the server", and the user chooses to re-create it on the server or discard it; events with unsynced changes in a removed calendar are kept the same way.
- Q: May a user change a repeating event, or only view it and delete the whole series? → A: Repeating events are read-only in this release: they can be viewed, and the whole series can be deleted after confirmation; changing them is done in another calendar app.
- Q: If the app cannot tell which time zone the reader is set to, what should it do? → A: Use the reader's own setting when it can be read; otherwise ask the user once to pick a time zone from a list the first time a calendar is used, remember it, and allow changing it in Settings.
- Q: If the reader's storage cannot restrict who can read the saved password file, may the app still store the password? → A: Yes; it stores it anyway and states plainly at account setup and in About that this storage cannot protect the file and anyone with USB access can read it, recommending an app-specific password.
- Q: For an account with mail only, how should the app notice that the reader's clock is wrong? → A: It compares the reader's clock with the time stamps the servers report during a normal sync (the time a calendar server reports, or the date the mail server stamps on a message the app just saved to Sent or Drafts) and warns if they differ by more than 10 minutes; with no such time stamp there is no warning.
- Q: On the home screen, how many of today's and tomorrow's events are listed before the rest are hidden behind a link? → A: Up to 6, soonest first (today's before tomorrow's), then a line "+ n more" that opens the Agenda.

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Read my mail offline (Priority: P1)

The user connects one mail account once, syncs on Wi-Fi, and later reads recent mail with no
connection: browsing folders, scanning the list of messages, opening a message with its quoted text
and attachment list, and downloading an attachment when online again.

**Why this priority**: Reading mail on an e-reader without a connection is the core reason for the
app. Everything else builds on a working account setup, folder list, sync and reader.

**Independent Test**: With a test mail account holding anonymized messages, set up the account, sync,
switch Wi-Fi off, and read a plain-text mail, an HTML-only mail and a mail with attachments. This
alone delivers a usable offline mail reader.

**Acceptance Scenarios**:

1. **Given** no account is configured, **When** the user enters server, user name and password and
   runs the connection test, **Then** the app reports success, or a specific reason: wrong
   credentials, unreadable or untrusted server certificate, server unreachable, or network
   unavailable.
2. **Given** a valid account and Wi-Fi, **When** the user syncs, **Then** the folder list (Inbox,
   Sent, Drafts, Trash and user folders) appears and mail of the last N days is available with
   sender, subject, date and unread/flag state, including readable bodies.
3. **Given** a completed sync and no connection, **When** the user opens any synced message, **Then**
   it is shown completely without any network access.
4. **Given** a mail that only contains an HTML part, **When** the user opens it, **Then** the app
   shows a readable text version, with links visible as text and nothing loaded from the internet.
5. **Given** a mail with attachments, **When** the user opens it, **Then** each attachment is listed
   with name and size; **When** the user taps one while online and under the size limit, **Then** it
   downloads on demand; **When** it exceeds the limit or the device is offline, **Then** the user is
   told why it was not downloaded.
6. **Given** a mail older than N days, **When** the user asks for older mail in a folder while
   online, **Then** older messages are fetched on demand.

---

### User Story 2 - See my agenda (Priority: P1)

The user connects one calendar source (a calendar server account, a read-only calendar feed address,
or both), syncs, and then checks appointments offline: an agenda of upcoming events, a day view and
a week or month overview, and the details of an event.

**Why this priority**: Checking "what is next" on the e-reader is the second reason for the app and is
independent of mail.

**Independent Test**: With a test calendar containing single and recurring events in several time
zones, sync, go offline, and verify the agenda, day view, week/month overview and event details.

**Acceptance Scenarios**:

1. **Given** a calendar server account, **When** the user runs the connection test and syncs, **Then**
   the available calendars are listed and the user chooses which are shown.
2. **Given** only a read-only calendar feed address, **When** the user adds it, **Then** its events
   appear in the agenda and are shown as read-only.
3. **Given** a synced calendar, **When** the user opens the agenda, **Then** upcoming events are
   listed in time order with date, time and title, page by page.
4. **Given** a synced calendar, **When** the user opens the day view and the week or month overview,
   **Then** events appear on the correct days and times in the user's time zone, and a day with
   events is visibly marked in the overview.
5. **Given** an event with location, description, attendees and a recurrence rule, **When** the user
   opens it, **Then** all of these are shown, and a recurring event appears on every correct date,
   skipping excluded dates and showing edited single occurrences.
6. **Given** a recurring event that crosses a daylight-saving change, **When** the user views dates
   before and after the change, **Then** the event stays at the same local time on each date.

---

### User Story 3 - Write, reply and forward mail (Priority: P2)

The user writes a new message, replies, replies to all or forwards, offline if necessary. The
message waits in an outbox and is sent when the user sends it and a connection exists. Drafts are
saved automatically.

**Why this priority**: Answering mail completes the mail workflow but needs the reader (story 1) first.

**Independent Test**: Compose and reply while offline, kill the app, restart, find the draft intact,
send while online, and verify exactly one copy was sent and one copy is in Sent.

**Acceptance Scenarios**:

1. **Given** an open message, **When** the user chooses reply, reply to all or forward, **Then** a
   plain-text draft opens with recipients, subject and quoted original filled in appropriately.
2. **Given** a draft being typed, **When** the app is closed or killed at any moment, **Then** the next
   start offers the draft with at most the last 2 seconds of typing missing.
3. **Given** a finished draft and no connection, **When** the user taps Send, **Then** the message is
   placed in the outbox, shown as waiting, and sent at the next sync or when the user retries.
4. **Given** a message in the outbox and a connection, **When** sending succeeds, **Then** the message
   leaves the outbox and a copy appears in Sent (saved by the app, or by the server when the account's
   "server files sent mail itself" switch is on; never both). **When** the server accepted the message but
   saving the copy fails, **Then** the message is not resent and only the saving of the copy is retried.
5. **Given** a send that fails clearly (for example the server refuses the recipient), **When** the
   user looks at the outbox, **Then** the message is still there, marked failed with the reason, and
   can be edited, retried or deleted.
6. **Given** a send whose outcome is unknown (connection lost after the message was transmitted),
   **When** the user looks at the outbox, **Then** the message is marked "may have been sent", is not
   resent automatically, and the user decides.

---

### User Story 4 - Create and edit events (Priority: P2)

The user creates, changes and deletes events on the reader, offline if necessary. Changes are sent
to the calendar server at the next sync.

**Why this priority**: Planning on the device is valuable but secondary to seeing the agenda.

**Independent Test**: Create, edit and delete events offline, sync, and verify the server shows the
changes; edit the same event on the server in between and verify a conflict keeps both versions.

**Acceptance Scenarios**:

1. **Given** a writable calendar, **When** the user creates an event with title, start, end (or
   all-day), location and description, **Then** it appears immediately in all views and is marked as
   not yet synced.
2. **Given** an existing event, **When** the user changes it, **Then** the change is shown at once and
   is sent at the next sync; the not-yet-synced mark disappears after confirmation by the server.
3. **Given** an existing event, **When** the user deletes it and confirms the question, **Then** it
   disappears and the deletion is sent at the next sync.
4. **Given** the same event was changed on the reader and on the server, **When** the next sync runs,
   **Then** neither change is lost: both versions are kept and the user is told so and can resolve it.
5. **Given** a read-only calendar or feed, **When** the user tries to edit, **Then** the app explains
   that this calendar is read-only.
6. **Given** a repeating event, **When** the user opens it, **Then** it is shown with its repeat rule and
   cannot be changed; **When** the user deletes it and confirms, **Then** the whole series is deleted at the
   next sync; **When** the user tries to change it, **Then** the app explains that repeating events are
   changed in another calendar app.

---

### User Story 5 - Organize and find mail (Priority: P3)

The user marks mail read or unread, flags it, moves it to another folder, deletes it to Trash,
sorts the list and searches the downloaded mail.

**Why this priority**: Useful housekeeping, but reading and writing come first.

**Independent Test**: Perform each action offline on test mail, sync, and verify the server state;
search for a sender and a subject word in a list of 5,000 headers.

**Acceptance Scenarios**:

1. **Given** a message, **When** the user marks it read/unread or flagged, **Then** the list shows it at
   once and the change reaches the server at the next sync.
2. **Given** a message, **When** the user moves it to another folder, **Then** it leaves the current
   list and appears in the target folder after the next sync.
3. **Given** a message, **When** the user deletes it and confirms, **Then** it moves to Trash; permanent
   deletion from Trash asks for a separate, explicit confirmation (an account without a Trash folder
   gives the same warning and confirmation before every delete).
4. **Given** a folder, **When** the user sorts by date, sender or subject, **Then** the list is reordered.
5. **Given** downloaded mail, **When** the user searches for a word, **Then** matching messages (sender,
   subject, and body text that is stored on the device) are listed within a few seconds, offline.
6. **Given** the user starts typing a recipient, **When** the address appeared in cached mail, **Then**
   matching addresses are suggested.

---

### User Story 6 - Home screen, sync control and settings (Priority: P3)

The user sees at a glance the unread count, the next events and the time of the last sync, starts a
sync manually, and manages settings: account, how many days of mail to keep, custom certificate
authority, Auto Sync, and the About page with license texts.

**Why this priority**: Orientation and control; the app works without it, but offline/unsynced state
must always be clear.

**Independent Test**: Open the app offline and verify the home screen shows cached unread count, next
events, the last sync time and an offline indication; change the sync window and verify the next sync
respects it.

**Acceptance Scenarios**:

1. **Given** the app is started, **When** the home screen appears, **Then** it shows the unread count,
   today's and tomorrow's events, the last successful sync time and whether the device is offline or
   changes are waiting to be synced.
2. **Given** the app is started with a connection, **When** it opens, **Then** a sync starts
   automatically; **When** the user taps Sync, **Then** a sync runs and shows progress and result.
3. **Given** Auto Sync is on, **When** the user sends a message or saves an event, **Then** a sync of
   that change is attempted shortly afterwards; **When** Auto Sync is off, **Then** nothing is sent
   until the user syncs.
4. **Given** the settings page, **When** the user changes the number of days of mail kept, **Then** the
   next sync downloads or removes mail to match, without removing drafts, outbox items or unsynced
   changes.
5. **Given** a private server whose certificate is issued by the user's own authority, **When** the user
   places the authority's certificate file in the app's data folder, **Then** Settings shows that a
   custom authority is loaded and the connection test succeeds; **When** a certificate cannot be
   verified, **Then** the connection is refused with no option to bypass the check.
6. **Given** the About page, **When** the user opens it offline, **Then** the app version and the full
   license texts of all included third-party components are shown.

---

### Edge Cases

- **Huge mailbox**: a folder with tens of thousands of messages must not make the app slow, run out of
  memory, or fill the storage; only the configured window is downloaded, and the user is told that older
  mail exists on the server.
- **Malformed mail**: broken structure, missing headers, truncated content, invalid or unknown character
  sets, extremely long lines, huge or deeply nested parts; the message is shown as far as it is readable
  or marked unreadable, and the app and the sync continue.
- **Character sets**: German umlauts, other Western and Eastern European sets, UTF-8, and encoded subjects
  and sender names display correctly; characters the font cannot show are replaced visibly.
- **HTML-only, empty and text-less mails**: shown as readable text, or as an explicit "no readable text"
  note with the attachments still available.
- **Very large attachment or message**: refused with the size limit stated; never loaded fully into
  memory without a limit.
- **Expired, mismatching or untrusted certificate**: the connection is refused and the reason is named
  (expired, wrong host, unknown authority); nothing is sent, and credentials are never sent.
- **Wrong or revoked password**: reported as an authentication problem, distinct from network problems;
  local data stays untouched.
- **Network drops during sync**: the next sync continues without duplicates or gaps.
- **Server timeout during send**: see story 3 - the message is never resent automatically when the outcome
  is unknown; double-tapping Send or repeated retries never produce two copies.
- **App killed, battery empty, or device put to sleep** during sync, compose or event editing: no draft,
  outbox message or edited event is lost.
- **Mailbox reset on the server** (folder recreated, message numbers change): the local copy is rebuilt
  without losing drafts, outbox items or unsynced changes.
- **Daylight-saving transitions and time zones**: events in non-local zones, events at the nonexistent or
  repeated hour, all-day events, and floating-time events appear at the intended local times.
- **Recurring events**: weekly, monthly and yearly rules, end dates and counts, excluded dates, edited
  single occurrences, and series crossing a daylight-saving change.
- **Unusual calendar data**: events without end, with extremely long text, with unknown time zone names,
  or with unsupported recurrence parts are shown as far as possible and never cause a crash.
- **Same message in two folders or moved elsewhere on the server** while a local action is pending: the
  local action is applied or reported with a "could not apply" note (see FR-019), never silently dropped.
- **Storage nearly full**: the app warns and stops downloading instead of corrupting data; unsynced work
  stays safe.
- **Device clock wrong**: the app compares the reader's clock with the time stamps the servers report during a
  normal sync (the time a calendar server reports, or the date the mail server stamps on a message the app just
  saved to Sent or Drafts) and shows a warning when they differ by more than 10 minutes. It adds no extra
  connection for this; with no such time stamp there is no warning.

## Requirements *(mandatory)*

### Functional Requirements

**Accounts and connection**

- **FR-001**: Users MUST be able to set up exactly one mail account (incoming and outgoing server, user
  name, password) and run a connection test that reports success or a specific cause for failure:
  wrong credentials, certificate not trusted/expired/not matching, server unreachable, or no network.
- **FR-002**: Users MUST be able to set up one calendar source: a calendar server account whose calendars
  are discovered automatically and chosen by the user, and/or one read-only calendar feed address.
- **FR-003**: Connections MUST be encrypted and certificate-verified at all times; there MUST be no
  setting to turn verification off or to accept an unverifiable certificate. Users MAY add their own
  certificate authority by placing a certificate file in the app's data folder, and Settings MUST show
  whether one is loaded.
- **FR-004**: The app MUST store the mail and calendar passwords as plain text in a file with restrictive
  file permissions (readable only by the app) where the file system allows it, without obfuscation and
  without a PIN or passphrase. After saving, the app MUST check whether the permissions actually took
  effect. The app MUST state plainly on the account setup page and in the About page how the password is
  stored: when the permissions are in effect, that the file is readable only by the app; when the storage
  cannot enforce them, that the storage cannot protect the file and anyone with USB access to the reader can
  read it. In both cases it MUST recommend app-specific passwords, and it still stores the password.
  Passwords MUST NOT appear in logs, error messages or exported files.

**Mail: sync and reading**

- **FR-005**: The app MUST show the account's folders, including Inbox, Sent, Drafts, Trash and user-created
  folders. Every mail folder is treated alike, including Gmail labels: there is no Gmail-specific label
  handling, a message under two labels appears in each of those folders, and move and delete act only on
  the folder the user is in. Gmail's "All Mail" and Spam folders are not synced by default; the user can
  turn them on in Settings.
  The app MUST identify the Sent, Drafts and Trash folders from the roles the server announces; if a
  role is missing it MUST match common German and English folder names (for example Sent, Gesendet,
  Drafts, Entwürfe, Trash, Papierkorb); if a folder is still unclear it MUST ask the user once during
  account setup and remember the choice. The choice can be changed in Settings.
- **FR-006**: The app MUST download, per folder, message headers and readable bodies for the last N days
  (N configurable, with a default of 30), and MUST fetch older messages on demand.
- **FR-007**: The message list MUST show sender, subject, date, unread state and flag state, in pages, and
  MUST support sorting by date, sender and subject.
- **FR-008**: The message reader MUST show plain-text mail directly, MUST show HTML mail converted to readable
  text (headings, lists, line breaks, and links as visible text), and MUST show quoted text distinguishably.
- **FR-009**: The reader MUST NOT load anything from the internet when a message is opened or synced (no images,
  styles, or tracking content), and MUST NOT open links automatically. Amendment 2026-10-16 (owner): pictures
  that are parts of the message (`cid:`) are shown; the remote pictures of one message are loaded only when
  the user taps "Load pictures" on that message, which says beforehand that the sender is told that the mail
  was read. Only https, plain GETs, bounded size and number; no styles, fonts or frames ever. Long link
  addresses are shown in a shortened form (start and end); a tap on one shows it whole; links are never opened.
- **FR-010**: Attachments MUST be listed with name and size and downloaded only when the user asks, subject
  to a size limit that is stated when it blocks a download.
- **FR-011**: Messages MUST remain readable offline once synced, including their downloaded attachments.

**Mail: writing**

- **FR-012**: Users MUST be able to compose a new message, reply, reply to all, and forward, in plain text.
- **FR-013**: Drafts MUST be saved automatically on the device while typing and restored after any
  interruption, and MUST be listed in the Drafts folder. Drafts MUST also be mirrored to the account's
  Drafts folder on the server at sync (not on every keystroke), so they can be continued in other mail
  apps. A draft MUST be kept on the device until the server has confirmed the mirrored copy. When a draft
  is sent, or deleted by the user (with confirmation), its server copy is removed at the next sync. If the
  same draft was changed both on the device and on the server, both versions MUST be kept as separate
  drafts and the user told so; neither is discarded automatically.
- **FR-014**: Sending MUST be an explicit user action. A sent message MUST first be placed in an outbox that
  survives restarts, and the outbox MUST show each message as waiting, sending, failed (with reason), or
  "may have been sent".
- **FR-015**: A message MUST NOT be sent twice by the app: when the outcome of a send is unknown, the app
  MUST NOT resend automatically and MUST ask the user; repeated taps on Send MUST NOT create duplicates.
- **FR-016**: After a successful send, exactly one copy MUST be stored in the Sent folder. By default the
  app saves the copy itself on the mail server; when the account's "server files sent mail
  itself" switch is on, the app MUST NOT save a copy and relies on the server. The switch is preset to on
  for Gmail and Outlook.com and off otherwise, and can be changed in Settings. If saving the copy fails
  after the server accepted the message, the app MUST retry only the saving of the copy and MUST NOT
  resend the message.
- **FR-017**: Failed outbox messages MUST be retryable, editable and deletable by the user.

**Mail: organizing**

- **FR-018**: Users MUST be able to mark messages read/unread and flagged/unflagged, move them between
  folders, and delete them; deletion MUST ask for confirmation and move the message to Trash, and
  permanent deletion MUST ask for a separate confirmation. If the account has no
  Trash folder, the app MUST NOT create one; deleting a message MUST then warn that the deletion is
  permanent for this account and MUST require the same separate explicit confirmation as permanent
  deletion, every time.
- **FR-019**: Changes made offline MUST be remembered and applied to the server at the next sync, and MUST
  not be lost if the server state changed meanwhile. If the server changed the same message meanwhile,
  read and flag states MUST be merged one by one: the local change wins for each flag the user changed,
  the server state wins for the others. If the message was moved or deleted on the server, the local
  action MUST NOT be applied and the user MUST be shown a "could not apply" note; no local action is
  dropped silently.
- **FR-020**: Users MUST be able to search downloaded mail by sender, recipient, subject and stored body
  text, without a connection, and the app SHOULD suggest addresses from cached mail while a recipient is
  typed.

**Calendar**

- **FR-021**: The app MUST provide an agenda (upcoming events in time order), a day view, and a week or month
  overview, all paged and without smooth scrolling. The app MUST download events only for a fixed window
  from 30 days before to 365 days after today, moving forward with each sync; the window is not
  configurable. Recurring events are expanded within that window. Dates outside the window show no
  events, and the views MUST say that the calendar is only available for that period. Events outside the
  window that the user created or changed and that are not yet confirmed by the server MUST NOT be
  removed (see FR-032).
- **FR-022**: The event view MUST show title, start and end (or all-day), time zone where it differs from the
  user's, location, description, attendees and the recurrence in words.
- **FR-023**: Event times MUST be shown correctly for events in any time zone, across daylight-saving
  changes, for all-day events and for floating times. All time-zone rules MUST come from an IANA time-zone
  database bundled with the app (updated with app releases), never from the device's system time-zone
  data, so results are identical on the host tests and on the device. The bundled database MUST be listed
  with the other third-party components (see FR-038). The user's own time zone is the one chosen in
  Settings; if none was chosen, the reader's own setting when the app can read it; if that cannot be read
  either, the app MUST ask the user once to pick a time zone from a list (the first time a calendar is used),
  remember the choice, and never show times in an assumed zone without the user having chosen it.
- **FR-024**: Recurring events MUST be expanded correctly, honoring end date or count, excluded dates and
  edited single occurrences.
- **FR-025**: Users MUST be able to create, change and delete events on writable calendars while offline;
  deletion MUST ask for confirmation. This applies to events that do not repeat. A repeating event is
  read-only in this release: the user can view it and delete the whole series (with confirmation), and the
  app MUST explain that changing it is done in another calendar app.
- **FR-026**: Unsynced event changes MUST be marked visibly until confirmed by the server.
- **FR-027**: If an event was changed both on the device and on the server, the app MUST keep both versions
  and make the conflict visible; neither version may be discarded automatically.
  The same applies when the event, or its whole calendar, was removed on the server while the device holds
  unsynced changes: the edited version MUST be kept and marked "deleted on the server" until the user
  chooses to re-create it on the server or to discard it (discarding asks for confirmation).
- **FR-028**: Read-only calendars and feeds MUST be clearly marked and refuse edits with an explanation.
- **FR-029**: The home screen MUST show today's and tomorrow's events, soonest first (today's before
  tomorrow's), up to 6 of them; when there are more, it MUST show a line "+ n more" that opens the Agenda, so
  no event is silently hidden. There are no timed reminders or alarms in this release.

**Sync and offline behavior**

- **FR-030**: Sync MUST run on app start (when a connection exists), when the user requests it, and shortly
  after a message is sent or an event is saved when Auto Sync is on (default on, switchable). The app
  MUST NOT sync or use the network in the background otherwise.
- **FR-031**: Sync MUST be safe to interrupt at any moment (connection loss, sleep, empty battery, app
  killed) and MUST continue correctly the next time without duplicating or losing anything.
- **FR-032**: Drafts, outbox messages and unsynced event or mail changes MUST NEVER be deleted, evicted or
  overwritten before the server has confirmed them, including when storage is reduced or the sync
  window is shortened.
- **FR-033**: The app MUST show clearly at all times whether the device is offline, when the last successful
  sync happened, and whether local changes are waiting to be synced.
- **FR-034**: All features that need no server contact (reading, writing, searching, viewing the calendar,
  creating events) MUST work with no network.
- **FR-035**: Sync problems MUST be reported with a specific, understandable reason and MUST leave local data
  intact.

**Home, settings, information**

- **FR-036**: The home screen MUST show the unread count, the next events (as FR-029) and the last sync time.
- **FR-037**: Settings MUST allow changing the account details, the number of days of mail kept, Auto Sync,
  the maximum attachment size, the "server files sent mail itself" switch, whether Gmail's "All Mail" and
  Spam folders are synced, which folders serve as Sent, Drafts and Trash, and show the custom certificate
  authority status.
- **FR-038**: The About page MUST show the app version and the full text of all third-party licenses, available
  offline.

**Privacy and safety**

- **FR-039**: The app MUST NOT send user data anywhere except to the servers the user configured.
- **FR-040**: Malformed or hostile mail and calendar content MUST NOT crash the app, hang the sync, or
  execute anything; unreadable items are shown as unreadable.
- **FR-041**: Diagnostic information the app keeps MUST NOT contain passwords, addresses, subjects, message
  text, event titles, attendees, locations, server addresses, or file names.

**Usability on e-ink**

- **FR-042**: The interface MUST be high-contrast black and white, use large touch targets, no animations, and
  paged lists, and work with the on-screen keyboard.

### Out of Scope for the first release

Multiple accounts; push notification of new mail, live updates or any background sync; rendering of HTML
mail with formatting or images; rich-text composing; encrypted or signed mail; an address book (apart from
suggesting addresses from cached mail; server contacts are specified in `specs/002-server-contacts/spec.md`); answering meeting invitations; shared calendars; tasks;
timed reminders and alarms; widgets; dark mode; non-password sign-in methods for the mail and calendar accounts;
changing repeating events (single occurrences or whole series).

### Key Entities

- **Account**: one mail account and one calendar source; server details, user name, credential, the
  certificate trust state, sync window, the "server files sent mail itself" switch and options.
- **Folder**: a mail folder with its name, role (Inbox, Sent, Drafts, Trash, other), and sync state.
- **Message**: header information (sender, recipients, subject, date), read and flag state, readable body,
  attachment list, folder, and a local-change state (synced, pending change, pending deletion).
- **Attachment**: name, size, type, and whether it has been downloaded to the device.
- **Draft**: an unsent message under edit, saved continuously on the device and mirrored to the server's
  Drafts folder at sync, with a sync state (synced, pending, conflict).
- **Outbox Item**: a message to be sent with its state (waiting, sending, failed with reason, may have been
  sent) and its unambiguous identity for duplicate prevention.
- **Calendar**: a source of events with its name, writability, and whether it is shown.
- **Event**: title, start, end or all-day, time zone, location, description, attendees, recurrence rule,
  exclusions, edited occurrences, and a sync state (synced, pending, conflict, deleted on the server).
- **Conflict**: two versions of the same event kept side by side until the user decides, or an edited event
  that was removed on the server kept until the user re-creates or discards it (discarding asks for confirmation).
- **Sync State**: when the last sync happened, what is waiting to be sent, and any problem found.

## Success Criteria *(mandatory)*

### Measurable Outcomes

Device figures are targets for the InkPad One and stay "unverified" until measured there.

- **SC-001**: A new user can set up the mail account, pass the connection test and read their first mail in
  under 5 minutes, using only the on-screen keyboard.
- **SC-002**: After a completed sync, 100% of the messages in the sync window can be opened and read with
  the device offline.
- **SC-003**: The app starts and shows the home screen in under 5 seconds on the device (cold start,
  cached data, no network wait).
- **SC-004**: A folder with 5,000 downloaded message headers can be browsed page by page, and each page
  turn takes under 1 second; searching it returns results in under 3 seconds.
- **SC-005**: Opening a downloaded message takes under 2 seconds, including a 1 MB message.
- **SC-006**: After the app is killed or the battery is removed at a random moment during sync, compose,
  send or event editing, repeated 100 times in automated tests, 0 drafts, outbox messages or edited events
  are lost.
- **SC-007**: In automated tests that inject connection loss, timeouts and repeated taps at every step of
  sending, a message is sent at most once in 100% of runs, and every message whose outcome is unknown is
  shown to the user as such.
- **SC-008**: When the same event is changed on the device and the server, 100% of such cases keep both
  versions.
- **SC-009**: Events in a test set of at least 30 recurring and time-zone cases, including daylight-saving
  crossings, show on the expected dates and local times in 100% of cases.
- **SC-010**: A test collection of at least 50 malformed mails and 20 malformed calendar items never crashes
  or hangs the app, and each is shown as far as readable or as unreadable.
- **SC-011**: When idle (app open, no user action, Auto Sync finished), the app makes no network
  connection, verified in tests.
- **SC-012**: 100% of connection attempts with an expired, mismatching or untrusted certificate are refused,
  and 0 credentials are sent in those attempts.
- **SC-013**: Locally stored mail does not exceed a limit that the user sees and can change (default
  200 MB); at the limit older, non-essential mail is removed first and 0 unsynced items are removed.
- **SC-014**: A first sync of 30 days of mail (about 500 messages, without attachments) completes in under
  10 minutes on ordinary home Wi-Fi.

## Assumptions

- The user has one mail account that offers standard encrypted access for reading and sending, and one
  calendar server (Nextcloud or OpenCloud style) or one calendar feed address; Gmail and Microsoft accounts
  are usable only with an app-specific password.
- The first release targets the PocketBook InkPad One; other models are untested.
- Password-based sign-in with app-specific passwords is the only sign-in method; no browser-based sign-in.
- The user's main language is German or English; the interface is bilingual only if the plan finds it cheap.
- The default sync window is 30 days; the default attachment size limit is 10 MiB; the default local storage
  limit is 200 MB. All three can be changed in Settings.
- "Search over downloaded mail" covers headers always and body text for downloaded messages.
- A mail account and a calendar source are independent; either can be used without the other.
- Mail composing offers a signature-free plain-text body; attachments can be received but adding attachments to
  outgoing mail is not part of this release.
- This specification is deliberately one release-sized feature; the plan may split delivery into increments
  by story priority.
- The app is a sibling of notes-pocketbook, is separate from it, and does not share data with it.

### Assumptions that need a live server or the device to verify

All of these are unverified until exercised against real services or on the InkPad One.

- That Gmail and Outlook.com file a copy in Sent themselves when a message is submitted (the basis of the
  preset of the "server files sent mail itself" switch, FR-016) is unverified until checked against live accounts.
- The target mail servers (a standards-compliant server, Gmail, Outlook.com, the author's own provider)
  support the folder, flag, move and sent-copy behavior described here; how each names its special folders
  (Sent, Drafts, Trash) may differ.
- Servers behave as assumed when a send is interrupted (whether a message was accepted or not).
- The calendar server supports calendar discovery, change detection, conditional writes and conflict
  signalling as assumed, and handles recurring events with edited occurrences as assumed.
- Servers with private certificate authorities work with the custom-certificate mechanism.
- The device can keep the reader responsive with the stated mailbox sizes, startup time and storage limit.
- The bundled time-zone database is current enough for correct daylight-saving handling until the next app
  release; when the reader exposes its time-zone setting the app uses its name only (rules always come from
  the bundled database); whether the reader exposes it is unverified until the device probe.
- Wi-Fi reconnect, sleep and wake behavior of the device during sync matches expectations.
- The device's networking components support the encrypted mail protocols needed (to be confirmed in the plan).
