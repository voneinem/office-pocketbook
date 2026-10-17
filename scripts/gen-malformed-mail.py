#!/usr/bin/env python3
"""Generates tests/fixtures/malformed/mail/*.eml: hostile and broken mail for the "never crash" tests (SC-010).
Developers only; the output is committed. Deterministic. Only reserved example domains are used.
   python3 scripts/gen-malformed-mail.py"""
import os

OUT = os.path.join(os.path.dirname(__file__), '..', 'tests', 'fixtures', 'malformed', 'mail')
os.makedirs(OUT, exist_ok=True)
HDR = b'From: a@example.org\r\nTo: b@example.org\r\nSubject: s\r\nDate: Mon, 05 Oct 2026 10:00:00 +0000\r\nMIME-Version: 1.0\r\n'
files = {}

def add(name, data):
    files[name + '.eml'] = data if isinstance(data, bytes) else data.encode('latin-1')

def mp(body, ctype=b'multipart/mixed; boundary="B"'):
    return HDR + b'Content-Type: ' + ctype + b'\r\n\r\n' + body

add('empty_file', b'')
add('only_newlines', b'\r\n\r\n\r\n')
add('headers_without_blank_line', b'From: a@example.org\r\nSubject: no body separator')
add('header_without_colon', b'From a@example.org\r\nSubject\r\n\r\nbody\r\n')
add('binary_garbage', bytes((i * 131 + 17) & 0xFF for i in range(3000)))
add('nul_bytes_in_headers', b'From: a@exa\x00mple.org\r\nSubject: nu\x00ll\r\n\r\nbody\x00body\r\n')
add('bare_cr_line_ends', b'From: a@example.org\rSubject: cr only\r\rbody line\rsecond\r')
add('lf_only_line_ends', b'From: a@example.org\nSubject: lf only\nContent-Type: text/plain\n\nbody\n')
add('missing_boundary_param', mp(b'--B\r\nContent-Type: text/plain\r\n\r\nx\r\n--B--\r\n', b'multipart/mixed'))
add('empty_boundary', mp(b'--\r\nContent-Type: text/plain\r\n\r\nx\r\n----\r\n', b'multipart/mixed; boundary=""'))
add('unterminated_multipart', mp(b'--B\r\nContent-Type: text/plain\r\n\r\nno closing boundary\r\n'))
add('boundary_never_appears', mp(b'Just text, the boundary is never used.\r\n'))
add('boundary_inside_text', mp(b'--B\r\nContent-Type: text/plain\r\n\r\nline\r\n--BX not a boundary\r\n--B--\r\n'))
add('only_preamble', mp(b'preamble only\r\n--B--\r\n'))
add('part_without_headers', mp(b'--B\r\n\r\nbody of a part without headers\r\n--B--\r\n'))
add('part_header_never_ends', mp(b'--B\r\nContent-Type: text/plain\r\nX-Long: ' + b'a' * 2000 + b'\r\n'))
add('bad_base64', HDR + b'Content-Type: text/plain\r\nContent-Transfer-Encoding: base64\r\n\r\n!!!not base64!!!\r\n====\r\nAAAA\r\n')
add('base64_truncated', HDR + b'Content-Type: text/plain\r\nContent-Transfer-Encoding: base64\r\n\r\nR3LDvMOfZSBh')
add('bad_quoted_printable', HDR + b'Content-Type: text/plain\r\nContent-Transfer-Encoding: quoted-printable\r\n\r\n=ZZ=G1 trailing =\r\n=')
add('unknown_transfer_encoding', HDR + b'Content-Type: text/plain\r\nContent-Transfer-Encoding: x-uuencode\r\n\r\nbegin 644 x\r\n')
add('bad_content_type', HDR + b'Content-Type: ;;;=\r\n\r\nbody\r\n')
add('content_type_without_slash', HDR + b'Content-Type: texthtml\r\n\r\nbody\r\n')
add('huge_charset_name', HDR + b'Content-Type: text/plain; charset=' + b'x' * 5000 + b'\r\n\r\nbody\r\n')
add('charset_with_quotes_and_spaces', HDR + b'Content-Type: text/plain; charset = " utf-8 "\r\n\r\nbody \xe4\r\n')
add('utf8_overlong_and_surrogates', HDR + b'Content-Type: text/plain; charset=utf-8\r\n\r\n\xc0\xaf \xed\xa0\x80 \xf4\x90\x80\x80 \xff\xfe\r\n')
add('utf16_without_bom_odd_length', HDR + b'Content-Type: text/plain; charset=utf-16\r\n\r\nA\x00B\x00C')
add('encoded_word_garbage', b'From: =?UTF-8?B?????=\r\nSubject: =?=?=?=?=?= =?UTF-8?Q?=\r\n\r\nbody\r\n')
add('encoded_word_huge_charset', b'Subject: =?' + b'x' * 3000 + b'?Q?abc?=\r\n\r\nbody\r\n')
add('duplicate_content_type_headers', HDR + b'Content-Type: text/plain\r\nContent-Type: text/html\r\nContent-Type: multipart/mixed; boundary=B\r\n\r\n<p>x</p>\r\n')
add('thousands_of_headers', HDR + b''.join(b'X-H%d: v\r\n' % i for i in range(4000)) + b'\r\nbody\r\n')
add('header_block_over_64k', HDR + b'X-Big: ' + b'a' * 70000 + b'\r\n\r\nbody\r\n')
# a line over 1 MiB is built in memory by tests/unit/test_malformed_mail.cpp (it would add 1 MB to the repository)
add('plain_8bit_without_charset', HDR + b'Content-Type: text/plain\r\n\r\nGr\xfc\xdfe \x93quoted\x94 and \xe2\x82\xac mixed\r\n')
add('folded_header_never_ends', b'Subject: start\r\n' + b' continued\r\n' * 3000 + b'\r\nbody\r\n')
add('date_garbage', b'From: a@example.org\r\nDate: not a date at all\r\nSubject: s\r\n\r\nbody\r\n')
add('date_far_future', b'From: a@example.org\r\nDate: Mon, 05 Oct 9999 10:00:00 +0000\r\nSubject: s\r\n\r\nbody\r\n')
add('from_header_garbage', b'From: <<<>>> ,,, "unterminated, a@@b\r\nTo: ((((((\r\nSubject: s\r\n\r\nbody\r\n')
add('address_list_bomb', b'To: ' + b', '.join(b'u%d@example.org' % i for i in range(3000)) + b'\r\nSubject: s\r\n\r\nbody\r\n')
add('message_rfc822_empty', mp(b'--B\r\nContent-Type: message/rfc822\r\n\r\n\r\n--B--\r\n'))
add('message_rfc822_self_reference', mp(b'--B\r\nContent-Type: message/rfc822\r\n\r\n' + HDR + b'Content-Type: message/rfc822\r\n\r\nFrom: x@example.org\r\n\r\n--B--\r\n'))
add('html_unterminated_tags', HDR + b'Content-Type: text/html\r\n\r\n<html><body><p>open <a href="x <b>bold <i>italic <script>var x = "</scr' )
add('html_unterminated_comment_and_cdata', HDR + b'Content-Type: text/html\r\n\r\n<p>before</p><!-- never closed <p>hidden?</p><![CDATA[ also open')
add('html_attribute_garbage', HDR + b'Content-Type: text/html\r\n\r\n<a href=\'\'\'"""=== href=javascript:alert(1) onclick=x>link</a><img src=data:image/png;base64,AAAA>')
add('html_entity_garbage', HDR + b'Content-Type: text/html\r\n\r\n<p>&#99999999999; &#x110000; &#xD800; &#0; &unknown; &amp &#; &#x;</p>')
add('html_deep_nesting', HDR + b'Content-Type: text/html\r\n\r\n' + b'<div><blockquote>' * 3000 + b'deep' )
add('html_many_links', HDR + b'Content-Type: text/html\r\n\r\n' + b''.join(b'<a href="https://example.org/%d">l%d</a> ' % (i, i) for i in range(3000)))
add('multipart_many_parts', mp(b''.join(b'--B\r\nContent-Type: text/plain\r\n\r\npart %d\r\n' % i for i in range(400)) + b'--B--\r\n'))
add('multipart_nested_12_deep', HDR + b''.join(b'Content-Type: multipart/mixed; boundary="L%d"\r\n\r\n--L%d\r\n' % (i, i) for i in range(12))[:0] + b'Content-Type: multipart/mixed; boundary="L0"\r\n\r\n' + b''.join(b'--L%d\r\nContent-Type: multipart/mixed; boundary="L%d"\r\n\r\n' % (i, i + 1) for i in range(12)) + b'--L12\r\nContent-Type: text/plain\r\n\r\ndeep text\r\n' + b''.join(b'--L%d--\r\n' % i for i in range(12, -1, -1)))
add('attachment_name_garbage', mp(b'--B\r\nContent-Type: application/x; name="' + b'..\\..\\/etc/passwd\x00\r\n\xff' + b'"\r\nContent-Disposition: attachment; filename="../../../x"\r\n\r\nDATA\r\n--B--\r\n'))
add('attachment_name_huge', mp(b'--B\r\nContent-Type: application/x\r\nContent-Disposition: attachment; filename="' + b'n' * 100000 + b'"\r\n\r\nDATA\r\n--B--\r\n'))
add('rfc2231_continuation_bomb', mp(b'--B\r\nContent-Type: application/x\r\nContent-Disposition: attachment' + b''.join(b'; filename*%d*=UTF-8\'\'%%C3' % i for i in range(3000)) + b'\r\n\r\nDATA\r\n--B--\r\n'))

assert len(files) >= 50, len(files)
for name, data in sorted(files.items()):
    with open(os.path.join(OUT, name), 'wb') as f:
        f.write(data)
print(len(files), 'files written to', os.path.normpath(OUT))
