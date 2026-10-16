#!/usr/bin/env bash
# Imports the IANA time-zone database as compact ("slim") TZif files into third_party/tzdata/ (research R-16, T103).
# Source: the compact text form `tzdata.zi` of a tz release (default: the one of the tzdata package of this machine,
# /usr/share/zoneinfo/tzdata.zi; `apt install tzdata` provides it and `zic`). Output: third_party/tzdata/zoneinfo/<Area>/<Name>
# (links are written as copies, so the folder can be copied to a FAT file system) and third_party/tzdata/VERSION.
#
#   scripts/update-tzdata.sh [path/to/tzdata.zi]
set -euo pipefail

repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
src="${1:-/usr/share/zoneinfo/tzdata.zi}"
out="$repo/third_party/tzdata"

command -v zic >/dev/null || { echo "update-tzdata: zic not found (apt install tzdata)" >&2; exit 1; }
[ -r "$src" ] || { echo "update-tzdata: cannot read $src" >&2; exit 1; }
release="$(sed -n 's/^# version \(.*\)$/\1/p' "$src" | head -1)"
[ -n "$release" ] || { echo "update-tzdata: no '# version' line in $src" >&2; exit 1; }

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
zic -b slim -d "$tmp/zi" "$src"

rm -rf "$out/zoneinfo"
mkdir -p "$out/zoneinfo"
# dereference hard and symbolic links: every name is its own small file
cp -rL "$tmp/zi/." "$out/zoneinfo/"
# only the zone files: no leap-second or POSIX trees are produced, but drop anything unexpected
find "$out/zoneinfo" -type f ! -size +0 -delete

count="$(find "$out/zoneinfo" -type f | wc -l)"
bytes="$(find "$out/zoneinfo" -type f -printf '%s\n' | awk '{s+=$1} END {print s+0}')"
sha="$(sha256sum "$src" | cut -d' ' -f1)"
cat > "$out/VERSION" <<VERSION
IANA time zone database $release (public domain)
Source: https://www.iana.org/time-zones (tzdata.zi of the tz release $release)
Release: $release
tzdata.zi SHA-256: $sha
Built with: zic -b slim ($(zic --version 2>&1 | head -1))
Files: $count zone files, $bytes bytes
Imported: $(date -u +%Y-%m-%d)
VERSION
echo "update-tzdata: release $release, $count files, $bytes bytes -> $out"
