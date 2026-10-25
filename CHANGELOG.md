# Changelog

Versions follow MAJOR.MINOR.PATCH. A branch that changes the product raises VERSION and adds an entry here
(`scripts/check-version-bump.sh`). The tag `v<version>` is set after the merge.

## 0.4.1 - 2026-10-25

The release package follows the PB Store package contract (spec 004), so the PB Store can list PB Office. The app is the 0.4.0 app apart from its version string;
nothing of it changed, and the unverified items of 0.4.0 (the hand-over, `docs/device-test-checklist.md` DT-HO-1 ... DT-HO-9) are still unverified.

### What changed in the package
- `manifest.json` at the top of the zip, made at build time from `packaging/manifest.template.json` and the `VERSION` file. The zip holds `manifest.json` and the `applications/` folder and
  nothing else; `INSTALL.md` is no longer inside it (it is in the repository and linked from each release).
- The zip is written by `pbstore-catalog pack` (appstore-pocketbook, **tag v0.1.4 or newer**: older tools reject the `+` in the names of the time zone files), packed twice and compared, then checked with
  `pbstore-catalog check-package` and the size budget. `scripts/package-device.sh` finds the tool through `PBSTORE_CATALOG` or `PATH` and stops with a message when it is missing.
- `scripts/check-release.sh` checks that the tag, `VERSION`, the zip name and the manifest version agree. Tagging and publishing stay a deliberate step (README, "Building from source").
- Unchanged: the zip name `pboffice-inkpad-one-<version>.zip` and every file under `applications/` (622 files; `diff -r` against the 0.4.0 zip of 2026-10-23: no difference).

### Verified on the host (2026-10-25, `pbstore-catalog` 0.1.4)
- `pbstore-catalog check-package` on `pboffice-inkpad-one-0.4.1.zip` (4,660,878 bytes, sha256 `19b52cd00fd191112692cd6e9305fb994626d229ab3c381dab3ba786132e6012`): `package ok`. The same check on the 0.4.0 zip of the old layout reported exactly E-PKG-04 and E-PKG-05.
- Two separate builds gave identical zips; the zip root holds only `manifest.json` and `applications/`; the manifest lists 622 files; `scripts/check-release.sh` passes for `v0.4.1`.
- Without the tool, `scripts/package-device.sh` stops at once, names the tool, the variable and `PATH`, and leaves no zip.

### Not verified
- `min_firmware` in the manifest is `6.11.1437`, the firmware of the owner's reader (confirmed by the owner on 2026-10-25, `docs/device-findings.md`). Nothing is claimed for other firmware.
- The manual install from the new zip on the reader, what the PB Store's installer does with the package on a reader, and the published release have not been tried.

## 0.4.0 - 2026-10-23

Text from other apps (spec 003). Another app on the reader can start PB Office with a text, a subject and one recipient (`docs/handoff-for-app-authors.md`), and PB
Office opens a new message with them. Everything below is tested on the host; **nothing of it has been tried on a reader yet** (`docs/device-test-checklist.md`
DT-HO-1 ... DT-HO-9). A test sender and a stand-in app for the experiments are in the package (`INSTALL.md`, "Hand-over probe").

### What it does
- The text becomes the body of a new draft, the subject and recipient are prefilled and editable. The draft is saved before it is shown, so nothing is lost if the app is
  killed. **Nothing is sent without your tap.** A message you were writing is saved first and stays as its own draft; two hand-overs are two drafts.
- A text that is too long (over 8192 bytes), incomplete or not understood is refused with a short notice, never cut short. A subject that is too long, or a recipient that is
  not one plain address, is left out and you are told; control characters and invalid text are replaced by a visible replacement character; a subject or recipient can add
  no mail header.
- Without a mail account the text waits (in memory only) while the account set-up is open and becomes a draft when the account is saved; if you leave the set-up, you are told
  that it was not kept.

### Privacy
- The text is never written to a log or shown in a notice. The launcher takes it into a file in `system/pboffice/handoff/` and starts itself again without it, so it is not left
  in the command line of the running program; the file is removed once the draft exists, or after 10 minutes. The reader's storage cannot be made private: the text waits
  there for seconds. A new rule of the constitution check (R11) keeps the launcher from printing or logging its arguments.

### Not verified, and a known limit
- **A hand-over to a PB Office that is already running is not handled yet.** What the system does with a second start of a running app is not known (it may only bring the
  running copy to the front, and the text then does not arrive); two experiments on the reader decide how it is built (`specs/003-receive-shared-text/research.md` R-05,
  experiments E1 and E2). Until then a text handed to a running PB Office can be lost, and the sender gets no sign of it. Close PB Office before handing over.
- The limit of 8192 bytes, the return of the sender when the user leaves PB Office, the timing, and that the text is not visible to other programs after the launcher's restart
  are all unverified on the reader.

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
