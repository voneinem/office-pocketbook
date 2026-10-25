# Contract: Packaging Scripts

## `scripts/package-device.sh <built-arm-elf> [version] [out-dir] [built-arm-pbo-probe] [built-arm-pbo-handoff-probe]`

Arguments, `--check-size`, `--self-test`, the default version (`VERSION` file) and the output name
`<out-dir>/pboffice-inkpad-one-<version>.zip` are unchanged.

New or changed behavior:

| Step | Behavior | On failure |
|------|----------|------------|
| 1 Tool | `PBSTORE_CATALOG` (executable file) if set and non-empty, else `pbstore-catalog` on `PATH`. First step, before any other check | exit 1, no ZIP: `pbstore-catalog not found (PBSTORE_CATALOG is not set; searched PATH). Put it on PATH or set PBSTORE_CATALOG ...`; a set variable naming a non-executable: `PBSTORE_CATALOG is set but is not an executable file: <value>` |
| 2 Inputs | as today, minus `INSTALL.md`; plus `packaging/manifest.template.json` | exit 1 `missing input: <file>` |
| 3 Version | must be `MAJOR.MINOR.PATCH`; the binary must contain it (as today) | exit 1 |
| 4 Stage | `applications/` as today; `manifest.json` from the template with `@VERSION@` replaced; no `INSTALL.md` | exit 1 if a `@...@` placeholder is left |
| 5 Pack | `pack <stage> --out <tmp>/a.zip`, again to `b.zip` | exit 1 `pbstore-catalog pack failed` |
| 6 Compare | `cmp a.zip b.zip`; on success one line on stderr: `reproducible: two packs are identical (sha256 <hash>)` | exit 1 `package is not reproducible: two builds from the same files differ` |
| 7 Check | `check-package a.zip`, then the 25 MB budget | exit 1 `package check failed`, or the size message |
| 8 Publish | existing ZIP of that version removed before step 5; `mv a.zip <out-dir>/...zip`; print the path on stdout | - |

Exit 0 only after step 8. stdout carries only the ZIP path (as today); the tool's findings and size/SHA-256 go to stderr.

## `scripts/check-release.sh [zip] [tag]` and `--self-test`

| Exit | Meaning |
|------|---------|
| 0 | tag, `VERSION`, ZIP name and manifest version agree: `release check passed (<version>)` |
| 1 | a mismatch (one line each) or a ZIP without a readable `manifest.json` |
| 2 | the ZIP does not exist |

Defaults: ZIP `dist/pboffice-inkpad-one-<VERSION>.zip` under the repository root, tag `v<VERSION>`.

## `packaging/manifest.template.json`

The manifest of research R-4 without `files`, with `"version": "@VERSION@"`. Fixed values are changed only by editing this file.
