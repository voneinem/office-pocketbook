#!/usr/bin/env python3
"""Generates tests/fixtures/malformed/vcard/*.vcf: hostile and broken contact cards for the "never crash" tests (spec 002 FR-026, SC-008).
Developers only; the output is committed. Deterministic. Fictional data and reserved example domains only. The really large inputs (a card of
300 KiB, a line of 1 MiB, ...) are generated inside tests/unit/test_malformed_vcard.cpp instead of being committed.
   python3 scripts/gen-malformed-vcard.py"""
import os

ROOT = os.path.join(os.path.dirname(__file__), '..', 'tests', 'fixtures', 'malformed', 'vcard')
os.makedirs(ROOT, exist_ok=True)
files = {}


def add(name, data):
    files[name + '.vcf'] = data if isinstance(data, bytes) else data.encode('latin-1')


def card(body, version='3.0'):
    return ('BEGIN:VCARD\r\nVERSION:%s\r\nUID:malformed-0001\r\n' % version).encode() + body + b'END:VCARD\r\n'


def lcg(n, seed=7):
    out = bytearray()
    x = seed
    for _ in range(n):
        x = (x * 1103515245 + 12345) & 0x7FFFFFFF
        out.append((x >> 16) & 0xFF)
    return bytes(out)


add('01_truncated_mid_line', b'BEGIN:VCARD\r\nVERSION:3.0\r\nUID:malformed-0001\r\nFN:Trunc')
add('02_no_end', b'BEGIN:VCARD\r\nVERSION:3.0\r\nUID:malformed-0001\r\nFN:No End\r\nTEL:+1 555 0300\r\n')
add('03_no_begin', b'VERSION:3.0\r\nUID:malformed-0001\r\nFN:No Begin\r\nEND:VCARD\r\n')
add('04_binary_garbage', lcg(4096))
add('05_empty_file', b'')
add('06_only_begin', b'BEGIN:VCARD\r\n')
add('07_two_cards', card(b'FN:First\r\n') + card(b'FN:Second\r\n'))
add('08_no_uid', b'BEGIN:VCARD\r\nVERSION:3.0\r\nFN:No Uid\r\nTEL:+1 555 0301\r\nEND:VCARD\r\n')
add('09_bad_charset', card(b'FN;CHARSET=BOGUS-9:J\xfcrgen \xe4\xf6\r\nN;CHARSET=BOGUS-9:M\xfcller;J\xfcrgen;;;\r\n'))
add('10_qp_broken', card(b'FN;ENCODING=QUOTED-PRINTABLE:Br=ZZoken =\r\nNOTE;ENCODING=QUOTED-PRINTABLE:trailing=\r\n'))
add('11_nested_begin', card(b'FN:Nested\r\nBEGIN:VCARD\r\nFN:Inner\r\nEND:VCARD\r\n'))
add('12_nul_bytes', card(b'FN:Nul\x00Byte\r\nNOTE:a\x00b\x00c\r\nTEL:+1 555\x000302\r\n'))
add('13_unterminated_fold', b' continuation first\r\n' + card(b'FN:Fold\r\n NOTE continues\r\n'))
add('14_tel_101', card(b''.join(b'TEL:+1 555 %04d\r\n' % i for i in range(101))))
add('15_email_101', card(b''.join(b'EMAIL:u%d@example.org\r\n' % i for i in range(101))))
add('16_adr_101', card(b''.join(b'ADR:;;%d Example Street;Town;;00000;Land\r\n' % i for i in range(101))))
add('17_lone_cr_line_ends', b'BEGIN:VCARD\rVERSION:3.0\rUID:malformed-0001\rFN:Lone CR\rTEL:+1 555 0303\rEND:VCARD\r')
add('18_invalid_utf8_name', card(b'FN:Bad \xff\xfe\xc3 name\r\nN:\xc3\x28;\xa0\xa1;;;\r\n'))
add('19_unbalanced_quotes', card(b'TEL;TYPE="HOME:+1 555 0304\r\nEMAIL;TYPE=WORK:"a@example.org\r\nADR;LABEL="x;y:;;street;;;;\r\n'))
add('20_version_99', card(b'FN:Future\r\n', version='99.0'))
add('21_lines_without_colon', card(b'FN Missing Colon\r\nTEL +1 555 0305\r\nJUST TEXT\r\n:no name\r\n;;;\r\n'))
add('22_long_params', card(b'TEL;' + b'X-PARAM=' + b'a' * 3000 + b';' + b'TYPE=' + b','.join([b'WORK'] * 200) + b':+1 555 0306\r\n'))
add('23_many_params_groups', card(b'a.b.c.d.TEL;' + b';'.join(b'X%d=v' % i for i in range(300)) + b':+1 555 0307\r\n'))
add('24_huge_structured_values', card(b'N:' + b';' * 5000 + b'\r\nADR:' + b';' * 5000 + b'\r\nORG:' + b';' * 5000 + b'\r\n'))
add('25_escapes_at_the_end', card(b'FN:Ends with backslash\\\r\nNOTE:\\\r\nORG:\\;\\\r\nN:\\\\\\;\\n\\N\r\n'))
add('26_html_and_script', card(b'FN:<script>alert(1)</script>\r\nNOTE:<img src=http://tracker.example.org/x.png>\r\nURL:javascript:alert(1)\r\nEMAIL:<b>a</b>@example.org\r\n'))
add('27_crlf_inside_value_attack', card(b'FN:Inject\\r\\nEND:VCARD\\r\\nBEGIN:VCARD\r\nNOTE:x\r\n'))
add('28_photo_without_data_and_bad_base64', card(b'FN:Photo\r\nPHOTO;ENCODING=b;TYPE=JPEG:!!!not base64!!!\r\nPHOTO:\r\n'))

for name, data in sorted(files.items()):
    with open(os.path.join(ROOT, name), 'wb') as f:
        f.write(data)
print('%d malformed vCard fixtures written' % len(files))
