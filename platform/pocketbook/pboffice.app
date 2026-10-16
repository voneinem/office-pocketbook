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
