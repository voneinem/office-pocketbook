---

description: "Task list for Store-Conforming Release Package (office-pocketbook)"
---

# Tasks: Store-Conforming Release Package

**Input**: Design documents from `/specs/004-store-package-contract/`

**Prerequisites**: [plan.md](plan.md), [spec.md](spec.md), [research.md](research.md), [data-model.md](data-model.md), [contracts/packaging.md](contracts/packaging.md), [quickstart.md](quickstart.md); constitution v1.2.0

**Tests**: REQUIRED. Constitution Principle I (Test-First): in every story the test tasks come first and MUST be observed failing (the explicit "RED" task) before the implementation tasks. The tests are shell scripts registered in CTest; they need no SDK and no reader.

**Organization**: setup, the template (foundation), then the user stories in priority order (US1 P1, US2 and US3 P2, US4 P3), then polish and release. This is a packaging change only: no file under `core/`, `bridge/`, `qml/` or `app/` changes, and no file name or content under `applications/` changes (FR-008, FR-013).

## Format: `- [ ] T### [P] [US#] [TAG] Description with file path`

- **[P]**: can run in parallel with its neighbours (different files, no dependency on an unfinished task)
- **[US#]**: the user story of `spec.md` (only in story phases)
- **Environment tags** (the default is *host-only*: Linux x86-64, no device, no SDK):
  - **[TOOL]**: needs `pbstore-catalog` v0.1.4 or newer. Its path is given by the environment (`PBSTORE_CATALOG`) or `PATH`; never write it into a tracked file (constitution R5).
  - **[SDK]**: needs the PocketBook SDK on the build machine (`POCKETBOOK_SDK`, never hard-coded) but not the device.
  - **[DEVICE]**: needs the InkPad One and the owner. Its result stays *unverified* until a date and a value are recorded (Principle IX).
  - **[OWNER]**: outward-facing or the owner's decision. Never done without the owner's explicit go (FR-011).
- The test script follows `~/Projekte/notes-pocketbook` (`tests/packaging/test_package_device.sh`, `scripts/check-release.sh`, `scripts/package-device.sh`) as a model; copy the ideas, rename `pbnotes` to `pboffice`, and keep PB Office's own guards (probe arguments, 25 MB budget, image plugins, `--self-test`, `--check-size`). Never edit files of PB Notes.
- Commit and push only when the owner asks. Mark a task `[X]` when it is done and its check passes.
- All text written is English (Principle X). No absolute home paths in tracked files (R5).

---

## Phase 1: Setup

**Purpose**: housekeeping and the baseline that the later guards compare with. No behavior change.

- [X] T001 Confirm the working branch is `feature/004-store-package-contract` (`.specify/branch-convention.yml`: `{type}/{seq}-{kebab}`; create it from `main` if the owner has not) and that `.specify/feature.json` names `specs/004-store-package-contract`.
- [X] T002 [TOOL] Get `pbstore-catalog` v0.1.4 or newer: build it from a checkout of the appstore-pocketbook repository at that tag in a folder outside this repository (for example with `git archive v0.1.4`, then `cmake -S . -B build -G Ninja && cmake --build build --target pbstore-catalog`), export `PBSTORE_CATALOG` to the built tool, and confirm `"$PBSTORE_CATALOG" --version` prints `pbstore-catalog 0.1.4` or newer. The older binary in that checkout's `build-host` rejects `+` in file names (research R-1) and must not be used.
- [X] T003 Record the baseline BEFORE any script change (FR-008, SC-003): with the unchanged `scripts/package-device.sh`, a fake binary (a text file holding the contents of `VERSION`), no probe arguments, and `PBO_QT_IMAGE_PLUGINS` and `POCKETBOOK_SDK` unset, build a package into a temporary folder, list the files under `applications/` (`unzip -Z1 <zip> | grep '^applications/' | grep -v '/$' | LC_ALL=C sort`) and write the list to `tests/packaging/applications.expected`. Check that it lists the fixed files plus the 598 zone files of `third_party/tzdata/zoneinfo` and the zone folder's own `VERSION` file (the 0.4.0 ZIP of the last device build has 622 files under `applications/`: 599 in `pboffice-zoneinfo` and 23 others; this fake run leaves out the 3 probe files and the 2 image-plugin files, so expect 617 lines), and that no plugin or probe file is in it.

---

## Phase 2: Foundational (blocks every user story): the manifest template

**Purpose**: the one file all stories need.

- [X] T004 Create `packaging/manifest.template.json` with exactly the values of research R-4 and no `files` list: `schema_version` `"1.0"`; `id` `"pboffice"`; `name` `"PB Office"`; `version` `"@VERSION@"`; `min_firmware` `"6.11.1437"`; `target_device` `"InkPad One"`; `menu_entry` {`id` `"U_PbOffice"`, `title` `"PB Office"`, `path` `"applications/pboffice.app"` (relative to the storage root, not the absolute `/mnt/ext1/...`), `icon` `"applications/icons/pboffice.bmp"`, `focused_icon` `"applications/icons/pboffice_f.bmp"`, `group` `"@General"`}; `data_dir` `"system/pboffice"`. Two-space indentation, final newline. The value of `min_firmware` is a candidate until T033.
- [X] T005 Create the test script `tests/packaging/test_package_device.sh` (executable) with the harness only: `check`/`check_not`/`case_start`/`skip` helpers, `real_tool` (uses `PBSTORE_CATALOG` if it names an executable, else the first `pbstore-catalog` on `PATH`), the `stub` helper for test doubles of the tool, `new_stub_env`, `build <out-dir> [version]` (runs `scripts/package-device.sh` on a fake binary containing the version, sets `rc`, `$tmp/stdout`, `$tmp/stderr`), `json_get`, and the closing summary (`FAILED` / `all cases skipped` exit 77 / `ok (N cases, M skipped)`). Register it in `tests/CMakeLists.txt` as `packaging_script` with label `script` and `SKIP_RETURN_CODE 77` (so "all skipped" is a skip, not a failure), next to `launcher_script`.

---

## Phase 3: User Story 1 - The store accepts the release package (Priority: P1) MVP

**Goal**: the built ZIP has `manifest.json` and `applications/` at its root and passes `pbstore-catalog check-package`.

**Independent Test**: build the package, run `check-package` on it, list the root entries (quickstart Q2 to Q4).

### Tests first

- [X] T006 [US1] In `tests/packaging/test_package_device.sh` add the case "package passes the store check": with the real tool (SKIP when none is installed) build a package and run `check-package` on it; expect exit 0 and a ZIP named `pboffice-inkpad-one-<VERSION>.zip`.
- [X] T007 [US1] Add the case "top level and manifest" with a stub tool that keeps a copy of the tree it was asked to pack: the tree holds exactly `applications` and `manifest.json` and no `INSTALL.md`; the manifest has the 13 values of T004 and `version` equals the contents of `VERSION`; also check that the generated file has no `files` key and no `@` placeholder.
- [X] T008 [US1] Add the case "version comes from one source": a build with the explicit version `9.8.7` (fake binary holding it) names the ZIP `pboffice-inkpad-one-9.8.7.zip` and writes `9.8.7` into the manifest.
- [X] T009 [US1] RED: run `ctest --test-dir build-host -R packaging_script --output-on-failure` (configure `build-host` first if needed) and record that the cases of T006 to T008 fail against today's script (it still puts `INSTALL.md` at the root and has no manifest).

### Implementation

- [X] T010 [US1] In `scripts/package-device.sh` replace the `zip` call: stage `applications/` as today into the temporary folder, remove `INSTALL.md` from the inputs and from the stage (also from the `for f in ...` list of required inputs), add `packaging/manifest.template.json` to the required inputs, validate `$VERSION` against `^[0-9]+\.[0-9]+\.[0-9]+$` (message `version is not MAJOR.MINOR.PATCH: <value>`), write `manifest.json` into the stage with `sed "s/@VERSION@/$VERSION/g"`, fail with `manifest template has an unfilled placeholder: <name>` if `@[A-Z_]+@` remains, then `pack <stage> --out <tmp>/a.zip` and `check-package <tmp>/a.zip` with the tool found by `command -v pbstore-catalog` (the full lookup comes in T017), the 25 MB budget check on `<tmp>/a.zip`, and `mv` to `<out-dir>/pboffice-inkpad-one-<version>.zip`. Keep every other guard (version string in the binary, licence texts, probe arguments, plugins note) and print only the ZIP path on stdout.
- [X] T011 [US1] GREEN: the cases of T006 to T008 pass; `scripts/package-device.sh --self-test` and `--check-size` still pass.

**Checkpoint**: with the tool on `PATH` the package passes the store's check. This is the MVP.

---

## Phase 4: User Story 2 - Reproducible build, clear failure when the tool is missing (Priority: P2)

**Goal**: two packs are compared byte for byte, the tool is found through `PBSTORE_CATALOG` or `PATH`, and every failure ends before a ZIP is left behind.

**Independent Test**: quickstart Q5b and Q5c; the stub-tool cases below.

### Tests first

- [X] T012 [US2] Add the case "tool missing": `PBSTORE_CATALOG` empty and a `PATH` holding only the commands the script needs (symlinks, as in the PB Notes test) gives exit 1 and a message that contains `pbstore-catalog not found`, `PBSTORE_CATALOG` and `PATH`, no `missing input`, and no ZIP in the output folder.
- [X] T013 [US2] Add the case "environment variable": `PBSTORE_CATALOG` naming an executable stub that is not on `PATH` works and is the tool that was called; the same variable naming a missing file gives exit 1 and `PBSTORE_CATALOG is set but is not an executable file`, even when a good tool is on `PATH`, and the stub log stays empty.
- [X] T014 [US2] Add the case "reproducibility": a stub whose `pack` writes different bytes each time gives exit 1, `package is not reproducible`, no ZIP; a good stub is called exactly twice with `pack` and once with `check-package`, and a successful build prints `reproducible: two packs are identical (sha256 <hash>)` on stderr (FR-007, contract step 6).
- [X] T015 [US2] Add the case "check failure leaves nothing": a stub whose `check-package` prints `error[E-PKG-04] x: y` and exits 1 gives exit 1, that line and `package check failed` on stderr, and removes an older ZIP of the same version from the output folder. Add the case "bad inputs": versions `1.2` and `1.2.x` are refused; a copy of the repository's `scripts/` and `packaging/` with a placeholder `@X@` added to the template gives `unfilled placeholder: @X@`; a removed template gives `missing input: ... manifest.template.json`. Add the case "size budget": `PBO_MAX_ZIP_BYTES=1000` gives exit 1 and no ZIP. Add the case "pack fails": a stub whose `pack` exits 1 gives exit 1, `pbstore-catalog pack failed`, no ZIP. Add the case "existing guards" (FR-009): a fake binary that does not contain the version gives exit 1 and `version mismatch`; a license text named by `licenses/manifest.txt` that is missing (use the copied-repository trick, with the file removed from the copy) gives exit 1 and `license text named by licenses/manifest.txt is missing`; both leave no ZIP.
- [X] T016 [US2] RED: run `ctest --test-dir build-host -R packaging_script --output-on-failure` and record which of T012 to T015 fail against the script as it is after T010.

### Implementation

- [X] T017 [US2] In `scripts/package-device.sh` move the tool lookup to the first step, before any other check: `PBSTORE_CATALOG` if set and non-empty must be an executable file (else `PBSTORE_CATALOG is set but is not an executable file: <value>`, exit 1); if unset or empty, `command -v pbstore-catalog`, else exit 1 with `pbstore-catalog not found (PBSTORE_CATALOG is not set; searched PATH). Put it on PATH or set PBSTORE_CATALOG to the tool (build it from the appstore-pocketbook repository, tag v0.1.4 or newer).` No default path anywhere.
- [X] T018 [US2] In the same script pack twice (`a.zip`, `b.zip` in the temporary folder), `cmp -s` them and fail with `package is not reproducible: two builds from the same files differ`; send the tool's own output to stderr (`>&2`) so stdout stays the ZIP path; remove an existing ZIP of the same version before packing; every failure path exits before the `mv`; after a successful `cmp` print `reproducible: two packs are identical (sha256 <hash>)` to stderr. Update the header comment of the script (layout list without `INSTALL.md`, the manifest, the tool lookup, the two packs).
- [X] T019 [US2] GREEN: all cases of T012 to T015 pass; with the real tool, build twice into two folders and `cmp` the two ZIPs (SC-002).

**Checkpoint**: the build is reproducible, checked, and fails clearly.

---

## Phase 5: User Story 3 - A manual install keeps working and the instructions stay findable (Priority: P2)

**Goal**: same ZIP name and same files under `applications/`; the release page and the README lead to `INSTALL.md`.

**Independent Test**: quickstart Q5 and the guard case below.

### Tests first

- [X] T020 [US3] Add the case "applications set unchanged (guard)": with the stub tool, the staged `applications/` file list (with a fake binary, no probe arguments, no image plugins, `PBO_QT_IMAGE_PLUGINS` and `POCKETBOOK_SDK` unset inside the test) equals `tests/packaging/applications.expected` from T003, and no `INSTALL.md` is staged. Add the case "probe files": with fake probe binaries as 4th and 5th arguments the stage also holds `applications/pbo-probe.bin`, `applications/pbo-handoff-probe.bin` and `applications/pbo-handoff-target.app`, and nothing else is added or lost.
- [X] T021 [US3] Add the case "instructions stay reachable": `README.md` links `](INSTALL.md)` and no longer says that the file is part of the package; `INSTALL.md` still contains `merging it with the existing` (the manual route) and now mentions `manifest.json`.
- [X] T022 [US3] RED: run `ctest -R packaging_script` and record that the doc case fails (the `INSTALL.md` sentence is not there yet) while the guard cases already pass.

### Implementation

- [X] T023 [P] [US3] In `INSTALL.md` add one short paragraph at the top of "1. Copy the files": the package also holds a `manifest.json` at its root (used by the PB Store; the manual route ignores it), and `INSTALL.md` itself is not in the package, it is in the repository and linked from each release. Change nothing else; the registration step keeps the absolute `/mnt/ext1/applications/pboffice.app` path.
- [X] T024 [P] [US3] In `README.md` update the install and build text: the install package follows the PB Store package contract, `manifest.json` is made from `packaging/manifest.template.json` and the `VERSION` file, the script packs twice, compares, checks, and stops with a message when the tool is missing; the tool is `pbstore-catalog` from appstore-pocketbook, **tag v0.1.4 or newer**, on `PATH` or named by `PBSTORE_CATALOG` (no default location); `unzip` and `python3` are needed by the release check; remove `zip` from the prerequisites if it is listed. Keep the link to `INSTALL.md`.
- [X] T025 [US3] GREEN: T020 and T021 pass. Then, with the real tool and the 0.4.0 binary, build the package from `build-arm/app/pboffice.app` with the same extra arguments and environment as the 0.4.0 build (the probe binaries as 4th and 5th argument, and `POCKETBOOK_SDK` or `PBO_QT_IMAGE_PLUGINS` set so that the image plugins are included), then compare the list of files under `applications/` of the new package with that of `build-arm/dist/pboffice-inkpad-one-0.4.0.zip` (names and, for the same binary, contents: `unzip -p ... | sha256sum` per file or `diff -r` of two unpacked trees); expect 0 differences (SC-003). The version is 0.4.0 here, so run this before T031 changes `VERSION`, or compare against a package built from the same binary.

**Checkpoint**: manual install and the instructions are unchanged for the reader.

---

## Phase 6: User Story 4 - The release is tagged with the version the package declares (Priority: P3)

**Goal**: tag, `VERSION`, ZIP name and manifest version agree, checked by a script; publishing stays the owner's step.

**Independent Test**: `scripts/check-release.sh --self-test`, quickstart Q7.

### Tests first

- [X] T026 [US4] Write `scripts/check-release.sh` starting with its `--self-test` only (throw-away repository folder and ZIPs made with `python3 -m zipfile`): cases "everything agrees" (exit 0), "tag differs from the manifest version" (1), "tag without the `v` prefix" (1), "`VERSION` differs from the manifest version" (1), "package name differs from the manifest version" (1), "package without manifest.json" (1), "package does not exist" (2). Use `pboffice-inkpad-one-<version>.zip` as the name. Register `check_release_selftest` in `tests/CMakeLists.txt` (label `script`).
- [X] T027 [US4] RED: run `ctest -R check_release_selftest` and record that it fails (the check function is not written yet).

### Implementation

- [X] T028 [US4] Complete `scripts/check-release.sh` as in the contract (`contracts/packaging.md`): `check <repo-root> <zip> <tag>`; defaults `dist/pboffice-inkpad-one-<VERSION>.zip` and `v<VERSION>`; read `"version"` from `unzip -p <zip> manifest.json` with `sed`; one line per mismatch on stderr; exit 0 `release check passed (<version>)`, 1, or 2; the header comment states usage and exit codes. Make it executable.
- [X] T029 [US4] GREEN: `scripts/check-release.sh --self-test` and `ctest -R check_release_selftest` pass. Add the release steps (tag and release must not exist yet, locally, on the remote and as a GitHub release; `git tag -a`, `gh release create` with the ZIP and a tag-pinned `INSTALL.md` link in the notes, verify the downloaded ZIP) to `README.md` as steps of the maintainer, each marked as done only on explicit decision to publish, chained so that `check-release.sh` runs before the tag; they match `quickstart.md`.

**Checkpoint**: a release cannot be prepared with disagreeing versions.

---

## Phase 7: Polish, version bump and release

**Purpose**: the gates, the rebuilt binary, the real-tool walk-through and the release.

- [X] T030 [P] Run `scripts/check-constitution.sh` and its self-test; confirm no absolute local path and no tool path in tracked files (R5), and that `scripts/package-device.sh`, `scripts/check-release.sh` and `packaging/` are not flagged. Fix findings in the new files only.
- [X] T031 Raise `VERSION` to `0.4.1` and add a `## 0.4.1 - <date>` entry to `CHANGELOG.md` in the style of the existing entries: the release package follows the PB Store package contract (manifest at the root, `INSTALL.md` moved out of the package, packed with `pbstore-catalog` v0.1.4 or newer, reproducible and checked); the app is the 0.4.0 app apart from its version string; the state of the unverified items of 0.4.0 is unchanged; the minimum firmware in the manifest: write the value and the word "confirmed" only after the owner has answered T033, otherwise "candidate, not yet confirmed". If the owner chooses to release 0.4.0 instead (research R-8), skip the bump and run the pre-merge gate with `PBO_NO_BUMP=1`, and say so in the changelog.
- [X] T032 [SDK] Rebuild the ARM binary so it carries the new version (`cmake -S . -B build-arm -G Ninja -DTARGET_TYPE=ARM -DCMAKE_BUILD_TYPE=Release && cmake --build build-arm`, `POCKETBOOK_SDK` from the environment; see `README.md`) and build the package: `scripts/package-device.sh build-arm/app/pboffice.app "" build-arm/dist build-arm/app/probe/pbo-probe.bin`. Then run quickstart Q3, Q4 and Q5c with the real tool (a missing tool: unset `PBSTORE_CATALOG` and use a `PATH` without it) and record the results (SHA-256, size, `package ok`) in the changelog entry or the pull request text.
- [X] T033 [DEVICE] [OWNER] The owner reads the firmware version on the reader (Settings > About device) and confirms or corrects `6.11.1437`; then set `min_firmware` in `packaging/manifest.template.json` if it differs, and add the version and the date to `docs/device-findings.md` ("tested on firmware ...; the app has not been tried on any other"). Until this is done `min_firmware` stays a candidate and the release (T036) must not be made.
- [X] T034 [DEVICE] Manual route on the reader (quickstart Q6, UNVERIFIED until done): unzip the new package, copy `applications/` as `INSTALL.md` says, start PB Office; About page and Back work, nothing of `system/pboffice/` is touched. Only after the owner says "mounted" or "Install!"; never touch PB Notes or InkFeed files. Record the date and the result in `docs/device-test-checklist.md` next to the install items.
- [X] T035 Run `scripts/pre-merge.sh` (version bump check, host build, all tests including `packaging_script`, `check_release_selftest` and `package_size_selftest`, secret scan) until it passes; run `scripts/check-release.sh build-arm/dist/pboffice-inkpad-one-<version>.zip v<version>` and expect `release check passed (<version>)`. Check FR-013: `git diff main --stat -- core bridge qml app` shows nothing (the version string comes from `VERSION`, not from a source file).
- [X] T036 [OWNER] Release, only on the owner's explicit go and after the merge: follow the release steps of `quickstart.md` (tag, remote tag and GitHub release must not exist yet, else stop; tag `v<version>`; push the tag; `gh release create` with `build-arm/dist/pboffice-inkpad-one-<version>.zip` attached and the tag-pinned link to `INSTALL.md` in the notes), then run Q8: download the attached ZIP, `pbstore-catalog check-package` on it exits 0 and its SHA-256 equals the one printed when it was built. Record the result in the release notes (not in a commit after the tag). Do not touch the store repository (out of scope).
- [X] T037 Update the project memory `project-004-store-package` (done items, the confirmed firmware, the released version) and mark every finished task above `[X]`.

---

## Dependencies and order

- Phase 1 first. T003 (the baseline) MUST be done before T010 changes the script.
- T004 and T005 block every story. T004 and T005 are independent of each other ([P]).
- US1 (T006 to T011) is the MVP. US2 (T012 to T019) edits the same script after US1. US3 (T020 to T025) depends on T010 (the staged tree) and its docs tasks T023, T024 are independent of the scripts. US4 (T026 to T029) is independent of the others except that its README step is written after T024.
- Within a story: tests, then RED, then implementation, then GREEN.
- Polish: T030 any time after the scripts exist; T031 before T032; T032 before T034 and T035; T033 answered before the changelog wording of T031 is final, and before T036; T036 last.

## Parallel opportunities

- T004 with T005 (different files).
- T023 with T024 (INSTALL.md and README.md), and both with T026 to T028 (a different script).
- T030 with T031.
- Tests T006 to T008, T012 to T015, T020 and T021 are in one file and therefore written one after the other.

## Implementation strategy

1. MVP: Phases 1 to 3. A package that passes the store's check, with the tool on `PATH`. Stop and look at the checker's output.
2. Add US2 for reproducibility and the clear failure, then US3 for the guard and docs, then US4 for the release gate.
3. Polish: bump to 0.4.1, rebuild, run the real-tool checks, confirm the firmware with the owner, run the merge gate. Tag and publish only on the owner's go.
4. Honesty (Principle IX): the checker verdict on the real package is **verified** once T032 has run; the manual install on the reader (T034), the firmware value (T033) and the downloaded release (T036) stay **unverified** until their dates and results are recorded.
