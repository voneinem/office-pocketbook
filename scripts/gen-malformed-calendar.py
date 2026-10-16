#!/usr/bin/env python3
"""Generates tests/fixtures/malformed/ical/*.ics and tests/fixtures/malformed/xml/*.xml: hostile and broken calendar data for
the "never crash" tests (SC-010). Developers only; the output is committed. Deterministic. Reserved example domains only.
   python3 scripts/gen-malformed-calendar.py"""
import os

ROOT = os.path.join(os.path.dirname(__file__), '..', 'tests', 'fixtures', 'malformed')
os.makedirs(os.path.join(ROOT, 'ical'), exist_ok=True)
os.makedirs(os.path.join(ROOT, 'xml'), exist_ok=True)
ics = {}
xml = {}


def add_ics(name, data):
    ics[name + '.ics'] = data if isinstance(data, bytes) else data.encode('latin-1')


def add_xml(name, data):
    xml[name + '.xml'] = data if isinstance(data, bytes) else data.encode('latin-1')


def cal(body):
    return b'BEGIN:VCALENDAR\r\nVERSION:2.0\r\nPRODID:-//example.org//malformed//EN\r\n' + body + b'END:VCALENDAR\r\n'


def ev(props):
    return b'BEGIN:VEVENT\r\nUID:m1@example.org\r\n' + props + b'END:VEVENT\r\n'


add_ics('empty_file', b'')
add_ics('only_newlines', b'\r\n\r\n\r\n')
add_ics('binary_garbage', bytes((i * 131 + 17) & 0xFF for i in range(4000)))
add_ics('nul_bytes', b'BEGIN:VCALENDAR\r\nBEGIN:VEVENT\r\nUID:a\x00b\r\nSUMMARY:x\x00y\r\nDTSTART:2026\x001014T093000Z\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n')
add_ics('no_end_vcalendar', b'BEGIN:VCALENDAR\r\nBEGIN:VEVENT\r\nUID:a\r\nDTSTART:20261014T093000Z\r\nEND:VEVENT\r\n')
add_ics('no_end_vevent', cal(b'BEGIN:VEVENT\r\nUID:a\r\nDTSTART:20261014T093000Z\r\n'))
add_ics('end_without_begin', b'END:VEVENT\r\nEND:VCALENDAR\r\nEND:VALARM\r\n')
add_ics('mismatched_nesting', cal(b'BEGIN:VEVENT\r\nUID:a\r\nBEGIN:VALARM\r\nEND:VEVENT\r\nDTSTART:20261014T093000Z\r\nEND:VALARM\r\n'))
add_ics('line_without_colon', cal(ev(b'SUMMARY\r\nDTSTART 20261014T093000Z\r\nDTSTART:20261014T093000Z\r\n')))
add_ics('empty_names_and_values', cal(ev(b':\r\n;;;:x\r\nSUMMARY:\r\nDTSTART:\r\nX-:\r\n')))
add_ics('quote_never_closes', cal(ev(b'ATTENDEE;CN="Doe, Jane:mailto:jane@example.org\r\nDTSTART:20261014T093000Z\r\n')))
add_ics('parameters_without_values', cal(ev(b'DTSTART;TZID;VALUE=:20261014T093000\r\nATTENDEE;;;;CN=:mailto:\r\n')))
add_ics('bad_dates', cal(ev(b'DTSTART:20261332T256199Z\r\n') + ev(b'DTSTART:2026-10-14T09:30:00Z\r\n') + ev(b'DTSTART:99999999T999999\r\n') + ev(b'DTSTART:00000101\r\n')))
add_ics('dtend_before_dtstart', cal(ev(b'DTSTART:20261014T093000Z\r\nDTEND:20160101T000000Z\r\n')))
add_ics('bad_duration', cal(ev(b'DTSTART:20261014T093000Z\r\nDURATION:P\r\n') + ev(b'DTSTART:20261014T093000Z\r\nDURATION:PT99999999999999999H\r\n') + ev(b'DTSTART:20261014T093000Z\r\nDURATION:-P1D\r\n')))
add_ics('tzid_garbage', cal(ev(b'DTSTART;TZID=../../etc/passwd:20261014T093000\r\n') + ev(b'DTSTART;TZID=' + b'Z' * 5000 + b':20261014T093000\r\n') + ev(b'DTSTART;TZID=\\\\x:20261014T093000\r\n')))
add_ics('rrule_hostile', cal(ev(b'DTSTART:20261014T093000Z\r\nRRULE:FREQ=SECONDLY;INTERVAL=1\r\n') + ev(b'DTSTART:20261014T093000Z\r\nRRULE:FREQ=DAILY;INTERVAL=2147483647;COUNT=2147483647\r\n') + ev(b'DTSTART:20261014T093000Z\r\nRRULE:FREQ=YEARLY;BYMONTH=2;BYMONTHDAY=30\r\n') + ev(b'DTSTART:20261014T093000Z\r\nRRULE:' + b'FREQ=DAILY;' * 200 + b'\r\n')))
add_ics('rrule_dtstart_extremes', cal(ev(b'DTSTART:00010101T000000Z\r\nRRULE:FREQ=DAILY\r\n') + ev(b'DTSTART:99991231T235959Z\r\nRRULE:FREQ=DAILY\r\n') + ev(b'DTSTART;VALUE=DATE:99991231\r\nRRULE:FREQ=YEARLY\r\n')))
add_ics('exdate_rdate_garbage', cal(ev(b'DTSTART:20261014T093000Z\r\nRRULE:FREQ=DAILY;COUNT=3\r\nEXDATE:,,,x,,20261015T093000Z,\r\nRDATE;VALUE=PERIOD:20261020T090000Z/PT1H,garbage/,/\r\nEXDATE;TZID=Nowhere:20261016T093000\r\n')))
add_ics('recurrence_id_without_master', cal(b'BEGIN:VEVENT\r\nUID:o\r\nRECURRENCE-ID:20261015T093000Z\r\nDTSTART:20261015T100000Z\r\nEND:VEVENT\r\nBEGIN:VEVENT\r\nUID:o\r\nRECURRENCE-ID:garbage\r\nDTSTART:20261015T100000Z\r\nEND:VEVENT\r\n'))
add_ics('overrides_without_end_and_cancelled', cal(ev(b'DTSTART:20261014T093000Z\r\nRRULE:FREQ=DAILY;COUNT=3\r\n') + b'BEGIN:VEVENT\r\nUID:m1@example.org\r\nRECURRENCE-ID:20261015T093000Z\r\nSTATUS:CANCELLED\r\nDTSTART:20261015T093000Z\r\nEND:VEVENT\r\n'))
add_ics('escapes_everywhere', cal(ev(b'SUMMARY:\\\\\\\\\\n\\\r\nDESCRIPTION:\\\r\nLOCATION:\\x\\\\,\\;\\n\\N\\\r\nDTSTART:20261014T093000Z\r\n')))
add_ics('folding_garbage', b'BEGIN:VCALENDAR\r\n ' + b'\r\n '.join([b'x'] * 1000) + b'\r\n\tBEGIN:VEVENT\r\n UID:a\r\n DTSTART:20261014T093000Z\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n')
add_ics('many_attendees', cal(ev(b'DTSTART:20261014T093000Z\r\n' + b''.join(b'ATTENDEE:mailto:p%d@example.org\r\n' % i for i in range(3000)))))
add_ics('many_events', cal(b''.join(b'BEGIN:VEVENT\r\nUID:e%d\r\nDTSTART:20261014T093000Z\r\nEND:VEVENT\r\n' % i for i in range(3000))))
add_ics('deep_nesting', b'BEGIN:VCALENDAR\r\n' + b'BEGIN:X-A\r\n' * 5000 + b'BEGIN:VEVENT\r\nUID:a\r\nDTSTART:20261014T093000Z\r\nEND:VEVENT\r\n')
add_ics('invalid_utf8', cal(ev(b'SUMMARY:Caf\xe9 \xff\xfe \xc0\xaf \xed\xa0\x80\r\nDESCRIPTION:\xf8\x88\x80\x80\x80\r\nDTSTART:20261014T093000Z\r\n')))
add_ics('cr_only_and_mixed_line_ends', b'BEGIN:VCALENDAR\rBEGIN:VEVENT\nUID:a\r\nDTSTART:20261014T093000Z\rSUMMARY:mixed\n\rEND:VEVENT\r\nEND:VCALENDAR')
add_ics('vtimezone_garbage', cal(b'BEGIN:VTIMEZONE\r\nTZID:Europe/Berlin\r\nBEGIN:STANDARD\r\nDTSTART:garbage\r\nRRULE:FREQ=YEARLY;BYMONTH=99\r\nEND:STANDARD\r\n') + ev(b'DTSTART;TZID=Europe/Berlin:20261014T093000\r\n'))
add_ics('html_instead_of_calendar', b'<!DOCTYPE html><html><body><h1>404 Not Found</h1></body></html>')
add_ics('vcard_instead_of_calendar', b'BEGIN:VCARD\r\nVERSION:3.0\r\nFN:Alice Example\r\nEND:VCARD\r\n')

# (the very large inputs - 1 MiB lines, 250,000 nodes, 9 MiB documents, 10,000 levels - are built in the tests, not stored)

# XML answers of a CalDAV server
add_xml('empty_file', b'')
add_xml('only_whitespace', b'  \r\n\t\r\n')
add_xml('binary_garbage', bytes((i * 131 + 17) & 0xFF for i in range(4000)))
add_xml('billion_laughs', b'<?xml version="1.0"?><!DOCTYPE lolz [<!ENTITY lol "lol"><!ENTITY lol2 "&lol;&lol;&lol;&lol;&lol;&lol;&lol;&lol;&lol;&lol;"><!ENTITY lol3 "&lol2;&lol2;&lol2;&lol2;&lol2;&lol2;&lol2;&lol2;&lol2;&lol2;">]><lolz>&lol3;</lolz>')
add_xml('external_entity', b'<?xml version="1.0"?><!DOCTYPE d [<!ENTITY x SYSTEM "file:///etc/passwd">]><d>&x;</d>')
add_xml('external_dtd', b'<!DOCTYPE d SYSTEM "http://example.org/evil.dtd"><d/>')
add_xml('processing_instruction', b'<?xml version="1.0"?><?xml-stylesheet href="x.xsl"?><d/>')
add_xml('pi_inside', b'<d><?pi data?></d>')
add_xml('unknown_entity', b'<d xmlns="DAV:">&nbsp;&copy;&unknown;</d>')
add_xml('bad_numeric_references', b'<d>&#0;&#xD800;&#x110000;&#99999999999;&#x;&#;&#xZZ;</d>')
add_xml('unterminated_everything', b'<d><a><b>text')
add_xml('mismatched_tags', b'<a><b></a></b>')
add_xml('two_roots', b'<a/><b/>')
add_xml('text_after_root', b'<a/>trailing text')
add_xml('unterminated_comment', b'<a><!-- never closed </a>')
add_xml('unterminated_cdata', b'<a><![CDATA[never closed</a>')
add_xml('attribute_garbage', b'<a x=1 y="2 z=\'3\'/>')
add_xml('duplicate_attributes', b'<a x="1" x="2"/>')
add_xml('unbound_prefix', b'<d:multistatus><x:response/></d:multistatus>')
add_xml('xmlns_garbage', b'<a xmlns:="x" xmlns:b xmlns="" xmlns:c=\'\'><b:x/></a>')
add_xml('nul_bytes', b'<a>te\x00xt</a\x00>')
add_xml('invalid_utf8', b'<a x="\xff\xfe">\xc0\xaf\xed\xa0\x80</a>')
add_xml('bom_then_garbage', b'\xef\xbb\xbf\xef\xbb\xbf<a/>')
add_xml('multistatus_half_valid', b'<d:multistatus xmlns:d="DAV:"><d:response><d:href>/a</d:href><d:propstat><d:prop><d:getetag>"1"</d:getetag></d:prop><d:status>HTTP/1.1 200 OK</d:status></d:propstat></d:response><d:response><d:href>')

for name, data in ics.items():
    with open(os.path.join(ROOT, 'ical', name), 'wb') as f:
        f.write(data)
for name, data in xml.items():
    with open(os.path.join(ROOT, 'xml', name), 'wb') as f:
        f.write(data)
print('%d ical and %d xml files written' % (len(ics), len(xml)))
