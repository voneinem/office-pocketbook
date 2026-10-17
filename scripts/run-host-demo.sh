#!/usr/bin/env bash
# Try the app on the host against a demo mailbox, without any real account (quickstart.md, section 3).
#
#   scripts/run-host-demo.sh [build-dir]        build-dir defaults to build-host
#
# Starts tests/tools/pbo_fake_imap (a fake IMAP server with a throw-away certificate authority, UNVERIFIED against real
# servers), gives the app that authority as its custom-ca.pem in a throw-away data folder, and starts the desktop app.
# Enter the values the script prints in the account form: Test, Save, and the inbox appears after the first sync.
# Everything is removed when the app closes.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${1:-$ROOT/build-host}"
SERVER="$BUILD/tests/pbo_fake_imap"
APP="$BUILD/app/pboffice"
for f in "$SERVER" "$APP"; do
    [ -x "$f" ] || { echo "missing $f - build the host target first: cmake -S . -B build-host -G Ninja && cmake --build build-host" >&2; exit 1; }
done

tmp="$(mktemp -d)"
server_pid=""
cleanup() { [ -n "$server_pid" ] && kill "$server_pid" 2>/dev/null || true; rm -rf "$tmp"; }
trap cleanup EXIT

mkdir -p "$tmp/data"
"$SERVER" --ca-out "$tmp/data/custom-ca.pem" > "$tmp/server.out" 2> "$tmp/server.err" &
server_pid=$!
for _ in $(seq 1 50); do
    grep -q '^host=' "$tmp/server.out" 2>/dev/null && break
    kill -0 "$server_pid" 2>/dev/null || { echo "the demo server did not start:" >&2; cat "$tmp/server.err" >&2; exit 1; }
    sleep 0.1
done
info="$(grep '^host=' "$tmp/server.out" || true)"
[ -n "$info" ] || { echo "the demo server did not report its port" >&2; exit 1; }

echo "Demo mail server (fake, for trying the app only):"
echo "  $info"
echo "In the account form use: server = localhost, the port above, encryption = TLS, the user name and password above,"
echo "any address such as alice@example.org. Outgoing server = localhost with the smtp port above (the connection test tries both servers)."
echo
PBO_DATA_DIR="$tmp/data" "$APP"
