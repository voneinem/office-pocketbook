# Bug Verification: Settings page has no paging, lower rows are unreachable

- **Slug**: settings-page-no-paging
- **Tested**: 2026-10-25
- **Assessment**: ./assessment.md
- **Fix**: ./fix.md
- **Result**: verified (host tests 2026-10-25; on the reader by the owner the same day, see below)

## Summary

The fix holds: the new test failed before the change and passes after it, the whole local gate (173 tests, UI style, translations, version bump, secret scan) is green with no regression, and the owner confirmed on the InkPad One (0.4.2, firmware 6.11.1437) that Settings can be paged down to "About, licenses" (log: `about.opened`, exit code 0, no error line).

## Checks Performed

| Check | Command / Action | Result | Notes |
|-------|------------------|--------|-------|
| Reproduction (post-fix), automated equivalent | `ctest --test-dir build-host -R calendar_flow` (`test_the_settings_page_can_be_paged_to_every_row`) | pass | in a 800 x 600 window; failed before the change with "the settings list has no name settingsView" |
| Reproduction (post-fix) on the reader | 0.4.2 installed on the InkPad One (622 files compared, 0 differences); the owner opened Settings and paged down to "About, licenses" | pass | owner: "Works!"; log: launch 15:21:43, `about.opened` 15:21:53, exit code 0, no error line, `pboffice.db` untouched |
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

- The owner's confirmation was "Works!"; the number of pages, the look of the page break (a row cut at the break shows on both pages) and the timing were not recorded.
- The host test window is 800 x 600 and the page overflows there by only about 38 px; the test proves reachability and the layout rule, not the reader's page count.

## Recommendation

Close the bug: verified on the host and on the reader. Merge the fix, tag `v0.4.2` after the merge and publish the package on the owner's go.
