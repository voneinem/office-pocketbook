# PB Office

Offline-first **email, calendar and contacts** for the PocketBook InkPad One: an IMAP/SMTP mail client, a CalDAV calendar
and a CardDAV address book built for an e-ink screen, with no background network activity.

![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)
![Version](https://img.shields.io/badge/version-0.3.0-informational.svg)
![Platform: PocketBook InkPad One](https://img.shields.io/badge/platform-PocketBook%20InkPad%20One-lightgrey.svg)
![Status: in development](https://img.shields.io/badge/status-in%20development-orange.svg)

PB Office is a sibling of [notes-pocketbook](https://github.com/voneinem/notes-pocketbook) (MIT, same author). It
is a separate app: it has its own binary, launcher and data folder and never touches PB Notes.

> **Status: in development.** The code is tested on the host against fake IMAP, SMTP and CalDAV servers and has
> been tried on one InkPad One with the author's own accounts. Behavior against other providers, battery-pull
> recovery and the performance budgets on the device are not yet verified. See [Verification](#verification).

## Table of contents

- [Features](#features)
- [Requirements](#requirements)
- [Installation](#installation)
- [Usage](#usage)
- [Credentials and privacy](#credentials-and-privacy)
- [Building from source](#building-from-source)
- [Testing](#testing)
- [Project layout](#project-layout)
- [Verification](#verification)
- [Documentation](#documentation)
- [Contributing](#contributing)
- [License](#license)

## Features

**Mail**
- One IMAP/SMTP account. TLS only, no plain-text fallback, certificate verification is never switched off.
- Reads and searches offline. Writes, replies, replies to all and forwards. Drafts are kept on the reader and
  mirrored to the Drafts folder.
- Sending uses an outbox that never resends a message of unknown outcome by itself.
- Mark read/unread, star, move and delete. Delete goes to the Trash; deleting for good needs a second confirmation.
- Bodies and attachment lists are fetched on sync. Attachments are downloaded on request into `Downloads`.
- Pictures that are parts of a mail are shown. Pictures on the sender's server load only after a tap on that one
  mail, which tells you beforehand that the sender learns the mail was read.

**Calendar**
- A CalDAV account (Nextcloud-style servers) and/or a read-only `.ics` feed.
- Agenda, day and month views in your own time zone (time zone data is bundled).
- Create, change and delete events. Repeating events are read only. A change made on both sides keeps both
  versions until you decide.

**Contacts**
- The address books of your calendar server account (CardDAV; the same address and password, nothing more to set up).
- A list in name order with search, the full contact on a page of its own. Create, change and delete contacts; a change
  made on both sides keeps both versions until you decide. Address books can be hidden in Settings.
- Typing a recipient suggests the addresses of your contacts first.
- Photos are not stored on the reader and are left untouched on the server; properties the app does not show
  (birthday, links, groups) are kept when you change a contact.

**Syncing**
- A sync at app start (only when Wi-Fi is already connected), the Sync button, and an automatic sync a few seconds
  after your own changes (this switches Wi-Fi on).
- Everything that waits for the server is marked "not synced".

**Start page and settings**
- Unread mail and today/tomorrow at a glance.
- Settings for Auto Sync, days of mail kept, attachment limit, space for mail, special folders, accounts and
  time zone. Warnings for a wrong reader clock and for full storage.

The interface is English with a German translation (Settings > Language; texts in `qml/strings_de.tsv`).

## Requirements

- A PocketBook InkPad One (other models are untested).
- A mail account with IMAP and SMTP over TLS. App-specific passwords are recommended (Gmail and Outlook.com
  require them).
- Optional: a CalDAV/CardDAV server (Nextcloud first, OpenCloud second) or an `.ics` feed address over `https`.
  Contacts need the server account, not the feed.

## Installation

Install from the release package `pboffice-inkpad-one-<version>.zip`: copy the `applications/` folder onto the
reader and register the app in the Applications menu. The full steps are in [`INSTALL.md`](INSTALL.md).

## Usage

1. Open **PB Office** from the Applications menu.
2. Enter the mail account (server, port, security, user name, password, address) and use **Test** before **Save**.
3. Optionally add the calendar account in Settings; its address books appear under **Contacts** on the start page.
4. Use the page buttons to turn pages. Everything you change is stored on the reader first and reaches the server
   at the next sync.

## Credentials and privacy

- Passwords are stored **as plain text** in a file in the app's data folder (`system/pboffice/credentials`), with
  the strictest permissions the reader's storage allows. The storage is FAT, which probably cannot enforce them:
  anyone with USB access to the reader can read the file. The app measures the protection after saving and says
  so on the account page and in About. Use app-specific passwords.
- Logs hold counts and codes only: no address, subject, contact or file name.
- Contacts live in the database on the reader and nowhere else; no photos are stored, and nothing is sent except to the
  server you configured. The first start of this version upgrades the database to a new format (one way): a copy is
  kept until the next start, and an older build refuses the new file.
- The app opens no link and loads no remote content unless you tap "Load pictures" on one mail (one plain GET per
  picture over https, without cookies or referrer; the sender learns that the mail was read).
- It connects only to the servers you configure.

The details and the tests behind each claim are in
[`docs/reference/security-review.md`](docs/reference/security-review.md).

## Building from source

Host build and tests (Linux x86-64, CMake, Qt 6):

```bash
cmake -S . -B build-host && cmake --build build-host -j
```

Device build (ARM) needs the PocketBook SDK. Point `POCKETBOOK_SDK` at it; never hard-code the path:

```bash
export POCKETBOOK_SDK=/path/to/pocketbook-sdk
cmake -S . -B build-arm -G Ninja -DTARGET_TYPE=ARM -DCMAKE_BUILD_TYPE=Release
cmake --build build-arm
scripts/package-device.sh build-arm/app/pboffice.app "" build-arm/dist build-arm/app/probe/pbo-probe.bin
```

The app carries its own libcurl 8.18.0, because the reader's libcurl is too old to read IMAP answers. Build it once
with `scripts/build-curl-arm.sh` before configuring the ARM build. See also `cmake/sdk.cmake`.

## Testing

```bash
ctest --test-dir build-host --output-on-failure
```

The tests run against in-process fake IMAP, SMTP and CalDAV servers and anonymized fixtures; the tests say so
("UNVERIFIED against real servers"). The full local gate before a merge is:

```bash
scripts/pre-merge.sh
```

## Project layout

| Path | Content |
|------|---------|
| `core/` | C++17, Qt-free and InkView-free: transports, parsers, storage, sync (builds and tests on the host) |
| `bridge/` | The Qt layer that exposes the core to QML |
| `qml/` | The Qt 6 / Qt Quick UI (e-ink widgets, pages) |
| `app/`, `platform/pocketbook/` | InkView glue and the launcher script |
| `cmake/`, `scripts/` | Host build, ARM cross-build, pre-merge gate, packaging |
| `third_party/`, `licenses/`, `NOTICE` | SQLite, Mozilla CA bundle, Noto Emoji, time zone data and their licenses |
| `tests/` | Unit, contract and integration tests, fixtures, support code |
| `docs/` | Device and live-server checklists, findings, reference analyses |
| `specs/` | Specification, plan and tasks of each feature |

## Verification

Two checklists keep the verification honest. Every item stays "unverified" until a date and a result are written
next to it:

- [`docs/device-test-checklist.md`](docs/device-test-checklist.md): the InkPad One
- [`docs/live-server-checklist.md`](docs/live-server-checklist.md): real mail and calendar providers

The open work is listed in [`specs/001-email-calendar-mvp/tasks.md`](specs/001-email-calendar-mvp/tasks.md) (items
marked LIVE and DEVICE) and in `specs/001-email-calendar-mvp/research.md` ("Open items to verify").

## Documentation

- [`INSTALL.md`](INSTALL.md): installing on the reader
- [`CHANGELOG.md`](CHANGELOG.md): release notes
- [`specs/001-email-calendar-mvp/`](specs/001-email-calendar-mvp/): specification, plan, research, contracts, tasks
- [`.specify/memory/constitution.md`](.specify/memory/constitution.md): the project rules
- [`docs/reference/`](docs/reference/): SDK and InkFeed analyses, security review

## Contributing

Contributions are welcome. The project follows the Spec Kit flow (specification, plan, tasks, implementation) and the
rules in the [constitution](.specify/memory/constitution.md); the main ones:

- Write tests first and see them fail before the implementation.
- Keep `core/` free of Qt and device libraries.
- Never weaken TLS verification, add plain-text fallbacks or load remote content automatically.
- Write documentation, comments and commit messages in English (localization files may use other languages).
- Run `scripts/pre-merge.sh` before a merge.

Branches follow `feature/NNN-name`. Open an issue before starting larger changes.

## License

MIT, see [`LICENSE`](LICENSE). Third-party components are listed in [`NOTICE`](NOTICE) and in the app under
Settings > About, with every license text, offline. The code this project started from, notes-pocketbook and parts
of InkFeed, is MIT as well; see `NOTICE` for the exact notices.
