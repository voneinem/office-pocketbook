# Bug Verification: Settings page has no paging, lower rows are unreachable

- **Slug**: settings-page-no-paging
- **Tested**: 2026-10-25
- **Assessment**: ./assessment.md
- **Fix**: ./fix.md
- **Result**: partial (verified on the host; the original symptom was seen on the reader and has **not** been re-tried there)

## Summary

The fix holds on the host: the new test failed before the change and passes after it, and the whole local gate (173 tests, UI style, translations, version bump, secret scan) is green with no regression. The assessment's reproduction is "open Settings on the InkPad One and try to reach About"; that was not repeated on the reader (it needs the 0.4.2 package installed), so the result is `partial`, not `verified`.

## Checks Performed

| Check | Command / Action | Result | Notes |
|-------|------------------|--------|-------|
| Reproduction (post-fix), automated equivalent | `ctest --test-dir build-host -R calendar_flow` (`test_the_settings_page_can_be_paged_to_every_row`) | pass | in a 800 x 600 window; failed before the change with "the settings list has no name settingsView" |
| Reproduction (post-fix) on the reader | open Settings on the InkPad One, page down to "About, licenses" | not-run | needs the 0.4.2 package on the reader (DT-SET-1 stays unverified) |
| New / updated tests | `ctest --test-dir build-host -R calendar_flow` | pass | 25 cases, the older Settings tests unchanged and passing |
| Regression suite | `scripts/pre-merge.sh` | pass | 173 of 173 tests, secret scan clean |
| UI style | `scripts/check-ui-style.sh .` | pass | exit 0 |
| Translations | `python3 scripts/extract-strings.py --check` | pass | 519 strings, 519 translations, 0 problems |
| Version bump | `scripts/check-version-bump.sh` | pass | VERSION 0.4.2, base 0.4.1, CHANGELOG entry present |

## Output Excerpts

```text
100% tests passed, 0 tests failed out of 173
version check passed (VERSION 0.4.2, base 0.4.1)
pre-merge checks passed
```

## Residual Risks

- The reader's screen is 1404 x 1872; the host test window is 800 x 600 and the page overflows there by only about 38 px (no accounts), so the number of pages on the reader (probably two) and the look of the page break are untested.
- The step of one page is the screen height minus 80 px; a row at the break can be cut and then shows on both pages. Same behavior as the other pages with `PageButtons`; not checked on the Settings rows.
- No ARM build or package of 0.4.2 has been made yet.

## Recommendation

Merge the fix and release 0.4.2 only after the reader check: build the ARM binary and the package (`scripts/package-device.sh`), install on the owner's go, open Settings, page down to "About, licenses", and record the date and result in DT-SET-1. If the page break or the buttons look wrong on the reader, reopen with the screenshot and re-run `/speckit-bug-assess`.
