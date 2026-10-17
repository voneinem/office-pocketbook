// T190: pictures of a mail. An HTML body with pictures is shown with them (the pictures are listed, their place in the text is marked); a message whose
// HTML has none keeps the plain-text alternative as before. Inline pictures are the parts of a multipart/related that the HTML names by Content-ID; they are
// not attachments. Nothing is fetched here.
#include <string>

#include "pbo/mime.h"
#include "test_support.h"

using pbo::ParsedMessage;

namespace {

const std::string kMark = "\xEF\xBF\xBC";

std::string crlf(std::string s) {
    std::string out;
    for (char c : s) {
        if (c == '\n') out += '\r';
        out += c;
    }
    return out;
}

const char* kHead = "From: Shop <shop@example.org>\nTo: alice@example.org\nSubject: Offer\nDate: Fri, 16 Oct 2026 14:01:36 +0200\nMessage-ID: <o1@example.org>\nMIME-Version: 1.0\n";

// plain + (html with a cid picture and the picture itself)
std::string alternative_with_inline_picture() {
    return crlf(std::string(kHead) +
        "Content-Type: multipart/alternative; boundary=\"alt\"\n\n"
        "--alt\nContent-Type: text/plain; charset=utf-8\n\nPlain version of the offer.\n\n"
        "--alt\nContent-Type: multipart/related; boundary=\"rel\"\n\n"
        "--rel\nContent-Type: text/html; charset=utf-8\n\n<html><body><p>Hello</p><img src=\"cid:banner@x\" alt=\"Banner\" width=\"600\"><p>Bye</p></body></html>\n\n"
        "--rel\nContent-Type: image/png; name=\"b.png\"\nContent-Transfer-Encoding: base64\nContent-ID: <banner@x>\nContent-Disposition: inline; filename=\"b.png\"\n\niVBORw0KGgo=\n\n"
        "--rel--\n\n--alt--\n");
}

std::string alternative_html_without_pictures() {
    return crlf(std::string(kHead) +
        "Content-Type: multipart/alternative; boundary=\"alt\"\n\n"
        "--alt\nContent-Type: text/plain; charset=utf-8\n\nPlain version.\n\n"
        "--alt\nContent-Type: text/html; charset=utf-8\n\n<p>Html <b>version</b></p>\n\n--alt--\n");
}

std::string alternative_with_remote_picture() {
    return crlf(std::string(kHead) +
        "Content-Type: multipart/alternative; boundary=\"alt\"\n\n"
        "--alt\nContent-Type: text/plain; charset=utf-8\n\nPlain version.\n\n"
        "--alt\nContent-Type: text/html; charset=utf-8\n\n<p>See</p><a href=\"https://shop.example.org/offer\"><img src=\"https://img.example.org/a.jpg\" alt=\"Offer\"></a>\n\n--alt--\n");
}

}  // namespace

PB_TEST(an_html_body_with_an_inline_picture_is_shown_with_it_and_the_picture_is_not_an_attachment) {
    const ParsedMessage m = pbo::parse_message(alternative_with_inline_picture());
    PB_CHECK(m.body_kind == ParsedMessage::Body::Text);
    PB_CHECK(m.text.find("Plain version") == std::string::npos);   // the HTML alternative won because it has a picture
    PB_CHECK(m.text.find("Hello") != std::string::npos);
    PB_CHECK(m.text.find(kMark + "1" + kMark) != std::string::npos);
    PB_CHECK_EQ(m.images.size(), size_t(1));
    const pbo::MessageImage& im = m.images[0];
    PB_CHECK_EQ(im.kind, std::string("cid"));
    PB_CHECK_EQ(im.alt, std::string("Banner"));
    PB_CHECK_EQ(im.mime_type, std::string("image/png"));
    PB_CHECK_EQ(im.section, std::string("2.2"));
    PB_CHECK_EQ(im.encoding, std::string("base64"));
    PB_CHECK(im.size > 0);
    PB_CHECK_EQ(im.width, 600);
    PB_CHECK(m.attachments.empty());
}

PB_TEST(an_html_alternative_without_pictures_leaves_the_plain_text_as_the_body) {
    const ParsedMessage m = pbo::parse_message(alternative_html_without_pictures());
    PB_CHECK_EQ(m.text, std::string("Plain version."));
    PB_CHECK(m.images.empty());
}

PB_TEST(a_remote_picture_is_listed_as_remote_and_nothing_is_fetched) {
    const ParsedMessage m = pbo::parse_message(alternative_with_remote_picture());
    PB_CHECK(m.text.find(kMark + "1" + kMark) != std::string::npos);
    PB_CHECK_EQ(m.images.size(), size_t(1));
    PB_CHECK_EQ(m.images[0].kind, std::string("remote"));
    PB_CHECK_EQ(m.images[0].src, std::string("https://img.example.org/a.jpg"));
    PB_CHECK(m.images[0].section.empty());
    PB_CHECK_EQ(m.links.size(), size_t(1));
}

PB_TEST(a_cid_that_names_no_part_is_listed_without_a_section) {
    std::string raw = alternative_with_inline_picture();
    const size_t at = raw.find("cid:banner@x");
    raw.replace(at, 12, "cid:other@x");
    const ParsedMessage m = pbo::parse_message(raw);
    PB_CHECK_EQ(m.images.size(), size_t(1));
    PB_CHECK_EQ(m.images[0].kind, std::string("cid"));
    PB_CHECK(m.images[0].section.empty());
}

PB_TEST(plain_text_cannot_carry_a_picture_marker) {
    const std::string raw = crlf(std::string(kHead) + "Content-Type: text/plain; charset=utf-8\n\nbefore " + kMark + "1" + kMark + " after\n");
    const ParsedMessage m = pbo::parse_message(raw);
    PB_CHECK(m.text.find(kMark) == std::string::npos);
    PB_CHECK(m.images.empty());
}

PB_TEST_MAIN()
