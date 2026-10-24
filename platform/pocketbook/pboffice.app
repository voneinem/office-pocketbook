#!/bin/sh
# PocketBook launcher for pboffice on the InkPad One.
#
# Installed at /mnt/ext1/applications/pboffice.app, with the ELF binary beside it as
# pboffice.bin. Runtime data and the log live under /mnt/ext1/system/pboffice.
# Adapted from the InkFeed launcher (MIT, see NOTICE).
#
# Device notes (InkPad One: RK3566, aarch64 kernel, 32-bit armhf userland):
#   - Qt 6.x ships under /usr/lib with its QPA plugins in /usr/plugins/platforms and the
#     QML modules under /usr/qml. The binary was linked for a /usr/local/qt6 prefix that
#     does not exist here, so point Qt at the real locations below.
#   - Qt Quick needs a GL context: use the EGL QPA (pocketbook2egl).
#   - Content rotation (g-sensor) and devicePixelRatio scaling are handled in the app.

APPDIR=/mnt/ext1/applications
DATADIR=/mnt/ext1/system/pboffice

# Hand-over from another app (spec 003, contracts/handoff-contract.md): the system starts this script with the marker pbo-mail-v1 and the text, subject and
# recipient as arguments. A shell cannot overwrite its own command line, and this script lives as long as the app, so the arguments are written to a private
# pending entry (system/pboffice/handoff/<id>/, one raw file per argument, renamed when complete; the app reads it, saves a draft and removes it, and an entry
# older than 10 minutes is removed unread) and the script then executes itself again WITHOUT arguments: the text is not in the command line of the long-lived
# process. The card is vfat, so the entry cannot be made private there; it waits for seconds. Nothing in the arguments is ever printed, logged, evaluated or
# expanded anywhere else in this script (scripts/check-constitution.sh rule R11).
# handoff-capture-begin
if [ "$#" -gt 0 ]; then
    if [ "$1" = "pbo-mail-v1" ]; then
        (
            umask 077
            id="$(date +%s 2>/dev/null || echo 0)-$$"
            mkdir -p "$DATADIR/handoff/$id.tmp" 2>/dev/null || exit 0
            n=0
            first=1
            for a in "$@"; do
                if [ "$first" = 1 ]; then first=0; continue; fi
                [ "$n" -ge 8 ] && break
                printf '%s' "$a" > "$DATADIR/handoff/$id.tmp/$n"
                n=$((n + 1))
            done
            mv "$DATADIR/handoff/$id.tmp" "$DATADIR/handoff/$id" 2>/dev/null
        )
    fi
    exec sh "$0"
fi
# handoff-capture-end

mkdir -p "$DATADIR"
# Keep the log of the launch before this one: a start that fails must not erase the evidence of the one before it.
[ -f "$DATADIR/pboffice.log" ] && mv -f "$DATADIR/pboffice.log" "$DATADIR/pboffice.log.prev" 2>/dev/null
exec >"$DATADIR/pboffice.log" 2>&1

echo "=== pboffice launch: $(date 2>/dev/null) ==="
# What the system looks like at this start (no personal data). Each command may be missing on the reader.
echo "--- diagnostics: uid/groups: $(id 2>/dev/null || echo unknown)"
echo "--- diagnostics: gpu node: $(ls -l /dev/mali0 2>/dev/null || echo 'not found')"
echo "--- diagnostics: uptime: $(uptime 2>/dev/null || echo unknown)"
echo "--- diagnostics: memory: $(grep -E '^(MemFree|MemAvailable)' /proc/meminfo 2>/dev/null | tr -s ' ' | tr '\n' ' ')"
echo "--- diagnostics: other app processes: $(ps 2>/dev/null | grep -E 'pboffice\.bin|inkfeed\.bin' | grep -v grep || echo none)"

export LD_LIBRARY_PATH="/usr/lib:/usr/local/lib:$LD_LIBRARY_PATH"
export QT_QPA_PLATFORM_PLUGIN_PATH="/usr/plugins/platforms"
export QT_PLUGIN_PATH="/usr/plugins"
export QT_QPA_PLATFORM="pocketbook2egl"
export QML_IMPORT_PATH="/usr/qml"
export QML2_IMPORT_PATH="/usr/qml"

cd /mnt/ext1

# Device probe (INSTALL.md, "Device probe"): when the file run-probe exists in the data folder, this start runs the
# probe script instead of the app, writes probe.txt there and removes the flag, so the next start is a normal one.
if [ -e "$DATADIR/run-probe" ]; then
    rm -f "$DATADIR/run-probe"
    echo "=== pboffice: run-probe flag found, running the probe instead of the app ==="
    sh "$APPDIR/pboffice-probe.sh" "$APPDIR/pbo-probe.bin" "$DATADIR" > "$DATADIR/probe.txt" 2>&1
    echo "=== pboffice: probe finished, output in probe.txt ==="
    exit 0
fi

# Hand-over probe (INSTALL.md, "Hand-over probe"; spec 003): when the file run-handoff-probe exists in the data folder, this start runs the small InkView
# program that starts PB Office (or its stand-in) from another app with a text, instead of the app, once. It writes handoff-probe.txt there and no text.
if [ -e "$DATADIR/run-handoff-probe" ]; then
    rm -f "$DATADIR/run-handoff-probe"
    echo "=== pboffice: run-handoff-probe flag found, running the hand-over probe instead of the app ==="
    exec "$APPDIR/pbo-handoff-probe.bin" "$DATADIR"
fi

# Listener probe (experiment E2 of spec 003): when the file run-listener-probe exists in the data folder, this start runs the APP itself, with PBO_HANDOFF_PROBE=1,
# so that it registers a request listener that only records what a second start by another app delivers (counts and lengths, never the text) in
# listener-probe.txt. A file content of "free" also sets PBO_HANDOFF_PROBE_FREE=1 (the probe then frees what UnpackParameters returned). The flag is consumed.
if [ -e "$DATADIR/run-listener-probe" ]; then
    if grep -qx free "$DATADIR/run-listener-probe" 2>/dev/null; then PBO_HANDOFF_PROBE_FREE=1; export PBO_HANDOFF_PROBE_FREE; fi
    rm -f "$DATADIR/run-listener-probe"
    PBO_HANDOFF_PROBE=1
    export PBO_HANDOFF_PROBE
    echo "=== pboffice: run-listener-probe flag found, the app registers the probe listener ==="
fi

# The log is a file, so the app's own stdout is block-buffered and lost when the process aborts; line buffering
# keeps the start-up lines of a failed start (stdbuf is not on every reader).
run_app() {
    if command -v stdbuf >/dev/null 2>&1; then stdbuf -oL -eL "$APPDIR/pboffice.bin"; else "$APPDIR/pboffice.bin"; fi
}

# Everything this attempt wrote to the log (stdout and stderr share the file).
mark=$(wc -c < "$DATADIR/pboffice.log" 2>/dev/null || echo 0)
run_app
code=$?
echo "=== pboffice.bin exit code: $code ==="

# Seen once: the system refused the GPU node (/dev/mali0, errno 13) and Qt aborted at once (exit 134). A reboot
# cleared it; it is not known whether it passes by itself, so try exactly one more time and record both results.
if [ "$code" -eq 134 ] && tail -c +"$((mark + 1))" "$DATADIR/pboffice.log" 2>/dev/null | grep -q 'mali0'; then
    echo "=== pboffice: GPU device refused (mali0), retrying once in 2 s ==="
    sleep 2
    run_app
    echo "=== pboffice.bin exit code (retry): $? ==="
fi
