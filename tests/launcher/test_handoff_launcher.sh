#!/usr/bin/env bash
# The reader's launcher (platform/pocketbook/pboffice.app) on the host, with a fake pboffice.bin, for the hand-over of spec 003:
#   - the arguments after the marker pbo-mail-v1 become one raw file each in handoff/<id>/ (byte for byte, nothing interpreted),
#   - the launcher then executes itself again WITHOUT arguments, so the text is not in the command line of the long-lived process,
#   - the app is started exactly once and with no arguments, no folder named *.tmp is left behind,
#   - a start without arguments, or with a first argument that is not the marker, is an ordinary start and leaves no entry.
#
#   tests/launcher/test_handoff_launcher.sh [repo-root]
set -euo pipefail

ROOT="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"
LAUNCHER="$ROOT/platform/pocketbook/pboffice.app"
tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
fail=0
check() { if ! "$@"; then echo "FAIL: $*" >&2; fail=1; fi; }

sh -n "$LAUNCHER" || { echo "FAIL: launcher is not valid sh" >&2; exit 1; }

# A fake pboffice.bin that records how it was started: how often, with how many arguments, and the command line of the launcher that started it
# (/proc/$PPID/cmdline): after the launcher's exec that command line must not hold any argument of the hand-over.
setup() {
    rm -rf "$tmp/app" "$tmp/data" "$tmp/root" "$tmp/count" "$tmp/bin_argc" "$tmp/parent_cmdline" "$tmp/pwned"
    mkdir -p "$tmp/app" "$tmp/root"
    cat > "$tmp/app/pboffice.bin" <<EOF
#!/bin/sh
echo x >> "$tmp/count"
echo "\$#" > "$tmp/bin_argc"
tr '\\000' ' ' < /proc/\$PPID/cmdline > "$tmp/parent_cmdline"
echo "\${PBO_HANDOFF_PROBE:-unset} \${PBO_HANDOFF_PROBE_FREE:-unset}" > "$tmp/probe_env"
for p in /proc/[0-9]*; do printf '%s: ' "\${p#/proc/}"; tr '\\000' ' ' < "\$p/cmdline" 2>/dev/null; echo; done > "$tmp/all_cmdlines"
echo "the app prints its own line" >&2
EOF
    chmod +x "$tmp/app/pboffice.bin"
    sed -e "s#/mnt/ext1/applications#$tmp/app#; s#/mnt/ext1/system/pboffice#$tmp/data#; s#cd /mnt/ext1#cd $tmp/root#" \
        -e 's#^sleep 2$#true#; s#^    sleep 2$#    true#' "$LAUNCHER" > "$tmp/launch.sh"
}
launch() { setup; sh "$tmp/launch.sh" "$@"; }
runs() { if [ -e "$tmp/count" ]; then wc -l < "$tmp/count" | tr -d ' '; else echo 0; fi; }
entries() { if [ -d "$tmp/data/handoff" ]; then ls -1 "$tmp/data/handoff" | wc -l | tr -d ' '; else echo 0; fi; }
entry() { ls -1d "$tmp/data/handoff"/* 2>/dev/null | head -1 || true; }
same_bytes() { printf '%s' "$1" | cmp -s - "$2"; }   # the file holds exactly the string, no terminator

# 1. the arguments after the marker become one raw file each, byte for byte, whatever they contain
n=0
while IFS= read -r -d '' text; do
    n=$((n + 1))
    launch pbo-mail-v1 "text=$text" "textlen=${#text}" "to=bob@example.org"
    check [ "$(entries)" = 1 ]
    e="$(entry)"
    check [ -n "$e" ]
    check same_bytes "text=$text" "$e/0"
    check same_bytes "textlen=${#text}" "$e/1"
    check same_bytes "to=bob@example.org" "$e/2"
    check [ ! -e "$e/3" ]
    check [ ! -e "$tmp/pwned" ]                                  # nothing in the text was run
    case "$e" in *.tmp) echo "FAIL: an unfinished folder is left: $e" >&2; fail=1 ;; esac
    check [ "$(runs)" = 1 ]                                      # the app was started exactly once
    check [ "$(cat "$tmp/bin_argc")" = 0 ]                       # ... with no arguments
done < <(printf '%s\0' \
    'plain text' \
    $'two\nlines\n' \
    $'trailing newline\n' \
    "it's \"quoted\" and 'single'" \
    '$(touch "'"$tmp"'/pwned")' \
    '`touch "'"$tmp"'/pwned"`' \
    '; touch "'"$tmp"'/pwned"' \
    '&& touch "'"$tmp"'/pwned" &&' \
    '-n --help -e' \
    '* ? [a-z] ~ ${HOME} $HOME $1 $@' \
    'back\slash \n literal' \
    'Gr'$'\xc3\xbc''ße '$'\xe2\x82\xac'' '$'\xf0\x9f\x98\x80' \
    'pboffice.bin inkfeed.bin')
check [ "$n" = 13 ]

# 2. big texts: 1500 and 8192 bytes
for size in 1500 8192; do
    text="$(head -c "$size" /dev/zero | tr '\0' 'x')"
    launch pbo-mail-v1 "text=$text" "textlen=$size"
    check same_bytes "text=$text" "$(entry)/0"
done

# 3. an empty argument is an empty file, and the files of the entry are private (umask 077) where the file system keeps modes
launch pbo-mail-v1 "" "to=bob@example.org"
e="$(entry)"
check [ -e "$e/0" ]
check [ ! -s "$e/0" ]
check same_bytes "to=bob@example.org" "$e/1"
check [ "$(stat -c %a "$e")" = 700 ]
check [ "$(stat -c %a "$e/1")" = 600 ]

# 4. the launcher's own command line, as the app sees it, holds none of the arguments (it executed itself again)
launch pbo-mail-v1 "text=UNIQUE-WORD-9d41" "textlen=21" "subject=UNIQUE-WORD-9d41"
check [ -s "$tmp/parent_cmdline" ]
check bash -c '! grep -q "UNIQUE-WORD-9d41" "$1"' _ "$tmp/parent_cmdline"
check bash -c '! grep -q "pbo-mail-v1" "$1"' _ "$tmp/parent_cmdline"

# 5. no arguments: an ordinary start; no hand-over folder is made
launch
check [ "$(entries)" = 0 ]
check [ ! -d "$tmp/data/handoff" ]
check [ "$(runs)" = 1 ]
check [ "$(cat "$tmp/bin_argc")" = 0 ]

# 6. a first argument that is not the marker: the arguments are dropped, the app starts without them
launch "/mnt/ext1/some/other.file" "text=DROPPED-WORD" "textlen=12"
check [ "$(entries)" = 0 ]
check [ "$(runs)" = 1 ]
check [ "$(cat "$tmp/bin_argc")" = 0 ]
check bash -c '! grep -q "DROPPED-WORD" "$1"' _ "$tmp/parent_cmdline"
launch "pbo-mail-v2" "text=x" "textlen=1"                         # another version: not understood, dropped
check [ "$(entries)" = 0 ]
check [ "$(runs)" = 1 ]

# 7. the marker alone: the app starts; no crash, no loop
launch pbo-mail-v1
check [ "$(runs)" = 1 ]
check [ "$(cat "$tmp/bin_argc")" = 0 ]

# 8. more than 8 arguments after the marker: at most 8 files are written and the app starts
launch pbo-mail-v1 a1=1 a2=2 a3=3 a4=4 a5=5 a6=6 a7=7 a8=8 a9=9 a10=10 a11=11
check [ "$(runs)" = 1 ]
check [ "$(ls -1 "$(entry)" | wc -l | tr -d ' ')" -le 8 ]

# 9. the hand-over probe: a run-handoff-probe flag file makes the launcher run the probe instead of the app, once, with the data folder as its argument
setup
printf '#!/bin/sh\necho "probe args: $1" > "%s/probe_ran"\n' "$tmp" > "$tmp/app/pbo-handoff-probe.bin"; chmod +x "$tmp/app/pbo-handoff-probe.bin"
mkdir -p "$tmp/data"; : > "$tmp/data/run-handoff-probe"
sh "$tmp/launch.sh"
check [ ! -e "$tmp/count" ]                                  # the app was not started
check [ ! -e "$tmp/data/run-handoff-probe" ]                 # the flag is consumed: no loop
check grep -q "probe args: $tmp/data" "$tmp/probe_ran"
sh "$tmp/launch.sh"                                          # next start: the app again
check [ "$(runs)" = 1 ]

# 10. the listener probe (experiment E2): a run-listener-probe flag file makes the launcher start the APP with PBO_HANDOFF_PROBE=1 once; the content
# "free" also sets PBO_HANDOFF_PROBE_FREE=1; a normal start sets neither
launch
check [ "$(cat "$tmp/probe_env")" = "unset unset" ]
setup
mkdir -p "$tmp/data"; : > "$tmp/data/run-listener-probe"
sh "$tmp/launch.sh"
check [ "$(cat "$tmp/probe_env")" = "1 unset" ]
check [ "$(runs)" = 1 ]                                      # the app itself is started
check [ ! -e "$tmp/data/run-listener-probe" ]                # the flag is consumed
sh "$tmp/launch.sh"
check [ "$(cat "$tmp/probe_env")" = "unset unset" ]          # next start: normal
setup
mkdir -p "$tmp/data"; echo free > "$tmp/data/run-listener-probe"
sh "$tmp/launch.sh"
check [ "$(cat "$tmp/probe_env")" = "1 1" ]

# 11. privacy (spec 003 User Story 3, FR-015..017): while the app runs, no process of the host has the text in its command line (the launcher executed
# itself again), the log of the launch and of the one before hold none of it, even when the text names the programs that the diagnostics search for, and the
# only files that hold it are the files of the pending entry
word="UNIQUE-$(date +%s%N)-$$"   # made at run time: no outer command line can hold it
launch pbo-mail-v1 "text=$word pboffice.bin inkfeed.bin" "textlen=$((${#word} + 24))" "subject=$word" "to=$word@example.org"
sh "$tmp/launch.sh" pbo-mail-v1 "text=$word pboffice.bin inkfeed.bin" "textlen=$((${#word} + 24))" "subject=$word"   # a second start: the first log becomes .prev
check [ -s "$tmp/all_cmdlines" ]
check bash -c '! grep -q "$1" "$2"' _ "$word" "$tmp/all_cmdlines"
grep "$word" "$tmp/all_cmdlines" 2>/dev/null | cut -c1-160 | sed 's/^/a process holds the text: /' >&2 || true
check [ -f "$tmp/data/pboffice.log.prev" ]
check bash -c '! grep -q "$1" "$2" "$3"' _ "$word" "$tmp/data/pboffice.log" "$tmp/data/pboffice.log.prev"
check grep -q 'diagnostics: other app processes' "$tmp/data/pboffice.log"
holders="$(grep -rl "$word" "$tmp" 2>/dev/null | sed "s#^$tmp/##" | grep -v '^data/handoff/' || true)"
check [ -z "$holders" ]
[ -z "$holders" ] || echo "the text is also in: $holders" >&2
check [ -n "$(grep -rl "$word" "$tmp/data/handoff" 2>/dev/null || true)" ]     # and it IS in the entry: the app has something to read

# 12. the log of the launch has no trace of the arguments' count of files beyond what the launcher says about itself, and the log exists
launch pbo-mail-v1 "text=hi" "textlen=2"
check [ -f "$tmp/data/pboffice.log" ]
check grep -q 'pboffice launch' "$tmp/data/pboffice.log"

[ "$fail" = 0 ] && echo "handoff launcher tests passed"
exit "$fail"
