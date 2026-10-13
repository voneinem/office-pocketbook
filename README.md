# office-pocketbook

Offline-first **email and calendar** app for the PocketBook InkPad One (e-ink, 32-bit ARM Linux).
Sibling of [notes-pocketbook](https://github.com/voneinem/notes-pocketbook) (MIT, same author).

Status: **bootstrap**. The tree is a baseline copy of notes-pocketbook v1.10.0 (commit `57d5a08`);
the product is specified with [Spec Kit](https://github.com/github/spec-kit) (`/speckit.constitution`,
`/speckit.specify`, `/speckit.plan`, ...).

## What the baseline contains

Reusable infrastructure, to keep:

- `CMakeLists.txt`, `cmake/` - host build and ARM cross-build with the PocketBook SDK (`POCKETBOOK_SDK`)
- `app/`, `platform/pocketbook/` - InkView glue, launcher script
- `scripts/` - pre-merge gate, constitution/fixture/UI-style checks, packaging, device probe
- `third_party/` (SQLite, Mozilla CA bundle, Noto Emoji), `licenses/`, `NOTICE`
- Qt-free `core/` building blocks: HTTP/curl transport, CA selection, JSON, logging, UTF-8, paging,
  sync scheduler/status, About box
- generic QML widgets in `qml/` (dialogs, buttons, switches, pickers, top/bottom bars, Style.qml)
- `tests/support/` helpers (fake transport, TLS test server, tmp dirs)

Notes-specific code, still present only so the baseline builds and its tests stay green. Remove or replace
it as the office-pocketbook plan dictates: Nextcloud Notes API client, note store, Markdown engine,
attachments, note pages and bridges, and the `pbnotes` namespace/app name (rename before the first device build,
otherwise the package would overwrite the notes app).

## Reference material

`docs/reference/` holds the notes-pocketbook constitution, the SDK and InkFeed analyses and the security review.
Read them when running `/speckit.constitution` and `/speckit.plan`.

## Build (host)

```bash
cmake -S . -B build-host && cmake --build build-host -j && ctest --test-dir build-host --output-on-failure
```

Device build: see `INSTALL.md` and `cmake/sdk.cmake` (`-DTARGET_TYPE=ARM`, SDK via `$POCKETBOOK_SDK`).

## License

MIT, see `LICENSE`; third-party components in `NOTICE`.
