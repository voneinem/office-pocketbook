# Implementation Plan: Store-Conforming Release Package

**Branch**: `feature/004-store-package-contract` | **Date**: 2026-10-25 | **Spec**: [spec.md](spec.md)

**Input**: Feature specification from `/specs/004-store-package-contract/spec.md`

## Summary

The release ZIP gets a `manifest.json` at its root, loses `INSTALL.md` from its root and is assembled by the store's own
tool `pbstore-catalog pack`, so that `pbstore-catalog check-package` passes. It is a **packaging change only**: no app
code, no QML, no core, no schema. The files under `applications/` are the ones `scripts/package-device.sh` already installs.
PB Notes did the same change (its spec 011); the design is the same, adapted to what PB Office puts into `applications/`.

1. **Manifest template** `packaging/manifest.template.json`: every fixed value and one placeholder, `@VERSION@`.
2. **`scripts/package-device.sh`** finds the tool (`PBSTORE_CATALOG`, else `PATH`; a clear failure before any work), stages
   the same `applications/` tree plus the manifest, packs twice, requires byte-identical results, runs `check-package` and
   only then puts the ZIP into the output folder. `INSTALL.md` is no longer an input or an entry. The existing guards
   (version string in the binary, 25 MB budget, licence texts, `--self-test`, `--check-size`) stay.
3. **`scripts/check-release.sh`**: tag = `VERSION` = ZIP name = manifest version, with a `--self-test`. Tagging and
   publishing stay a deliberate step of the owner, documented in `quickstart.md`.
4. **Docs**: `README.md` (tool, release steps), `INSTALL.md` (one sentence: the package also holds a `manifest.json`; the
   manual route is unchanged), `CHANGELOG.md`.
5. **Release 0.4.1** (PATCH, see R-8): the installed app is the 0.4.0 app apart from its version string.

Three things were found while planning and are decided in [research.md](research.md):

- **The tool must be v0.1.4 or newer, not v0.1.3 (R-1).** v0.1.3 rejects the 14 time-zone files with a `+` in the name
  (`Etc/GMT+5` ...) with E-PKG-03; v0.1.4 (tagged 2026-10-25) allows `+`. With 0.1.4 the current ZIP trips exactly E-PKG-04
  and E-PKG-05 and a trial package passes. Removing or renaming the 14 files would have changed the app's data (FR-008, FR-013).
- **The manifest's launcher path is relative to the reader's storage** (`applications/pboffice.app`), not the absolute
  `/mnt/ext1/...` of the hand-made `view.json` entry (R-4).
- **`min_firmware` is `6.11.1437`, pending the owner's confirmation** (R-5).

## Technical Context

**Language/Version**: Bash (`#!/usr/bin/env bash`, `set -euo pipefail`, as the other scripts); JSON for the template. No C++/QML.

**Primary Dependencies**: `pbstore-catalog` from `appstore-pocketbook`, **tag v0.1.4 or newer** (contract schema 1.0), an
external build tool of the maintainer: not vendored, not a dependency of the app. `unzip` (read the manifest back in
`check-release.sh`), `cmp`, `sed`, `mktemp`. `zip` is no longer used by the package script.

**Storage**: none. A template in `packaging/`, a temporary staging folder, the result in `dist/`-style output folder
(git-ignored).

**Testing**: shell tests registered in CTest like `launcher_script` and `check_version_bump_selftest`:
`tests/packaging/test_package_device.sh` (drives the script with a fake ARM "binary" and **stub** `pbstore-catalog` for the
failure paths, runs the real tool for the happy path when installed, else reports SKIP) and `scripts/check-release.sh
--self-test`. The end-to-end walk-through on the real binary is `quickstart.md`.

**Target Platform**: Linux x86-64 maintainer machine (the ARM binary is cross-built as before); the package targets the
InkPad One; `min_firmware` is the candidate 6.11.1437 until the owner confirms it on the reader (research R-5, T033).

**Project Type**: single application with a host-testable core, unchanged; this feature touches build and release scripts.

**Performance Goals**: two packs and one check on a 4.7 MB package take a few seconds (trial: well under 5 s). A missing
tool is reported before any staging work (SC-004: under 10 s).

**Constraints**: no machine-specific path to the tool in the repository (constitution VIII; `check-constitution.sh` R5);
the file names and contents under `applications/` and the ZIP name unchanged; no valid-looking ZIP after a failed build;
nothing in the app changes except the version string that the bump brings.

**Scale/Scope**: 1 new JSON template, 1 script changed, 1 script added, 1 test script, 3 doc files, `VERSION`.

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.* (Constitution 1.2.0)

| Principle | Pre-research | Post-design | Evidence |
|-----------|--------------|-------------|----------|
| I. Test-First | PASS | PASS | Each behavior is a script outcome a shell test can observe: manifest content, top-level entries, tool lookup order, missing tool, non-reproducible pack, failing check, tag mismatch. `tasks.md` orders `tests/packaging/test_package_device.sh` and the `check-release.sh` self-test (observed failing against today's script) before the script changes |
| II. Host-testable core | PASS | PASS | `core/` is not touched. The tests run on the host without the SDK (a fake binary holding the version string) |
| III. No silent data loss | PASS | PASS | No user data is touched. The package still contains nothing of `system/pboffice/`; `data_dir` tells the store to keep the folder on removal. Manual upgrade steps are unchanged |
| IV. Security & privacy | PASS | PASS | No network (`pack` and `check-package` are offline, per the store's CLI contract). No secret, no user data. The tool comes from `PATH` or a variable the maintainer controls; nothing is downloaded or run from the network |
| V. Simplicity & bounded footprint | PASS | PASS | No new dependency of the app; the tool is a maintainer's build tool, pinned by tag (v0.1.4 or newer) in the README. `sed` on one placeholder, no JSON tool. The 25 MB budget stays. Rejected alternatives: R-3, R-6 |
| VI. E-ink UX & battery | N/A | N/A | No UI, no runtime behavior |
| VII. Licensing hygiene | PASS | PASS | `package-device.sh` is already attributed in `NOTICE`; `check-release.sh` and the test follow PB Notes (same author, MIT). `pbstore-catalog` (same author, MIT) is run, not copied. No component added or removed |
| VIII. Quality gates | PASS | PASS | Packages still come from `scripts/package-device.sh`. New mechanical rules (manifest content, tag agreement) come with self-tests/tests proving they detect a violation. The tool location comes only from the environment or `PATH`, like `POCKETBOOK_SDK` |
| IX. Honest verification | PASS | PASS | What was tried is labelled: the checker verdict is **verified** with tool 0.1.4 on the 0.4.0 ZIP and on a trial package (R-1); the device-side effect of the store installer, the `min_firmware` value and a real release are **unverified** until done (quickstart Q6-Q8, research R-5) |
| X. English documentation | PASS | PASS | All new text is English |
| Stack and build constraints | PASS | PASS | No change of stack; build tooling only |
| Release version | PASS | PASS | Last phase of `tasks.md`: `VERSION` 0.4.1 + `CHANGELOG.md` entry, enforced by `check-version-bump.sh`; tag `v0.4.1` after the merge, checked by `check-release.sh` |

No Complexity Tracking entry is needed.

## Project Structure

### Documentation (this feature)

```text
specs/004-store-package-contract/
├── plan.md              # This file
├── research.md          # Phase 0: R-1 ... R-9
├── data-model.md        # Phase 1: package, manifest, template, release
├── quickstart.md        # Phase 1: build, check, tag, publish, verify
├── contracts/
│   └── packaging.md     # script interfaces, template, messages
├── checklists/requirements.md
└── tasks.md             # Phase 2 (/speckit-tasks, not created here)
```

### Source Code (repository root)

```text
packaging/
└── manifest.template.json        # NEW   fixed manifest values + @VERSION@

scripts/
├── package-device.sh             # CHG   find tool, stage manifest, pack twice + cmp, check-package, publish ZIP;
│                                 #       INSTALL.md no longer an input or entry; header comment updated
└── check-release.sh              # NEW   tag == VERSION == ZIP name == manifest version (+ --self-test)

tests/
├── CMakeLists.txt                # CHG   register packaging_script and check_release_selftest
└── packaging/
    ├── test_package_device.sh    # NEW   fake binary + stub/real pbstore-catalog; failure paths
    └── applications.expected     # NEW   the file list under applications/ of the package built from the repository inputs

README.md                         # CHG   build + tool + release steps; INSTALL.md is not in the package
INSTALL.md                        # CHG   one sentence about manifest.json; the manual route is unchanged
CHANGELOG.md, VERSION             # CHG   0.4.1
docs/device-findings.md           # CHG   firmware the app was tested on (after the owner confirms it)
```

**Structure Decision**: keep the single packaging script and add the template in a new top-level `packaging/` folder, as
PB Notes does. The tag check is its own small script because it runs at release time, after the ZIP exists, and follows
the pattern of the other `scripts/check-*.sh` gates (with `--self-test`).

## Complexity Tracking

No constitution violation; nothing to justify.
