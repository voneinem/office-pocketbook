# Feature Specification: Receive Shared Text

**Feature Branch**: `feature/003-receive-shared-text`

**Created**: 2026-10-23

**Status**: Draft

**Input**: User description: "Known from a test on the reader (2026-10-23): one app can start another with text as a parameter; the text arrives complete and unchanged (tested to 1500 characters, non-ASCII letters and line breaks included), and the calling app stays open in the background. Not yet known, and to be proven on the reader in the plan: the behaviour when PB Office is already running (the system may only bring the running copy forward, so the text could be lost), and that the launcher passes the text on without printing or logging it and without leaving it in a long-lived command line. Test code is under: nextcloud-pocketbook."

## Overview

Other PocketBook apps of the same family (the first one is PB Cloud, which creates share links for files) want
to hand a piece of text to PB Office so that the user can mail it: "Mail this link". The reader has no
system clipboard, so text cannot be copied from one app and pasted into another; starting PB Office with the
text as a parameter is the only way, and a test on the reader on 2026-10-23 showed that it works.

This feature makes PB Office the receiving side. When another app starts PB Office with a text, PB Office opens a
new message whose body is that text. The other app may also hand over a subject and one recipient; PB Office
prefills them, and the user can change or remove them. The user completes the message and sends it like any
other; the message is a draft from the first moment, so the text is never lost. Everything handed over is
untrusted input that may hold a secret (a share link) or a private address, so it is never printed, logged or
left visible anywhere outside the message the user is writing, and nothing is ever sent without the user's tap.

The sending side (the button in PB Cloud) belongs to the other app and is out of scope here; this feature
defines what PB Office must do when it is started with text, whoever started it.

## Clarifications

### Session 2026-10-23

- Q: If the reader's system cannot deliver the text to a PB Office that is already running, what should the feature do? → A: Ship with a fallback: PB Office restarts itself or exits cleanly so that the next start receives the text; if even that fails, it tells the user that nothing was handed over and to try again.
- Q: Should another app be able to pass anything besides the text? → A: Yes: the text (body), an optional subject and an optional recipient, all prefilled and editable by the user (the owner chose this over text only).
- Q: Should PB Office accept a hand-over from any app on the reader, or only from named apps? → A: Any app; the text is plain, untrusted text and every hand-over only creates a draft that the user must send.

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Mail a text from another app when PB Office is not running (Priority: P1)

The user is in PB Cloud, looking at a share link, and chooses "Mail this link". PB Office starts and shows a new
message with the link already in the body, and the subject and recipient too if the other app supplied them. The
user fills in whatever is missing (the recipient with the existing address suggestions), may change anything, and
sends the message, offline if necessary. When the user leaves PB Office, they are back in
the app they came from.

**Why this priority**: This is the whole reason for the feature, and it is the case that the test on the reader
already showed to work. It alone delivers "mail a link from the reader without typing it twice".

**Independent Test**: With PB Office closed, start it from a test program with a fixed text (as the 2026-10-23
probe did), and check that a new message is open with exactly that text as body, that the message exists as a
draft, and that it can be sent. Repeat with texts of 1 character, 1500 characters, non-ASCII letters (for
example German umlauts and the euro sign) and line breaks, and with and without a subject and a recipient.

**Acceptance Scenarios**:

1. **Given** PB Office is not running and an account is set up, **When** another app starts it with a text,
   **Then** PB Office opens a new message editor whose body is exactly that text (every character, every line
   break), with recipient and subject empty when the other app gave none, and the cursor in the first empty
   field (recipient, then subject, then body).
2. **Given** the new message from a hand-over, **When** the user completes it and taps Send, **Then** it follows
   the normal rules for a new message (outbox when offline, sent at the next sync or retry), and the message
   that arrives has the handed-over text as its body and the recipient and subject as shown at that moment.
3. **Given** a message opened by a hand-over, **When** the app is closed or killed at any moment after the editor
   appeared, **Then** the next start offers the draft with the text intact (the existing draft rule).
4. **Given** a text of up to 1500 characters with non-ASCII letters and line breaks, **When** it is handed
   over, **Then** the body equals the sent text character for character.
5. **Given** the user started PB Office from another app, **When** they leave PB Office (Back or close), **Then**
   they return to the app they came from, as the test on the reader showed for a hand-over to a stand-in app.
6. **Given** the hand-over, **When** the editor is shown, **Then** no network activity and no Wi-Fi switching
   has been caused by the hand-over itself (the existing rules for the start-up sync and for the sync after
   the user's own change apply unchanged).
7. **Given** the other app also handed over a subject and a recipient, **When** the editor appears, **Then** the
   subject and recipient are prefilled and clearly visible, the user can change or remove each, and nothing is
   sent until the user taps Send.
8. **Given** the other app handed over a recipient that is not one plain, valid mail address (a list of
   addresses, a display name with extra text, a malformed address), **When** the editor appears, **Then** the
   recipient field stays empty, the user is told in plain words that the recipient was not used (without
   repeating it), and the text and subject still arrive.

---

### User Story 2 - Hand over text while PB Office is already running (Priority: P1)

PB Office was started earlier and is still open in the background, perhaps showing the inbox or a message the
user was writing. The user is in another app, chooses "Mail this link", and PB Office comes to the front with a
new message holding the text. Nothing the user was doing in PB Office is lost or overwritten.

**Why this priority**: A reader keeps apps open in the background, so this is a normal situation, not a rare
one. It is also the case with a real risk: the system may only bring the running copy forward and drop the
text. If that cannot be made reliable, the feature must say so honestly instead of losing text silently, which
is why it is P1 and is proven on the reader first (the plan carries the proof).

**Independent Test**: On the reader, start PB Office, leave it running in the background, hand over a text from a
test program, and check that PB Office shows a new message with that text. Repeat while PB Office has a
half-written message open, while it shows the inbox, while a sync is running, and while it shows a
dialog. Each time, nothing of the earlier state is lost, and the text is either in a new message or the user
is told that it did not arrive.

**Acceptance Scenarios**:

1. **Given** PB Office is running in the background on its inbox, **When** another app hands over a text,
   **Then** PB Office comes to the front with a new message whose body is exactly that text.
2. **Given** PB Office is running with an unsent message open in the editor, **When** a text is handed over,
   **Then** the open message is saved as a draft first, unchanged, and the handed-over text opens in a separate
   new message; neither is merged into the other, and the user can reach the earlier draft again.
3. **Given** PB Office is busy (a sync is running, or a dialog is open), **When** a text is handed over,
   **Then** the sync is not interrupted, the dialog is not answered on the user's behalf, and the new message
   is shown as soon as the user can act on it.
4. **Given** the system cannot deliver the text to the running PB Office (it only brought the running copy
   forward and the text did not arrive), **When** that happens, **Then** PB Office falls back by restarting itself
   or exiting cleanly so that the next start receives the text, and the user ends up in a new message with the
   complete text without handing it over again. Nothing the user had open is lost by the restart (an open
   unsent message is saved first, scenario 2).
5. **Given** even the fallback fails, **When** the user looks at PB Office, **Then** no message with a wrong or
   partial text exists, and what the user sees makes clear that nothing was handed over, so that they go back and
   try again; the text is never silently dropped while PB Office pretends nothing happened.
6. **Given** two hand-overs follow each other quickly, **When** both texts arrive, **Then** each becomes its own
   new message; neither replaces the other.

---

### User Story 3 - The handed-over text stays private (Priority: P1)

A share link can be as good as a key to the user's files, and a password may be spelled out next to it. The user
needs to know that the hand-over does not spread the text around: it (and the subject and recipient that may come with it) is not printed on the screen outside the
message, not written to any log, and not left visible to other programs on the reader after PB Office has taken
it over.

**Why this priority**: The constitution forbids secrets in logs, and the text is exactly the kind of data this
rule is meant for. A hand-over that works but leaks is worse than none.

**Independent Test**: After handing over a text, a subject and a recipient that each contain a unique marker word, search everything the reader
keeps after the hand-over for those markers: the log of PB Office, the log of the launcher, the list of running
programs, and any files in the application's folders other than the draft itself. The markers are found only in
the draft (and in a server copy of the draft once synced, which is the user's own mail).

**Acceptance Scenarios**:

1. **Given** a text with a unique marker was handed over, **When** the logs of PB Office and of its launcher are
   read afterwards, **Then** they do not contain the text, the subject or the recipient, or any part of them, only
   that a hand-over happened, how long the text was in characters and which fields were present (no more).
2. **Given** PB Office has taken a text over and is running, **When** the list of running programs is read,
   **Then** the text is not part of it: it does not remain as a long-lived start parameter of PB Office or of
   the launcher.
3. **Given** a hand-over succeeded, **When** the temporary place through which the text travelled is looked at,
   **Then** nothing of the text is left there after the message is open, including after a failed or abandoned
   hand-over.
4. **Given** an error occurs during a hand-over, **When** PB Office shows or logs the error, **Then** the text
   is not part of the message shown to the user, of the log, or of any exported diagnostic.
5. **Given** the text contains something that looks like a command, a path, a web address or markup, **When**
   it is handed over, **Then** it is treated only as characters of the body: nothing is run, opened, fetched or
   interpreted, and web addresses are shown but not opened automatically (the existing rule for links).

---

### User Story 4 - Odd, large or unwanted hand-overs are handled safely (Priority: P2)

Not every start of PB Office comes from a friendly app: the text, subject or recipient may be empty, very long,
broken, or meant for a situation PB Office cannot serve (no account yet). PB Office never crashes on it, never shortens text without
saying so, and never creates a message the user did not ask for.

**Why this priority**: These cases are rare, but they decide whether the feature can be trusted; the happy
paths (stories 1 to 3) come first.

**Independent Test**: Hand over an empty text, a text of 1500, 5000 and 100000 characters, a text with control
characters and invalid UTF-8, a subject with a line break, a recipient that is a list or carries a hidden
header, and a text while no account is set up; check each result against the scenarios.

**Acceptance Scenarios**:

1. **Given** a hand-over in which text, subject and recipient are all empty (or only white space), **When** PB
   Office starts, **Then** it starts normally (home screen) and creates no message.
   **Given** only a recipient or only a subject was handed over, **Then** a message is created with just that
   content and an empty body.
2. **Given** a text longer than the size PB Office accepts, **When** it is handed over, **Then** either the
   whole text arrives or the user is told that the text was too long and nothing was created; the text is never
   cut short without the user being told.
3. **Given** a text with control characters or bytes that are not valid text, **When** it is handed over,
   **Then** PB Office does not crash, shows what is readable, replaces what is not with a visible replacement
   character, and does not let the control characters change the layout or run anything.
4. **Given** no account is set up, **When** a text is handed over, **Then** PB Office offers the account set-up
   first and, when it is done, opens the new message with the text; if the user cancels the set-up, they are
   told that the text was not kept and can hand it over again from the other app.
5. **Given** PB Office is started in the normal way (the user taps its icon, no text), **When** it starts,
   **Then** it behaves exactly as before this feature.
6. **Given** a hand-over to a PB Office that is locked or in a state where it cannot show an editor (for
   example while the first sync of a new account is running), **When** the state ends, **Then** the new
   message is shown; the text is not lost in between.

---

### Edge Cases

- A handed-over subject or recipient carries a line break and a second header (for example a hidden blind copy):
  the line break is replaced or the recipient is refused (FR-013, FR-021, FR-022); no header is added.
- A handed-over recipient is an address the user has never written to: it is shown in the editor like any other,
  and the user decides by tapping Send; there is no warning beyond what the editor already shows.
- The same text is handed over twice (the user tapped twice): two messages are created, each a draft; the
  user can delete one. PB Office does not guess that it is a duplicate.
- The calling app is closed by the system while PB Office is in the foreground (the reader needs the memory):
  the new message is still complete; on leaving PB Office the user lands wherever the system puts them.
- The hand-over happens while the reader is nearly out of battery or storage: the existing draft rules for low
  storage apply; the user is told if the draft cannot be saved, and the text is not shown as safe when it is not.
- The text is a share link with a password on the next line: both lines arrive unchanged; PB Office does not
  split, hide or reformat them.
- The user opens the new message and leaves without typing anything: it remains an ordinary draft that the user
  can delete (with the usual confirmation), no automatic deletion.
- The text contains right-to-left characters, emoji, or characters the reader's font cannot draw: they arrive
  unchanged in the draft; what the font cannot draw is shown as a replacement shape only on screen.
- The hand-over text is longer than one screen: the editor pages it like any long body.
- Two apps hand over at the same moment: each text ends up in its own message or is refused with a message;
  texts are never mixed.

## Requirements *(mandatory)*

### Functional Requirements

**Receiving**

- **FR-001**: PB Office MUST accept a text handed over by another app when it is started, and open a new message
  whose body is exactly that text, with the subject and the recipient prefilled when the other app handed them over
  (FR-021) and empty otherwise, ready for the user to complete.
- **FR-002**: The body MUST equal the handed-over text character for character, including non-ASCII letters,
  symbols and line breaks, for texts of at least 1500 characters (the size shown to work in the test of 2026-10-23).
- **FR-003**: A message created by a hand-over MUST be saved as a draft from the moment the editor appears,
  under the same rules as any new message (automatic saving, mirrored to the server's Drafts folder at the next
  sync, no loss of more than the last 2 seconds of typing).
- **FR-004**: A hand-over MUST NOT send anything and MUST NOT cause network activity or Wi-Fi switching by itself;
  sending stays a separate, explicit act of the user, however much the other app handed over. A handed-over
  recipient is only a prefilled value in the editor, never a confirmation to send.
- **FR-005**: The hand-over MUST work whether or not PB Office is already running. When the system cannot deliver
  the text to a running PB Office, PB Office MUST fall back by restarting itself or exiting cleanly so that the next
  start receives the text, without losing anything the user had open (FR-006). If that fallback also fails, the user
  MUST be able to tell that nothing was handed over (see FR-009).
- **FR-006**: A hand-over to a running PB Office MUST NOT change, close, send or discard anything the user was
  doing there; an open, unsent message is saved first and stays reachable as its own draft.
- **FR-007**: Each hand-over MUST create its own message. Two texts MUST NOT be merged, and one MUST NOT replace
  another.
- **FR-008**: When no account is set up, the hand-over MUST lead to the account set-up and then to the new message;
  if the set-up is cancelled, the user MUST be told that the text was not kept.
- **FR-009**: A hand-over whose text could not be received completely MUST NOT produce a message with wrong or
  partial text. The user MUST be told that the text did not arrive, in plain words, without the text itself.
- **FR-010**: When PB Office is started without a text, its behavior MUST be exactly the same as before this feature.

**Safety of the text**

- **FR-011**: The handed-over text, subject and recipient MUST be treated as untrusted plain text: they MUST NOT be
  run, interpreted, opened, fetched, or given any effect other than being the characters of the body, the subject
  and the recipient field. Web addresses in it follow the
  existing rule: shown, never opened or loaded automatically.
- **FR-012**: The size accepted MUST be bounded. A text over the bound MUST be refused completely with a message
  to the user, never cut short silently. The bound MUST be at least 1500 characters and is fixed in the plan
  together with the measured limit of the system.
- **FR-013**: Control characters and invalid text MUST be handled without crashing: PB Office keeps the readable
  part, shows a visible replacement for the rest, and never lets them change the screen or the message headers. A line break in the subject or recipient MUST NOT be
  able to add or change a header (no hidden second recipient, no hidden blind copy).
- **FR-014**: A hand-over in which text, subject and recipient are all empty or hold only white space MUST create no
  message. If at least one of them has content, the message is created with that content.

**Privacy**

- **FR-015**: The handed-over text, subject and recipient MUST NOT be written to any log (PB Office, its launcher, or the system log lines
  PB Office produces), error message, diagnostic or exported file. A log MAY say that a hand-over happened, how long the text was in characters, and which of the three fields were
  present.
- **FR-016**: The handed-over text, subject and recipient MUST NOT remain visible to other programs after PB Office has taken it over: it
  MUST NOT remain as the start parameter of a long-running program, and any temporary place that carried it MUST be
  emptied as soon as the message exists, and also when the hand-over fails or is abandoned.
- **FR-017**: The launcher and any helper that passes the handed-over values on MUST NOT print them. A hand-over that cannot meet
  FR-015 and FR-016 MUST NOT be released, however well it otherwise works.
- **FR-018**: The hand-over MUST NOT widen the set of things PB Office does without the user: no new network
  connection, no background activity, no new permission on the reader.

**Subject and recipient**

- **FR-021**: Besides the text, a hand-over MAY carry one subject and one recipient, both optional. The subject is one
  line (line breaks are replaced by spaces) and bounded to at least 200 characters; over the bound it is refused
  with a message to the user, not cut short silently, while the text still arrives. The recipient is exactly one
  plain mail address, with no list, no display name, no copy or blind copy; the plan fixes the exact rules and the
  limit of the system.
- **FR-022**: A recipient that is not one valid plain address MUST NOT be prefilled, and the user MUST be told that it
  was not used (without repeating it); the text and the subject are unaffected. A prefilled subject and recipient
  MUST be editable and removable like anything the user typed.

**Checks and honesty**

- **FR-019**: Every behavior that depends on the reader's system (start of PB Office from another app, the running
  copy being brought forward, the text arriving, the leaving of PB Office returning to the caller, the text not
  appearing in the list of running programs or in logs) MUST be labelled "unverified" in the plan, tasks and tests
  until it was observed on the InkPad One, and the observation MUST be recorded with its date.
- **FR-020**: The behavior MUST be testable on a computer without the reader for everything that does not depend on the
  system: building the message from a text, the size bound, the handling of odd characters, "one hand-over, one
  message", and "the text is not in the log", using a test program that hands over text in the same way as the
  other app.

### Key Entities

- **Hand-over**: One act of another app starting PB Office with a text. Attributes: the text (untrusted, bounded,
  plain), an optional subject (one line, bounded), an optional recipient (one plain address), the length of the
  text in characters, the time it arrived. It lives only until the message exists, and then only as that message.
  It carries no attachment, copy or blind-copy recipient, folder, action or sender identity.
- **Hand-over message**: A new message (draft) whose body is the text of one hand-over, with the subject and
  recipient of that hand-over when it had valid ones. It is an ordinary draft in
  every respect (editable, deletable with confirmation, mirrored, sendable) and has no link back to the hand-over.
- **Calling app**: The app that started PB Office; any app may do so. It is not known to PB Office, and it is not changed by it; it stays
  open in the background and is where the user returns to.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: From tapping "Mail this link" in the other app to a new message with the text ready to edit takes under
  10 seconds when PB Office was not running, and under 3 seconds when it was already running (device figures are
  targets for the InkPad One and stay "unverified" until measured there).
- **SC-002**: In 20 consecutive hand-overs on the reader (10 with PB Office closed, 10 with it running in different
  states, including any that need the fallback of FR-005), 20 of 20 either show the complete text in a new message or tell the user that nothing arrived; 0 messages
  hold wrong or partial text and 0 hand-overs vanish without a sign.
- **SC-003**: For texts of 1, 100 and 1500 characters, with and without non-ASCII letters and line breaks, the body
  matches the sent text in 100 percent of the cases.
- **SC-004**: After 20 hand-overs of texts with a unique marker, a search of every log and of the list of running
  programs finds the marker 0 times.
- **SC-005**: A message that was open and unsent when a hand-over arrived is found complete in the draft list in 100
  percent of the cases, and the earlier state of PB Office is the same as before (same folder, same sync state).
- **SC-006**: A user who has never used the feature can mail a link from the other app, including completing the
  recipient and subject that the other app did not supply, in under 2 minutes on the reader.
- **SC-007**: Starting PB Office in the normal way takes no longer than before this feature (within the existing
  start-up budget of 5 seconds).

## Assumptions

- The sending app starts PB Office through the system's own way of starting one application from another, passing
  the text as a parameter; the test of 2026-10-23 showed that this delivers the text unchanged for up to 1500 bytes
  and that the calling app keeps running in the background. The plan decides the exact mechanism and whether the
  parameter itself is used or only a signal that a text is waiting (FR-016 requires that the text not remain visible
  in the list of running programs).
- Any app may hand text to PB Office, not only PB Cloud (confirmed by the owner, 2026-10-23); PB Office treats the
  text as plain, untrusted text, does not care who sent it, and does not try to identify the caller. A hand-over
  can only create a draft; sending stays the user's act (FR-004).
- The other app may hand over the text (body), an optional subject and one optional recipient, all prefilled and
  editable (confirmed by the owner, 2026-10-23). A handed-over recipient is more trusted by nobody: the user sees
  it in the editor and must tap Send, so a link cannot go to an address that the user never saw.
- PB Office has one mail account (spec 001); a hand-over opens a message for that account. A choice among several
  accounts is out of scope.
- The text is plain; formatting, pictures and attachments cannot be handed over. A hand-over cannot name copy or
  blind-copy recipients, several recipients, a folder, an account or an action; those are the user's choices in
  PB Office.
- "Mail this link" and any other button in the other app are not part of this feature. A user-facing way to try the
  feature without another app is the test program of the plan, not a screen in PB Office.
- The existing rules stay as they are: the start-up sync runs only when Wi-Fi is already connected; a sync after the
  user's own change switches Wi-Fi on; no polling; drafts are mirrored to the server; link texts are shown but not
  opened. A hand-over message is a draft like any other and takes part in these rules only through the user's own
  actions on it.
- The interface is bilingual (English source text, German translation) like the rest of the app; the messages of this
  feature follow that.
- Version and CHANGELOG are raised by this feature as the last task (a feature adds a MINOR step).

### Observations on the reader that this feature builds on

Source: the sibling project `nextcloud-pocketbook`, `docs/device-findings.md`, section "Starting one app from
another with text" (observed 2026-10-23, InkPad One, with a stand-in target program that only recorded what it
received; the target was **not PB Office**).

- One app can start another with text as a parameter; the text arrives as one parameter, complete and unchanged
  (spaces, quotes, non-ASCII letters, euro sign, line breaks), tested up to 1500 bytes.
- The call returns at once, the target starts within about a second, and the calling app keeps running in the
  background and is brought back after the target ended.
- The system's own log lines about the call name only the application's path, not the text.

### Assumptions that need the device to verify

All of these are unverified until exercised on the InkPad One with PB Office itself as the target.

- That a hand-over to a **running** PB Office delivers the text, rather than only bringing the running copy forward
  (the system looks the application up by name, so a second start may be turned into a switch to the running copy).
  If it does not, the fallback of FR-005 applies, and the plan must prove on the reader that the fallback works
  (the restart or clean exit, the text arriving at the next start, and the text not being left behind, FR-016).
- That the launcher passes the text on without printing or logging it, and that the text is not left as a
  long-lived command-line parameter of PB Office or the launcher.
- That the system keeps the calling app available for the return after PB Office was already running before the
  hand-over.
- That texts above 1500 bytes arrive complete, or are refused, up to the bound fixed in the plan.
- That the cold start of PB Office with a hand-over stays within the figures of SC-001 and SC-007.
