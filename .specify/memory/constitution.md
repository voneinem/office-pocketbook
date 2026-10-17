# Office Pocketbook Constitution

## Core Principles

### I. Test-First (NON-NEGOTIABLE)
Tests MUST be written before the implementation they cover and MUST be observed failing before the
implementation is written (Red-Green-Refactor), for every feature and bug fix. No change is merged
while any test fails. Behavior at system boundaries - IMAP, SMTP, CalDAV/HTTP, MIME and iCalendar
parsing, SQLite storage and schema migrations - MUST be covered by integration or contract tests
that run against in-process fake servers and anonymized fixtures, never real mail, real calendars
or real accounts. Parsers of untrusted input (MIME, iCalendar, server responses) MUST have tests for
malformed, truncated and oversized input. A change to a boundary format or contract MUST include a
contract test.

Rationale: mail and appointments are irreplaceable user data; failing-first tests prove the tests
can detect the defect, and fake servers make boundary behavior repeatable without a device.

### II. Host-Testable Core and Layered Architecture
The code is layered as follows, and the dependency direction MUST NOT be reversed:

- `core/` - C++17, Qt-free and InkView-free: IMAP, SMTP, CalDAV, MIME, iCalendar, storage, sync.
- `bridge/` - the Qt layer exposing the core to QML.
- `qml/` - the Qt 6 / Qt Quick UI.
- `app/` and `platform/pocketbook/` - device glue (InkView, launcher).

`core/` MUST NOT include Qt, InkView or any SDK-only library, and MUST build and run its complete
test suite on Linux x86-64 with CMake/ctest without the device or the PocketBook SDK. The build
MUST provide both a host target and a device (ARM) target.

Rationale: fast, device-free tests make Test-First practical and keep the core portable.

### III. No Silent Data Loss
User data MUST NOT be lost or silently overwritten, whatever happens to the device (Wi-Fi drop,
sleep, battery death, app kill).

- Drafts, the outbox (unsent mail) and locally edited or created calendar events MUST be persisted
  before any network operation and MUST NOT be discarded, evicted or overwritten until the server
  has confirmed them.
- Sync MUST be resumable and idempotent: repeating an interrupted sync produces the same result as
  an uninterrupted one. An outgoing message MUST NOT be sent twice; if the outcome of a send is
  unknown (for example a timeout after the message body was transmitted), the message MUST be
  kept, marked as uncertain, and the user informed instead of being resent automatically.
- Calendar writes and deletes MUST be conditional (If-Match with the stored ETag). An HTTP 412 or any
  other detected conflict is a conflict: both versions MUST be kept and the user MUST be able to
  resolve it; neither version may be discarded automatically. Mail changes MUST be keyed by UID
  and UIDVALIDITY; when UIDVALIDITY changes the local copy is re-synced without dropping unsent or
  unconfirmed local work.
- Deleting mail or events, and any permanent deletion (emptying Trash, expunge), MUST require
  explicit user confirmation. Deleting mail moves it to Trash by default.

Rationale: a last-writer-wins sync or an automatic resend destroys data or embarrasses the user
in ways that cannot be undone.

### IV. Security & Privacy
Mail and calendars are private by default. The project MUST collect only the data needed for its
function, MUST NOT transmit user data to third parties, and MUST NOT commit secrets, credentials or
real user data to the repository. All external input MUST be validated before use.

- TLS certificate and host-name verification MUST be enabled for every connection. Disabling or
  weakening verification, including as a development or device workaround, MUST NOT be
  implemented, and no setting, option or flag may trust unverifiable certificates (this includes
  "trust self-signed" options). Trust anchors come from the device CA store when it is usable,
  otherwise from a CA file bundled with the application; a user-supplied `custom-ca.pem` in the
  application data directory is added to them, and Settings MUST show whether a custom CA is
  loaded.
- Only `imaps://`, `smtps://` and `https://` endpoints, or IMAP/SMTP with STARTTLS where the upgrade
  is enforced (CURLUSESSL_ALL semantics, no continuation in plaintext), are accepted. There MUST be
  no plaintext fallback and no downgrade on failure. Credentials MUST NOT be sent to a different
  host after a redirect.
- HTML mail MUST NOT be rendered by a web engine. It is converted to plain text or a small,
  fixed set of markup. Remote content (images, stylesheets, tracking pixels, fonts) MUST NOT be
  loaded when a message is synced or opened, and links are shown but never opened automatically.
  The owner's decision of 2026-10-16 allows exactly one exception: the user may load the remote
  PICTURES of ONE message by an explicit tap on that message (never for all mail, never by a
  setting, never at sync). The page MUST say beforehand that loading tells the sender the message
  was read; only `https://`, one plain GET each, without credentials, cookies or referrer, bounded
  in size and number, content judged by its first bytes, nothing else (no styles, fonts, frames).
  Pictures that are parts of the message itself (`cid:`) are not remote content.
- Attachments MUST be fetched only on demand, subject to a size limit, MUST be treated as untrusted
  data, and MUST NOT be executed or opened by anything other than a viewer the user chose.
- Secrets (passwords, tokens) MUST NOT appear in logs, error messages shown to the user, or
  exported files. Logs MUST NOT contain mail addresses, subjects, bodies, calendar titles,
  attendees, locations, server URLs, file names or paths.
- Credential storage MUST be described honestly in the README and the About box. App passwords are
  the recommended mechanism and the default documentation path. Where credentials are stored
  unencrypted on the device, the user MUST be told so; every plan that touches credentials MUST
  evaluate a better-than-plaintext option (restrictive file permissions at minimum) and record the
  decision.

Rationale: mail and calendars reveal personal information and carry credentials that grant access
to other accounts; an unverified connection or a rendered remote image leaks both.

### V. Simplicity & Bounded Footprint
Implement only what the current specification requires (YAGNI). The default dependency set is
libcurl, OpenSSL and SQLite (plus Qt for the UI layer). A further dependency MUST NOT be added
without a written justification in the plan's complexity-tracking section covering its license,
binary size, ARM cross-compile feasibility and the simpler alternative that was rejected. Dependencies
MUST be pinned and reviewed.

Every plan MUST state measurable budgets for RAM, storage and cold-start time on the InkPad One,
and tests (marked unverified until measured on the device) MUST check them. Mailbox size MUST be
bounded: sync headers for the last N days only (N configurable, with a default), bodies and
attachments on demand, with cache eviction that never touches drafts, the outbox or unconfirmed
local changes.

Rationale: a small, readable codebase is easier to secure and test, and the device has little
memory, storage and battery.

### VI. E-Ink UX & Battery
The UI MUST be designed for e-ink: black-and-white high contrast, large touch targets, no
animations, page-based lists instead of smooth or kinetic scrolling, minimal screen refreshes, and
operation with the on-screen keyboard. The app MUST NOT poll or sync in the background. Network
activity happens only on app start, on explicit user request, and shortly after a send or save
(Auto Sync, on by default and switchable). The UI MUST always show the offline or unsynced state and
the time of the last sync.

Rationale: e-ink panels ghost and refresh slowly, and background radios drain a battery that is
meant to last weeks.

### VII. Licensing Hygiene
The project license is the MIT License; `LICENSE` states "Copyright (c) 2026 Volker von Einem".
Third-party components MUST be listed in `NOTICE`, in `licenses/manifest.txt` and in the offline
About box, and only licenses compatible with MIT distribution of the binary are allowed (no GPL
code linked or copied). GPL and other incompatible projects (for example K-9 Mail, Etar, DAVx5) are
reference-only: no code, strings, comments or documentation text MAY be copied; behavior MAY be
reimplemented from public specifications (RFC 3501, 5321, 5322, 2045-2049, 4791, 5545, 6638)
and behavior-only descriptions. Patterns adapted from InkFeed (MIT) and from notes-pocketbook
(MIT, same author) MUST be attributed in `NOTICE` where required.

Rationale: the binary is distributed to other users; unclear licensing blocks that and
copied GPL code would force a license change.

### VIII. Quality Gates
Before any merge `scripts/pre-merge.sh` MUST pass: build, all tests, the secret scan, the
mechanical constitution checks (`scripts/check-constitution.sh`), the fixture check
(`scripts/check-fixtures.sh`), the UI style check (`scripts/check-ui-style.sh`) and the version
bump check. The mechanical checks MUST be extended with every new rule that can be checked by a
script (for example plaintext IMAP/SMTP URLs, mail data in log calls) together with a self-test
proving that the rule detects a violation. Device packages MUST be built with
`scripts/package-device.sh`. The ARM cross-build MUST be reproducible and take the SDK location only
from the `POCKETBOOK_SDK` environment variable or the `-DSDK_PATH` CMake option; SDK or toolchain
paths MUST NOT be hard-coded in build files, scripts or documentation examples.

Rationale: a gate that runs locally and mechanically catches the violations that review misses.

### IX. Specs-Driven Workflow & Honest Verification
Every feature follows the Spec Kit flow: specification, clarification, plan, tasks, analysis,
implementation. The plan's Constitution Check MUST pass, or each deviation MUST be justified,
before design proceeds. Behavior that needs a real IMAP, SMTP or CalDAV server, or the real
device, MUST be labelled "unverified" in specs, tests and fixtures until it has been exercised
there; verification results are recorded when obtained. Hand-written fixtures that imitate server
responses carry the `.unverified` marker.

Rationale: protocol quirks differ per server; claiming verified behavior that was only simulated
misleads the next reader.

### X. English Documentation
All documentation and developer-facing text MUST be written in English: Markdown files (README,
specs, plans, research, tasks, checklists, CHANGELOG, `docs/`), code comments, identifiers, test
names, commit messages, pull request texts, log and error keys, script output and CI messages.
The English text of a user-visible string is its source text (the `qsTr` argument).

Other languages are allowed only for:
- application localization files (for example `qml/strings_de.tsv`) and the translated texts they
  hold;
- test data and expected values that exercise non-English input or locale behavior (for example
  German umlauts, character sets, German folder names such as `Entwürfe`, German output of a
  localized function), kept as data and explained by an English comment or test name;
- verbatim names and quotations that are not translatable (server folder names, product names).

Rationale: one documentation language keeps the code base readable for every contributor and
reviewer, while localization files and non-English test data are the product's purpose, not its
documentation.

## Security & Data Handling Constraints

- Data on a source device or server MUST NOT be deleted or overwritten without explicit user
  confirmation; operations on source data are read-only by default.
- Fixtures MUST be anonymized and use reserved example domains and addresses.
- Mail and calendar content received from a server MUST be size-limited and parsed defensively;
  a malformed item MUST be skipped or shown as unreadable, never crash the app or the sync.
- The application data directory and the database file MUST use restrictive permissions where the
  platform allows it.

## Technology Stack & Build Constraints

- The stack is C++17, Qt 6.8 Quick/QML for the UI, CMake, libcurl for networking, OpenSSL for TLS
  and SQLite for storage (see `docs/reference/inkfeed-analysis.md` and
  `docs/reference/pocketbook-sdk-analysis.md`). Changing any of these requires a constitution
  amendment.
- Whether the device's libcurl supports IMAP and SMTP MUST be verified (and recorded in the plan's
  research) before the email design is fixed.
- The baseline copied from notes-pocketbook still carries its name (`pbnotes`) and notes-specific code.
  The app, binary, launcher, data directory and package name MUST be changed before the first device
  build so that office-pocketbook never overwrites or shares data with notes-pocketbook.

## Licensing & Attribution

- Build, launcher and packaging patterns adapted from InkFeed keep their attribution in `NOTICE`.
- The README MUST state the license before the first public push.
- Adding or removing a third-party component requires updating `NOTICE`, `licenses/manifest.txt`
  and the About box in the same change.

## Development Workflow

- Every change goes through review; the reviewer MUST verify compliance with this constitution and
  MUST reject TLS-verification bypasses, plaintext fallbacks, hard-coded SDK paths, remote-content
  loading, code copied from GPL projects and documentation outside English (Principle X).
- Branches follow the gitflow convention in `.specify/branch-convention.yml`
  (`feature/NNN-name`); bugs are triaged with the bug extension (`.specify/bugs/<slug>/`).
- Test gates: all tests pass and no new secrets are detected before merge (Principle VIII).
- Release version: every merged change to the product raises `VERSION` (MINOR for a feature, PATCH
  for a fix, MAJOR for a breaking change such as an irreversible data upgrade) and adds an entry to
  `CHANGELOG.md`. The bump is a task of the feature (last phase of `tasks.md`) and is enforced by
  `scripts/check-version-bump.sh` in `scripts/pre-merge.sh`. After the merge the release is tagged
  `v<version>`. Changes nobody can see (refactoring) may opt out with the variable documented in
  `scripts/check-version-bump.sh`, stated in the pull request.
- Database schema upgrades are one-way and MUST be tested; an older build MUST refuse a newer
  database rather than damage it.

## Governance

This constitution supersedes other project practices. Amendments MUST be made by editing this file
with a documented rationale and a version bump, and MUST include any migration steps needed for
existing specs, plans or code. A principle may be relaxed only by amendment, never by an
undocumented exception in a plan.

Versioning follows semantic versioning: MAJOR for backward-incompatible removal or redefinition of
principles or governance; MINOR for a new principle or section or materially expanded guidance;
PATCH for clarifications and wording fixes.

Compliance is reviewed in every pull request and in a mandatory Constitution Check in every plan,
which lists each principle with pass, justified deviation or not applicable. Unjustified violations
block the merge.

**Version**: 1.2.0 | **Ratified**: 2026-10-13 | **Last Amended**: 2026-10-17
