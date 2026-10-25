# Feature Specification: Store-Conforming Release Package

**Feature Branch**: `feature/004-store-package-contract`

**Created**: 2026-10-25

**Status**: Draft

**Input**: User description: "Build the PB Office release package so that it follows the PB Store package contract (appstore-pocketbook, tag v0.1.3; see Assumptions: v0.1.4 or newer is needed). The PB Store lists apps only if their release ZIP follows the contract. The release trips E-PKG-04 (INSTALL.md at the ZIP root; only manifest.json and the applications/ tree are allowed there) and E-PKG-05 (no manifest.json). Needed: a manifest.json at the ZIP root created at build time from a template in the repository; INSTALL.md leaves the ZIP root; the package is built with `pbstore-catalog pack`, built twice and compared, and passes `pbstore-catalog check-package`; file name and the files under applications/ stay the same; the release is tagged v<version> with the ZIP attached and the tag equals the manifest version. Out of scope: the store entry in appstore-pocketbook, any change of the app itself."

## Overview

PB Office is installed today by unzipping a release ZIP onto the reader and registering the app by hand in the reader's
menu configuration (`INSTALL.md`). The PB Store, a catalog that lists PocketBook apps, accepts an app only when its
release ZIP follows the store's package contract: the ZIP root holds a `manifest.json` and the `applications/` tree
and nothing else. The current PB Office ZIP breaks two rules of that contract: it carries `INSTALL.md` at the root
(E-PKG-04) and it has no `manifest.json` (E-PKG-05), so the store would not list PB Office.

This feature changes how the release package is put together, not the app. The package gets a manifest that tells
the store (and any tool that reads it) what the app is, which reader it is for and how it appears in the reader's
menu; the install instructions move out of the ZIP root; the package is produced by the store's own packing tool so
that it is, by construction, what the store's checker accepts; and the release is published under a tag that names
exactly the version the manifest declares. A reader owner who installs by hand, as today, must see no difference in
the files that land on the reader.

The store's own entry for PB Office (in the catalog repository) and any change to the app itself are out of scope.

## User Scenarios & Testing *(mandatory)*

### User Story 1 - The store accepts the release package (Priority: P1)

The owner builds the release package and runs the store's package checker on it. The checker reports no errors, so
the PB Store can list PB Office and a reader owner can install it from the store.

**Why this priority**: This is the whole point of the feature. Without a package the checker accepts, PB Office
cannot be listed and every other part is moot.

**Independent Test**: Build the package from a clean checkout and run the store's package checker on the result. It
reports no errors, in particular neither E-PKG-04 nor E-PKG-05. List the ZIP's root entries: only `manifest.json` and
`applications/`.

**Acceptance Scenarios**:

1. **Given** the repository at a release version, **When** the owner builds the package and runs the package
   checker on it, **Then** the checker passes with no error.
2. **Given** the built package, **When** its root entries are listed, **Then** they are exactly `manifest.json` and
   the `applications/` tree; `INSTALL.md` is not among them.
3. **Given** the built package, **When** its `manifest.json` is read, **Then** it declares schema version "1.0", id
   `pboffice`, name `PB Office`, the version found in the repository's version file, the minimum firmware (the lowest tested firmware, see FR-002), target device "InkPad One", the menu entry (id `U_PbOffice`, title `PB Office`, the launcher path
   relative to the reader's storage, and the icon and focused icon paths as `INSTALL.md` registers them today), group `@General` and data folder `system/pboffice`.
4. **Given** the ZIP of the release that is live now, **When** the package checker is run on it before any change,
   **Then** the findings are recorded; if it reports anything besides E-PKG-04 and E-PKG-05, that is added to this
   feature's work before anything is built.

---

### User Story 2 - The package is built the same way every time, and a missing tool is called out (Priority: P2)

The owner (or anyone with a checkout) builds the package twice and gets byte-identical files, so the checksum of a
release can be trusted and compared with a rebuild. If the store's packing tool is not installed, the build stops
at once and says what is missing and how to point to it.

**Why this priority**: A store entry will carry a checksum of the ZIP; a build that differs run to run makes every
rebuild a different release. A clear failure keeps a half-built or hand-made ZIP from being published by accident.

**Independent Test**: Build the package twice from the same inputs and compare the two ZIPs byte for byte. Then
build with the packing tool unavailable and read the message.

**Acceptance Scenarios**:

1. **Given** the same inputs, **When** the package is built twice, **Then** the two ZIPs are identical and the build
   reports that it compared them.
2. **Given** two builds that differ, **When** they are compared, **Then** the build fails and says so; no ZIP is left
   as a release candidate.
3. **Given** the packing tool is available on the search path or named by an environment variable, **When** the
   package is built, **Then** the tool is found either way, and no machine-specific location is written into the
   repository.
4. **Given** the packing tool cannot be found, **When** the package is built, **Then** the build stops before
   producing any ZIP and the message names the tool, the environment variable that can point to it and the search
   path it looked in.
5. **Given** the build succeeded, **When** the build finishes, **Then** the package checker has been run on the
   result and a failing check fails the build.

---

### User Story 3 - A manual install keeps working and the instructions stay findable (Priority: P2)

A reader owner who installs by hand, without the store, still downloads the same-named ZIP, copies `applications/`
onto the reader and registers the app as before. The install instructions are no longer inside the ZIP, so the
release page points to them.

**Why this priority**: The owner and other people install this way today; the store is an addition, not a
replacement. Moving the instructions must not make them harder to find.

**Independent Test**: Compare the file list under `applications/` of the new package with that of the previous
release: same paths, same content for the same app build. Open the release page and follow the link to the
instructions.

**Acceptance Scenarios**:

1. **Given** the new package and the previous release's package built from the same app binary, **When** the file
   lists under `applications/` are compared, **Then** the names and the contents are the same.
2. **Given** the new package, **When** its file name is read, **Then** it is `pboffice-inkpad-one-<version>.zip`, as
   before.
3. **Given** the release page, **When** a reader owner looks for install steps, **Then** a link leads to the install
   instructions of that version in the repository.
4. **Given** the install instructions, **When** they are read, **Then** they no longer describe `INSTALL.md` as part
   of the unzipped package, and they still describe the manual copy and the menu registration, which names the same entry as the
   manifest (id, title, group, icons; the launcher path in the form `view.json` needs).

---

### User Story 4 - The release is tagged with the version the package declares (Priority: P3)

The owner publishes the release under the tag `v<version>` with the ZIP attached, and the tag, the manifest version,
the ZIP name and the version shown in the app are one and the same.

**Why this priority**: The store resolves a release by its tag and compares it with the manifest; a mismatch makes
the store refuse the entry. It comes last because it needs a finished package and the owner's go for publishing.

**Independent Test**: Read the tag name, the manifest inside the attached ZIP and the file name of the ZIP of the
published release. All carry the same version.

**Acceptance Scenarios**:

1. **Given** a release, **When** its tag, the ZIP name and the manifest version are compared, **Then** they agree.
2. **Given** a version file that differs from the intended tag, **When** the release is prepared, **Then** the
   preparation stops with a message and nothing is tagged or published.
3. **Given** a tag that already exists for another commit, **When** the release is prepared, **Then** it stops and
   the existing tag is left untouched.

---

### Edge Cases

- No `pbstore-catalog` is installed (the case on the owner's computer today): the build fails clearly (User Story 2);
  the work of this feature cannot be verified until the tool is obtained. Nothing may be faked.
- The packing tool or the checker refuses the manifest (unknown field, wrong type): the build fails with the tool's
  message; the template is corrected, not the checker bypassed.
- The version file holds a value the manifest cannot carry (extra text, a blank line, a build suffix): the build
  fails rather than writing a wrong version.
- File times, file order or the directory the build runs in must not change the result (reproducibility).
- The package contains the optional probe programs only when they were built; with and without them the package
  still passes the checker.
- The package size budget of the existing packaging (25 MB) still applies and a package above it still fails.
- The version in the compiled app differs from the version file: the build still refuses, as it does today.
- A release page already exists for the tag, or the ZIP is already attached: the preparation says so and does not
  silently replace a published file.
- Tagging and publishing are visible outside the machine and cannot be undone cleanly: they happen only on the
  owner's explicit go.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: The release package MUST contain at its root only `manifest.json` and the `applications/` tree.
- **FR-002**: The package MUST contain a `manifest.json` that declares: schema version "1.0"; id `pboffice`; name
  `PB Office`; the version from the repository's version file; the minimum firmware (the firmware version of the InkPad One on which the owner has tested the app, read from the reader itself and recorded in the repository with the date; see Assumptions); target device
  "InkPad One"; one menu entry with id `U_PbOffice`, title `PB Office`, the launcher path relative to the reader's storage
  (`applications/pboffice.app`, the form the store's contract asks for), and the icon and the focused icon paths as
  `INSTALL.md` registers them today; group `@General`; data folder `system/pboffice`.
- **FR-003**: `manifest.json` MUST be generated at build time from a template kept in the repository; the only
  value that changes between releases is the version, taken from the version file. A hand-edited manifest in the
  package or the repository's tree is not allowed.
- **FR-004**: `INSTALL.md` MUST NOT be in the package. The instructions stay in the repository and the release page
  links to the instructions of the released version. The instructions themselves MUST be updated so they do not
  describe the file as part of the ZIP and so their menu registration names the same entry as the manifest (id, title,
  group, icons); the launcher path stays in the absolute form the reader's `view.json` needs.
- **FR-005**: The package MUST be assembled with the store's packing tool, and the result MUST pass the store's
  package checker. A failing check MUST fail the build and leave no release candidate behind.
- **FR-006**: The packing tool MUST be found through the search path or through a named environment variable. No
  machine-specific location may appear in the repository. If the tool is not found, the build MUST stop before
  producing a ZIP and say which tool, which environment variable and which search path.
- **FR-007**: The build MUST produce the package twice and compare the two results byte for byte. It MUST fail if
  they differ and report plainly that the comparison was made and its result.
- **FR-008**: The package's file name (`pboffice-inkpad-one-<version>.zip`) and the paths and contents of the files
  under `applications/` MUST stay as they are, for the same app build, so a manual install and a reader that already
  has the app keep working.
- **FR-009**: The existing guards of the packaging MUST keep working: the version in the app binary must match the
  version file, the size budget must hold, the licence texts named by the licence manifest must be present.
- **FR-010**: A release MUST be tagged `v<version>`, with the ZIP attached, and the tag, the manifest version and the
  ZIP name MUST agree. If they do not, or the tag already exists on another commit, or a release for the tag already
  exists, preparation MUST stop without tagging or publishing. The agreement of the versions is checked by a script; the
  existing-tag and existing-release checks are steps of the documented release procedure, run by the owner before
  anything is tagged.
- **FR-011**: Tagging and publishing MUST happen only on the owner's explicit go; building the package never tags
  or publishes.
- **FR-012**: Before the change is built, the checker MUST be run on the current release ZIP and its findings
  recorded, to confirm that E-PKG-04 and E-PKG-05 are the only findings.
- **FR-013**: The build MUST NOT change the app: no source file of the app, no behavior, no stored data format.

### Key Entities

- **Release package**: the ZIP the owner publishes. Holds the manifest and the app's files; named by version.
- **Manifest**: a small description at the package root: which app, which version, which reader, how the app appears
  in the reader's menu, where its data lives. Generated per release from a template.
- **Manifest template**: the repository's fixed description with the version left open; the single place where the
  menu entry, target device and data folder are defined.
- **Release**: the published page for a tag with the ZIP attached and a link to the install instructions.
- **Install instructions**: the human-readable manual steps; kept in the repository, linked from the release.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: The store's package checker reports zero errors on the release package (today: two, E-PKG-04 and
  E-PKG-05).
- **SC-002**: Two builds from the same inputs produce ZIPs with identical checksums in 100% of runs.
- **SC-003**: The file list under `applications/` of the new package equals the previous release's for the same app
  build: 0 files added, removed or changed.
- **SC-004**: With the packing tool absent, the build stops in under 10 seconds, produces no ZIP and names the
  missing tool and how to point to it, in 100% of runs.
- **SC-005**: For a published release the tag, the manifest version and the ZIP name carry the same version: 0
  differences.
- **SC-006**: A reader owner can get from the release page to the install steps in one click.
- **SC-007**: A reader that follows the manual steps with the new package ends up with the same installed files and
  the same menu entry as with the previous release.

## Assumptions

- The package contract is the one of the store catalog at tag **v0.1.4 or newer**, not v0.1.3 as the request says. Found
  while planning (2026-10-25): v0.1.3 rejects the file names with a `+` in the time-zone data of the app (`Etc/GMT+5` and
  13 more, E-PKG-03); v0.1.4, tagged the same day, allows `+`. The exact fields and rules are taken from the tool's own
  checker and schema, and where they differ from this description the tool wins and the difference is recorded.
- No PB Office release has been published yet (no release exists on the repository, the newest local tag is `v0.2.0`,
  the version file says 0.4.0), so the "check the current release ZIP first" step (FR-012) is done on the ZIP the present
  packaging script built (`pboffice-inkpad-one-0.4.0.zip`), and repeated on the published ZIP as soon as one exists.
  **Result (2026-10-25, tool 0.1.4): exactly E-PKG-04 (`INSTALL.md`) and E-PKG-05 (no `manifest.json`), nothing else**;
  with tool 0.1.3 the 14 `+` names are reported as well. The tool is not on `PATH`: it is built from the store checkout.
- The version to release is whatever the version file holds at release time (0.4.0 now); which version is released
  first, and whether the earlier unreleased versions get tags, is the owner's decision at release time.
- The install instructions stay in the repository, in the same file, and the release page links to them (the same
  way PB Notes does), rather than being moved into a documentation folder or the ZIP's `applications/` tree.
- The app binary is built once; reproducibility concerns the packaging step given the same binary and the same
  inputs.
- The menu entry in the manifest repeats what `INSTALL.md` registers today, with one difference the contract forces: the
  store's contract wants the launcher path relative to the reader's storage (`applications/pboffice.app`), while the
  hand-made `view.json` entry uses the absolute `/mnt/ext1/applications/pboffice.app`. The two icon paths are the same
  in both. The manual instructions keep the absolute path.
- The existing development rules apply: tests first where a test can be written for a script (the package script
  already has self-tests), commit and push only on the owner's request, and no change to PB Notes' or InkFeed's files.
- The store entry for PB Office is handled outside this repository.
- **min_firmware** (decided 2026-10-25; candidate value `6.11.1437`, see below): the owner reads the firmware version on their own InkPad One (Settings, About device) and gives it; that value is the minimum, because it is the only one tested. It is not guessed and not derived from the SDK or from the Qt version. **The value itself has not been given yet**: it is needed before the template is finished, and the reader was not connected when this was written. Where the version is recorded (`docs/device-findings.md`, with the date) is a planning matter.
  Evidence for the candidate `6.11.1437`: the store's list of reserved folder names was taken on 2026-10-24 from "a PocketBook reader, model 1030, firmware 6.11.1437"; PB Notes declares the same value; the reader holds an update notice for that version. None of these proves the installed version, so the owner's reading of the About page confirms it before release.
