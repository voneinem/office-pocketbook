#!/usr/bin/env bash
# Every test that runs against an in-process fake server must say so (constitution Principle IX: behavior that
# needs a real server stays "unverified"). Rule: a test source that includes tests/support/fake_*_server.h must
# invoke PBO_FAKE_SERVER_TEST().
#
#   scripts/check-fake-markers.sh [repo-root]    scan (default: this repository)
#   scripts/check-fake-markers.sh --self-test    prove that the rule detects a violation
set -euo pipefail

scan() {  # scan <root> : prints findings, returns 1 if any
    local root="$1" bad=0 f
    while IFS= read -r -d '' f; do
        # the include must be a real include; the marker must be code, not a comment
        if grep -Eq '^[[:space:]]*#include[[:space:]]+"fake_[a-z]+_server\.h"' "$f" &&
           ! grep -Eq '^[[:space:]]*PBO_FAKE_SERVER_TEST\(\)' "$f"; then
            echo "FAKE-MARKER ${f#"$root"/}: includes a fake server but does not invoke PBO_FAKE_SERVER_TEST()"
            bad=1
        fi
    done < <(find "$root/tests" -type f -name 'test_*.cpp' -not -path '*/templates/*' -print0 2>/dev/null)
    return $bad
}

if [ "${1:-}" = "--self-test" ]; then
    tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
    mkdir -p "$tmp/tests/unit"
    fail=0
    check() {  # check <pass|fail> <description>
        local rc=0
        scan "$tmp" >/dev/null 2>&1 || rc=$?
        if { [ "$1" = pass ] && [ $rc -eq 0 ]; } || { [ "$1" = fail ] && [ $rc -ne 0 ]; }; then echo "ok   $2"; else echo "FAIL $2 (rc=$rc)"; fail=1; fi
    }
    printf '#include "test_support.h"\nint main() { return 0; }\n' > "$tmp/tests/unit/test_plain.cpp"
    check pass "a test without a fake server is fine"
    printf '#include "fake_imap_server.h"\nPBO_FAKE_SERVER_TEST();\nint main() { return 0; }\n' > "$tmp/tests/unit/test_marked.cpp"
    check pass "a fake-server test with the marker is fine"
    printf '#include "fake_imap_server.h"\nint main() { return 0; }\n' > "$tmp/tests/unit/test_unmarked.cpp"
    check fail "a fake-server test without the marker is reported"
    rm "$tmp/tests/unit/test_unmarked.cpp"
    printf '#include "fake_caldav_server.h"\nint main() { return 0; }\n' > "$tmp/tests/unit/test_unmarked_dav.cpp"
    check fail "the rule covers the CalDAV fake as well"
    printf '#include "fake_smtp_server.h"\n// PBO_FAKE_SERVER_TEST mentioned only in a comment\nint main() { return 0; }\n' > "$tmp/tests/unit/test_comment.cpp"
    check fail "a marker that only appears in a comment does not count"
    exit $fail
fi

ROOT="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
scan "$ROOT"
