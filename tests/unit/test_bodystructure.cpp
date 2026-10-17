// T059: parse_bodystructure, choose_text_sections and assemble_from_parts.
#include <string>

#include "pbo/bodystructure.h"
#include "pbo/charset.h"
#include "test_support.h"

using pbo::BodyPart;

namespace {

const char* kPlain = "(\"TEXT\" \"PLAIN\" (\"CHARSET\" \"UTF-8\") NIL NIL \"7BIT\" 120 5 NIL NIL NIL NIL)";
const char* kAlt =
    "((\"TEXT\" \"PLAIN\" (\"CHARSET\" \"utf-8\") NIL NIL \"QUOTED-PRINTABLE\" 100 3 NIL NIL NIL)"
    "(\"TEXT\" \"HTML\" (\"CHARSET\" \"utf-8\") NIL NIL \"QUOTED-PRINTABLE\" 400 9 NIL NIL NIL) \"ALTERNATIVE\" (\"BOUNDARY\" \"b\") NIL NIL)";
const char* kMixed =
    "((\"TEXT\" \"PLAIN\" (\"CHARSET\" \"utf-8\") NIL NIL \"7BIT\" 50 2 NIL NIL NIL)"
    "(\"APPLICATION\" \"PDF\" (\"NAME\" \"report.pdf\") NIL NIL \"BASE64\" 4000 NIL (\"ATTACHMENT\" (\"FILENAME\" \"report.pdf\")) NIL) \"MIXED\" (\"BOUNDARY\" \"m\") NIL NIL)";
const char* kNested =
    "(((\"TEXT\" \"PLAIN\" (\"CHARSET\" \"utf-8\") NIL NIL \"BASE64\" 60 2 NIL NIL NIL)"
    "((\"TEXT\" \"HTML\" (\"CHARSET\" \"utf-8\") NIL NIL \"7BIT\" 90 3 NIL NIL NIL)"
    "(\"IMAGE\" \"PNG\" NIL \"<logo>\" NIL \"BASE64\" 800 NIL (\"INLINE\" NIL) NIL) \"RELATED\" (\"BOUNDARY\" \"r\") NIL NIL) \"ALTERNATIVE\" (\"BOUNDARY\" \"a\") NIL NIL)"
    "(\"APPLICATION\" \"ZIP\" NIL NIL NIL \"BASE64\" 1200 NIL (\"ATTACHMENT\" (\"FILENAME\" \"archive.zip\")) NIL) \"MIXED\" (\"BOUNDARY\" \"o\") NIL NIL)";
}  // namespace

PB_TEST(a_single_text_part) {
    auto bs = pbo::parse_bodystructure(kPlain);
    PB_CHECK(bs.has_value());
    PB_CHECK(!bs->multipart());
    PB_CHECK_EQ(bs->mime(), std::string("text/plain"));
    PB_CHECK_EQ(bs->params["charset"], std::string("UTF-8"));
    PB_CHECK_EQ(bs->encoding, std::string("7bit"));
    PB_CHECK_EQ(bs->size, static_cast<uint64_t>(120));
    PB_CHECK_EQ(bs->section, std::string("1"));
    PB_CHECK(pbo::choose_text_sections(*bs) == std::vector<std::string>({"1"}));
}

PB_TEST(multipart_alternative_numbers_its_parts_and_prefers_plain) {
    auto bs = pbo::parse_bodystructure(kAlt);
    PB_CHECK(bs.has_value());
    PB_CHECK(bs->multipart());
    PB_CHECK_EQ(bs->subtype, std::string("alternative"));
    PB_CHECK_EQ(bs->children.size(), static_cast<size_t>(2));
    PB_CHECK_EQ(bs->children[0].section, std::string("1"));
    PB_CHECK_EQ(bs->children[1].section, std::string("2"));
    PB_CHECK_EQ(bs->children[1].mime(), std::string("text/html"));
    PB_CHECK(pbo::choose_text_sections(*bs) == std::vector<std::string>({"1"}));
}

PB_TEST(html_only_is_chosen_when_there_is_no_plain_text) {
    auto bs = pbo::parse_bodystructure("((\"TEXT\" \"HTML\" (\"CHARSET\" \"utf-8\") NIL NIL \"7BIT\" 90 3 NIL NIL NIL) \"ALTERNATIVE\" (\"BOUNDARY\" \"b\") NIL NIL)");
    PB_CHECK(bs.has_value());
    PB_CHECK(pbo::choose_text_sections(*bs) == std::vector<std::string>({"1"}));
}

PB_TEST(mixed_with_an_attachment_and_its_disposition_file_name) {
    auto bs = pbo::parse_bodystructure(kMixed);
    PB_CHECK(bs.has_value());
    PB_CHECK(pbo::choose_text_sections(*bs) == std::vector<std::string>({"1"}));
    PB_CHECK_EQ(bs->children[1].disposition, std::string("attachment"));
    PB_CHECK_EQ(bs->children[1].disposition_params["filename"], std::string("report.pdf"));
    PB_CHECK_EQ(bs->children[1].section, std::string("2"));
}

PB_TEST(nested_parts_get_dotted_section_numbers) {
    auto bs = pbo::parse_bodystructure(kNested);
    PB_CHECK(bs.has_value());
    PB_CHECK_EQ(bs->children[0].children[0].section, std::string("1.1"));
    PB_CHECK_EQ(bs->children[0].children[1].children[0].section, std::string("1.2.1"));
    PB_CHECK_EQ(bs->children[0].children[1].children[1].section, std::string("1.2.2"));
    PB_CHECK_EQ(bs->children[1].section, std::string("2"));
    PB_CHECK(pbo::choose_text_sections(*bs) == std::vector<std::string>({"1.1"}));
}

PB_TEST(a_forwarded_message_is_a_leaf_and_its_inner_structure_is_skipped) {
    const std::string fwd =
        "((\"TEXT\" \"PLAIN\" (\"CHARSET\" \"utf-8\") NIL NIL \"7BIT\" 30 1 NIL NIL NIL)"
        "(\"MESSAGE\" \"RFC822\" NIL NIL NIL \"7BIT\" 500 (\"Mon, 05 Oct 2026 10:00:00 +0000\" \"Inner\" ((NIL NIL \"ben\" \"example.org\")) NIL NIL NIL NIL NIL NIL \"<i@example.org>\") "
        "(\"TEXT\" \"PLAIN\" (\"CHARSET\" \"utf-8\") NIL NIL \"7BIT\" 100 4 NIL NIL NIL) 20 NIL (\"ATTACHMENT\" (\"FILENAME\" \"forwarded.eml\")) NIL NIL) \"MIXED\" (\"BOUNDARY\" \"f\") NIL NIL)";
    auto bs = pbo::parse_bodystructure(fwd);
    PB_CHECK(bs.has_value());
    PB_CHECK_EQ(bs->children.size(), static_cast<size_t>(2));
    PB_CHECK_EQ(bs->children[1].mime(), std::string("message/rfc822"));
    PB_CHECK(bs->children[1].children.empty());
    PB_CHECK_EQ(bs->children[1].disposition_params["filename"], std::string("forwarded.eml"));
    PB_CHECK(pbo::choose_text_sections(*bs) == std::vector<std::string>({"1"}));
}

PB_TEST(missing_fields_nil_values_literals_and_extended_parameters) {
    auto bs = pbo::parse_bodystructure("(\"text\" \"plain\" NIL NIL NIL \"7bit\" 10)");   // no lines, no extension data
    PB_CHECK(bs.has_value());
    PB_CHECK_EQ(bs->size, static_cast<uint64_t>(10));
    bs = pbo::parse_bodystructure("((\"APPLICATION\" \"X\" (\"NAME\" {11}\r\nGr\xC3\xBC\xC3\x9F" "e.bin) NIL NIL \"BASE64\" 5 NIL (\"ATTACHMENT\" NIL) NIL) \"MIXED\" NIL NIL NIL)");
    PB_CHECK(bs.has_value());
    PB_CHECK_EQ(bs->children[0].params["name"], std::string("Gr\xC3\xBC\xC3\x9F" "e.bin"));
    bs = pbo::parse_bodystructure("((\"APPLICATION\" \"PDF\" NIL NIL NIL \"BASE64\" 5 NIL (\"ATTACHMENT\" (\"FILENAME*\" \"UTF-8''gr%C3%BC%C3%9Fe.pdf\")) NIL) \"MIXED\" NIL NIL NIL)");
    PB_CHECK(bs.has_value());
    PB_CHECK_EQ(bs->children[0].disposition_params["filename"], std::string("gr\xC3\xBC\xC3\x9F" "e.pdf"));
    bs = pbo::parse_bodystructure("((\"APPLICATION\" \"PDF\" NIL NIL NIL \"BASE64\" 5 NIL (\"ATTACHMENT\" (\"FILENAME*0*\" \"UTF-8''gr%C3%BC\" \"FILENAME*1*\" \"%C3%9Fe.pdf\")) NIL) \"MIXED\" NIL NIL NIL)");
    PB_CHECK(bs.has_value());
    PB_CHECK_EQ(bs->children[0].disposition_params["filename"], std::string("gr\xC3\xBC\xC3\x9F" "e.pdf"));
}

PB_TEST(hostile_structures_give_nothing_not_a_crash) {
    PB_CHECK(!pbo::parse_bodystructure("").has_value());
    PB_CHECK(!pbo::parse_bodystructure("garbage").has_value());
    PB_CHECK(!pbo::parse_bodystructure("(((((").has_value());
    PB_CHECK(!pbo::parse_bodystructure("(\"TEXT\" \"PLAIN\" (\"CHARSET\" \"UTF-8\"").has_value());   // truncated
    PB_CHECK(!pbo::parse_bodystructure(std::string(10000, '(')).has_value());
    std::string deep;
    for (int i = 0; i < 40; ++i) deep += "(";
    deep += kPlain;
    for (int i = 0; i < 40; ++i) deep += " \"MIXED\" NIL NIL NIL)";
    PB_CHECK(!pbo::parse_bodystructure(deep).has_value());                                           // deeper than 32
    PB_CHECK(!pbo::parse_bodystructure("(\"TEXT\" \"PLAIN\" NIL NIL NIL \"7BIT\" 99999999999999999999999)").has_value());
    PB_CHECK(!pbo::parse_bodystructure("(\"TEXT\" \"PLAIN\" NIL NIL NIL \"7BIT\" 1 {99999999}\r\nx)").has_value());
    // every prefix of a valid structure: nullopt or a structure, never a crash
    const std::string full = kNested;
    for (size_t n = 0; n <= full.size(); ++n) (void)pbo::parse_bodystructure(full.substr(0, n));
}

PB_TEST(the_body_is_built_from_the_fetched_sections_in_their_transfer_encoding) {
    auto bs = pbo::parse_bodystructure(
        "((\"TEXT\" \"PLAIN\" (\"CHARSET\" \"ISO-8859-1\") NIL NIL \"QUOTED-PRINTABLE\" 30 2 NIL NIL NIL)"
        "(\"APPLICATION\" \"PDF\" (\"NAME\" \"report.pdf\") NIL NIL \"BASE64\" 400 NIL (\"ATTACHMENT\" (\"FILENAME\" \"report.pdf\")) NIL) \"MIXED\" (\"BOUNDARY\" \"m\") NIL NIL)");
    PB_CHECK(bs.has_value());
    const pbo::ParsedMessage m = pbo::assemble_from_parts(*bs, {{"1", "Gr=FC=DFe aus K=F6ln,=\r\n bis bald\r\n"}});
    PB_CHECK(m.body_kind == pbo::ParsedMessage::Body::Text);
    PB_CHECK_EQ(m.text, std::string("Gr\xC3\xBC\xC3\x9F" "e aus K\xC3\xB6ln, bis bald"));
    PB_CHECK_EQ(m.note, std::string(""));
    PB_CHECK_EQ(m.attachments.size(), static_cast<size_t>(1));
    PB_CHECK_EQ(m.attachments[0].section, std::string("2"));
    PB_CHECK_EQ(m.attachments[0].name, std::string("report.pdf"));
    PB_CHECK_EQ(m.attachments[0].mime_type, std::string("application/pdf"));
    PB_CHECK_EQ(m.attachments[0].encoding, std::string("base64"));
    PB_CHECK_EQ(m.attachments[0].size, static_cast<uint64_t>(300));       // 400 base64 bytes are about 300 decoded
}

PB_TEST(html_is_converted_with_a_note_and_links_are_kept) {
    auto bs = pbo::parse_bodystructure("(\"TEXT\" \"HTML\" (\"CHARSET\" \"utf-8\") NIL NIL \"BASE64\" 100 3 NIL NIL NIL)");
    PB_CHECK(bs.has_value());
    // "<p>See <a href=\"https://www.example.org/x\">this</a></p>"
    const pbo::ParsedMessage m = pbo::assemble_from_parts(*bs, {{"1", "PHA+U2VlIDxhIGhyZWY9Imh0dHBzOi8vd3d3LmV4YW1wbGUub3JnL3giPnRoaXM8L2E+PC9wPg=="}});
    PB_CHECK_EQ(m.text, std::string("See this [1]"));
    PB_CHECK_EQ(m.note, std::string("html_converted"));
    PB_CHECK_EQ(m.links.size(), static_cast<size_t>(1));
    PB_CHECK_EQ(m.links[0], std::string("https://www.example.org/x"));
}

PB_TEST(an_unknown_charset_and_an_unfetched_section) {
    auto bs = pbo::parse_bodystructure("(\"TEXT\" \"PLAIN\" (\"CHARSET\" \"x-nope\") NIL NIL \"8BIT\" 10 1 NIL NIL NIL)");
    pbo::ParsedMessage m = pbo::assemble_from_parts(*bs, {{"1", "Gr\xFC\xDF" "e"}});
    PB_CHECK_EQ(m.text, std::string("Gr\xC3\xBC\xC3\x9F" "e"));
    PB_CHECK_EQ(m.note, std::string("charset_unsupported"));
    m = pbo::assemble_from_parts(*bs, {});                                  // nothing fetched
    PB_CHECK(m.body_kind == pbo::ParsedMessage::Body::NoReadableText);
}

PB_TEST(a_message_with_only_an_attachment_has_no_readable_text) {
    auto bs = pbo::parse_bodystructure("((\"APPLICATION\" \"PDF\" NIL NIL NIL \"BASE64\" 400 NIL (\"ATTACHMENT\" (\"FILENAME\" \"scan.pdf\")) NIL) \"MIXED\" (\"BOUNDARY\" \"m\") NIL NIL)");
    PB_CHECK(bs.has_value());
    PB_CHECK(pbo::choose_text_sections(*bs).empty());
    const pbo::ParsedMessage m = pbo::assemble_from_parts(*bs, {});
    PB_CHECK(m.body_kind == pbo::ParsedMessage::Body::NoReadableText);
    PB_CHECK_EQ(m.attachments.size(), static_cast<size_t>(1));
}

PB_TEST(inline_pictures_of_an_html_body_are_not_attachments) {
    auto bs = pbo::parse_bodystructure(kNested);
    PB_CHECK(bs.has_value());
    const pbo::ParsedMessage m = pbo::assemble_from_parts(*bs, {{"1.1", "TmVzdGVkIHBsYWluIHRleHQu"}});   // base64 "Nested plain text."
    PB_CHECK_EQ(m.text, std::string("Nested plain text."));
    PB_CHECK_EQ(m.attachments.size(), static_cast<size_t>(1));
    PB_CHECK_EQ(m.attachments[0].name, std::string("archive.zip"));
    PB_CHECK_EQ(m.attachments[0].section, std::string("2"));
}

PB_TEST_MAIN()
