# Bug Assessment: Settings page has no paging, lower rows are unreachable

- **Slug**: settings-page-no-paging
- **Created**: 2026-10-25
- **Source**: pasted text (owner's report from the reader, with a screenshot of the Settings page)
- **Verdict**: valid
- **Severity**: medium

## Report (verbatim or summarized)

The Settings page of PB Office (`qml/pages/SettingsPage.qml`) cannot be scrolled and has no page buttons, so everything below the "Konten" row (the rest of "Adressbücher", the other sections, and "About, licenses" with the version) is unreachable on the InkPad One. Seen on the reader on 2026-10-25 with 0.4.0 and 0.4.1 (screenshot: the list is cut off at the bottom edge, no up/down buttons). Suspected cause: the page's `Flickable` has `interactive: false` and no `PageButtons`; the contacts rows (0.3.0) made the content taller than the screen. Expected: like other long pages the Settings page shows page up/down buttons and every row can be reached (e-ink rules: no kinetic scrolling, page-based). About is still reachable through the info icon of the message list.

## Symptom

On the reader the Settings page shows only what fits on one screen (1404 x 1872 px). The list is cut at the bottom edge in the middle of the "Adressbücher" section ("Konten", "Nur lesbar"); nothing lets the user reach the rest. Expected: every row of the page can be reached by page buttons, as on the contact, licence and event pages.

## Reproduction

1. Start PB Office on the InkPad One with a configured mail and calendar account (the contacts address-book rows are shown when the calendar server has address books).
2. Open Settings (gear on the Home page or the message list).
3. Try to reach "About, licenses" at the end of the page: there is no scroll bar, no page buttons, and dragging does nothing.

Observed by the owner on 2026-10-25 (0.4.0 and 0.4.1). [NEEDS CLARIFICATION: whether the page already overflowed in 0.2.0 without address books; not needed for the fix.]

## Suspected Code Paths

- `qml/pages/SettingsPage.qml:63-70` — the page's only content holder is `Flickable { anchors { ... bottom: parent.bottom } contentHeight: column.implicitHeight + 40; interactive: false; clip: true }`. With `interactive: false` nothing can move `contentY`, and the page has no `PageButtons` to do it, so any content below the first screen is clipped and unreachable.
- `qml/pages/SettingsPage.qml:164-190` — the "Address books" rows (added in commit 864816d, 0.3.0, +22 lines) and further variable-height rows (special-folder choices, error, storage-full and clock-ahead notes, custom-CA text) make the page taller than one screen.
- `qml/PageButtons.qml` — the existing page-wise "up / down" control for a `Flickable`; moves `contentY` by one screen minus 80 px (or by whole rows with `snap`); `showIndicator` adds "n / total".
- `qml/pages/ContactPage.qml:67-185` — the pattern to follow: a clipped `area` above a `bottomBar` (`Style.bottomBarHeight` = 92) that holds `PageButtons { target: view; showIndicator: true }`; the `Flickable` is `interactive: false` there too.
- `tests/qml/tst_calendar_flow.qml:330` — the one test of the Settings page; the test window is 800 x 600, so the page overflows there as well, but the test only looks up children by `objectName` and never checks whether a row is visible or reachable.

## Root Cause Hypothesis

The page was built as a `Flickable` with dragging switched off (e-ink rule: no kinetic scrolling) but, unlike the other long pages, it was never given the page buttons that replace dragging. It worked while the content fitted on one screen; growth of the page (address books in 0.3.0, possibly earlier) made the lower part unreachable. The test only checks that children exist, so nothing noticed. Confidence: high for the mechanism (the code shows no way to change `contentY`); medium for "since 0.3.0".

## Proposed Remediation

**Preferred**: give the Settings page the same structure as `ContactPage`: put the `Flickable` into a clipped area between the top bar and a new bottom bar, and put `PageButtons { target: <the flickable>; showIndicator: true }` in that bottom bar (no `snap`, because the rows have different heights; the default step of one screen minus 80 px keeps a cut row visible on both pages). Keep `interactive: false` and `boundsBehavior: Flickable.StopAtBounds`. Keep all `objectName`s so existing tests are untouched; name the flickable `settingsView`. Dialogs (`ChoiceDialog`) stay as they are.

**Alternatives**:
- Make the `Flickable` interactive (drag): rejected, kinetic scrolling smears on e-ink and breaks constitution principle VI.
- Convert the page to a `ListView` with `ModelPager`: rejected as far larger than the bug, with a risk to the many row types.
- A "More" link to a second Settings page: rejected, changes the information structure.

**Files likely to change**:
- `qml/pages/SettingsPage.qml` (area, bottom bar, `PageButtons`)
- `tests/qml/tst_calendar_flow.qml` (new test next to line 330)
- `docs/device-test-checklist.md` (a row for the Settings paging, unverified until tried on the reader)
- `VERSION`, `CHANGELOG.md` (0.4.2, PATCH)

**Tests to add or update** (test first, observed failing):
- In the QML flow tests (window 800 x 600, so the page overflows): the Settings page has `pageUp` and `pageDown`; `pageUp` is disabled at the top and `pageDown` is enabled; tapping `pageDown` repeatedly makes `settingsAbout` lie fully inside the visible area of the flickable and disables `pageDown`; `pageUp` returns to the top.
- A guard that the bottom bar does not hide the last row: after the last page `settingsAbout` is not behind the bottom bar.
- `check_translations` must still pass: "Page up" and "Page down" are already translated for `PageButtons`.

## Risks & Considerations

- The viewport shrinks by the 92 px bottom bar; the rows near the page break may be cut (the 80 px overlap shows them on the next page). Acceptable, same as the licence and event pages.
- The page height changes when notes appear (error, storage full, clock), so a page number can change; `PageButtons` follows `contentHeight`.
- The change is in the UI layer only: no core, no schema, no data. It is visible to users, so it is a PATCH release (0.4.2) with a changelog entry; the store package must be rebuilt and reinstalled and the on-reader result stays **unverified** until the owner tries it (DT row).
- Packaging: the new release needs `pbstore-catalog` v0.1.4 or newer again (the tool of 0.4.1 lived in the session scratchpad).

## Open Questions

- [NEEDS CLARIFICATION: should the page show "n / total" between the arrows (as the contact page does) or only the arrows? Default in the fix: show it.]
