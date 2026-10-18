#!/usr/bin/env python3
"""Generates the vCard fixtures of spec 002 (Server Contacts): tests/fixtures/vcard/*.unverified.vcf (one card per client style) and
tests/fixtures/vcard/corpus/*.unverified.vcf (24+ varied cards for the SC-007 edit round trip). Developers only; the output is committed.
Deterministic. Fictional people, reserved example domains (example.org) and fictional phone numbers only. Everything here is written by hand
from the specifications and is UNVERIFIED against real clients and servers (hence the .unverified marker).
   python3 scripts/gen-contacts-corpus.py"""
import base64
import os

ROOT = os.path.join(os.path.dirname(__file__), '..', 'tests', 'fixtures', 'vcard')
os.makedirs(os.path.join(ROOT, 'corpus'), exist_ok=True)


def fold(line):
    """RFC 6350 folding at 75 octets (continuation lines start with one space)."""
    b = line.encode('utf-8')
    if len(b) <= 75:
        return line
    out = []
    pos = 0
    first = True
    while pos < len(b):
        n = 75 if first else 74
        end = min(len(b), pos + n)
        while end < len(b) and (b[end] & 0xC0) == 0x80:   # do not cut inside a UTF-8 sequence
            end -= 1
        out.append(('' if first else ' ') + b[pos:end].decode('utf-8'))
        pos = end
        first = False
    return '\r\n'.join(out)


def card(lines, eol='\r\n'):
    return (eol.join(fold(l) for l in lines) + eol).encode('utf-8')


def pseudo_photo(n_bytes, seed):
    """Deterministic pseudo random bytes (a stand-in for a JPEG), base64 encoded."""
    x = seed
    data = bytearray(b'\xff\xd8\xff\xe0')
    while len(data) < n_bytes:
        x = (x * 1103515245 + 12345) & 0x7FFFFFFF
        data.append((x >> 16) & 0xFF)
    return base64.b64encode(bytes(data)).decode('ascii')


files = {}


def add(path, data):
    files[path] = data


# ---- one card per client style (vcard/*.unverified.vcf) ------------------------------------------------------------------------------
add('nextcloud_sabre_30.unverified.vcf', card([
    'BEGIN:VCARD', 'VERSION:3.0', 'PRODID:-//Sabre//Sabre VObject 4.5//EN', 'UID:7a1c3f10-0000-4000-8000-000000000001',
    'FN:Anna Beispiel', 'N:Beispiel;Anna;;;', 'ORG:Example Works GmbH;Sales', 'TITLE:Head of Sales',
    'TEL;TYPE=WORK,VOICE:+1 555 0100', 'TEL;TYPE=CELL:+1 555 0101', 'EMAIL;TYPE=WORK:anna@example.org', 'EMAIL;TYPE=HOME:anna.home@example.org',
    'ADR;TYPE=WORK:;;1 Example Street;Exampletown;EX;12345;Exampleland', 'NOTE:Met at the fair.\\nSecond line.', 'URL:https://www.example.org/anna',
    'BDAY:1985-04-12', 'CATEGORIES:friends,work', 'REV:20260101T120000Z', 'END:VCARD']))

add('apple_groups_30.unverified.vcf', card([
    'BEGIN:VCARD', 'VERSION:3.0', 'PRODID:-//Apple Inc.//iOS 17//EN', 'N:Muster;Max;;;', 'FN:Max Muster', 'ORG:Example Bank;',
    'item1.EMAIL;type=INTERNET;type=pref:max@example.org', 'item1.X-ABLabel:_$!<Work>!$_',
    'item2.EMAIL;type=INTERNET:max.privat@example.org', 'item2.X-ABLabel:Private mail',
    'TEL;type=CELL;type=VOICE;type=pref:+1 555 0110', 'item3.TEL:+1 555 0111', 'item3.X-ABLabel:_$!<Mother>!$_',
    'item4.ADR;type=pref:;;2 Example Lane;Exampletown;;54321;Exampleland', 'item4.X-ABADR:xx', 'item4.X-ABLabel:_$!<Home>!$_',
    'BDAY:1990-11-02', 'X-SOCIALPROFILE;type=twitter:x-apple:examplemax', 'UID:7a1c3f10-0000-4000-8000-000000000002', 'REV:2026-02-01T08:00:00Z', 'END:VCARD']))

add('android_30.unverified.vcf', card([
    'BEGIN:VCARD', 'VERSION:3.0', 'N:Schulze;Karl;Heinz;Dr.;jun.', 'FN:Dr. Karl Heinz Schulze jun.', 'TEL;TYPE=CELL:0170 555 0120', 'TEL;TYPE=HOME:+1 555 0121',
    'EMAIL;TYPE=HOME:karl@example.org', 'ADR;TYPE=HOME:;;3 Example Road;Exampletown;;11111;Exampleland', 'NOTE:Android note', 'UID:7a1c3f10-0000-4000-8000-000000000003', 'END:VCARD']))

add('thunderbird_40.unverified.vcf', card([
    'BEGIN:VCARD', 'VERSION:4.0', 'UID:urn:uuid:7a1c3f10-0000-4000-8000-000000000004', 'FN:Lena Köhler', 'N:Köhler;Lena;;;', 'NICKNAME:Lenchen',
    'EMAIL;PREF=1:lena@example.org', 'TEL;VALUE=uri;TYPE=cell:tel:+1-555-0130', 'ADR;TYPE=work:;;4 Example Square;Exampletown;;22222;Exampleland',
    'NOTE:Umlaute: ä ö ü ß', 'X-CUSTOM-FIELD:keep me', 'END:VCARD']))

add('outlook_21_qp.unverified.vcf', card([
    'BEGIN:VCARD', 'VERSION:2.1', 'UID:7a1c3f10-0000-4000-8000-000000000005', 'N;CHARSET=ISO-8859-1;ENCODING=QUOTED-PRINTABLE:M=FCller;J=F6rg;;;', 'FN;CHARSET=ISO-8859-1;ENCODING=QUOTED-PRINTABLE:J=F6rg M=FCller',
    'TEL;CELL;VOICE:+1 555 0140', 'TEL;WORK;FAX:+1 555 0141', 'EMAIL;PREF;INTERNET:joerg@example.org',
    'NOTE;ENCODING=QUOTED-PRINTABLE:Zeile eins=0D=0AZeile zwei', 'END:VCARD']))

add('photo_card.unverified.vcf', card([
    'BEGIN:VCARD', 'VERSION:3.0', 'UID:7a1c3f10-0000-4000-8000-000000000006', 'FN:Pia Foto', 'N:Foto;Pia;;;', 'EMAIL:pia@example.org',
    'PHOTO;ENCODING=b;TYPE=JPEG:' + pseudo_photo(20 * 1024, 6), 'BDAY:2000-01-31', 'URL:https://www.example.org/pia', 'X-CUSTOM-FIELD:stays', 'END:VCARD']))

add('nameless_org.unverified.vcf', card([
    'BEGIN:VCARD', 'VERSION:3.0', 'UID:7a1c3f10-0000-4000-8000-000000000007', 'ORG:Example Hardware Store', 'TEL;TYPE=WORK:+1 555 0150',
    'EMAIL;TYPE=WORK:shop@example.org', 'END:VCARD']))

# ---- the corpus (vcard/corpus/NN_*.unverified.vcf): varied cards for the lossless edit round trip -----------------------------------------
corpus = []


def c(name, lines, eol='\r\n'):
    corpus.append((name, card(lines, eol)))


uid = [100]


def u():
    uid[0] += 1
    return 'UID:7a1c3f10-0000-4000-8000-%012d' % uid[0]


c('simple_name_phone', ['BEGIN:VCARD', 'VERSION:3.0', u(), 'FN:Ada Alpha', 'N:Alpha;Ada;;;', 'TEL:+1 555 0201', 'END:VCARD'])
c('two_phones_two_emails', ['BEGIN:VCARD', 'VERSION:3.0', u(), 'FN:Bo Beta', 'N:Beta;Bo;;;', 'TEL;TYPE=CELL:+1 555 0202', 'TEL;TYPE=WORK:+1 555 0203',
                           'EMAIL;TYPE=WORK:bo@example.org', 'EMAIL;TYPE=HOME:bo.home@example.org', 'END:VCARD'])
c('address_home_work', ['BEGIN:VCARD', 'VERSION:3.0', u(), 'FN:Cy Gamma', 'N:Gamma;Cy;;;', 'ADR;TYPE=HOME:;;5 Example Way;Exampletown;EX;33333;Exampleland',
                       'ADR;TYPE=WORK:;;6 Example Park;Exampletown;EX;33334;Exampleland', 'END:VCARD'])
c('pobox_and_extended_address', ['BEGIN:VCARD', 'VERSION:3.0', u(), 'FN:Di Delta', 'N:Delta;Di;;;', 'ADR;TYPE=WORK:PO Box 7;Suite 2;7 Example Plaza;Exampletown;;44444;Exampleland', 'END:VCARD'])
c('birthday_url_categories', ['BEGIN:VCARD', 'VERSION:3.0', u(), 'FN:Ed Epsilon', 'N:Epsilon;Ed;;;', 'EMAIL:ed@example.org', 'BDAY:1975-06-30', 'URL:https://www.example.org/ed',
                             'CATEGORIES:family,sports', 'TITLE:Coach', 'END:VCARD'])
c('x_properties', ['BEGIN:VCARD', 'VERSION:3.0', u(), 'FN:Flo Zeta', 'N:Zeta;Flo;;;', 'TEL:+1 555 0204', 'X-ANNIVERSARY:2010-05-05', 'X-CUSTOM-ONE:one', 'X-CUSTOM-TWO;LANG=en:two', 'END:VCARD'])
c('apple_group_email_label', ['BEGIN:VCARD', 'VERSION:3.0', u(), 'FN:Gus Eta', 'N:Eta;Gus;;;', 'item1.EMAIL;type=INTERNET;type=pref:gus@example.org', 'item1.X-ABLabel:_$!<Work>!$_',
                             'item2.EMAIL;type=INTERNET:gus2@example.org', 'item2.X-ABLabel:Club', 'END:VCARD'])
c('apple_group_address', ['BEGIN:VCARD', 'VERSION:3.0', u(), 'FN:Hal Theta', 'N:Theta;Hal;;;', 'item1.ADR;type=HOME;type=pref:;;8 Example Alley;Exampletown;;55555;Exampleland',
                         'item1.X-ABADR:xx', 'item1.X-ABLabel:_$!<Home>!$_', 'END:VCARD'])
c('apple_group_phone_custom_label', ['BEGIN:VCARD', 'VERSION:3.0', u(), 'FN:Ida Iota', 'N:Iota;Ida;;;', 'item1.TEL:+1 555 0205', 'item1.X-ABLabel:Grandma', 'TEL;type=CELL:+1 555 0206', 'END:VCARD'])
c('version_21_qp_umlauts', ['BEGIN:VCARD', 'VERSION:2.1', u(), 'N;CHARSET=ISO-8859-1;ENCODING=QUOTED-PRINTABLE:L=E4ngle;J=FCrgen;;;',
                           'FN;CHARSET=ISO-8859-1;ENCODING=QUOTED-PRINTABLE:J=FCrgen L=E4ngle', 'TEL;CELL:+1 555 0207', 'EMAIL;INTERNET:juergen@example.org', 'END:VCARD'])
c('version_40_uri_tel', ['BEGIN:VCARD', 'VERSION:4.0', u(), 'FN:Kai Kappa', 'N:Kappa;Kai;;;', 'TEL;VALUE=uri;TYPE="work,voice":tel:+1-555-0208', 'EMAIL;TYPE=work:kai@example.org', 'KIND:individual', 'END:VCARD'])
c('nameless_org_only', ['BEGIN:VCARD', 'VERSION:3.0', u(), 'ORG:Lambda Supplies', 'TEL;TYPE=WORK:+1 555 0209', 'END:VCARD'])
c('nameless_email_only', ['BEGIN:VCARD', 'VERSION:3.0', u(), 'EMAIL:mu@example.org', 'END:VCARD'])
c('nameless_phone_only', ['BEGIN:VCARD', 'VERSION:3.0', u(), 'TEL:+1 555 0210', 'END:VCARD'])
c('umlaut_and_emoji_names', ['BEGIN:VCARD', 'VERSION:3.0', u(), 'FN:Zoë Ünal 🙂', 'N:Ünal;Zoë;;;', 'EMAIL:zoe@example.org', 'NOTE:Größe 42 🎉', 'END:VCARD'])
c('long_folded_note', ['BEGIN:VCARD', 'VERSION:3.0', u(), 'FN:Nina Nu', 'N:Nu;Nina;;;', 'NOTE:' + ' '.join(['This is a long note line number %d that must be folded.' % i for i in range(12)]), 'END:VCARD'])
c('many_entries', ['BEGIN:VCARD', 'VERSION:3.0', u(), 'FN:Otto Omikron', 'N:Omikron;Otto;;;'] + ['TEL;TYPE=CELL:+1 555 03%02d' % i for i in range(12)]
                  + ['EMAIL:otto%d@example.org' % i for i in range(12)] + ['END:VCARD'])
c('over_limit_note', ['BEGIN:VCARD', 'VERSION:3.0', u(), 'FN:Pat Pi', 'N:Pi;Pat;;;', 'TEL:+1 555 0211', 'NOTE:' + ('long note text ' * 700).strip(), 'END:VCARD'])
c('over_limit_phone', ['BEGIN:VCARD', 'VERSION:3.0', u(), 'FN:Quin Rho', 'N:Rho;Quin;;;', 'TEL;TYPE=WORK:' + '+1 555 0212 ext ' + '9' * 70, 'TEL;TYPE=CELL:+1 555 0213', 'END:VCARD'])
c('photo_5k', ['BEGIN:VCARD', 'VERSION:3.0', u(), 'FN:Rae Sigma', 'N:Sigma;Rae;;;', 'EMAIL:rae@example.org', 'PHOTO;ENCODING=b;TYPE=JPEG:' + pseudo_photo(5 * 1024, 21), 'X-CUSTOM-FIELD:keep', 'END:VCARD'])
c('photo_200k', ['BEGIN:VCARD', 'VERSION:3.0', u(), 'FN:Sol Tau', 'N:Tau;Sol;;;', 'TEL:+1 555 0214', 'PHOTO;ENCODING=b;TYPE=JPEG:' + pseudo_photo(160 * 1024, 22), 'BDAY:1999-12-31', 'END:VCARD'])
c('photo_40k_logo_key', ['BEGIN:VCARD', 'VERSION:3.0', u(), 'FN:Tia Upsilon', 'N:Upsilon;Tia;;;', 'EMAIL:tia@example.org', 'PHOTO;ENCODING=b;TYPE=PNG:' + pseudo_photo(30 * 1024, 23),
                        'LOGO;ENCODING=b;TYPE=PNG:' + pseudo_photo(3 * 1024, 24), 'KEY;ENCODING=b:' + pseudo_photo(2 * 1024, 25), 'END:VCARD'])
c('lf_line_endings', ['BEGIN:VCARD', 'VERSION:3.0', u(), 'FN:Uma Phi', 'N:Phi;Uma;;;', 'TEL:+1 555 0215', 'NOTE:unix line ends', 'END:VCARD'], eol='\n')
c('escaped_characters', ['BEGIN:VCARD', 'VERSION:3.0', u(), 'FN:Val Chi', 'N:Chi;Val;;;', 'ORG:Chi\\, Psi & Co\\; Ltd', 'NOTE:back\\\\slash\\, comma\\; semicolon\\nnew line', 'END:VCARD'])
c('multiple_org_components_and_title', ['BEGIN:VCARD', 'VERSION:3.0', u(), 'FN:Wes Psi', 'N:Psi;Wes;;;', 'ORG:Psi Industries;R&D;Lab 3', 'TITLE:Engineer', 'ROLE:Tester', 'END:VCARD'])
c('prefix_suffix_additional', ['BEGIN:VCARD', 'VERSION:3.0', u(), 'FN:Prof. Xan Omega Ypsilon III', 'N:Ypsilon;Xan;Omega;Prof.;III', 'EMAIL:xan@example.org', 'END:VCARD'])
c('fn_only', ['BEGIN:VCARD', 'VERSION:3.0', u(), 'FN:Only A Formatted Name', 'TEL:+1 555 0216', 'END:VCARD'])
c('given_only', ['BEGIN:VCARD', 'VERSION:3.0', u(), 'FN:Mama', 'N:;Mama;;;', 'TEL;TYPE=CELL:+1 555 0217', 'END:VCARD'])
c('multi_type_labels', ['BEGIN:VCARD', 'VERSION:3.0', u(), 'FN:Yan Label', 'N:Label;Yan;;;', 'TEL;TYPE=HOME,VOICE,PREF:+1 555 0218', 'TEL;TYPE=WORK,FAX:+1 555 0219', 'TEL;TYPE=PAGER:+1 555 0220',
                       'EMAIL;TYPE=INTERNET,HOME:yan@example.org', 'END:VCARD'])

for i, (name, data) in enumerate(corpus, 1):
    add('corpus/%02d_%s.unverified.vcf' % (i, name), data)

for rel, data in sorted(files.items()):
    with open(os.path.join(ROOT, rel), 'wb') as f:
        f.write(data)
print('%d vCard fixtures written (%d in the corpus)' % (len(files), len(corpus)))
