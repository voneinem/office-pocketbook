#!/usr/bin/env bash
# `pbo-probe --imap-selftest` against the demo server (pbo_fake_imap) on the host (T053): every check passes, the
# output names each check, and it never contains message data (subjects, senders, bodies, addresses).
#
#   tests/launcher/test_imap_selftest.sh <pbo-probe> <pbo_fake_imap>
set -uo pipefail

PROBE="${1:?usage: test_imap_selftest.sh <pbo-probe> <pbo_fake_imap>}"
SERVER="${2:?usage: test_imap_selftest.sh <pbo-probe> <pbo_fake_imap>}"
command -v openssl >/dev/null 2>&1 || { echo "skipped: the openssl command is not available"; exit 77; }
tmp="$(mktemp -d)"; server_pid=""
cleanup() { [ -n "$server_pid" ] && kill "$server_pid" 2>/dev/null; rm -rf "$tmp"; }
trap cleanup EXIT
fail=0
check() { if ! "$@"; then echo "FAIL: $*" >&2; fail=1; fi; }

"$SERVER" --ca-out "$tmp/ca.pem" > "$tmp/server.out" 2> "$tmp/server.err" &
server_pid=$!
for _ in $(seq 1 50); do grep -q '^host=' "$tmp/server.out" 2>/dev/null && break; sleep 0.1; done
port="$(sed -n 's/.*port=\([0-9]*\).*/\1/p' "$tmp/server.out")"
[ -n "$port" ] || { echo "FAIL: the demo server did not start"; cat "$tmp/server.err"; exit 1; }

out="$("$PROBE" --imap-selftest localhost "$port" "$tmp/ca.pem" --bad-login --append Drafts 2>&1)"; rc=$?
echo "$out"
check [ "$rc" -eq 0 ]
for name in connect capability list status search fetch_header_literal fetch_missing_paren session_in_sync fetch_body_url fetch_body_limit wrong_password append; do
    check grep -q "^PASS $name\$" <<<"$out"
done
check grep -q '^INFO multi_literal_fetch ' <<<"$out"        # characterization of the libcurl behavior (research R-01)
if grep -q '^FAIL' <<<"$out"; then echo "FAIL: a check failed" >&2; fail=1; fi
# no message data, no credentials
for secret in "Demo message" "Planung" "Rechnung" "@example.org" "pw-for-tests" "alice"; do
    if grep -qF -- "$secret" <<<"$out"; then echo "FAIL: the output contains '$secret'" >&2; fail=1; fi
done

# against a closed port: a clean failure, not a hang or crash
bad="$("$PROBE" --imap-selftest localhost 1 "$tmp/ca.pem" 2>&1)"; brc=$?
check [ "$brc" -ne 0 ]
check grep -q '^FAIL connect ' <<<"$bad"

[ "$fail" -eq 0 ] && echo "imap self-test passed"
exit "$fail"
