#!/bin/sh
# Stand-in target of the hand-over experiment E1 (spec 003, research "Device experiments"). NOT PB Office: a small long-running InkView program that records
# how another app started it, to learn what the system does when an app is started a second time while it is already running. Started by OpenBook2 from the
# test sender (pbo-handoff-probe.bin), which uses fixed test strings. This script only counts: it writes the number and the lengths of its arguments, never the
# arguments, to system/pboffice/handoff-probe.txt, and then replaces itself by the probe in target mode (so no argument stays in a long-lived command line).

APPDIR=/mnt/ext1/applications
DATADIR=/mnt/ext1/system/pboffice
mkdir -p "$DATADIR"
count=$#
lens=""
for a in "$@"; do
    l="$(printf '%s' "$a" | wc -c | tr -d ' ')"
    lens="$lens${lens:+,}$l"
done
echo "target-launcher: t=$(date +%s 2>/dev/null) entered argc=$count lens=$lens" >> "$DATADIR/handoff-probe.txt"
exec "$APPDIR/pbo-handoff-probe.bin" target "$DATADIR"
