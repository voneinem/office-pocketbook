# Bug Fix: Settings page has no paging, lower rows are unreachable

- **Slug**: settings-page-no-paging
- **Fixed**: 2026-10-25
- **Assessment**: ./assessment.md
- **Status**: applied (host-verified; on the reader **unverified**)

## Summary

The Settings page keeps its non-draggable `Flickable` (e-ink) but now ends above a bottom bar that holds the same `PageButtons` as the contact page (page up, "n / total", page down), so every row down to "About, licenses" can be reached.

## Changes

| File | Change | Notes |
|------|--------|-------|
| `qml/pages/SettingsPage.qml` | modified | the `Flickable` got `id: view`, `objectName: "settingsView"` and now ends at the new `bottomBar` instead of the page bottom; a `bottomBar` item with `PageButtons { target: view; showIndicator: true }` was added (copy of the `ContactPage` pattern, without `snap` because the rows differ in height) |
| `tests/qml/tst_calendar_flow.qml` | added test | `test_the_settings_page_can_be_paged_to_every_row` |
| `VERSION`, `CHANGELOG.md` | modified | 0.4.2 (PATCH, user-visible UI fix; required by `scripts/check-version-bump.sh`) |
| `docs/device-test-checklist.md` | added row | DT-SET-1, unverified until tried on the reader |

## Diff Highlights

```qml
Flickable {
    id: view
    objectName: "settingsView"
    anchors { left: parent.left; right: parent.right; top: topBar.bottom; bottom: bottomBar.top }
    ...
}
Item {
    id: bottomBar
    anchors { bottom: parent.bottom; left: parent.left; right: parent.right }
    height: Style.bottomBarHeight
    Rectangle { y: 0; width: parent.width; height: Style.hairlineWidth; color: Style.hairline }
    PageButtons { id: pager; anchors.centerIn: parent; target: view; showIndicator: true }
}
```

## Tests Added or Updated

- `tests/qml/tst_calendar_flow.qml::test_the_settings_page_can_be_paged_to_every_row` (window 800 x 600, so the page overflows): the list is named `settingsView`; page up is disabled and page down enabled at the top; "About, licenses" starts out of sight; paging down reaches the last page, where About lies fully inside the view and page down is disabled; the view ends above the page buttons (nothing hidden behind them); paging up returns to `contentY == 0`.
  Observed failing before the change (`the settings list has no name settingsView`), passing after.

## Local Verification

- `ctest --test-dir build-host -R calendar_flow`: 25 cases pass (the new one included); the older Settings tests are unchanged and pass.
- `scripts/check-ui-style.sh .` exit 0; `scripts/extract-strings.py --check`: 519 strings, 519 translations, 0 problems ("Page up" / "Page down" were already translated); `scripts/check-version-bump.sh`: passed (0.4.2, base 0.4.1).
- Full gate `scripts/pre-merge.sh`: see the result recorded by `/speckit-bug-test`.
- Not checked: the look and feel on the reader's 1404 x 1872 screen (DT-SET-1, unverified).

## Deviations from Assessment

- The test needed `wait(100)` after creating the page (the column lays out its rows when the page is first shown) and compares the bottom of the view with the **centre** of the page buttons, because the 96 px touch area of the buttons overhangs their 92 px bar by 2 px (the same on every page that uses `PageButtons`). Both are test details; the fix is as proposed.
- The open question of the assessment was answered with the proposed default: the "n / total" indicator is shown.

## Follow-ups

- Package 0.4.2 (rebuild the ARM binary, `scripts/package-device.sh`), install and try on the reader (DT-SET-1), then tag `v0.4.2` and publish on the owner's go, as for 0.4.1.
- The Settings page is long; if it grows further, a grouping or a second page would be better than more paging. Not needed now.
