# Changelog

Versions follow MAJOR.MINOR.PATCH. A branch that changes the product raises VERSION and adds an entry here
(`scripts/check-version-bump.sh`). The tag `v<version>` is set after the merge.

## 0.3.0 - 2026-10-18

Contacts from the calendar server. Everything below is tested on the host against fake servers (CardDAV included);
nothing is verified against a real server or on a reader yet (`docs/live-server-checklist.md` LV-K1 ..., 
`docs/device-test-checklist.md` DT-CT-1 ...).

### Contacts
- The address books of the calendar server account (CardDAV, same address and password). A list in name order with
  search, the full contact on a page of its own, address books can be hidden in Settings (nothing is deleted).
- Create, change and delete contacts offline. Changes are marked "not synced" and sent at the next sync with
  conditional requests; a deletion can be undone until the server confirmed it.
- A contact changed on both sides, or removed on the server after you changed it, keeps both versions and appears
  under "To decide".
- Changing a contact keeps what the app does not show (photo, birthday, links, groups) on the server. Photos are not
  stored on the reader. Entries too long to edit are kept unchanged and can be removed.
- Typing a recipient suggests the addresses of your contacts first, then those from mail.

### Data
- **One-way database upgrade (schema 2).** The first start migrates the database and keeps `pboffice.db.bak` until
  the next successful start; the previous build refuses the new file. See `INSTALL.md`.

## 0.2.0 - 2026-10-16

First version of PB Office, the offline-first email and calendar app for the PocketBook InkPad One, started
from the code of notes-pocketbook 1.10.0 (see `NOTICE`). Everything below is tested on the host against fake
servers; what has been tried on a reader and against real providers is recorded in
`docs/device-test-checklist.md` and `docs/live-server-checklist.md` (all other items are "unverified").

### Mail
- IMAP/SMTP account with TLS only (implicit TLS or enforced STARTTLS), connection test with a cause for every
  failure, provider presets for Gmail and Outlook.com, app-specific passwords recommended.
- Offline reading of a window of days (default 30): headers, bodies as text (HTML becomes text, links are
  shown and never opened, no remote content), attachment list, attachments saved on request into `Downloads`
  under their real name and never overwritten; older mail on demand; character sets of Western and Eastern
  Europe.
- Writing: drafts (autosave every 2 s, mirrored to the server's Drafts folder), reply, reply to all, forward,
  address suggestions, an outbox that never sends a message of unknown outcome again by itself, the Sent copy
  filed exactly once.
- Organizing: read/unread, star, move, delete (to the Trash; permanent only after a second question),
  sorting, search over the mail on the reader, changes made offline are marked "not synced" and sent at the next
  sync; a change the server cannot take goes to "To decide".

- Pictures of HTML mails: the pictures that are parts of the mail are shown in the text (gray, fitted to the page, whole lines); the ones on the
  sender's server wait for a tap on "Load pictures" for that one mail, which says beforehand that the sender is told that the mail was read
  (constitution 1.1.0, FR-009); long link addresses are shortened (a tap shows the whole address, nothing is opened).

### Calendar
- CalDAV account (Nextcloud-style servers) and a read-only calendar feed; agenda, day and month views in the
  user's own time zone with the bundled IANA data; repeating events shown with their rule in words.
- Create, change and delete events (everything the app does not show is kept); a change on both sides keeps
  both versions until the user decides; repeating events are read only.

### Syncing and settings
- Sync at the start (only when Wi-Fi is already on), the Sync button, and Auto Sync a few seconds after the
  user's own change (switches Wi-Fi on); nothing is sent when Auto Sync is off.
- Start page (unread mail, today and tomorrow, sync state); Settings: Auto Sync, days of mail kept (a shorter
  window removes older mail at the next sync, never anything with local work), largest attachment, space for
  mail with eviction of old mail, special folders, accounts, time zone, language (English, German), a note on the
  certificate authority from `custom-ca.pem`.
- Warnings for a wrong clock on the reader (from the calendar server's time and from the time the mail server
  stamps on a message the app appended) and for full storage.
- About: version, every license text offline, time zone data release, how the password file is protected.

### Safety
- Killing the app at random moments (battery pull) during autosave, sending, event editing and a whole sync
  loses and doubles nothing in the host tests (100 kills each).
- TLS verification cannot be switched off; libcurl tracing is never on; logs hold counts and codes only.
  See `docs/reference/security-review.md`.

### Known limits
- Repeating events cannot be changed; the German texts have not been looked at on the reader; the device
  performance budgets and the behaviour of real providers are unverified; copied text cannot be pasted into
  other apps (the reader has no system clipboard).
