# Contract: Device package, install layout and runtime data

**Feature**: [../spec.md](../spec.md) | **Research**: [../research.md](../research.md) R-02, R-13, R-21

Names replace the notes-pocketbook names everywhere (`pbnotes` -> `pboffice`) so the two apps never overwrite
or share data (constitution, Technology Stack). `scripts/package-device.sh` produces the zip; its checks
(`pre-merge.sh`) fail if any `pbnotes` name survives in tracked build, launcher or packaging files.

## 1. Install layout (unzipped onto the reader's ext1 partition)

```text
applications/pboffice.app                 launcher (shell script, adapted from the baseline/InkFeed launcher)
applications/pboffice.bin                 the program (ARM ELF)
applications/pboffice-cacert.pem          bundled Mozilla CA bundle (third_party/cacert)
applications/pboffice-emoji.ttf           Noto Emoji (monochrome)
applications/pboffice-emoji-OFL.txt
applications/pboffice-zoneinfo/           IANA time-zone data, compact TZif files (third_party/tzdata) + VERSION
applications/pboffice-licenses/           manifest.txt and every license text it names (About page, offline)
applications/icons/pboffice.bmp           122x122, 8 bit, opaque
applications/icons/pboffice_f.bmp
INSTALL.md
```

Registration in `system/config/desktop/view.json` (user edits it; backup first), as in the baseline:

```json
"U_PbOffice": {
  "path": "/mnt/ext1/applications/pboffice.app",
  "title": "PB Office",
  "icon": { "path": "applications/icons/pboffice.bmp" },
  "focused_icon": { "path": "applications/icons/pboffice_f.bmp" }
}
```

plus `"U_PbOffice"` in one `groups` application list. The id starts with `U_`, `path` is absolute.

## 2. Runtime data (`/mnt/ext1/system/pboffice/`)

| Entry | Purpose | Created by |
|-------|---------|------------|
| `pboffice.db` | SQLite database (data-model.md) | app at first start |
| `credentials` | passwords, plain text, mode 0600 where honored | app after account setup |
| `attachments/` | (unused since 2026-10-14; downloaded attachments go to `/mnt/ext1/Downloads`) | app |
| `custom-ca.pem` | optional user CA | user (FR-003) |
| `effective-ca.pem` | derived CA bundle | app at start |
| `pboffice.log`, `pboffice.log.prev` | launcher/app diagnostics, no personal data (FR-041) | launcher |
| `pboffice.db.bak` | only during a schema upgrade | app |

The launcher keeps the previous log, exports the Qt/EGL environment of the baseline, and records the
diagnostics lines of the baseline (uid, GPU node, uptime, memory) — none of which is personal data.

## 3. Build contract

- Host: `cmake -S . -B build-host && cmake --build build-host -j && ctest --test-dir build-host --output-on-failure`.
- Device: `cmake -S . -B build-arm -DTARGET_TYPE=ARM -DCMAKE_BUILD_TYPE=Release` with the SDK location **only**
  from `POCKETBOOK_SDK` or `-DSDK_PATH`; moc from the SDK host Qt, `rcc` from the host system (the SDK `rcc`
  needs ICU 76) — manual steps in `cmake/qt_manual.cmake` [REPO]. No SDK path in any tracked file (rule R2).
- SQLite is compiled from the vendored amalgamation with `SQLITE_ENABLE_FTS5`; no other new compile definitions.
- `scripts/package-device.sh <elf>` verifies the version string in the binary equals `VERSION`.
- The ARM link line adds nothing beyond the baseline (`curl`, `ssl`, `crypto`, `z`, Qt, `inkview`, `freetype`);
  `xml2` is **not** linked (in-house XML, research R-15).

## 4. Probe (research R-02), run by `scripts/probe-device.sh` with `pbo-probe`

Prints, without personal data: `curl_version_info()` version/protocol list/features, whether
`imaps/smtps/imap/smtp` are present; whether `/etc/ssl/certs` has usable PEM files; file-system type and
whether `chmod 600` is observable in the data directory; `TZ`, `/etc/localtime` target; `MemTotal`,
free storage; presence of `/usr/lib/gconv`; screen size/dpi. Results go to `docs/device-findings.md` with the
date; open items O-1, O-6, O-7, O-8 of the research close from it.
