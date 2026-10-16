// T057: parse_message on the fixture mails and on the limits: body selection, transfer encodings, charsets, attachments.
#include <string>

#include "fixtures.h"
#include "pbo/charset.h"
#include "pbo/mime.h"
#include "test_support.h"

using pbo::ParsedMessage;
using pbtest::fixture;

namespace {
ParsedMessage parse(const char* name) { return pbo::parse_message(fixture(std::string("mail/") + name)); }
bool has(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }
}  // namespace

PB_TEST(a_plain_text_message) {
    const ParsedMessage m = parse("plain_text.eml");
    PB_CHECK(m.body_kind == ParsedMessage::Body::Text);
    PB_CHECK_EQ(m.text, std::string("Hello Alice,\n\nthis is a plain message.\nSecond line.\n\nBye\nAnna"));
    PB_CHECK_EQ(m.from_name, std::string("Anna Beispiel"));
    PB_CHECK_EQ(m.from_addr, std::string("anna@example.org"));
    PB_CHECK_EQ(m.to, std::string("Alice <alice@example.org>"));
    PB_CHECK_EQ(m.subject, std::string("Plain text"));
    PB_CHECK_EQ(m.message_id, std::string("<plain@example.org>"));
    PB_CHECK_EQ(m.date_utc, static_cast<int64_t>(1791194400));
    PB_CHECK(m.attachments.empty());
    PB_CHECK(m.links.empty());
    PB_CHECK_EQ(m.note, std::string(""));
}

PB_TEST(an_html_only_message_is_converted_and_nothing_active_survives) {
    const ParsedMessage m = parse("html_only.eml");
    PB_CHECK(m.body_kind == ParsedMessage::Body::Text);
    PB_CHECK_EQ(m.note, std::string("html_converted"));
    PB_CHECK(has(m.text, "Welcome"));
    PB_CHECK(has(m.text, "Hello Alice, see the news [1]."));
    PB_CHECK(has(m.text, "- one\n- two"));
    PB_CHECK(has(m.text, "a | b"));
    PB_CHECK(has(m.text, "& <tag> \xC3\xA4\xC3\xB6\xC3\xBC \xE2\x82\xAC \xF0\x9F\x93\xA7"));
    for (const char* hidden : {"Hidden title", "alert", "color", "tracker", "pixel"}) PB_CHECK(!has(m.text, hidden));
    PB_CHECK_EQ(m.links.size(), static_cast<size_t>(1));
    PB_CHECK_EQ(m.links[0], std::string("https://www.example.org/news"));
}

PB_TEST(multipart_alternative_prefers_the_plain_text) {
    const ParsedMessage m = parse("multipart_alternative.eml");
    PB_CHECK_EQ(m.text, std::string("Plain version of the message."));
    PB_CHECK_EQ(m.note, std::string(""));
    PB_CHECK(m.attachments.empty());
}

PB_TEST(multipart_alternative_without_plain_text_uses_the_converted_html) {
    const ParsedMessage m = pbo::parse_message("Content-Type: multipart/alternative; boundary=\"X\"\r\n\r\n--X\r\nContent-Type: text/html\r\n\r\n<p>Only <b>html</b></p>\r\n--X--\r\n");
    PB_CHECK(m.body_kind == ParsedMessage::Body::Text);
    PB_CHECK_EQ(m.text, std::string("Only html"));
    PB_CHECK_EQ(m.note, std::string("html_converted"));
}

PB_TEST(attachments_are_listed_with_name_type_encoding_size_and_section) {
    const ParsedMessage m = parse("with_attachments.eml");
    PB_CHECK_EQ(m.text, std::string("Please find the documents attached."));
    PB_CHECK_EQ(m.attachments.size(), static_cast<size_t>(2));
    PB_CHECK_EQ(m.attachments[0].section, std::string("2"));
    PB_CHECK_EQ(m.attachments[0].name, std::string("report.pdf"));
    PB_CHECK_EQ(m.attachments[0].mime_type, std::string("application/pdf"));
    PB_CHECK_EQ(m.attachments[0].encoding, std::string("base64"));
    PB_CHECK_EQ(m.attachments[0].size, static_cast<uint64_t>(123));     // decoded size
    PB_CHECK_EQ(m.attachments[1].section, std::string("3"));
    PB_CHECK_EQ(m.attachments[1].name, std::string("gr\xC3\xBC\xC3\x9F" "e.png"));   // RFC 2231
    PB_CHECK_EQ(m.attachments[1].mime_type, std::string("image/png"));
    PB_CHECK_EQ(m.attachments[1].size, static_cast<uint64_t>(64));
}

PB_TEST(quoted_printable_in_latin1_with_soft_line_breaks) {
    const ParsedMessage m = parse("qp_latin1.eml");
    PB_CHECK_EQ(m.text, std::string("Gr\xC3\xBC\xC3\x9F" "e aus K\xC3\xB6ln, dies ist eine sehr lange Zeile, die umgebrochen wurde.\nEuro: \xE2\x82\xAC und Caf\xC3\xA9."));
    PB_CHECK_EQ(m.note, std::string(""));
}

PB_TEST(base64_in_utf8) {
    const ParsedMessage m = parse("base64_utf8.eml");
    PB_CHECK_EQ(m.text, std::string("Gr\xC3\xBC\xC3\x9F" "e aus K\xC3\xB6ln. Ein Satz mit Umlauten: \xC3\xA4\xC3\xB6\xC3\xBC\xC3\x9F \xE2\x82\xAC \xF0\x9F\x93\xA7"));
}

PB_TEST(encoded_header_words_in_the_message_headers) {
    const ParsedMessage m = parse("encoded_subject_2047.eml");
    PB_CHECK_EQ(m.subject, std::string("Gr\xC3\xBC\xC3\x9F" "e aus K\xC3\xB6ln"));
    PB_CHECK_EQ(m.from_name, std::string("J\xC3\xBCrgen M\xC3\xBCller"));
    PB_CHECK_EQ(m.from_addr, std::string("juergen@example.org"));
    PB_CHECK_EQ(m.to, std::string("Zo\xC3\xAB <zoe@example.org>"));
    PB_CHECK_EQ(m.date_utc, static_cast<int64_t>(1791194400 + 3600 - 7200));   // 11:00 +0200 = 09:00 UTC
}

PB_TEST(rfc2231_file_names_with_continuations) {
    const ParsedMessage m = parse("param_2231.eml");
    PB_CHECK_EQ(m.attachments.size(), static_cast<size_t>(1));
    PB_CHECK_EQ(m.attachments[0].name, std::string("Gr\xC3\xBC\xC3\x9F" "e Datei.txt"));
}

PB_TEST(windows_1250_text) {
    const ParsedMessage m = parse("charset_windows1250.eml");
    PB_CHECK_EQ(m.text, std::string("P\xC5\x99\xC3\xAD" "li\xC5\xA1 \xC4\xBE" "lu\xC5\xA5\xC4\x8D" "k\xC3\xBD k\xC5\xAF\xC5\x88 \xC3\xBA" "pi\xC4\x9B\xC4\x99."));
    PB_CHECK_EQ(m.note, std::string(""));
}

PB_TEST(an_unknown_charset_is_read_as_windows_1252_with_a_note) {
    const ParsedMessage m = parse("charset_unknown.eml");
    PB_CHECK(m.body_kind == ParsedMessage::Body::Text);
    PB_CHECK_EQ(m.text, std::string("Gr\xC3\xBC\xC3\x9F" "e aus K\xC3\xB6ln, text in an unknown set."));
    PB_CHECK_EQ(m.note, std::string("charset_unsupported"));
}

PB_TEST(nested_multipart_shows_the_html_with_its_inline_picture_and_lists_only_real_attachments) {
    const ParsedMessage m = parse("nested_multipart.eml");
    // the HTML alternative has a picture (the logo, a part of the message), so it is the body; its text alternative is not shown (test_mime_images)
    PB_CHECK_EQ(m.text, std::string("Nested html\n\xEF\xBF\xBC" "1\xEF\xBF\xBC"));
    PB_CHECK_EQ(m.images.size(), static_cast<size_t>(1));
    PB_CHECK_EQ(m.images[0].section, std::string("1.2.2"));
    PB_CHECK_EQ(m.attachments.size(), static_cast<size_t>(1));         // the inline logo in multipart/related is not an attachment
    PB_CHECK_EQ(m.attachments[0].name, std::string("archive.zip"));
    PB_CHECK_EQ(m.attachments[0].section, std::string("2"));
}

PB_TEST(a_forwarded_message_is_an_attachment_not_the_text) {
    const ParsedMessage m = parse("message_rfc822_attachment.eml");
    PB_CHECK_EQ(m.text, std::string("See the forwarded message."));
    PB_CHECK(!has(m.text, "Inner body"));
    PB_CHECK_EQ(m.attachments.size(), static_cast<size_t>(1));
    PB_CHECK_EQ(m.attachments[0].mime_type, std::string("message/rfc822"));
    PB_CHECK_EQ(m.attachments[0].name, std::string("forwarded.eml"));
    PB_CHECK_EQ(m.attachments[0].section, std::string("2"));
}

PB_TEST(empty_body_and_no_text_parts) {
    ParsedMessage m = parse("empty_body.eml");
    PB_CHECK(m.body_kind == ParsedMessage::Body::Text);
    PB_CHECK_EQ(m.text, std::string(""));
    m = parse("no_text_parts.eml");
    PB_CHECK(m.body_kind == ParsedMessage::Body::NoReadableText);
    PB_CHECK_EQ(m.attachments.size(), static_cast<size_t>(1));          // still listed
    PB_CHECK_EQ(m.attachments[0].name, std::string("scan.pdf"));
}

PB_TEST(quotes_and_reply_headers_are_kept) {
    const ParsedMessage m = parse("quoted_reply.eml");
    PB_CHECK(has(m.text, "> Can we meet on Thursday?\n> I could do 10:00.\n>> Earlier: the room is booked."));
    PB_CHECK_EQ(m.in_reply_to, std::string("<orig@example.org>"));
    PB_CHECK_EQ(m.references, std::string("<orig@example.org>"));
}

PB_TEST(text_parts_before_the_first_attachment_form_the_body) {
    const std::string raw =
        "Content-Type: multipart/mixed; boundary=\"B\"\r\n\r\n--B\r\nContent-Type: text/plain\r\n\r\nIntro\r\n--B\r\nContent-Type: text/plain\r\n\r\nSecond part\r\n"
        "--B\r\nContent-Type: application/pdf; name=\"a.pdf\"\r\nContent-Disposition: attachment\r\n\r\nPDF\r\n--B\r\nContent-Type: text/plain\r\n\r\nSignature after the attachment\r\n--B--\r\n";
    const ParsedMessage m = pbo::parse_message(raw);
    PB_CHECK_EQ(m.text, std::string("Intro\n\nSecond part"));
    PB_CHECK_EQ(m.attachments.size(), static_cast<size_t>(1));
    PB_CHECK_EQ(m.attachments[0].section, std::string("3"));
}

PB_TEST(a_text_part_with_a_file_name_is_an_attachment) {
    const ParsedMessage m = pbo::parse_message("Content-Type: multipart/mixed; boundary=\"B\"\r\n\r\n--B\r\nContent-Type: text/plain\r\n\r\nBody\r\n--B\r\n"
                                               "Content-Type: text/plain; name=\"notes.txt\"\r\nContent-Disposition: attachment; filename=\"notes.txt\"\r\n\r\nfile content\r\n--B--\r\n");
    PB_CHECK_EQ(m.text, std::string("Body"));
    PB_CHECK_EQ(m.attachments.size(), static_cast<size_t>(1));
    PB_CHECK_EQ(m.attachments[0].name, std::string("notes.txt"));
}

PB_TEST(limits_give_the_best_readable_result_and_never_crash) {
    // a text part over the limit is cut, with a note
    std::string big = "Content-Type: text/plain\r\n\r\n";
    for (int i = 0; i < 40000; ++i) big += "line number " + std::to_string(i) + " of a very long message body that goes on\r\n";
    pbo::MimeLimits small;
    small.max_text_part = 100000;
    ParsedMessage m = pbo::parse_message(big, small);
    PB_CHECK(m.body_kind == ParsedMessage::Body::Text);
    PB_CHECK(m.text.size() <= 100000);
    PB_CHECK(m.text.size() > 90000);
    PB_CHECK_EQ(m.note, std::string("truncated"));
    PB_CHECK(pbo::is_valid_utf8(m.text));
    // the whole input over the limit: unreadable, not a crash
    pbo::MimeLimits tiny;
    tiny.max_input = 1000;
    m = pbo::parse_message(std::string(5000, 'x'), tiny);
    PB_CHECK(m.body_kind == ParsedMessage::Body::Unreadable);
    // nesting deeper than the limit and more parts than the limit
    m = pbo::parse_message(fixture("malformed/mail/multipart_nested_12_deep.eml"));
    PB_CHECK(pbo::is_valid_utf8(m.text));
    m = pbo::parse_message(fixture("malformed/mail/multipart_many_parts.eml"));
    PB_CHECK(pbo::is_valid_utf8(m.text));
    PB_CHECK(m.attachments.size() <= 200);
    // a line over 1 MiB
    m = pbo::parse_message("Content-Type: text/plain\r\n\r\n" + std::string((1 << 20) + 100, 'a') + "\r\nnext line\r\n");
    PB_CHECK(pbo::is_valid_utf8(m.text));
}

PB_TEST(safe_file_names) {
    PB_CHECK_EQ(pbo::safe_file_name("report.pdf"), std::string("report.pdf"));
    PB_CHECK_EQ(pbo::safe_file_name("../../etc/passwd"), std::string("passwd"));
    PB_CHECK_EQ(pbo::safe_file_name("C:\\Users\\x\\evil.exe"), std::string("evil.exe"));
    PB_CHECK_EQ(pbo::safe_file_name(".hidden"), std::string("hidden"));
    PB_CHECK_EQ(pbo::safe_file_name("a\tb\nc\x01.txt"), std::string("a b c.txt"));
    PB_CHECK_EQ(pbo::safe_file_name(""), std::string("attachment"));
    PB_CHECK_EQ(pbo::safe_file_name("   "), std::string("attachment"));
    PB_CHECK_EQ(pbo::safe_file_name("..."), std::string("attachment"));
    const std::string longname = std::string(500, 'n') + ".pdf";
    const std::string cut = pbo::safe_file_name(longname);
    PB_CHECK(cut.size() <= 120);
    PB_CHECK(cut.size() > 100);
    PB_CHECK_EQ(cut.substr(cut.size() - 4), std::string(".pdf"));              // the extension survives
    PB_CHECK(pbo::is_valid_utf8(pbo::safe_file_name(std::string(200, '\xC3') + "\xA4.txt")));   // never cut inside a character
}

PB_TEST_MAIN()
