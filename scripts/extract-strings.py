#!/usr/bin/env python3
"""Lists every text that the QML files pass to qsTr() (the source strings of the interface), one per line, in a stable order.

    scripts/extract-strings.py            print the strings, escaped like qml/strings_de.tsv (\\n, \\t, \\\\)
    scripts/extract-strings.py --check    compare with qml/strings_de.tsv: missing entries, stale entries, placeholders that do not match

The check is what `ctest -R translations` runs (T175): the German table must cover every string, and a translation must keep the
placeholders (%1, %2, %n) of its source. Only plain string literals are found; a qsTr() that builds its text by joining is reported as unsupported.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
QML = ROOT / "qml"
TABLE = QML / "strings_de.tsv"

# qsTr( "..." ) or qsTr( '...' ), the first argument only
CALL = re.compile(r'qsTr\(\s*("(?:[^"\\]|\\.)*"|\'(?:[^\'\\]|\\.)*\')\s*([,)+])')
ESC = {"n": "\n", "t": "\t", "\\": "\\", '"': '"', "'": "'", "/": "/"}


def unescape_js(lit: str) -> str:
    body = lit[1:-1]
    out, i = [], 0
    while i < len(body):
        c = body[i]
        if c == "\\" and i + 1 < len(body):
            n = body[i + 1]
            if n == "u" and i + 5 < len(body) + 1:
                out.append(chr(int(body[i + 2:i + 6], 16)))
                i += 6
                continue
            out.append(ESC.get(n, n))
            i += 2
            continue
        out.append(c)
        i += 1
    return "".join(out)


def escape_tsv(text: str) -> str:
    return text.replace("\\", "\\\\").replace("\n", "\\n").replace("\t", "\\t")


def unescape_tsv(text: str) -> str:
    out, i = [], 0
    while i < len(text):
        if text[i] == "\\" and i + 1 < len(text):
            out.append({"n": "\n", "t": "\t", "\\": "\\"}.get(text[i + 1], text[i + 1]))
            i += 2
        else:
            out.append(text[i])
            i += 1
    return "".join(out)


def sources():
    found, unsupported = {}, []
    for path in sorted(QML.rglob("*.qml")):
        text = path.read_text(encoding="utf-8")
        for m in CALL.finditer(text):
            if m.group(2) == "+":
                unsupported.append(f"{path.relative_to(ROOT)}: {m.group(1)[:60]}")
                continue
            found.setdefault(unescape_js(m.group(1)), path.relative_to(ROOT).as_posix())
    return found, unsupported


def placeholders(text: str):
    return sorted(re.findall(r"%(?:[1-9]|n)", text))


def load_table():
    table = {}
    if not TABLE.exists():
        return table
    for number, line in enumerate(TABLE.read_text(encoding="utf-8").splitlines(), 1):
        if not line.strip() or line.startswith("#"):
            continue
        parts = line.split("\t")
        if len(parts) != 2:
            print(f"{TABLE.name}:{number}: expected one tab between source and translation")
            sys.exit(1)
        table[unescape_tsv(parts[0])] = unescape_tsv(parts[1])
    return table


def main():
    found, unsupported = sources()
    if "--check" not in sys.argv:
        for s in sorted(found):
            print(escape_tsv(s))
        return 0
    table = load_table()
    bad = 0
    for s in sorted(found):
        if s not in table:
            print(f"missing: {escape_tsv(s)}   ({found[s]})")
            bad += 1
            continue
        for part in table[s].split("||"):   # singular||plural
            if placeholders(part) != placeholders(s) and not (placeholders(s) == ["%n"] and "%n" not in part):
                print(f"placeholders differ: {escape_tsv(s)}  ->  {escape_tsv(part)}")
                bad += 1
        if not table[s].strip():
            print(f"empty translation: {escape_tsv(s)}")
            bad += 1
    for s in sorted(table):
        if s not in found:
            print(f"stale: {escape_tsv(s)}")
            bad += 1
    for u in unsupported:
        print(f"unsupported qsTr (joined text): {u}")
        bad += 1
    print(f"{len(found)} strings, {len(table)} translations, {bad} problem(s)")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
