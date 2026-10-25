# Quickstart: Store-Conforming Release Package

Validation guide; the interfaces are in [contracts/packaging.md](contracts/packaging.md), the values in
[research.md](research.md) R-4.

## Prerequisites

- `pbstore-catalog` **v0.1.4 or newer** from the appstore-pocketbook repository, on `PATH` or named by `PBSTORE_CATALOG`
  (the tool's own path; there is no default). Build it from that repository at the tag.
- A built ARM binary whose About version equals `VERSION` (`cmake -S . -B build-arm ... && cmake --build build-arm`,
  `POCKETBOOK_SDK` from the environment), `unzip`, `cmp`, `python3`.
- For Q6-Q8: the owner's go, and for Q6 the reader.

## Checks

| # | Scenario | Command | Expected |
|---|----------|---------|----------|
| Q1 | Host tests (US1-US4, no SDK) | `ctest --test-dir build-host -R 'packaging_script\|check_release_selftest\|package_size_selftest'` | all pass; the real-tool case reports SKIP when no tool is installed |
| Q2 | Build the package | `scripts/package-device.sh build-arm/app/pboffice.app "" build-arm/dist build-arm/app/probe/pbo-probe.bin` | exit 0, prints `build-arm/dist/pboffice-inkpad-one-<version>.zip` |
| Q3 | Store contract (SC-001) | `pbstore-catalog check-package build-arm/dist/pboffice-inkpad-one-<version>.zip` | exit 0, `package ok`, no `E-PKG-*` |
| Q4 | Top level and manifest (US1) | `unzip -Z1 <zip> \| grep -v '^applications/'`; `unzip -p <zip> manifest.json` | only `manifest.json`; values as in research R-4, `version` = `VERSION` |
| Q5 | Same files as before (SC-003) | compare `unzip -Z1 <zip> \| grep '^applications/'` with the same list of the 0.4.0 ZIP | no file added, removed or renamed; contents equal for the same binary |
| Q5b | Reproducible (SC-002) | run Q2 twice into two folders, `cmp` the two ZIPs, compare `sha256sum` | identical |
| Q5c | Missing tool (SC-004) | `env PBSTORE_CATALOG= PATH=/usr/bin:/bin scripts/package-device.sh ...` (no tool there) | exit 1 in under 10 s, message names tool, variable and search; no ZIP in the output folder |
| Q6 | Manual install still works (US3, SC-007) | unzip, copy `applications/` to the reader, register as `INSTALL.md` says, start PB Office | About page and Back work (UNVERIFIED until the owner does it) |
| Q7 | Tag agreement (US4, SC-005) | `scripts/check-release.sh build-arm/dist/pboffice-inkpad-one-<version>.zip v<version>` | `release check passed (<version>)` |

## Release (only on the owner's explicit go)

1. After the merge: `git rev-parse -q --verify refs/tags/v<version>` must print nothing (the tag is new, also on the remote:
   `git ls-remote --tags origin v<version>` prints nothing) and `gh release view v<version>` must fail (no release yet).
   If either exists, stop; nothing is tagged, replaced or deleted. Then `scripts/check-release.sh <zip> v<version>` must pass.
2. `git tag -a v<version> -m "PB Office <version>"`, `git push origin v<version>`.
3. `gh release create v<version> build-arm/dist/pboffice-inkpad-one-<version>.zip --title "PB Office <version>"` with the
   changelog entry and a link `<repository>/blob/v<version>/INSTALL.md` in the notes.
4. Q8 (UNVERIFIED until done): download the attached ZIP and run `pbstore-catalog check-package` on it; its SHA-256 equals the
   one printed in Q3. Record the result in the release notes (not in a commit after the tag). Then, outside this repository, the store entry points at `.../releases/download/v<version>/...`.

## Before the release

- The owner confirms `min_firmware` (Settings > About device); the value and the date go into `docs/device-findings.md`.
