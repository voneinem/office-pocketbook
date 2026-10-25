#!/usr/bin/env bash
#
# Assemble the InkPad One install bundle from a freshly built ARM binary.
# Adapted from InkFeed's packaging script (MIT, see NOTICE).
#
#   scripts/package-device.sh <built-arm-elf> [version] [out-dir] [built-arm-pbo-probe] [built-arm-pbo-handoff-probe]
#   scripts/package-device.sh --check-size <zip> [limit-bytes]   fail when the zip is larger than the budget (25 MB, research R-19)
#   scripts/package-device.sh --self-test                        prove that the size check works
#
# Produces <out-dir>/pboffice-inkpad-one-<version>.zip, laid out as the PB Store package contract asks
# (appstore-pocketbook): manifest.json and the applications/ tree at the top level, nothing else. A user can
# unzip it straight onto the reader's ext1 partition (copy applications/):
#
#   manifest.json                      created at build time from packaging/manifest.template.json
#   applications/pboffice.bin           the app (ARM ELF)
#   applications/pboffice.app           the launcher (shell script)
#   applications/pbo-probe.bin          the device probe (only when the 4th argument is given)
#   applications/pboffice-probe.sh      the probe script the launcher runs when run-probe exists (INSTALL.md)
#   applications/pbo-handoff-probe.bin, pbo-handoff-target.app   the hand-over probe: a test sender and a stand-in app (only when the 5th argument is given; INSTALL.md)
#   applications/pboffice-cacert.pem    bundled CA certificates (used when the device has none)
#   applications/pboffice-zoneinfo/     IANA time zone data (compact TZif files) and its VERSION file, for the calendar
#   applications/pboffice-plugins/imageformats/   the Qt JPEG and GIF image plugins of the SDK (pictures of mails; a copy the app falls back on when the
#                                      reader's own Qt has none): taken from $PBO_QT_IMAGE_PLUGINS or $POCKETBOOK_SDK/local/qt6/plugins/imageformats; skipped with a note when neither exists
#   applications/pboffice-emoji.ttf     monochrome emoji font (Noto Emoji, SIL OFL 1.1)
#   applications/pboffice-emoji-OFL.txt the font's license text
#   applications/pboffice-licenses/     license texts for the About screens: licenses/manifest.txt and every
#                                      file the manifest names, at the same relative path as in the repository
#   applications/icons/*.bmp           launcher icons
#
# The install / registration steps (INSTALL.md) are not part of the package; they live in the repository.
# The zip is written by `pbstore-catalog pack`, which adds the file list to the manifest. The tool comes from
# the appstore-pocketbook repository (tag v0.1.4 or newer); it is found through the environment variable
# PBSTORE_CATALOG (the tool's path) or else on PATH, and a missing tool stops the build before any work. The
# package is packed twice and the two results must be identical, then it is checked with
# `pbstore-catalog check-package` and against the size budget; only a package that passes is put into <out-dir>.
#
# The version defaults to the contents of the VERSION file, the same source CMakeLists.txt uses for the
# version compiled into the app. The script fails when the binary does not contain that version string,
# so the zip name and the About screen cannot disagree.
set -euo pipefail

# The size budget of the package (research R-19, K4): the zip must stay small enough for a quick copy to the reader.
MAX_ZIP_BYTES="${PBO_MAX_ZIP_BYTES:-26214400}"   # 25 MB

# check_zip_size <zip> [limit]: prints the size; returns 1 and says so when it is above the limit.
check_zip_size() {
    local zip="$1" limit="${2:-$MAX_ZIP_BYTES}" size
    size="$(stat -c %s "$zip")"
    echo "package size: $size bytes (limit $limit)"
    if [ "$size" -gt "$limit" ]; then
        echo "the package is larger than the budget of $limit bytes: $zip" >&2
        return 1
    fi
    return 0
}

if [ "${1:-}" = "--check-size" ]; then
    check_zip_size "${2:?usage: package-device.sh --check-size <zip> [limit-bytes]}" "${3:-$MAX_ZIP_BYTES}"
    exit $?
fi

if [ "${1:-}" = "--self-test" ]; then
    tmp="$(mktemp -d)"
    trap 'rm -rf "$tmp"' EXIT
    head -c 1000 /dev/zero > "$tmp/small.zip"
    head -c 3000 /dev/zero > "$tmp/big.zip"
    check_zip_size "$tmp/small.zip" 2000 >/dev/null || { echo "self-test failed: a small package was refused" >&2; exit 1; }
    if check_zip_size "$tmp/big.zip" 2000 >/dev/null 2>&1; then echo "self-test failed: a package over the limit was accepted" >&2; exit 1; fi
    check_zip_size "$tmp/small.zip" 1000 >/dev/null || { echo "self-test failed: a package exactly at the limit was refused" >&2; exit 1; }
    echo "package-device.sh self-test passed"
    exit 0
fi

BIN="${1:?usage: package-device.sh <built-arm-elf> [version] [out-dir]}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# The store's tool: PBSTORE_CATALOG if set, else PATH. Looked up first so a missing tool is reported before any work.
if [ -n "${PBSTORE_CATALOG:-}" ]; then
    [ -f "$PBSTORE_CATALOG" ] && [ -x "$PBSTORE_CATALOG" ] || {
        echo "PBSTORE_CATALOG is set but is not an executable file: $PBSTORE_CATALOG" >&2; exit 1; }
    PBSTORE="$PBSTORE_CATALOG"
else
    PBSTORE="$(command -v pbstore-catalog || true)"
    [ -n "$PBSTORE" ] || {
        echo "pbstore-catalog not found (PBSTORE_CATALOG is not set; searched PATH)." \
             "Put it on PATH or set PBSTORE_CATALOG to the tool (build it from the appstore-pocketbook repository, tag v0.1.4 or newer)." >&2
        exit 1; }
fi

VERSION="${2:-$(head -n 1 "$ROOT/VERSION" 2>/dev/null | tr -d '[:space:]')}"
[ -n "$VERSION" ] || { echo "no version: pass one or create $ROOT/VERSION" >&2; exit 1; }
[[ "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || { echo "version is not MAJOR.MINOR.PATCH: $VERSION" >&2; exit 1; }
OUT_DIR="${3:-dist}"
PROBE_BIN="${4:-}"
HANDOFF_BIN="${5:-}"

for f in "$BIN" \
         "$ROOT/platform/pocketbook/pboffice.app" \
         "$ROOT/third_party/cacert/cacert.pem" "$ROOT/third_party/tzdata/VERSION" \
         "$ROOT/third_party/noto-emoji/NotoEmoji.ttf" "$ROOT/third_party/noto-emoji/OFL.txt" \
         "$ROOT/res/icons/pboffice.bmp" "$ROOT/res/icons/pboffice_f.bmp" \
         "$ROOT/packaging/manifest.template.json"; do
    [ -f "$f" ] || { echo "missing input: $f" >&2; exit 1; }
done

MANIFEST="$ROOT/licenses/manifest.txt"
[ -f "$MANIFEST" ] || { echo "missing input: $MANIFEST" >&2; exit 1; }

# The About screen shows the version compiled into the binary; it must be the one the zip is named after.
if ! grep -aqF -- "$VERSION" "$BIN"; then
    echo "version mismatch: the binary does not contain the version string '$VERSION'." >&2
    echo "Rebuild (cmake reads the version from the VERSION file) or pass the version the binary was built with." >&2
    exit 1
fi

# Every license text the manifest names (comma-separated `text:` values), relative to the repository root.
license_files="$(sed -n 's/^text:[[:space:]]*//p' "$MANIFEST" | tr ',' '\n' | sed 's/^[[:space:]]*//; s/[[:space:]]*$//' | grep -v '^$' | sort -u)"
for t in $license_files; do
    [ -f "$ROOT/$t" ] || { echo "license text named by licenses/manifest.txt is missing: $t" >&2; exit 1; }
done

OUT_DIR="$(mkdir -p "$OUT_DIR" && cd "$OUT_DIR" && pwd)"
ZIP="$OUT_DIR/pboffice-inkpad-one-$VERSION.zip"

stage="$(mktemp -d)"
work="$(mktemp -d)"
trap 'rm -rf "$stage" "$work"' EXIT
apps="$stage/applications"
mkdir -p "$apps/icons"

install -m 777 "$BIN"                                   "$apps/pboffice.bin"
install -m 777 "$ROOT/platform/pocketbook/pboffice.app"  "$apps/pboffice.app"
install -m 777 "$ROOT/scripts/probe-device.sh"          "$apps/pboffice-probe.sh"
if [ -n "$PROBE_BIN" ]; then
    [ -f "$PROBE_BIN" ] || { echo "missing input: $PROBE_BIN" >&2; exit 1; }
    install -m 777 "$PROBE_BIN"                         "$apps/pbo-probe.bin"
fi
if [ -n "$HANDOFF_BIN" ]; then
    [ -f "$HANDOFF_BIN" ] || { echo "missing input: $HANDOFF_BIN" >&2; exit 1; }
    [ -f "$ROOT/platform/pocketbook/pbo-handoff-target.app" ] || { echo "missing input: platform/pocketbook/pbo-handoff-target.app" >&2; exit 1; }
    install -m 777 "$HANDOFF_BIN"                       "$apps/pbo-handoff-probe.bin"
    install -m 777 "$ROOT/platform/pocketbook/pbo-handoff-target.app" "$apps/pbo-handoff-target.app"
fi
install -m 644 "$ROOT/third_party/cacert/cacert.pem"    "$apps/pboffice-cacert.pem"
[ -d "$ROOT/third_party/tzdata/zoneinfo" ] || { echo "missing input: third_party/tzdata/zoneinfo (run scripts/update-tzdata.sh)" >&2; exit 1; }
mkdir -p "$apps/pboffice-zoneinfo"
cp -R "$ROOT/third_party/tzdata/zoneinfo/." "$apps/pboffice-zoneinfo/"
install -m 644 "$ROOT/third_party/tzdata/VERSION"       "$apps/pboffice-zoneinfo/VERSION"
find "$apps/pboffice-zoneinfo" -type d -exec chmod 755 {} + ; find "$apps/pboffice-zoneinfo" -type f -exec chmod 644 {} +
plugin_src="${PBO_QT_IMAGE_PLUGINS:-${POCKETBOOK_SDK:+$POCKETBOOK_SDK/local/qt6/plugins/imageformats}}"
if [ -n "$plugin_src" ] && [ -f "$plugin_src/libqjpeg.so" ]; then
    mkdir -p "$apps/pboffice-plugins/imageformats"
    for plugin in libqjpeg.so libqgif.so; do
        [ -f "$plugin_src/$plugin" ] && install -m 755 "$plugin_src/$plugin" "$apps/pboffice-plugins/imageformats/$plugin"
    done
else
    echo "note: no Qt image plugins found (set PBO_QT_IMAGE_PLUGINS or POCKETBOOK_SDK): the package relies on the reader's own JPEG and GIF support" >&2
fi
install -m 644 "$ROOT/third_party/noto-emoji/NotoEmoji.ttf" "$apps/pboffice-emoji.ttf"
install -m 644 "$ROOT/third_party/noto-emoji/OFL.txt"   "$apps/pboffice-emoji-OFL.txt"
install -D -m 644 "$MANIFEST" "$apps/pboffice-licenses/licenses/manifest.txt"
for t in $license_files; do
    install -D -m 644 "$ROOT/$t" "$apps/pboffice-licenses/$t"
done
install -m 666 "$ROOT/res/icons/pboffice.bmp"            "$apps/icons/pboffice.bmp"
install -m 666 "$ROOT/res/icons/pboffice_f.bmp"          "$apps/icons/pboffice_f.bmp"
sed "s/@VERSION@/$VERSION/g" "$ROOT/packaging/manifest.template.json" > "$stage/manifest.json"

left="$(grep -Eo '@[A-Z_]+@' "$stage/manifest.json" | head -n 1 || true)"
[ -z "$left" ] || { echo "manifest template has an unfilled placeholder: $left" >&2; exit 1; }

# A ZIP of the same version from an earlier build is replaced, never kept next to a failed build.
rm -f "$ZIP"
# Pack twice: the store's tool must give the same zip for the same files.
for n in a b; do
    "$PBSTORE" pack "$stage" --out "$work/$n.zip" >&2 || { echo "pbstore-catalog pack failed" >&2; exit 1; }
done
cmp -s "$work/a.zip" "$work/b.zip" || { echo "package is not reproducible: two builds from the same files differ" >&2; exit 1; }
echo "reproducible: two packs are identical (sha256 $(sha256sum "$work/a.zip" | cut -d' ' -f1))" >&2
"$PBSTORE" check-package "$work/a.zip" >&2 || { echo "package check failed" >&2; exit 1; }
check_zip_size "$work/a.zip" >&2 || exit 1
mv "$work/a.zip" "$ZIP"
echo "$ZIP"
