#!/usr/bin/env bash
# Mechanical checks of the project constitution (run by CTest and scripts/pre-merge.sh).
#
#   scripts/check-constitution.sh [repo-root]    scan (default: this repository)
#   scripts/check-constitution.sh --self-test    prove that every rule below detects a violation
#
# Rules (a finding makes the script exit non-zero):
#   R1 TLS verification is never disabled, in code AND shell scripts:
#        curl -k, curl --insecure, wget --no-check-certificate,
#        CURLOPT_SSL_VERIFYPEER / CURLOPT_SSL_VERIFYHOST set to 0 (VERIFYHOST must be 2),
#        CURLOPT_SSL_VERIFYSTATUS, CURLOPT_PROXY_SSL_VERIFYPEER, CURLOPT_PROXY_SSL_VERIFYHOST,
#        CURLOPT_DOH_SSL_VERIFYPEER, CURLOPT_DOH_SSL_VERIFYHOST, CURLOPT_DOH_SSL_VERIFYSTATUS
#        set to 0 / false.
#   R2 No hard-coded SDK locations ("pocketbook-sdk", "/opt/pocketbook") in build files and scripts.
#   R3 core/ is Qt-free and InkView-free.
#   R4 No logging call is given a password, user name, note content or title.
#   R5 No absolute local paths (user home directories, drive letters) in any tracked text file.
#   R6 No log::text / log::count key that is, starts with or ends with the word url, path, file, name
#        or address (e.g. "url", "file_name", "link_url") in core/, bridge/ and app/ (spec 002 FR-033: no link addresses, attachment names or file paths in logs).
#   R7 TLS is required for every mail/calendar connection: CURLOPT_USE_SSL is only ever set to
#        CURLUSESSL_ALL, and the literal schemes imap:// and smtp:// (no TLS) appear in core/, bridge/ and
#        app/ only in core/src/curl_session.cpp (tests/ may use them for the fake servers).
#   R8 No log::text / log::count key that is, starts with or ends with the word host, folder, subject, from,
#        to, recipient, title, summary, attendee, location, contact, phone, email, book, org or note in core/, bridge/ and app/ (no mail or
#        calendar content or server names in logs).
#   R9 The identifier pbnotes (the application this one was started from) appears in no tracked file except
#        under docs/reference/, in NOTICE and under licenses/ (the planning documents in specs/ and .specify/
#        describe the rename, tests/templates/ holds the old perf tests until they are rewritten).
#   R10 libcurl tracing is never switched on (it would write LOGIN / AUTH PLAIN passwords to the log):
#        CURLOPT_VERBOSE, CURLOPT_DEBUGFUNCTION, CURLOPT_DEBUGDATA, CURLOPT_STDERR, curl -v / --verbose /
#        --trace* in core/, bridge/, app/ and scripts/.
set -u

SELF="$(basename "${BASH_SOURCE[0]}")"

# Lists the text files to scan, relative to $1. Uses git when available, else find.
list_files() {
    local root="$1"
    if [ -d "$root/.git" ] && command -v git >/dev/null 2>&1; then
        git -C "$root" ls-files -z --cached --others --exclude-standard
    else
        (cd "$root" && find . -type f -not -path './.git/*' -not -path './build*' -print0 | sed -z 's#^\./##')
    fi
}

# report <rule> <file-label> <path> <regex>: prints the matching lines and returns 0 when there are any.
report() {
    local out
    out="$(grep -nE -- "$4" "$3" 2>/dev/null)" || return 1
    printf '%s\n' "$out" | sed "s#^#$1 $2:#"
    return 0
}

scan() {
    local root="$1" bad=0 f out
    while IFS= read -r -d '' f; do
        [ -f "$root/$f" ] || continue
        case "$f" in
            third_party/*|"scripts/$SELF"|.claude/*|*.bmp|*.zip|*.pem) continue ;;
        esac
        grep -Iq . "$root/$f" 2>/dev/null || continue   # skip binary/empty files
        local p="$root/$f"

        # R5: absolute local paths, everywhere
        if grep -nE '/home/[A-Za-z0-9._-]+/|/Users/[A-Za-z0-9._-]+/|(^|[^A-Za-z0-9])[A-Za-z]:\\[A-Za-z0-9_.-]' "$p" >/dev/null 2>&1; then
            grep -nE '/home/[A-Za-z0-9._-]+/|/Users/[A-Za-z0-9._-]+/|(^|[^A-Za-z0-9])[A-Za-z]:\\[A-Za-z0-9_.-]' "$p" | sed "s#^#R5 $f:#"
            bad=1
        fi

        case "$f" in
            core/*|bridge/*|app/*|tests/*|scripts/*|platform/*|cmake/*|CMakeLists.txt|*/CMakeLists.txt)
                # R1: TLS bypasses
                if grep -nE '(curl|wget)[^#|;&]*[[:space:]](-k|--insecure|--no-check-certificate)([[:space:]]|$)' "$p" >/dev/null 2>&1 ||
                   grep -nE 'CURLOPT_(PROXY_|DOH_)?SSL_VERIFY(PEER|HOST|STATUS)[[:space:]]*,[[:space:]]*(0|false|FALSE)' "$p" >/dev/null 2>&1 ||
                   grep -nE 'CURLOPT_SSL_VERIFYHOST[[:space:]]*,[[:space:]]*1[^0-9]' "$p" >/dev/null 2>&1 ||
                   grep -nE 'CURLOPT_SSL_VERIFYSTATUS' "$p" >/dev/null 2>&1; then
                    grep -nE '(curl|wget)[^#|;&]*[[:space:]](-k|--insecure|--no-check-certificate)([[:space:]]|$)|CURLOPT_(PROXY_|DOH_)?SSL_VERIFY(PEER|HOST|STATUS)[[:space:]]*,[[:space:]]*(0|false|FALSE|1[^0-9])|CURLOPT_SSL_VERIFYSTATUS' "$p" | sed "s#^#R1 $f:#"
                    bad=1
                fi
                ;;
        esac
        case "$f" in
            cmake/*|CMakeLists.txt|*/CMakeLists.txt|scripts/*|platform/*)
                # R2: hard-coded SDK locations
                if grep -nE 'pocketbook-sdk|/opt/pocketbook' "$p" >/dev/null 2>&1; then
                    grep -nE 'pocketbook-sdk|/opt/pocketbook' "$p" | sed "s#^#R2 $f:#"
                    bad=1
                fi
                ;;
        esac
        case "$f" in
            core/*)
                # R3: no Qt / InkView in the core
                if grep -nE '#[[:space:]]*include[[:space:]]*[<"](Q[A-Za-z]|inkview)|\bQObject\b|\bQString\b' "$p" >/dev/null 2>&1; then
                    grep -nE '#[[:space:]]*include[[:space:]]*[<"](Q[A-Za-z]|inkview)|\bQObject\b|\bQString\b' "$p" | sed "s#^#R3 $f:#"
                    bad=1
                fi
                ;;
        esac
        case "$f" in
            core/*|bridge/*|app/*)
                # R4: personal data into the log
                if grep -nE '(log::|qDebug|qWarning|qInfo|std::cerr|fprintf\(stderr)[^;]*(password|passwd|username|\.content|->content|\.title|->title)' "$p" >/dev/null 2>&1; then
                    grep -nE '(log::|qDebug|qWarning|qInfo|std::cerr|fprintf\(stderr)[^;]*(password|passwd|username|\.content|->content|\.title|->title)' "$p" | sed "s#^#R4 $f:#"
                    bad=1
                fi
                # R6: log keys that would carry addresses, names or paths
                if grep -nE 'log::(text|count)[[:space:]]*\([[:space:]]*"([A-Za-z0-9.]+_)?(url|path|file|name|address)(_[A-Za-z0-9_.]+)?"' "$p" >/dev/null 2>&1; then
                    grep -nE 'log::(text|count)[[:space:]]*\([[:space:]]*"([A-Za-z0-9.]+_)?(url|path|file|name|address)(_[A-Za-z0-9_.]+)?"' "$p" | sed "s#^#R6 $f:#"
                    bad=1
                fi
                ;;
        esac
        # R9: the old application's identifier (any file type, minus the allowed places)
        case "$f" in
            docs/reference/*|NOTICE|licenses/*|specs/*|.specify/*|tests/templates/*) ;;
            *) report R9 "$f" "$p" 'pbnotes' && bad=1 ;;
        esac
        case "$f" in
            core/*|bridge/*|app/*)
                # R7: TLS always on, no clear-text URL schemes outside the one place that builds URLs
                out="$(grep -nE 'CURLOPT_USE_SSL' "$p" 2>/dev/null | grep -v 'CURLUSESSL_ALL')" && {
                    printf '%s\n' "$out" | sed "s#^#R7 $f:#"; bad=1; }
                if [ "$f" != core/src/curl_session.cpp ]; then
                    report R7 "$f" "$p" '(imap|smtp)://' && bad=1
                fi
                # R8: log keys that would carry mail or calendar content or server names
                report R8 "$f" "$p" 'log::(text|count)[[:space:]]*\([[:space:]]*"([A-Za-z0-9.]+_)?(host|folder|subject|from|to|recipient|title|summary|attendee|location|contact|phone|email|book|org|note)(_[A-Za-z0-9_.]+)?"' && bad=1
                ;;
        esac
        case "$f" in
            core/*|bridge/*|app/*|scripts/*)
                # R10: no libcurl tracing
                report R10 "$f" "$p" 'CURLOPT_(VERBOSE|DEBUGFUNCTION|DEBUGDATA|STDERR)|(^|[^A-Za-z0-9_-])curl[^#|;&]*[[:space:]](-[A-Za-z]*v[A-Za-z]*|--verbose|--trace[a-z-]*)([[:space:]]|$)' && bad=1
                ;;
        esac
    done < <(list_files "$root")
    return $bad
}

if [ "${1:-}" = "--self-test" ]; then
    tmp="$(mktemp -d)"
    trap 'rm -rf "$tmp"' EXIT
    fail=0
    mkdir -p "$tmp/core/src" "$tmp/scripts" "$tmp/cmake" "$tmp/docs" "$tmp/bridge" "$tmp/app"
    expect() {  # expect <pass|fail> <description>
        scan "$tmp" >/dev/null 2>&1
        local rc=$?
        if { [ "$1" = pass ] && [ $rc -eq 0 ]; } || { [ "$1" = fail ] && [ $rc -ne 0 ]; }; then
            echo "ok   $2"
        else
            echo "FAIL $2 (rc=$rc)"; fail=1
        fi
    }
    violate() {  # violate <file> <content> <description>
        printf '%s\n' "$2" > "$tmp/$1"
        expect fail "$3"
        rm -f "$tmp/$1"
    }
    printf 'int main() { return 0; }\n' > "$tmp/core/src/ok.cpp"
    printf '#!/bin/sh\necho hello\n' > "$tmp/scripts/ok.sh"
    expect pass "a clean tree passes"
    printf 'curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1L);\ncurl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 2L);\n' > "$tmp/core/src/tls_ok.cpp"
    expect pass "VERIFYPEER 1 / VERIFYHOST 2 are allowed"; rm "$tmp/core/src/tls_ok.cpp"

    violate scripts/a.sh 'curl -k https://example.com' "R1: curl -k"
    violate scripts/a.sh 'curl -sS --insecure https://example.com' "R1: curl --insecure"
    violate scripts/a.sh 'wget --no-check-certificate https://example.com' "R1: wget --no-check-certificate"
    violate core/src/a.cpp 'curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);' "R1: VERIFYPEER 0"
    violate core/src/a.cpp 'curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);' "R1: VERIFYHOST 0"
    violate core/src/a.cpp 'curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 1L);' "R1: VERIFYHOST 1"
    violate core/src/a.cpp 'curl_easy_setopt(c, CURLOPT_SSL_VERIFYSTATUS, 0L);' "R1: VERIFYSTATUS"
    violate core/src/a.cpp 'curl_easy_setopt(c, CURLOPT_PROXY_SSL_VERIFYPEER, 0L);' "R1: PROXY_SSL_VERIFYPEER 0"
    violate core/src/a.cpp 'curl_easy_setopt(c, CURLOPT_DOH_SSL_VERIFYHOST, false);' "R1: DOH_SSL_VERIFYHOST false"
    violate cmake/a.cmake 'set(SDK_PATH /opt/pocketbook-sdk/x)' "R2: /opt/pocketbook path"
    violate scripts/a.sh 'SDK=../pocketbook-sdk-6.11/sdk' "R2: pocketbook-sdk path"
    violate core/src/a.cpp '#include <QString>' "R3: Qt include in core"
    violate core/src/a.cpp '#include <inkview.h>' "R3: inkview include in core"
    violate core/src/a.cpp 'log::text("x", account.password);' "R4: password into the log"
    violate core/src/a.cpp 'log::text("x", note.content);' "R4: note content into the log"
    violate core/src/a.cpp 'log::text("url", link);' "R6: url key in the log"
    violate core/src/a.cpp 'log::text("file_name", n);' "R6: file_name key in the log"
    violate core/src/a.cpp 'log::count("path", 1);' "R6: path key in the log"
    violate core/src/a.cpp 'log::text("address", a);' "R6: address key in the log"
    printf 'log::text("result", r); log::count("pulled", 3); log::path(p);\n' > "$tmp/core/src/log_ok.cpp"
    expect pass "result/count keys and log::path are allowed"; rm "$tmp/core/src/log_ok.cpp"
    # R7: TLS is always required (USE_SSL == ALL) and URL schemes are built in one place
    printf 'curl_easy_setopt(c, CURLOPT_USE_SSL, (long)CURLUSESSL_ALL);\n' > "$tmp/core/src/ssl_ok.cpp"
    expect pass "R7: CURLOPT_USE_SSL with CURLUSESSL_ALL is allowed"; rm "$tmp/core/src/ssl_ok.cpp"
    violate core/src/a.cpp 'curl_easy_setopt(c, CURLOPT_USE_SSL, CURLUSESSL_TRY);' "R7: USE_SSL TRY"
    violate core/src/a.cpp 'curl_easy_setopt(c, CURLOPT_USE_SSL, CURLUSESSL_CONTROL);' "R7: USE_SSL CONTROL"
    violate core/src/a.cpp 'curl_easy_setopt(c, CURLOPT_USE_SSL, 0L);' "R7: USE_SSL 0"
    violate core/src/a.cpp 'const char* u = "imap://mail.example.org/INBOX";' "R7: imap:// literal in core"
    violate core/src/a.cpp 'std::string u = std::string("smtp://") + host;' "R7: smtp:// literal in core"
    violate bridge/a.cpp 'auto u = "imap://x";' "R7: imap:// literal in bridge"
    mkdir -p "$tmp/tests"
    printf 'const char* u = "imap://localhost:1/";\n' > "$tmp/tests/t.cpp"
    printf 'const char* u = "smtp://localhost:1/";\n' > "$tmp/core/src/curl_session.cpp"
    expect pass "R7: imap:// / smtp:// literals are allowed in tests/ and core/src/curl_session.cpp"
    rm -f "$tmp/tests/t.cpp" "$tmp/core/src/curl_session.cpp"
    printf 'const char* u = "imaps://localhost/";\n' > "$tmp/core/src/imaps.cpp"
    expect pass "R7: imaps:// / smtps:// are not matched"; rm "$tmp/core/src/imaps.cpp"
    # R8: no mail/calendar content or server names as log fields
    for w in host folder subject from to recipient title summary attendee location; do
        violate core/src/a.cpp "log::text(\"$w\", v);" "R8: log key $w"
    done
    for w in contact phone email book org note; do   # spec 002 FR-027: no contact data in logs
        violate core/src/a.cpp "log::text(\"$w\", v);" "R8: log key $w"
    done
    violate core/src/a.cpp 'log::count("contact_name_len", 1);' "R8: log key contact_name_len"
    violate bridge/a.cpp 'log::text("primary_email", v);' "R8: log key primary_email"
    violate bridge/a.cpp 'log::text("sender_from", v);' "R8: log key sender_from"
    violate app/a.cpp 'log::count("folder_name_len", 1);' "R8: log key folder_name_len"
    printf 'log::text("result", r); log::count("pulled", 3); log::text("tofu", x); log::count("books", 2); log::count("contacts", 5); log::count("notes_total", 1);\n' > "$tmp/core/src/log_ok.cpp"
    expect pass "R8: other keys (also tofu, books, contacts as counts) are allowed"; rm "$tmp/core/src/log_ok.cpp"
    # R9: the old application's identifier stays out of this one
    violate docs/a.md 'see pbnotes for the idea' "R9: pbnotes in docs"
    violate core/src/a.cpp 'namespace pbnotes {}' "R9: pbnotes in code"
    mkdir -p "$tmp/docs/reference" "$tmp/licenses"
    mkdir -p "$tmp/specs/x"; printf 'pbnotes renamed\n' > "$tmp/specs/x/plan.md"
    printf 'pbnotes analysis\n' > "$tmp/docs/reference/a.md"
    printf 'pbnotes was the origin\n' > "$tmp/NOTICE"
    printf 'pbnotes\n' > "$tmp/licenses/a.txt"
    expect pass "R9: pbnotes is allowed in docs/reference/, NOTICE, licenses/ and specs/"
    rm -rf "$tmp/docs/reference" "$tmp/NOTICE" "$tmp/licenses/a.txt" "$tmp/specs"
    printf 'PB Notes and notes-pocketbook are fine\n' > "$tmp/docs/b.md"
    expect pass "R9: only the identifier pbnotes is matched"; rm "$tmp/docs/b.md"
    # R10: libcurl tracing would write LOGIN / AUTH PLAIN passwords to the log
    violate core/src/a.cpp 'curl_easy_setopt(c, CURLOPT_VERBOSE, 1L);' "R10: CURLOPT_VERBOSE"
    violate core/src/a.cpp 'curl_easy_setopt(c, CURLOPT_DEBUGFUNCTION, cb);' "R10: CURLOPT_DEBUGFUNCTION"
    violate bridge/a.cpp 'curl_easy_setopt(c, CURLOPT_DEBUGDATA, p);' "R10: CURLOPT_DEBUGDATA"
    violate app/a.cpp 'curl_easy_setopt(c, CURLOPT_STDERR, f);' "R10: CURLOPT_STDERR"
    violate scripts/a.sh 'curl -v imaps://mail.example.org' "R10: curl -v"
    violate scripts/a.sh 'curl -sSv https://example.org' "R10: curl -sSv"
    violate scripts/a.sh 'curl --verbose https://example.org' "R10: curl --verbose"
    violate scripts/a.sh 'curl --trace-ascii - https://example.org' "R10: curl --trace-ascii"
    printf 'curl -sS --max-time 5 https://example.org\n' > "$tmp/scripts/curl_ok.sh"
    expect pass "R10: curl without tracing is allowed"; rm "$tmp/scripts/curl_ok.sh"
    violate docs/a.md 'see /home/someone/project/file' "R5: Linux home path"
    violate docs/a.md 'see /Users/someone/project/file' "R5: macOS home path"
    violate docs/a.md 'see C:\Users\x\file' "R5: drive-letter path"
    exit $fail
fi

ROOT="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
if scan "$ROOT"; then
    echo "constitution checks passed"
else
    echo "constitution checks FAILED (see findings above)" >&2
    exit 1
fi
