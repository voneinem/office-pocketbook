#!/usr/bin/env bash
# scripts/package-device.sh on the host, with a fake ARM "binary" (a text file holding the version) and test doubles
# for the store's tool `pbstore-catalog`:
#   - the package gets a manifest.json at its root and nothing but applications/ besides it,
#   - the real tool (when installed) accepts the package,
#   - the tool is found through PBSTORE_CATALOG or PATH, a missing tool is reported before any work,
#   - two packs must be identical, the check must pass, and a failed build leaves no ZIP behind,
#   - the files under applications/ are the ones of release 0.4.0 (tests/packaging/applications.expected).
#
#   tests/packaging/test_package_device.sh [repo-root]
# Exit 77 (CTest: skipped) only when nothing could run.
set -euo pipefail

ROOT="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"
SCRIPT="$ROOT/scripts/package-device.sh"
tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
fail=0 ran=0 skipped=0
VERSION="$(head -n 1 "$ROOT/VERSION" | tr -d '[:space:]')"

check() { if ! "$@"; then echo "FAIL: $*" >&2; fail=1; fi; }
check_not() { if "$@"; then echo "FAIL (expected to fail): $*" >&2; fail=1; fi; }
case_start() { echo "== $1"; ran=$((ran + 1)); }
skip() { echo "SKIP: $1"; skipped=$((skipped + 1)); ran=$((ran - 1)); }

# The real tool: PBSTORE_CATALOG if it names an executable, else the first pbstore-catalog on PATH; empty when absent.
real_tool() {
    if [ -n "${PBSTORE_CATALOG:-}" ] && [ -x "$PBSTORE_CATALOG" ]; then echo "$PBSTORE_CATALOG"
    else command -v pbstore-catalog || true; fi
}

# Remember the real tool, then hide the variable: the cases below choose the tool through PATH or set the variable themselves.
REAL_TOOL="$(real_tool)"
unset PBSTORE_CATALOG
# The image plugins are found through these; the cases below expect a package without them.
unset POCKETBOOK_SDK PBO_QT_IMAGE_PLUGINS

# A test double for the tool: $1 = directory, $2 = file name, $3 = shell body (sees "$@").
stub() { mkdir -p "$1"; printf '#!/bin/sh\n%s\n' "$3" > "$1/$2"; chmod +x "$1/$2"; }

# Body of a well-behaved double: records its calls in $STUB_LOG, keeps the tree it was asked to pack in $STUB_SEEN,
# writes a fixed placeholder as the ZIP and accepts every package.
GOOD_STUB='echo "$*" >> "$STUB_LOG"
case "$1" in
    pack) rm -rf "$STUB_SEEN"; cp -R "$2" "$STUB_SEEN"; echo "stub zip" > "$4" ;;
    check-package) exit 0 ;;
    *) exit 2 ;;
esac'

# build <out-dir> [version]: run the script on a fake binary holding that version (default: VERSION);
# sets rc, and stdout/stderr in $tmp/stdout and $tmp/stderr. The caller sets PATH and PBSTORE_CATALOG as needed.
build() {
    local out="$1" ver="${2:-}"
    printf 'fake pboffice %s\n' "${ver:-$VERSION}" > "$tmp/pboffice.fake"
    rc=0
    "$SCRIPT" "$tmp/pboffice.fake" "$ver" "$out" > "$tmp/stdout" 2> "$tmp/stderr" || rc=$?
}

# json_get <file> <dotted.key>: print one value of a JSON file
json_get() { python3 -I -c 'import json,sys; d=json.load(open(sys.argv[1]))
for k in sys.argv[2].split("."): d=d[k]
print(d)' "$1" "$2"; }

# new_stub_env <name>: a fresh directory with a good double named pbstore-catalog; exports STUB_LOG and STUB_SEEN
new_stub_env() {
    local dir="$tmp/$1"
    rm -rf "$dir"; mkdir -p "$dir"
    export STUB_LOG="$dir/calls" STUB_SEEN="$dir/seen"
    : > "$STUB_LOG"
    stub "$dir/bin" pbstore-catalog "$GOOD_STUB"
    STUB_BIN="$dir/bin"; STUB_OUT="$dir/out"
}

# ---- User Story 1 -----------------------------------------------------------------------------------------------

case_start "package passes the store check"
tool="$REAL_TOOL"
if [ -z "$tool" ]; then
    skip "the real pbstore-catalog is not installed (PATH / PBSTORE_CATALOG)"
else
    # until the lookup is implemented the script only searches PATH, so the tool's directory goes there
    PATH="$(dirname "$tool"):$PATH" build "$tmp/real-out"
    check [ "$rc" = 0 ]
    zip="$tmp/real-out/pboffice-inkpad-one-$VERSION.zip"
    check [ -f "$zip" ]
    check "$tool" check-package "$zip" > /dev/null
fi

case_start "top level and manifest"
new_stub_env manifest
PATH="$STUB_BIN:$PATH" build "$STUB_OUT"
check [ "$rc" = 0 ]
check [ -d "$STUB_SEEN" ]
check [ "$(ls -A "$STUB_SEEN" 2> /dev/null | tr '\n' ' ')" = "applications manifest.json " ]
if [ -f "$STUB_SEEN/manifest.json" ]; then
    m="$STUB_SEEN/manifest.json"
    check [ "$(json_get "$m" schema_version)" = "1.0" ]
    check [ "$(json_get "$m" id)" = "pboffice" ]
    check [ "$(json_get "$m" name)" = "PB Office" ]
    check [ "$(json_get "$m" version)" = "$VERSION" ]
    check [ "$(json_get "$m" min_firmware)" = "6.11.1437" ]
    check [ "$(json_get "$m" target_device)" = "InkPad One" ]
    check [ "$(json_get "$m" menu_entry.id)" = "U_PbOffice" ]
    check [ "$(json_get "$m" menu_entry.title)" = "PB Office" ]
    check [ "$(json_get "$m" menu_entry.path)" = "applications/pboffice.app" ]
    check [ "$(json_get "$m" menu_entry.icon)" = "applications/icons/pboffice.bmp" ]
    check [ "$(json_get "$m" menu_entry.focused_icon)" = "applications/icons/pboffice_f.bmp" ]
    check [ "$(json_get "$m" menu_entry.group)" = "@General" ]
    check [ "$(json_get "$m" data_dir)" = "system/pboffice" ]
    check_not python3 -I -c 'import json,sys; sys.exit(0 if "files" in json.load(open(sys.argv[1])) else 1)' "$m"
    check_not grep -q '@[A-Z_]*@' "$m"
else
    echo "FAIL: no manifest.json was staged" >&2; fail=1
fi
check [ -z "$(find "$STUB_SEEN" -name 'INSTALL.md' 2> /dev/null)" ]

case_start "version comes from one source"
new_stub_env version
PATH="$STUB_BIN:$PATH" build "$STUB_OUT" 9.8.7
check [ "$rc" = 0 ]
check [ -f "$STUB_OUT/pboffice-inkpad-one-9.8.7.zip" ]
if [ -f "$STUB_SEEN/manifest.json" ]; then
    check [ "$(json_get "$STUB_SEEN/manifest.json" version)" = "9.8.7" ]
else
    echo "FAIL: no manifest.json was staged" >&2; fail=1
fi

# ---- User Story 2 -----------------------------------------------------------------------------------------------

# A PATH holding only what the script itself needs (no pbstore-catalog)
minpath="$tmp/minpath"; mkdir -p "$minpath"
for c in env bash sed grep head tr sort mktemp install mkdir rm cmp cat dirname basename mv cp stat find sha256sum cut chmod; do
    ln -sf "$(command -v "$c")" "$minpath/$c"
done
no_zip() { [ -z "$(find "$1" -name '*.zip' 2> /dev/null)" ]; }

case_start "tool missing"
new_stub_env missing
PBSTORE_CATALOG="" PATH="$minpath" build "$STUB_OUT"
check [ "$rc" = 1 ]
check grep -q 'pbstore-catalog not found' "$tmp/stderr"
check grep -q 'PBSTORE_CATALOG' "$tmp/stderr"
check grep -q 'PATH' "$tmp/stderr"
check_not grep -q 'missing input' "$tmp/stderr"
check no_zip "$STUB_OUT"

case_start "environment variable"
new_stub_env envvar
stub "$tmp/envvar/tools" mytool "$GOOD_STUB"
PBSTORE_CATALOG="$tmp/envvar/tools/mytool" PATH="$minpath" build "$STUB_OUT"
check [ "$rc" = 0 ]
check [ -s "$STUB_LOG" ]
check [ -f "$STUB_OUT/pboffice-inkpad-one-$VERSION.zip" ]
new_stub_env envbad
PBSTORE_CATALOG="$tmp/envbad/nonexistent" PATH="$STUB_BIN:$PATH" build "$STUB_OUT"
check [ "$rc" = 1 ]
check grep -q 'PBSTORE_CATALOG is set but is not an executable file' "$tmp/stderr"
check [ ! -s "$STUB_LOG" ]
check no_zip "$STUB_OUT"

case_start "reproducibility"
new_stub_env differ
stub "$tmp/differ/bin" pbstore-catalog 'case "$1" in
    pack) echo "$$ $(date +%s%N) $RANDOM" > "$4" ;;
    check-package) exit 0 ;;
esac'
PATH="$tmp/differ/bin:$PATH" build "$STUB_OUT"
check [ "$rc" = 1 ]
check grep -q 'package is not reproducible' "$tmp/stderr"
check no_zip "$STUB_OUT"
new_stub_env same
PATH="$STUB_BIN:$PATH" build "$STUB_OUT"
check [ "$rc" = 0 ]
check [ "$(grep -c '^pack ' "$STUB_LOG")" = 2 ]
check [ "$(grep -c '^check-package ' "$STUB_LOG")" = 1 ]
check grep -Eq '^reproducible: two packs are identical \(sha256 [0-9a-f]{64}\)$' "$tmp/stderr"
check [ "$(cat "$tmp/stdout")" = "$STUB_OUT/pboffice-inkpad-one-$VERSION.zip" ]

case_start "check failure leaves nothing"
new_stub_env badcheck
stub "$tmp/badcheck/bin" pbstore-catalog 'case "$1" in
    pack) echo "stub zip" > "$4" ;;
    check-package) echo "error[E-PKG-04] x: y" >&2; exit 1 ;;
esac'
mkdir -p "$STUB_OUT"; echo old > "$STUB_OUT/pboffice-inkpad-one-$VERSION.zip"
PATH="$tmp/badcheck/bin:$PATH" build "$STUB_OUT"
check [ "$rc" = 1 ]
check grep -q 'error\[E-PKG-04\] x: y' "$tmp/stderr"
check grep -q 'package check failed' "$tmp/stderr"
check no_zip "$STUB_OUT"

case_start "pack fails"
new_stub_env badpack
stub "$tmp/badpack/bin" pbstore-catalog 'case "$1" in
    pack) echo "error[E-PKG-07] manifest.json: x" >&2; exit 1 ;;
    check-package) exit 0 ;;
esac'
PATH="$tmp/badpack/bin:$PATH" build "$STUB_OUT"
check [ "$rc" = 1 ]
check grep -q 'pbstore-catalog pack failed' "$tmp/stderr"
check no_zip "$STUB_OUT"

case_start "bad inputs"
new_stub_env badin
PATH="$STUB_BIN:$PATH" build "$STUB_OUT" 1.2
check [ "$rc" = 1 ]
check grep -q 'version is not MAJOR.MINOR.PATCH' "$tmp/stderr"
PATH="$STUB_BIN:$PATH" build "$STUB_OUT" 1.2.x
check [ "$rc" = 1 ]
check grep -q 'version is not MAJOR.MINOR.PATCH' "$tmp/stderr"
# a different ROOT: scripts/ and packaging/ copied, the other inputs linked from the repository
troot="$tmp/troot"; rm -rf "$troot"; mkdir -p "$troot"
cp -R "$ROOT/scripts" "$ROOT/packaging" "$troot/"
for d in platform third_party res licenses VERSION LICENSE; do ln -s "$ROOT/$d" "$troot/$d"; done
sed -i 's/"InkPad One"/"InkPad One @X@"/' "$troot/packaging/manifest.template.json"
SCRIPT="$troot/scripts/package-device.sh" PATH="$STUB_BIN:$PATH" build "$STUB_OUT"
check [ "$rc" = 1 ]
check grep -q 'unfilled placeholder: @X@' "$tmp/stderr"
rm "$troot/packaging/manifest.template.json"
SCRIPT="$troot/scripts/package-device.sh" PATH="$STUB_BIN:$PATH" build "$STUB_OUT"
check [ "$rc" = 1 ]
check grep -q 'missing input: .*manifest.template.json' "$tmp/stderr"
check no_zip "$STUB_OUT"

case_start "size budget"
new_stub_env size
PBO_MAX_ZIP_BYTES=5 PATH="$STUB_BIN:$PATH" build "$STUB_OUT"
check [ "$rc" = 1 ]
check grep -q 'larger than the budget' "$tmp/stderr"
check no_zip "$STUB_OUT"

case_start "existing guards"
new_stub_env guards
printf 'a binary of another version 0.0.1\n' > "$tmp/other.fake"
rc=0; PATH="$STUB_BIN:$PATH" "$SCRIPT" "$tmp/other.fake" "" "$STUB_OUT" > /dev/null 2> "$tmp/stderr" || rc=$?
check [ "$rc" = 1 ]
check grep -q 'version mismatch' "$tmp/stderr"
check no_zip "$STUB_OUT"
# a license text named by licenses/manifest.txt that is missing from the (copied) repository
groot="$tmp/groot"; rm -rf "$groot"; mkdir -p "$groot"
cp -R "$ROOT/scripts" "$ROOT/packaging" "$ROOT/licenses" "$groot/"
for d in platform third_party res VERSION LICENSE; do ln -s "$ROOT/$d" "$groot/$d"; done
lic="$(sed -n 's/^text:[[:space:]]*//p' "$ROOT/licenses/manifest.txt" | tr ',' '\n' | sed 's/^[[:space:]]*//; s/[[:space:]]*$//' | grep -v '^$' | head -n 1)"
if [ -n "$lic" ] && [ -f "$groot/$lic" ]; then
    rm "$groot/$lic"
    SCRIPT="$groot/scripts/package-device.sh" PATH="$STUB_BIN:$PATH" build "$STUB_OUT"
    check [ "$rc" = 1 ]
    check grep -q 'license text named by licenses/manifest.txt is missing' "$tmp/stderr"
    check no_zip "$STUB_OUT"
else
    echo "FAIL: could not pick a license text to remove ($lic)" >&2; fail=1
fi

# ---- User Story 3 -----------------------------------------------------------------------------------------------

case_start "applications set unchanged (guard)"
new_stub_env applist
PATH="$STUB_BIN:$PATH" build "$STUB_OUT"
check [ "$rc" = 0 ]
check diff <(cd "$STUB_SEEN" && find applications -type f | LC_ALL=C sort) "$ROOT/tests/packaging/applications.expected"
check [ -z "$(find "$STUB_SEEN" -name 'INSTALL.md')" ]

case_start "probe files"
new_stub_env probes
printf 'fake probe\n' > "$tmp/pbo-probe.fake"; printf 'fake handoff probe\n' > "$tmp/pbo-handoff.fake"
printf 'fake pboffice %s\n' "$VERSION" > "$tmp/pboffice.fake"
rc=0; PATH="$STUB_BIN:$PATH" "$SCRIPT" "$tmp/pboffice.fake" "" "$STUB_OUT" "$tmp/pbo-probe.fake" "$tmp/pbo-handoff.fake" > /dev/null 2> "$tmp/stderr" || rc=$?
check [ "$rc" = 0 ]
check diff <(cd "$STUB_SEEN" && find applications -type f | LC_ALL=C sort) \
    <({ cat "$ROOT/tests/packaging/applications.expected"; printf '%s\n' applications/pbo-probe.bin applications/pbo-handoff-probe.bin applications/pbo-handoff-target.app; } | LC_ALL=C sort)

case_start "instructions stay reachable"
check grep -q '](INSTALL.md)' "$ROOT/README.md"
check_not grep -q 'INSTALL.md.*part of the package' "$ROOT/README.md"
check grep -q 'merging it with the existing' "$ROOT/INSTALL.md"
check grep -q 'manifest.json' "$ROOT/INSTALL.md"

# ---- end ---------------------------------------------------------------------------------------------------------
if [ "$fail" != 0 ]; then echo "FAILED"; exit 1; fi
if [ "$ran" = 0 ] && [ "$skipped" != 0 ]; then echo "all cases skipped"; exit 77; fi
echo "ok ($ran cases, $skipped skipped)"
