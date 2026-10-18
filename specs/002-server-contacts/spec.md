# Feature Specification: Server Contacts

**Feature Branch**: `feature/002-server-contacts`

**Created**: 2026-10-18

**Status**: Draft

**Input**: User description: "Support for contacts from server. New view with list of contacts. Option to create, delete and edit."

## Overview

The user's contacts live on their cloud server next to their calendar. This feature brings them to the
reader: a new Contacts view lists them page by page, shows the details of one contact, and lets the user
create, change and delete contacts, offline if necessary. Like events, contact changes made on the reader
are sent to the server at the next sync, and nothing the user typed is ever lost or silently overwritten.
It extends the first release (spec 001), which left an address book out of scope.

## Clarifications

### Session 2026-10-18

- Q: When typing a recipient in a new mail, should the app also suggest addresses from server contacts? → A: Yes: suggest from contacts and cached mail together, contacts first.

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Browse and read my contacts (Priority: P1)

The user syncs, opens the new Contacts view from the home screen, scans an alphabetical list of their
contacts page by page, finds one by typing part of a name, and opens it to see its details: phone numbers,
email addresses, postal address, organization and notes. This works with no connection once synced.

**Why this priority**: Reading the address book on the reader is the base of the feature; create, edit and
delete all start from the list and the detail view.

**Independent Test**: With a test contact server holding anonymized contacts, set up the account, sync,
switch Wi-Fi off, browse the list, search for a name and open several contacts. This alone delivers a
usable offline address book.

**Acceptance Scenarios**:

1. **Given** a server account that offers contacts, **When** the user syncs, **Then** its address books are
   listed and their contacts are available in the Contacts view, sorted by name, with their address book
   marked when more than one is shown.
2. **Given** a synced address book and no connection, **When** the user opens the Contacts view, **Then** the
   list is shown page by page, with large touch targets and page buttons instead of kinetic scrolling.
3. **Given** the list, **When** the user types part of a name, organization, phone number or email address,
   **Then** only matching contacts are listed, offline.
4. **Given** a contact, **When** the user opens it, **Then** all its stored details are shown, with several
   phone numbers, email addresses or postal addresses each labelled (for example home, work, mobile).
5. **Given** a contact has a detail the app does not display or edit, **When** the contact is opened, **Then**
   the app does not fail, and the detail is kept unchanged if the contact is later edited and synced.
6. **Given** a contact with no name but an organization or an email address, **When** the list is shown,
   **Then** it appears under that organization or address instead of being hidden.

---

### User Story 2 - Create a contact (Priority: P2)

The user adds a new person: a name, phone numbers, email addresses, a postal address, an organization and a
note, in the address book of their choice. The contact appears at once and is sent to the server at the next
sync.

**Why this priority**: Adding contacts on the go is the first write action; it needs the list (story 1).

**Independent Test**: Create a contact offline, restart the app, find it intact and marked "not synced", sync,
and verify that the server now holds it and the mark is gone.

**Acceptance Scenarios**:

1. **Given** a writable address book, **When** the user creates a contact with at least a name, an
   organization, a phone number or an email address, **Then** it appears in the list immediately and is
   marked as not yet synced.
2. **Given** the user has typed in the form, **When** the app is closed or killed, **Then** nothing of the saved
   contact is lost; an unsaved form is not required to survive.
3. **Given** a form with nothing filled in, **When** the user tries to save, **Then** the app explains that a
   contact needs at least a name, an organization, a phone number or an email address, and saves nothing.
4. **Given** an email address in the wrong form (for example without an "@"), **When** the user saves, **Then**
   the app points to that field and does not save until it is corrected or removed.
5. **Given** the server confirmed the new contact, **When** the next sync completes, **Then** the not-synced
   mark disappears.
6. **Given** there is more than one writable address book, **When** the user starts a new contact, **Then**
   the user can choose which one receives it, with the previously used one preselected.

---

### User Story 3 - Edit a contact (Priority: P2)

The user changes a contact's details (for example a new phone number or a corrected spelling). The change
shows at once and is sent at the next sync. If the same contact was changed on the server meanwhile, neither
version is lost.

**Why this priority**: Keeping contacts right is the main reason to write them; it needs the detail view.

**Independent Test**: Edit a contact offline, sync, and verify the server shows the change; edit the same
contact on the server in between, sync, and verify that both versions are kept.

**Acceptance Scenarios**:

1. **Given** a contact in a writable address book, **When** the user changes, adds or removes details and
   saves, **Then** the list and the detail view show the new state at once and the contact is marked as not
   yet synced.
2. **Given** a contact with details the app does not edit, **When** the user edits and syncs it, **Then**
   those details are still on the server afterwards.
3. **Given** the same contact was changed on the reader and on the server, **When** the next sync runs,
   **Then** neither change is lost: both versions are kept, the user is told, and can choose which to keep
   or edit them into one.
4. **Given** a contact was removed on the server while it had unsynced changes on the reader, **When** the
   next sync runs, **Then** the edited version is kept and marked "deleted on the server", and the user
   chooses to re-create it on the server or discard it (discarding asks for confirmation).
5. **Given** a contact in a read-only address book, **When** the user tries to edit it, **Then** the app
   explains that this address book is read-only.

---

### User Story 4 - Delete a contact (Priority: P2)

The user deletes a contact after a confirmation. It disappears from the list at once, and the deletion
reaches the server at the next sync.

**Why this priority**: Removing outdated contacts completes the address-book basics.

**Independent Test**: Delete a contact offline and confirm, verify it is gone from the list, sync, and verify
the server no longer holds it; try the same for a contact changed on the server meanwhile.

**Acceptance Scenarios**:

1. **Given** a contact in a writable address book, **When** the user chooses delete, **Then** the app asks
   for confirmation naming the contact; **When** the user confirms, **Then** it disappears from the list and
   the deletion is sent at the next sync.
2. **Given** the user cancels the confirmation, **When** the dialog closes, **Then** nothing changed.
3. **Given** a deletion waiting for the next sync, **When** the user looks at the Contacts view, **Then** the
   deletion is visible as waiting (for example in a "not synced" list or count) and can still be undone until
   the server has confirmed it.
4. **Given** the contact was changed on the server after the reader last synced it, **When** the next sync
   runs, **Then** the contact is not deleted on the server: it is kept as a conflict and the user decides.
5. **Given** a contact in a read-only address book, **When** the user tries to delete it, **Then** the app
   explains that this address book is read-only.

---

### User Story 5 - Contacts in sync, home and settings (Priority: P3)

Contacts follow the same rules as mail and events: they sync on app start, on request and shortly after a
contact change when Auto Sync is on, the reader always shows whether changes are waiting, and Settings let
the user choose which address books are shown.

**Why this priority**: Keeps the new view consistent with the rest of the app; the stories above work with
the sync that already exists.

**Independent Test**: Make contact changes offline, check the waiting marker, enable Wi-Fi, observe that the
change syncs a few seconds later, and verify that an idle app makes no connection.

**Acceptance Scenarios**:

1. **Given** unsynced contact changes, **When** the user looks at the Contacts view or the home screen,
   **Then** the number of waiting changes and the time of the last successful sync are visible.
2. **Given** Auto Sync is on and the user has saved or deleted a contact, **When** a few seconds have passed,
   **Then** a sync starts, switching Wi-Fi on if needed, exactly as after a changed event.
3. **Given** several address books, **When** the user opens Settings, **Then** each can be shown or hidden in
   the Contacts view; hiding one never deletes anything.
4. **Given** an account whose server offers no contacts, **When** the user opens the Contacts view, **Then**
   it explains that no address book was found on the server instead of showing an empty, unexplained page.
5. **Given** a failed contact sync, **When** the user looks at the sync status, **Then** a specific reason is
   shown and all local contacts and changes are intact.

---

### User Story 6 - Pick recipients from my contacts (Priority: P3)

While writing a mail, the user starts typing a recipient and gets suggestions from their contacts together
with the addresses already suggested from cached mail, contacts first.

**Why this priority**: It makes the address book useful for the core mail workflow, but it needs the synced
contacts of story 1 and the existing composer.

**Independent Test**: Sync test contacts, go offline, start a new mail, type part of a contact's name or
address, and verify that the matching addresses are suggested, contacts before cached-mail addresses.

**Acceptance Scenarios**:

1. **Given** synced contacts, **When** the user types part of a name, organization or email address in a
   recipient field, **Then** matching contact email addresses are suggested, offline, ahead of the
   addresses taken from cached mail.
2. **Given** a contact with several email addresses, **When** it matches, **Then** each address is suggested
   with the contact's name and its label, so the user picks the right one.
3. **Given** an address is both in a contact and in cached mail, **When** it is suggested, **Then** it
   appears once, as the contact.
4. **Given** a contact in a hidden address book, or a contact with a pending deletion, **When** the user types
   a recipient, **Then** its addresses are not suggested.
5. **Given** the user picks a suggestion, **When** the recipient is added, **Then** the composer behaves as it
   does for any other suggested address (nothing is sent or changed by picking it).

---

### Edge Cases

- **A very large address book** (thousands of contacts): the list stays browsable page by page and searching
  stays fast; contact photos are never stored or shown.
- **Malformed or hostile contact data from the server** (broken text, huge fields, missing name, strange
  character sets): the contact is shown as far as readable or as unreadable; the sync continues; nothing
  executes.
- **Duplicate contacts** (the same person twice): both are shown; the app does not merge or deduplicate.
- **A contact the server rejects** on save (for example a size limit or invalid content): the local version is
  kept, marked failed with the reason, and the user can edit, retry or discard it.
- **Interrupted sync** at any step: repeating it never duplicates a contact and never loses a pending change.
- **A contact edited twice offline**: the server sees one change that carries the latest state.
- **A contact created offline and deleted offline before any sync**: it never reaches the server.
- **Very long fields or many entries** in one contact: the detail view shows them page by page without
  cutting data off silently; an entry already longer than the app's input limits stays unchanged when the contact is
  edited (it is shown but not changeable, and can be removed).
- **Names with non-Latin characters, umlauts or emoji**: sorted, searched and shown without corruption.
- **Shrinking local storage or the sync window** never removes contacts with unsynced changes.

## Requirements *(mandatory)*

### Functional Requirements

**Account and synchronization**

- **FR-001**: The app MUST find the address books of the configured server account, list them, and let the
  user choose which are shown; a read-only address book MUST be marked as such.
- **FR-002**: The app MUST download the contacts of the shown address books at sync and keep them on the
  device so that the Contacts view works with no connection; contact photos and other embedded binary data MUST
  NOT be stored or shown, and the app MUST ask the server to leave them out; they stay on the server unchanged
  when a contact is edited (FR-015).
- **FR-003**: Contact sync MUST run at the same moments as the existing sync (app start with a connection,
  user request, shortly after a contact change when Auto Sync is on) and MUST NOT use the network in the
  background otherwise.
- **FR-004**: Contact sync MUST be safe to interrupt at any moment and resumable and idempotent: repeating an
  interrupted sync gives the same result as an uninterrupted one, with no duplicate and no lost contact.
- **FR-005**: Contact sync problems MUST be reported with a specific, understandable reason and MUST leave
  all local contacts and pending changes intact.
- **FR-006**: Contact data MUST be exchanged only with the server the user configured, over a verified
  encrypted connection, with the same certificate rules as mail and calendar; the credentials MUST NOT be
  sent to a different host.

**Contacts view**

- **FR-007**: The app MUST provide a Contacts view, reachable from the home screen, that lists contacts
  sorted by name, page by page, with large touch targets and no animation or kinetic scrolling.
- **FR-008**: Each list entry MUST show the contact's display name (or, if none, its organization, email
  address or phone number) and MUST mark contacts with changes not yet confirmed by the server.
- **FR-009**: The user MUST be able to search contacts offline by name, organization, phone number and email
  address, with the result shown in the same list.
- **FR-010**: The contact detail view MUST show the name, organization, all phone numbers, email addresses and
  postal addresses with their labels, and the note, and MUST show details too long for one page page by page.
- **FR-011**: The detail view MUST NOT open any link or start any call, email or navigation by itself; email
  addresses and phone numbers are shown as text.

**Create, edit, delete**

- **FR-012**: Users MUST be able to create a contact in a writable address book with: first and last name,
  organization, any number of labelled phone numbers, email addresses and postal addresses, and a note.
- **FR-013**: A contact MUST have at least a name, an organization, a phone number or an email address, and
  every email address MUST be in a valid form; otherwise the app MUST refuse to save and say which field to
  correct.
- **FR-014**: Users MUST be able to change and remove details of an existing contact in a writable address
  book.
- **FR-015**: Details the app does not display or edit MUST be preserved unchanged when a contact is edited and
  synced.
- **FR-016**: Users MUST be able to delete a contact in a writable address book only after an explicit
  confirmation that names the contact; cancelling MUST change nothing.
- **FR-017**: Read-only address books MUST refuse create, edit and delete with an explanation.
- **FR-018**: Created, changed and deleted contacts MUST be stored on the device before any network operation
  and applied to the server at the next sync; the Contacts view MUST show them at once.
- **FR-019**: A deletion waiting to be synced MUST stay visible as waiting and MUST be undoable until the
  server has confirmed it.

**Conflicts and data safety**

- **FR-020**: Contact writes and deletes MUST be conditional on the version of the contact last seen from the
  server; a changed contact on the server MUST NOT be overwritten or deleted by an older local version.
- **FR-021**: If a contact was changed both on the reader and on the server, the app MUST keep both versions,
  tell the user, and let the user choose which to keep or edit them into one; neither version may be
  discarded automatically.
- **FR-022**: If a contact with unsynced changes was removed on the server, or its whole address book was
  removed, the app MUST keep the edited version, mark it "deleted on the server", and let the user re-create
  it on the server or discard it after confirmation.
- **FR-023**: Contacts with unsynced changes, and pending deletions, MUST NEVER be deleted, evicted or
  overwritten before the server has confirmed them, including when storage is reduced.
- **FR-024**: A contact the server rejects MUST be kept locally, marked failed with the reason, and be
  retryable, editable or discardable by the user.
- **FR-025**: Hiding an address book in Settings MUST NOT delete any contact on the device or on the server.

**Safety, privacy and e-ink**

- **FR-026**: Malformed or hostile contact data MUST NOT crash the app or hang the sync; unreadable contacts
  are shown as unreadable, and field sizes are limited.
- **FR-027**: Diagnostic information MUST NOT contain names, phone numbers, email addresses, postal addresses,
  notes, address-book names, server addresses or file names.
- **FR-028**: The Contacts view MUST be high-contrast black and white, work with the on-screen keyboard, and
  always show the offline or unsynced state and the time of the last sync.

**Recipient suggestions**

- **FR-029**: While a recipient is typed in the mail composer, the app MUST suggest matching email addresses
  from the contacts together with the addresses already suggested from cached mail, contacts first, offline,
  each with the contact's name and the address label; an address present in both sources MUST appear once, as
  the contact.
- **FR-030**: Suggestions MUST NOT include addresses of contacts in hidden address books or with a pending
  deletion, and MUST NOT change or send anything by themselves.

### Out of Scope

Contact photos; contact groups and categories; merging or deduplicating contacts; importing or exporting
contact files; birthdays, anniversaries or reminders; calling, emailing or navigating from a contact;
sending mail straight from the Contacts view (recipient suggestions in the composer, FR-029, are the only link
between contacts and mail); sharing address books; contacts from sources other than the configured server (for example the
reader's own storage or file import); more than one server account.

### Key Entities

- **Address Book**: a collection of contacts on the server, with its name, whether it is writable, and
  whether it is shown.
- **Contact**: a person or organization with display name, name parts, organization, labelled phone numbers,
  email addresses and postal addresses, a note, the address book it belongs to, details the app does not edit
  (kept as they are), and a sync state (synced, pending creation, pending change, pending deletion, failed,
  conflict, deleted on the server).
- **Contact Conflict**: two versions of the same contact kept side by side until the user decides, or an
  edited contact that was removed on the server kept until the user re-creates or discards it.
- **Sync State**: when the last successful sync happened, how many contact changes are waiting, and any
  problem found (shared with the existing sync state).

## Success Criteria *(mandatory)*

### Measurable Outcomes

Device figures are targets for the InkPad One and stay "unverified" until measured there.

- **SC-001**: After a completed sync, 100% of the contacts of the shown address books can be listed, searched
  and opened with the device offline.
- **SC-002**: A user can find a contact by typing part of its name and read its phone number in under 20
  seconds, using only the on-screen keyboard.
- **SC-003**: A user can create a contact with a name, a phone number and an email address in under 2
  minutes, offline, using only the on-screen keyboard.
- **SC-004**: An address book of 2,000 contacts can be browsed page by page, each page turn takes under 1
  second, and a search returns results in under 2 seconds.
- **SC-005**: After the app is killed or the battery is removed at a random moment during contact editing or
  sync, repeated 100 times in automated tests, 0 saved contacts, pending changes or pending deletions are
  lost and 0 contacts are duplicated.
- **SC-006**: When the same contact is changed on the reader and on the server, or edited while deleted on
  the server, 100% of such cases keep both versions or the edited version, in automated tests.
- **SC-007**: A contact with details the app does not edit, edited and synced in a test set of at least 20
  varied contacts, keeps 100% of those details on the server.
- **SC-008**: A test collection of at least 20 malformed contact items never crashes or hangs the app, and
  each is shown as far as readable or as unreadable.
- **SC-009**: When idle (app open, no user action, Auto Sync finished), the app makes no network connection
  because of contacts, verified in tests.
- **SC-010**: 100% of contact connection attempts with an expired, mismatching or untrusted certificate are
  refused, and 0 credentials are sent in those attempts.
- **SC-011**: With 2,000 synced contacts and the device offline, typing part of a contact's name in a
  recipient field shows the matching addresses in under 1 second, in 100% of the cases of a test set of at least
  20 names (including umlauts and contacts with several addresses).
- **SC-012**: The Contacts view adds no more than 20 MB of storage for 2,000 contacts without photos and does
  not raise the cold start time of the app above the 5 seconds of the first release.

## Assumptions

- The contacts are on the same server account the user already configured for the calendar (Nextcloud or
  OpenCloud style); no separate account setup is needed, and an account that only has mail or only a calendar
  feed has no contacts. Server addresses and credentials are reused; whether a separate address is needed for
  some servers is left to the plan.
- Contacts are shown for the shown address books only; the first time the Contacts view is used, all address
  books of the account are shown.
- Sorting is by family name, then given name; a contact without a name sorts by its organization, email
  address or phone number.
- "Edit" covers the details listed in FR-012; other details of a contact are kept untouched (FR-015) but not
  shown or changed.
- Contacts have no size window like mail: all contacts of the shown address books are kept on the device; the
  storage budget in SC-012 bounds this.
- Contact synchronization reuses the existing sync triggers, Auto Sync switch, Wi-Fi rules (start-up sync only
  when Wi-Fi is already connected; a sync after the user's own change switches Wi-Fi on) and sync status line.
- The interface is bilingual (English source text, German translation) like the rest of the app.
- Spec 001 listed an address book as out of scope (apart from suggesting addresses from cached mail); this
  feature replaces that entry for server-hosted contacts and extends the recipient suggestion with them.

### Assumptions that need a live server or the device to verify

All of these are unverified until exercised against a real server or on the InkPad One.

- That the target servers (Nextcloud first, OpenCloud second) expose address books for discovery, change
  detection, conditional writes and conflict signalling as assumed, and accept the contact fields the app
  writes.
- That servers keep contact details the app does not edit when they are returned unchanged.
- That an address book of the stated size browses and searches within the stated times on the device.
