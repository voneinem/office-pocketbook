# Research: Store-Conforming Release Package

Each entry: decision, rationale, alternatives. "Trial" means a run made while planning (2026-10-25) in a scratch folder, on the
real `pboffice-inkpad-one-0.4.0.zip` of the last device build, with the store tool built from the `appstore-pocketbook`
checkout at tag v0.1.4 (`pbstore-catalog 0.1.4`) and, for comparison, with the older binary that was installed (0.1.0 line,
built 2026-10-24, before the `+` fix). Nothing in the repository or on the reader was changed.

## R-1 What the store's tool says about today's package (FR-012), and which tool version is needed

**Decision**: the contract version to build against is **appstore-pocketbook v0.1.4 or newer**, not v0.1.3 as the request says.

**Evidence** (baseline, FR-012):

| Tool | `check-package` on the current 0.4.0 ZIP |
|------|------------------------------------------|
| built 2026-10-24 (before the fix, same rules as v0.1.3) | exit 1: `E-PKG-04 INSTALL.md`, `E-PKG-05 manifest.json` **and 14 x `E-PKG-03`** for `applications/pboffice-zoneinfo/GMT+0` and `.../Etc/GMT+0` ... `GMT+12` (a `+` in a file name) |
| v0.1.4 | exit 1: exactly `error[E-PKG-04] INSTALL.md: "INSTALL.md" is not allowed at the package root ...` and `error[E-PKG-05] manifest.json: manifest.json is missing from the package root` |

`git log` of the store: commit "fix(contract): allow '+' in package file names" (2026-10-25) is in v0.1.4 and in neither v0.1.1
to v0.1.3; its contract text lists `+` among the allowed characters.

**Trial with v0.1.4**: the 0.4.0 ZIP unpacked, `INSTALL.md` removed, the template manifest (version 0.4.0) added,
`pbstore-catalog pack <dir> --out a.zip` twice: both exit 0, `cmp` says identical, `check-package a.zip` exit 0
("package ok", 4,660,895 bytes, `app: pboffice 0.4.0`), and the list of files under `applications/` is identical to that of
the 0.4.0 ZIP (622 files: 599 under `pboffice-zoneinfo`, that is 598 zones and its `VERSION` file, and 23 others).

**Rationale**: removing the 14 files or renaming them would change the app's data (the zone names are what a calendar server
sends as a time zone, and `Etc/GMT+12` is even mapped by the app: `core/src/tz.cpp`), which FR-008 and FR-013 forbid. The store
fixed its checker instead; the build only has to require the fixed tool.

**Alternatives**: drop the `+` files from the package (rejected: changes `applications/` and the app's time-zone coverage,
out of scope); rename them in the package (rejected: the app looks them up by their IANA name); ask the store to keep v0.1.3
(rejected: it is the store's decision and it already decided).

**Consequence**: the README names "v0.1.4 or newer". The script does not check the tool's version (the check would be
guesswork on `--version` text); `check-package` itself rejects what an old tool cannot pack, so a too old tool fails the build
with the E-PKG-03 lines instead of producing a package the store would refuse.

## R-2 Finding the tool: `PBSTORE_CATALOG`, else `PATH`

**Decision**: if the environment variable `PBSTORE_CATALOG` is set, it must name an executable file (the tool itself); if it is
unset or empty, `command -v pbstore-catalog`. A set-but-invalid variable is an error (no silent fall back to `PATH`). No
default path. The same resolved command runs `pack` and `check-package`. The lookup happens first, so a missing tool is
reported before any staging or input check.

**Missing-tool message** (stderr, exit 1, no ZIP): names the tool, the variable and the places searched:
`pbstore-catalog not found (PBSTORE_CATALOG is not set; searched PATH). Put it on PATH or set PBSTORE_CATALOG to the tool (build it from the appstore-pocketbook repository, tag v0.1.4 or newer).`

**Rationale**: the same rule the project applies to the SDK (`POCKETBOOK_SDK` only from the environment), and what PB Notes
does. The spec asks for "tool, variable and search path" in the message (US2 scenario 4).

**Alternatives**: an argument `--tool` (rejected: the script's positional arguments are used by the README and notes);
guessing a sibling checkout (rejected: a hidden path, and R5 forbids absolute paths in tracked files).

## R-3 Writing the manifest: one placeholder, `sed`

**Decision**: `packaging/manifest.template.json` holds the fixed values and `"version": "@VERSION@"`. The script runs
`sed "s/@VERSION@/$VERSION/g"` into the staging folder after checking `$VERSION` against `^[0-9]+\.[0-9]+\.[0-9]+$` (the rule
of `check-version-bump.sh`), so the value needs no JSON escaping, and fails if any `@[A-Z_]+@` is left. `files` is not in the
template: `pack` computes it.

**Rationale**: spec edge case "the version file holds a value the manifest cannot carry"; the leftover check covers a template
changed without the script.

**Alternatives**: `jq` (rejected: a new tool for one string); writing the JSON from the script (rejected: FR-003 wants a
template in the repository).

## R-4 The values of the manifest

**Decision** (every fixed value of the template):

| Field | Value | Source |
|-------|-------|--------|
| `schema_version` | `"1.0"` | the contract |
| `id` / `name` | `"pboffice"` / `"PB Office"` | request; `id` matches `^[a-z][a-z0-9-]{1,39}$` |
| `version` | `@VERSION@` | `VERSION` file |
| `min_firmware` | `"6.11.1437"` (pending, R-5) | owner |
| `target_device` | `"InkPad One"` | request |
| `menu_entry.id` / `title` | `"U_PbOffice"` / `"PB Office"` | `INSTALL.md`, `view.json` entry |
| `menu_entry.path` | `"applications/pboffice.app"` | see below |
| `menu_entry.icon` / `focused_icon` | `"applications/icons/pboffice.bmp"` / `".../pboffice_f.bmp"` | `INSTALL.md` |
| `menu_entry.group` | `"@General"` | request |
| `data_dir` | `"system/pboffice"` | request; not on the store's reserved list |

**`menu_entry.path` is relative to the storage root**, as the contract says ("Application path relative to the storage root;
MUST be listed in `files`"; `E-PKG-13` otherwise). The hand-made registration in `INSTALL.md` uses the absolute
`/mnt/ext1/applications/pboffice.app` because the reader's own `view.json` wants that; the store's installer turns the
relative path into the reader's form. `INSTALL.md` keeps its absolute path (the manual route is unchanged); the spec's
assumption about the path was corrected.

## R-5 `min_firmware`: 6.11.1437, to be confirmed by the owner

**Decision**: the template carries `6.11.1437`. It is the firmware the owner's InkPad One is documented to run: the store
repository's reserved-folder list says it was taken from "model 1030, firmware 6.11.1437" on 2026-10-24, PB Notes declares the
same value, and the reader holds an update notice `system/config/firmware/U1030.6.11.1437.json` (status "available", so by
itself it does not prove the installed version).

**Open input**: the owner reads Settings > About device on the reader and confirms the value; until then it is a candidate.
The tested range is one firmware only (the owner's single reader); the earlier test of 0.1.0 (2026-10-14) was on whatever
firmware was installed then. Nothing is claimed for older or newer firmware. After the confirmation the version and the date
are written into `docs/device-findings.md`.

**Alternatives**: a lower value "to be generous" (rejected: untested, constitution IX); no `min_firmware` (rejected: the
contract requires it).

## R-6 Where the install instructions go

**Decision** (the spec's open point; the default, PB Notes' way): `INSTALL.md` stays in the repository, `README.md` links it,
and each GitHub release carries a tag-pinned link in its notes
(`<repository>/blob/v<version>/INSTALL.md`). `INSTALL.md` gets one sentence saying the package also holds a `manifest.json`
for the store and that the manual route is unchanged. Its remaining text (copy `applications/`, register `U_PbOffice`) is still
true because the `applications/` set is unchanged. Its first step "Unzip the package and copy the `applications/` folder" is
already worded that way, so it needs no other edit.

**Alternatives**: a copy under `applications/` (rejected: changes the installed file set, FR-008); a docs folder inside the
ZIP (rejected: E-PKG-04).

## R-7 Reproducibility, verification and atomic publishing

**Decision**: order inside the script: resolve the tool, check the inputs, stage `applications/` and the manifest, remove any
existing target ZIP, `pack` twice into two temporary files, `cmp` (on success a stderr line says that the two packs were compared and are identical, with the SHA-256, FR-007), `check-package` on the first, size budget (25 MB), `mv`
to `<out-dir>/pboffice-inkpad-one-<version>.zip`, print the path as today. Every failure exits non-zero before the `mv`, so the
output folder never holds a new, unchecked ZIP; an old ZIP of the same version is removed first so it cannot be mistaken for
the result.

**Rationale**: `pack` is documented and observed (R-1) to be deterministic regardless of file times and order (fixed
timestamps, sorted entries); a second pack takes about a second and proves it for every build (FR-007).

**Alternatives**: trust the documentation (rejected by FR-007); stage twice from separate copies (rejected: the two copies are
the same inputs, the second adds nothing). The app binary is built once; reproducibility concerns the packaging step.

**Noted, not worked around**: `pack` writes every file with mode 0644, while the old ZIP carried 0777 on the launcher and
program. The reader's storage is FAT, where modes do not exist, and the manual route never relied on them. What the store's
installer does about the execute bit is the store's business.

## R-8 Version of this change: 0.4.1

**Decision**: PATCH, `VERSION` 0.4.1, tag `v0.4.1`.

**Rationale**: `scripts/check-version-bump.sh` counts `scripts/` and `packaging/` as product files, so the branch needs a
bump and a changelog entry; the constitution allows the opt-out only for changes nobody can see, and this is visible to the
store and to anyone building. The ARM binary must be rebuilt, because it carries the version string and the script refuses a
mismatch; the app is otherwise the 0.4.0 app. No tag `v0.3.0` or `v0.4.0` exists yet (the newest is `v0.2.0`), so releasing
0.4.0 itself would be possible, but it would change what "0.4.0" means after its zip was already built and handed around.

**Open for the owner**: this is the default of the plan; releasing as 0.4.0 with `PBO_NO_BUMP=1` is the alternative. Whether
0.3.0 and 0.4.0 also get tags is the owner's decision and is not part of this feature.

## R-9 Tag, version and package agree (FR-010)

**Decision**: `scripts/check-release.sh [<zip>] [<tag>]` (defaults: `dist/pboffice-inkpad-one-<VERSION>.zip` relative to the
repository root, tag `v<VERSION>`). It reads `manifest.json` from the ZIP with `unzip -p`, takes `"version"` with `sed` and
requires tag = `v` + manifest version, manifest version = `VERSION`, ZIP name = `pboffice-inkpad-one-<manifest version>.zip`.
One line per mismatch, exit 1; exit 2 when the ZIP is missing. `--self-test` builds throw-away ZIPs with `python3 -m zipfile`.
A tag that already exists on another commit, and a release that already exists for the tag, are checked by the release steps
of `quickstart.md` (`git rev-parse` and `gh release view` before anything is tagged or created), not by the script, because the
script must not depend on git state; the spec (FR-010) says so.

**Rationale**: the store checks the tag against the manifest only when the entry is added, a repository away (`E-ENT-14`);
failing locally before the release is published is cheaper. Not part of `pre-merge.sh` (no ZIP exists there).

**Alternatives**: do it inside `package-device.sh` (rejected: the tag may not exist at build time); a CI workflow (rejected: CI
is deferred in this project); automate `gh release create` (rejected: publishing is outward-facing and happens only on the
owner's go, FR-011).

## Out of scope, noted for later

- The catalog entry (`catalog/entries/pboffice.json`) in `appstore-pocketbook`; its `package_url` must use the tag chosen here.
- What the store's installer does on the reader (menu registration from the manifest, execute bits): unverified until the
  store app exists.
- The unverified hand-over features of 0.4.0 stay unverified; the package does not change them.

## Baseline

`pbstore-catalog check-package build-arm/dist/pboffice-inkpad-one-0.4.0.zip` (tool 0.1.4), exit 1:

```text
error[E-PKG-04] INSTALL.md: "INSTALL.md" is not allowed at the package root (only manifest.json and the applications/ tree)
error[E-PKG-05] manifest.json: manifest.json is missing from the package root
```
