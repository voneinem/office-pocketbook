// T058: HTML to readable text. Nothing is fetched, only text comes out, and hostile input completes quickly.
#include <chrono>
#include <string>
#include <vector>

#include "pbo/charset.h"
#include "pbo/html_text.h"
#include "test_support.h"

namespace {
std::string text_of(const std::string& html, std::vector<std::string>* links = nullptr) { return pbo::html_to_text(html, links); }
bool has(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }
}  // namespace

PB_TEST(head_script_style_svg_template_and_comments_are_skipped) {
    const std::string t = text_of("<html><head><title>Hidden</title><style>p{color:red}</style></head><body>"
                                  "<script>alert('x')</script><!-- comment --><svg><text>vector</text></svg><template><p>tpl</p></template>"
                                  "<p>Visible</p></body></html>");
    PB_CHECK_EQ(t, std::string("Visible"));
    PB_CHECK_EQ(text_of("<p>a</p><![CDATA[ hidden ]]><p>b</p>"), std::string("a\n\nb"));
}

PB_TEST(paragraphs_headings_line_breaks_and_rules) {
    PB_CHECK_EQ(text_of("<h1>Title</h1><p>First</p><p>Second<br>line</p><hr><div>Last</div>"), std::string("Title\n\nFirst\n\nSecond\nline\n\n-----\n\nLast"));
    PB_CHECK_EQ(text_of("one<br/>two<BR>three"), std::string("one\ntwo\nthree"));
    PB_CHECK_EQ(text_of("<p>  lots   of\n   white\tspace  </p>"), std::string("lots of white space"));
}

PB_TEST(lists_numbered_and_bulleted_and_nested) {
    PB_CHECK_EQ(text_of("<ul><li>one</li><li>two</li></ul>"), std::string("- one\n- two"));
    PB_CHECK_EQ(text_of("<ol><li>first</li><li>second</li></ol>"), std::string("1. first\n2. second"));
    PB_CHECK_EQ(text_of("<ul><li>a<ul><li>b</li></ul></li><li>c</li></ul>"), std::string("- a\n  - b\n- c"));
}

PB_TEST(tables_become_rows_with_bars) {
    PB_CHECK_EQ(text_of("<table><tr><td>a</td><td>b</td></tr><tr><th>c</th><td>d</td></tr></table>"), std::string("a | b\nc | d"));
    PB_CHECK_EQ(text_of("<p>before</p><table><tr><td>x</td></tr></table><p>after</p>"), std::string("before\n\nx\n\nafter"));
}

PB_TEST(rows_of_empty_cells_leave_no_lines_of_bars_but_a_bar_the_sender_wrote_stays) {
    // layout tables are full of empty cells: their separators carry nothing and are not shown
    PB_CHECK_EQ(text_of("<p>a</p><table><tr><td></td><td></td><td></td></tr></table><p>b</p>"), std::string("a\n\nb"));
    PB_CHECK_EQ(text_of("<table><tr><td>x</td></tr><tr><td> </td><td>&nbsp;</td></tr><tr><td>y</td></tr></table>"), std::string("x\ny"));
    // a cell with text keeps its separators, also when the first cell is empty
    PB_CHECK_EQ(text_of("<table><tr><td></td><td>b</td></tr></table>"), std::string(" | b"));
    PB_CHECK_EQ(text_of("<table><tr><td>a</td><td></td></tr></table>"), std::string("a |"));
    // a bar that is text of the mail is text
    PB_CHECK_EQ(text_of("<p>a</p><p>|</p><p>b</p>"), std::string("a\n\n|\n\nb"));
}

PB_TEST(blockquotes_get_quote_markers) {
    PB_CHECK_EQ(text_of("<p>Hi</p><blockquote>quoted</blockquote><p>bye</p>"), std::string("Hi\n\n> quoted\n\nbye"));
    PB_CHECK_EQ(text_of("<blockquote>outer<blockquote>inner</blockquote></blockquote>"), std::string("> outer\n>\n>> inner"));
}

PB_TEST(links_become_numbered_text_and_the_addresses_are_listed) {
    std::vector<std::string> links;
    const std::string t = text_of("<p>See <a href=\"https://www.example.org/news\">the news</a> and <a href='mailto:a@example.org'>write</a>, "
                                  "again <a href=\"https://www.example.org/news\">the news</a>.</p>", &links);
    PB_CHECK_EQ(t, std::string("See the news [1] and write [2], again the news [1]."));
    PB_CHECK_EQ(links.size(), static_cast<size_t>(2));
    PB_CHECK_EQ(links[0], std::string("https://www.example.org/news"));
    PB_CHECK_EQ(links[1], std::string("mailto:a@example.org"));
}

PB_TEST(unsafe_or_empty_link_targets_are_not_listed) {
    std::vector<std::string> links;
    const std::string t = text_of("<a href=\"javascript:alert(1)\">js</a> <a href=\"data:text/html;base64,AAAA\">data</a> <a href=\"\">empty</a> <a name=\"x\">anchor</a> "
                                  "<a href=\"#top\">top</a> <a href=\"ftp://example.org/f\">ftp</a>", &links);
    PB_CHECK_EQ(t, std::string("js data empty anchor top ftp"));
    PB_CHECK(links.empty());
}

PB_TEST(images_are_never_loaded_only_their_alt_text_is_shown) {
    std::vector<std::string> links;
    PB_CHECK_EQ(text_of("<p>a<img src=\"http://tracker.example.org/p.gif\" alt=\"\">b</p>", &links), std::string("ab"));
    PB_CHECK_EQ(text_of("<p>logo: <img src=\"cid:logo\" alt=\"Company logo\"></p>"), std::string("logo: [Company logo]"));
    PB_CHECK(links.empty());                                  // the tracker address is not even listed
    PB_CHECK_EQ(text_of("<iframe src=\"http://example.org\">frame text</iframe><object data=\"x\">obj</object><p>kept</p>"), std::string("kept"));
}

PB_TEST(character_references_numeric_and_named) {
    PB_CHECK_EQ(text_of("&amp; &lt;tag&gt; &quot;q&quot; &nbsp;|&nbsp;"), std::string("& <tag> \"q\" \xC2\xA0|\xC2\xA0"));
    PB_CHECK_EQ(text_of("&auml;&ouml;&uuml;&szlig;&Auml; &euro; &hellip; &mdash; &copy;"), std::string("\xC3\xA4\xC3\xB6\xC3\xBC\xC3\x9F\xC3\x84 \xE2\x82\xAC \xE2\x80\xA6 \xE2\x80\x94 \xC2\xA9"));
    PB_CHECK_EQ(text_of("&#8364; &#x20AC; &#X1F4E7;"), std::string("\xE2\x82\xAC \xE2\x82\xAC \xF0\x9F\x93\xA7"));
    // wrong ones stay as written, or become the replacement character when the number is not a character
    PB_CHECK_EQ(text_of("&unknown; &amp &#; &#x; AT&T"), std::string("&unknown; & &#; &#x; AT&T"));
    PB_CHECK(pbo::is_valid_utf8(text_of("&#99999999999; &#x110000; &#xD800; &#0; &#xFFFF;")));
}

PB_TEST(unterminated_markup_does_not_swallow_the_text) {
    PB_CHECK_EQ(text_of("before <b>bold"), std::string("before bold"));
    PB_CHECK_EQ(text_of("a < b and c > d"), std::string("a < b and c > d"));            // not a tag
    PB_CHECK(has(text_of("<p>x</p><!-- never closed <p>hidden</p>"), "x"));
    PB_CHECK(!has(text_of("<p>x</p><!-- never closed <p>hidden</p>"), "hidden"));
    PB_CHECK(!has(text_of("<p>x</p><script>never closed <p>hidden</p>"), "hidden"));
    PB_CHECK_EQ(text_of("<a href=\"unterminated attribute>text"), std::string(""));
    PB_CHECK_EQ(text_of(""), std::string(""));
    PB_CHECK_EQ(text_of("just text"), std::string("just text"));
}

PB_TEST(hostile_input_is_bounded_and_fast) {
    const auto t0 = std::chrono::steady_clock::now();
    std::string nest;
    for (int i = 0; i < 10000; ++i) nest += "<div><blockquote>";
    nest += "deep";
    const std::string out = text_of(nest);
    PB_CHECK(has(out, "deep"));
    PB_CHECK(pbo::is_valid_utf8(out));
    std::string tags;
    for (int i = 0; i < 200000; ++i) tags += "<a ";
    (void)text_of(tags);
    std::string links;
    std::vector<std::string> list;
    for (int i = 0; i < 3000; ++i) links += "<a href=\"https://example.org/" + std::to_string(i) + "\">l</a>";
    (void)text_of(links, &list);
    PB_CHECK(list.size() <= 500);                              // the list is capped
    pbo::MimeLimits small;
    small.max_text_part = 1000;
    PB_CHECK(pbo::html_to_text(std::string(100000, 'x'), nullptr, small).size() <= 1000);
    std::string noise;
    for (int i = 0; i < 100000; ++i) noise += static_cast<char>((i * 131 + 7) & 0xFF);
    PB_CHECK(pbo::is_valid_utf8(text_of(noise)));
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    PB_CHECK(ms < 3000);                                       // all of the above together, on the host
}

// ---- pictures (T190): with an image list the converter marks where a picture belongs; nothing is fetched here ----

namespace {
const std::string kMark = "\xEF\xBF\xBC";   // U+FFFC around the number of the picture
std::string with_images(const std::string& html, std::vector<pbo::HtmlImage>* images, std::vector<std::string>* links = nullptr) {
    std::vector<std::string> own;
    return pbo::html_to_text_continue(html, links ? links : &own, images);
}
}  // namespace

PB_TEST(a_picture_with_an_inline_or_https_source_becomes_a_marker_line_and_a_list_entry) {
    std::vector<pbo::HtmlImage> images;
    const std::string t = with_images("<p>before</p><img src=\"cid:logo@x\" alt=\"Logo\" width=\"200\" height=\"60\"><p>after</p>"
                                      "<img src=\"https://img.example.org/a/b.jpg?x=1\" alt=\"Banner\">", &images);
    PB_CHECK_EQ(t, "before\n\n" + kMark + "1" + kMark + "\n\nafter\n\n" + kMark + "2" + kMark);
    PB_CHECK_EQ(images.size(), size_t(2));
    PB_CHECK_EQ(images[0].src, std::string("cid:logo@x"));
    PB_CHECK_EQ(images[0].alt, std::string("Logo"));
    PB_CHECK_EQ(images[0].width, 200);
    PB_CHECK_EQ(images[0].height, 60);
    PB_CHECK_EQ(images[1].src, std::string("https://img.example.org/a/b.jpg?x=1"));
}

PB_TEST(without_an_image_list_nothing_changes_and_other_sources_stay_text) {
    std::vector<pbo::HtmlImage> images;
    PB_CHECK_EQ(text_of("<p>logo: <img src=\"cid:logo\" alt=\"Company logo\"></p>"), std::string("logo: [Company logo]"));
    // plain http (no TLS), data:, file:, relative and script addresses are never listed: the alt text stays
    const std::string t = with_images("<img src=\"http://x.example.org/a.png\" alt=\"A\"><img src=\"data:image/png;base64,AAAA\" alt=\"B\">"
                                      "<img src=\"file:///etc/passwd\" alt=\"C\"><img src=\"/rel.png\" alt=\"D\"><img src=\"javascript:x\" alt=\"E\">", &images);
    PB_CHECK(images.empty());
    PB_CHECK(has(t, "[A]") && has(t, "[B]") && has(t, "[C]") && has(t, "[D]") && has(t, "[E]"));
}

PB_TEST(tracking_pixels_are_not_listed_and_the_same_picture_shares_one_entry) {
    std::vector<pbo::HtmlImage> images;
    const std::string t = with_images("<img src=\"https://t.example.org/p.gif\" width=\"1\" height=\"1\"><img src=\"https://t.example.org/q.gif\" width=\"0\">"
                                      "<img src=\"https://t.example.org/s.gif\" height=\"1\">"
                                      "<img src=\"https://i.example.org/same.png\"><img src=\"https://i.example.org/same.png\">", &images);
    PB_CHECK_EQ(images.size(), size_t(1));
    PB_CHECK_EQ(t, kMark + "1" + kMark + "\n" + kMark + "1" + kMark);
}

PB_TEST(a_linked_picture_keeps_its_link_marker_and_the_marker_character_in_the_text_is_removed) {
    std::vector<pbo::HtmlImage> images;
    std::vector<std::string> links;
    const std::string t = with_images("<a href=\"https://shop.example.org/x\"><img src=\"cid:a\" alt=\"\"></a><p>text " + kMark + "7" + kMark + " end</p>", &images, &links);
    PB_CHECK_EQ(links.size(), size_t(1));
    PB_CHECK(has(t, kMark + "1" + kMark));
    PB_CHECK(has(t, "[1]"));
    PB_CHECK(!has(t, kMark + "7"));               // a marker typed into the mail cannot point at a picture
    PB_CHECK(has(t, "text 7 end"));
}

PB_TEST(the_list_is_capped_and_hostile_sources_are_refused) {
    std::vector<pbo::HtmlImage> images;
    std::string html;
    for (int i = 0; i < 100; ++i) html += "<img src=\"https://i.example.org/" + std::to_string(i) + ".png\">";
    with_images(html, &images);
    PB_CHECK(images.size() <= 40);
    std::vector<pbo::HtmlImage> more;
    with_images("<img src=\"https://i.example.org/" + std::string(3000, 'a') + "\"><img src=\"https://user:pw@i.example.org/a.png\"><img src=\"https://i.example.org/a b.png\">", &more);
    PB_CHECK(more.empty());                       // too long, with credentials in the address, or with a blank
}

PB_TEST_MAIN()
