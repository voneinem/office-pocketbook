#pragma once
// HTML to readable text (research R-07). Nothing is fetched, nothing is executed: images, scripts, styles and frames are
// ignored, links are listed as text. Total on any input.

#include <string>
#include <string_view>
#include <vector>

#include "pbo/mime.h"

namespace pbo {

// Block elements become lines, lists get "- " or "1. ", table rows become "a | b", blockquotes get "> " markers, <hr> a
// rule, <br> a line break; <head>, <script>, <style>, <svg>, <template> and comments are skipped; a link "text" with an
// address becomes "text [n]" and links->at(n-1) is the address (equal addresses share a number; only http, https and
// mailto addresses are listed). Character references are decoded (numeric and the HTML 4 named ones). Output is capped by
// `limits.max_text_part`, input by `limits.max_input`, nesting at 100.
std::string html_to_text(std::string_view html, std::vector<std::string>* links, const MimeLimits& limits = {});

// The same, but `links` is NOT cleared: addresses already in it keep their numbers and new ones continue the numbering
// (several html parts of one message share one list).
std::string html_to_text_continue(std::string_view html, std::vector<std::string>* links, const MimeLimits& limits = {});

// A picture of the HTML that the reader may show: `src` is "cid:<content-id>" (a part of the message) or an https address. Nothing is fetched here.
struct HtmlImage {
    std::string src;
    std::string alt;
    int width = 0, height = 0;   // the attributes of the tag, 0 = not given
};

// With `images` the converter marks where a picture belongs: a line of its own holding U+FFFC, the number of the picture (1-based, an index into
// *images, which is NOT cleared) and U+FFFC again. Only cid: and https: sources are listed (not http, data, file or relative ones: their alt text stays);
// tracking pixels (a side of at most 3 pixels) are dropped; the same address shares one entry; at most 40 pictures. U+FFFC in the mail's own text is
// removed so that no text can point at a picture.
std::string html_to_text_continue(std::string_view html, std::vector<std::string>* links, std::vector<HtmlImage>* images, const MimeLimits& limits = {});

}  // namespace pbo
