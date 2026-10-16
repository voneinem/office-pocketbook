# office-pocketbook (PB Office)

Offline-first **email and calendar** app for the PocketBook InkPad One (e-ink, 32-bit ARM Linux).
Sibling of [notes-pocketbook](https://github.com/voneinem/notes-pocketbook) (MIT, same author).

Status: **in development**. The specification, plan and task list are in
[`specs/001-email-calendar-mvp/`](specs/001-email-calendar-mvp/); the project rules are in
[`.specify/memory/constitution.md`](.specify/memory/constitution.md). Nothing in this repository has been
verified on a real device or against real mail and calendar servers yet; the open verification work is
listed in `specs/001-email-calendar-mvp/research.md` ("Open items to verify").

## What the app does (state of the code)

- **Mail**: one IMAP/SMTP account (TLS only, no plain-text fallback; pictures that are parts of a mail are shown, pictures on the sender's server only after a tap on that one mail, which says that the sender is told). Reads offline, searches offline, writes, replies, forwards; drafts are kept on the reader and mirrored to the Drafts folder; sending uses an outbox that never resends a message of unknown outcome by itself. Mark read/unread, star, move and delete (to the Trash, permanent only after a second question). Bodies and attachment lists are fetched on sync; attachments are downloaded on request into `Downloads`.
- **Calendar**: a CalDAV account (Nextcloud-style servers) and/or a read-only `.ics` feed. Agenda, day and month views in the user's own time zone (bundled tz data), events can be created, changed and deleted; repeating events are read only. A change made on both sides keeps both versions until the user decides.
- **Syncing**: a sync at the start of the app (only when Wi-Fi is already connected), the Sync button, and an automatic sync a few seconds after the user's own changes (it switches Wi-Fi on). Everything that waits for the server is marked "not synced".
- **Start page and settings**: unread mail and today/tomorrow at a glance; settings for Auto Sync, days of mail kept, attachment limit, space for mail, the special folders, the accounts and the time zone; warnings for a wrong reader clock and for full storage.

The interface is English with a German translation (Settings > Language; texts in `qml/strings_de.tsv`).

All of this has been tested on the host against fake IMAP/SMTP/CalDAV servers (the tests say so: "UNVERIFIED against real servers") and parts of it have been tried on one InkPad One with the owner's own mail and calendar accounts. Provider behaviour beyond that, the battery-pull recovery on the device and the performance budgets on the device are still to be measured (`specs/001-email-calendar-mvp/tasks.md`, items marked LIVE and DEVICE).

## Credentials and privacy

Passwords are stored **as plain text** in a file in the app's data folder (`system/pboffice/credentials`), with the strictest permissions the reader's storage allows. The reader's storage is FAT, which probably cannot enforce them: anyone with USB access to the reader can read the file. The app measures the protection after saving and says so on the account page and in About; use app-specific passwords. Logs hold counts and codes only; no address, subject or file name. The app opens no link; it loads no remote content unless you tap "Load pictures" on one mail (one plain GET per picture over https, without cookies or referrer; the sender learns that the mail was read); and it connects only to the servers you configure (TLS verification is never switched off). The details and the tests behind each claim are in [`docs/reference/security-review.md`](docs/reference/security-review.md).

## Checklists

Two checklists keep the verification honest: [`docs/device-test-checklist.md`](docs/device-test-checklist.md) (the InkPad One) and [`docs/live-server-checklist.md`](docs/live-server-checklist.md) (real providers). Every item stays "unverified" until a date and a result are written next to it.

## Layout

- `core/` - C++17, Qt-free and InkView-free: transports, parsers, storage, sync (builds and tests on the host)
- `bridge/` - the Qt layer exposing the core to QML
- `qml/` - the Qt 6 / Qt Quick UI (e-ink widgets, pages)
- `app/`, `platform/pocketbook/` - InkView glue and the launcher script
- `cmake/`, `scripts/` - host build, ARM cross-build with the PocketBook SDK (`POCKETBOOK_SDK`), pre-merge gate, packaging
- `third_party/` (SQLite, Mozilla CA bundle, Noto Emoji), `licenses/`, `NOTICE`
- `tests/` - unit, contract and integration tests, fixtures, support code
- `docs/reference/` - SDK and InkFeed analyses, the notes-pocketbook constitution and security review

## Build (host)

```bash
cmake -S . -B build-host && cmake --build build-host -j && ctest --test-dir build-host --output-on-failure
```

Device build: see `INSTALL.md` and `cmake/sdk.cmake` (`-DTARGET_TYPE=ARM`, SDK via `$POCKETBOOK_SDK`). The app carries its own libcurl 8.18.0 (the reader's is too old to read IMAP answers): build it once with `scripts/build-curl-arm.sh`, then configure the ARM build.
The full local gate before a merge is `scripts/pre-merge.sh`.

## License

MIT, see `LICENSE`; third-party components in `NOTICE` (the list is also in the app under Settings > About, with every license text, offline). The code that this project started from, notes-pocketbook and parts of InkFeed, is MIT as well; see `NOTICE` for the exact notices.
