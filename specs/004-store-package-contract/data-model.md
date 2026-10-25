# Data Model: Store-Conforming Release Package

No stored user data and no database are involved. The entities are files.

## Release package

`<out-dir>/pboffice-inkpad-one-<version>.zip` (name unchanged, FR-008).

| Part | Content | Rule |
|------|---------|------|
| `manifest.json` | the manifest, with the file list added by `pack` | at the ZIP root, at most 256 KiB |
| `applications/` | exactly the files `scripts/package-device.sh` installs today (launcher, program, probe files when built, CA bundle, zone data, image plugins when found, emoji font and licence, licence texts, icons) | same paths, same contents for the same app build |
| anything else | none | E-PKG-04 otherwise; `INSTALL.md` is no longer here |

Size budget 25 MB (unchanged); the 0.4.0 trial package is 4.66 MB.

## Manifest (generated)

Source: `packaging/manifest.template.json`, one placeholder `@VERSION@`. Values: research R-4. The generated manifest has
no `files` list; `pbstore-catalog pack` adds it (path, size, SHA-256, sorted by path).

| Field | Changes between releases? |
|-------|---------------------------|
| `version` | yes: the `VERSION` file |
| `min_firmware` | only by a reviewed change of the template, when the owner tests on another firmware |
| everything else | no |

Validation: `VERSION` must be `MAJOR.MINOR.PATCH`; no `@...@` placeholder may remain; `check-package` validates the result.

## Release

| Item | Value |
|------|-------|
| Tag | `v<version>`; equals the manifest version (`scripts/check-release.sh`) |
| Attached file | the ZIP above, nothing else |
| Notes | the changelog entry and a tag-pinned link to `INSTALL.md` |

State transitions: staged (temporary folder) -> packed twice -> identical -> checked -> published to the output folder. Any
failure ends the run before the last step; nothing partial is left. Tagging and publishing happen later, by the owner.

## Install instructions

`INSTALL.md` in the repository root; not in the package. Linked from `README.md` and from each release.
