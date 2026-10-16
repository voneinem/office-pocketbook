#include "pbo/html_text.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>

#include "html_entities.h"
#include "pbo/charset.h"

namespace pbo {

namespace {

constexpr int kMaxDepth = 100;       // quote and list nesting that is tracked
constexpr size_t kMaxLinks = 500;
constexpr size_t kMaxHref = 2000;
constexpr size_t kMaxImageSrc = 2000;
constexpr size_t kMaxImages = 40;

char lc(char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }
bool is_alpha(char c) { return std::isalpha(static_cast<unsigned char>(c)) != 0; }

void put_utf8(std::string* out, uint32_t cp) {
    if (cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
    if (cp < 0x80) {
        *out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        *out += static_cast<char>(0xC0 | (cp >> 6));
        *out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        *out += static_cast<char>(0xE0 | (cp >> 12));
        *out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        *out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        *out += static_cast<char>(0xF0 | (cp >> 18));
        *out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        *out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        *out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

bool ieq_prefix(std::string_view s, size_t at, const char* word) {
    const size_t n = std::strlen(word);
    if (at + n > s.size()) return false;
    for (size_t i = 0; i < n; ++i)
        if (lc(s[at + i]) != word[i]) return false;
    return true;
}

// A reference at s[i] == '&': decodes it, sets *consumed, returns true; false = keep the '&' as text.
bool decode_reference(std::string_view s, size_t i, std::string* out, size_t* consumed) {
    size_t j = i + 1;
    if (j < s.size() && s[j] == '#') {
        ++j;
        bool hex = false;
        if (j < s.size() && (s[j] == 'x' || s[j] == 'X')) { hex = true; ++j; }
        const size_t digits = j;
        uint64_t v = 0;
        while (j < s.size() && (hex ? std::isxdigit(static_cast<unsigned char>(s[j])) : std::isdigit(static_cast<unsigned char>(s[j])))) {
            if (v < 0x1000000) v = v * (hex ? 16 : 10) + static_cast<uint64_t>(std::isdigit(static_cast<unsigned char>(s[j])) ? s[j] - '0' : std::tolower(static_cast<unsigned char>(s[j])) - 'a' + 10);
            ++j;
        }
        if (j == digits) return false;
        if (j < s.size() && s[j] == ';') ++j;
        put_utf8(out, static_cast<uint32_t>(v > 0x10FFFF ? 0 : v));
        *consumed = j - i;
        return true;
    }
    size_t k = j;
    while (k < s.size() && k - j < 10 && std::isalnum(static_cast<unsigned char>(s[k]))) ++k;
    if (k == j) return false;
    const std::string name(s.substr(j, k - j));
    const bool semi = k < s.size() && s[k] == ';';
    const auto* begin = html_entities::kEntities;
    const auto* end = begin + (sizeof html_entities::kEntities / sizeof html_entities::kEntities[0]);
    const auto* it = std::lower_bound(begin, end, name, [](const html_entities::Entity& e, const std::string& n) { return std::strcmp(e.name, n.c_str()) < 0; });
    if (it == end || name != it->name) return false;
    // without the semicolon only the four basic ones are understood, as browsers do for old mail
    if (!semi && name != "amp" && name != "lt" && name != "gt" && name != "quot") return false;
    put_utf8(out, it->code);
    *consumed = k - i + (semi ? 1 : 0);
    return true;
}

bool safe_href(std::string& h) {
    size_t b = 0, e = h.size();
    while (b < e && is_space(h[b])) ++b;
    while (e > b && is_space(h[e - 1])) --e;
    h = h.substr(b, e - b);
    if (h.empty() || h.size() > kMaxHref) return false;
    for (unsigned char c : h)
        if (c < 0x20 || c == 0x7F) return false;
    return ieq_prefix(h, 0, "http://") || ieq_prefix(h, 0, "https://") || ieq_prefix(h, 0, "mailto:");
}

struct Tag {
    std::string name;       // lower case
    bool closing = false;
    bool self_closing = false;
    std::string raw_attrs;  // everything after the name, up to '>'
};

// attribute value by name (decoded references), "" if absent
std::string attribute(const std::string& attrs, const char* name) {
    const size_t n = std::strlen(name);
    size_t i = 0;
    while (i < attrs.size()) {
        while (i < attrs.size() && (is_space(attrs[i]) || attrs[i] == '/')) ++i;
        const size_t ns = i;
        while (i < attrs.size() && !is_space(attrs[i]) && attrs[i] != '=' && attrs[i] != '>' && attrs[i] != '/') ++i;
        const std::string an = attrs.substr(ns, i - ns);
        while (i < attrs.size() && is_space(attrs[i])) ++i;
        std::string value;
        if (i < attrs.size() && attrs[i] == '=') {
            ++i;
            while (i < attrs.size() && is_space(attrs[i])) ++i;
            if (i < attrs.size() && (attrs[i] == '"' || attrs[i] == '\'')) {
                const char q = attrs[i++];
                while (i < attrs.size() && attrs[i] != q) value += attrs[i++];
                if (i < attrs.size()) ++i;
            } else {
                while (i < attrs.size() && !is_space(attrs[i])) value += attrs[i++];
            }
        } else if (i == ns) {
            ++i;   // no progress otherwise
        }
        if (an.size() == n) {
            bool same = true;
            for (size_t k = 0; k < n; ++k)
                if (lc(an[k]) != name[k]) same = false;
            if (same) {
                std::string decoded;
                for (size_t k = 0; k < value.size();) {
                    size_t used = 0;
                    if (value[k] == '&' && decode_reference(value, k, &decoded, &used)) k += used;
                    else decoded += value[k++];
                }
                return decoded;
            }
        }
    }
    return "";
}

bool in_list(const char* const* list, size_t n, const std::string& name) {
    for (size_t i = 0; i < n; ++i)
        if (name == list[i]) return true;
    return false;
}

class Converter {
public:
    Converter(const MimeLimits& limits, std::vector<std::string>* links, std::vector<HtmlImage>* images = nullptr) : lim_(limits), links_(links), images_(images) {}

    std::string run(std::string_view s) {
        if (s.size() > lim_.max_input) s = s.substr(0, lim_.max_input);
        size_t i = 0;
        std::string chunk;
        while (i < s.size() && !full()) {
            const char c = s[i];
            if (c == '<' && i + 1 < s.size() && (is_alpha(s[i + 1]) || s[i + 1] == '/' || s[i + 1] == '!' || s[i + 1] == '?')) {
                text(chunk);
                chunk.clear();
                i = markup(s, i);
                continue;
            }
            if (c == '&') {
                size_t used = 0;
                if (decode_reference(s, i, &chunk, &used)) { i += used; continue; }
            }
            chunk += c;
            ++i;
        }
        text(chunk);
        finish_line();
        // trailing blank lines and blanks go
        for (;;) {   // trailing blank lines (also quote-only ones like ">") and blanks go
            while (!out_.empty() && (out_.back() == '\n' || out_.back() == ' ')) out_.pop_back();
            const size_t nl = out_.rfind('\n');
            const size_t start = nl == std::string::npos ? 0 : nl + 1;
            if (start < out_.size() && out_.find_first_not_of('>', start) == std::string::npos) out_.resize(nl == std::string::npos ? 0 : nl);
            else break;
        }
        if (out_.size() > lim_.max_text_part) out_.resize(lim_.max_text_part);
        return sanitize_utf8(out_);
    }

private:
    bool full() const { return out_.size() >= lim_.max_text_part; }

    std::string quote_prefix() const { return std::string(static_cast<size_t>(std::min(bq_, kMaxDepth)), '>'); }

    // Appends one finished line to the output.
    void emit(const std::string& content) {
        if (!out_.empty()) out_ += '\n';
        const std::string q = quote_prefix();
        if (content.empty()) { out_ += q; last_blank_ = true; return; }
        out_ += q;
        if (!q.empty()) out_ += ' ';
        out_ += content;
        last_blank_ = false;
    }

    void finish_line() {
        if (cur_.empty()) {
            marker_.clear();
            return;
        }
        std::string line;
        if (list_depth_ > 0) line.assign(static_cast<size_t>(2 * (std::min(list_depth_, kMaxDepth) - 1)), ' ');
        if (!marker_.empty()) { line += marker_; marker_.clear(); }
        else if (list_depth_ > 0 && in_item_) line += "  ";
        line += cur_;
        cur_.clear();
        pending_space_ = false;
        emit(line);
    }

    void gap() {
        finish_line();
        if (!out_.empty() && !last_blank_) emit("");
    }

    void force_line() {   // <br>
        if (cur_.empty() && marker_.empty() && out_.empty()) return;
        if (cur_.empty()) { emit(""); return; }
        finish_line();
    }

    void text(const std::string& s) {
        for (size_t i = 0; i < s.size() && !full(); ++i) {
            const char c = s[i];
            if (c == '\xEF' && i + 2 < s.size() + 0 && s[i + 1] == '\xBF' && s[i + 2] == '\xBC') {   // U+FFFC is the picture marker: never from the mail's own text
                i += 2;
                continue;
            }
            if (in_pre_) {
                if (c == '\n') { if (cur_.empty()) emit(""); else finish_line(); }
                else if (c != '\r') cur_ += c;
                continue;
            }
            if (is_space(c)) { pending_space_ = true; continue; }
            if (pending_space_ && !cur_.empty()) cur_ += ' ';
            pending_space_ = false;
            cur_ += c;
        }
    }

    // Skips to just after `needle` (case-insensitive); the end of the input if it does not occur.
    static size_t skip_to(std::string_view s, size_t from, const char* needle) {
        const size_t n = std::strlen(needle);
        for (size_t i = from; i + n <= s.size(); ++i)
            if (ieq_prefix(s, i, needle)) return i + n;
        return s.size();
    }

    // Reads the markup at s[i] == '<'; returns the position after it.
    size_t markup(std::string_view s, size_t i) {
        if (s.compare(i, 4, "<!--") == 0) return skip_to(s, i + 4, "-->");
        if (s.compare(i, 9, "<![CDATA[") == 0) return skip_to(s, i + 9, "]]>");
        if (s[i + 1] == '!' || s[i + 1] == '?') return skip_to(s, i + 2, ">");

        Tag tag;
        size_t j = i + 1;
        if (s[j] == '/') { tag.closing = true; ++j; }
        while (j < s.size() && (std::isalnum(static_cast<unsigned char>(s[j])) || s[j] == '-' || s[j] == ':') && tag.name.size() < 32) tag.name += lc(s[j++]);
        // attributes up to the closing '>', honoring quoted values
        char quote = 0;
        char prev = 0;
        const size_t attrs_start = j;
        while (j < s.size()) {
            const char c = s[j];
            if (quote) { if (c == quote) quote = 0; }
            else if ((c == '"' || c == '\'') && prev == '=') quote = c;
            else if (c == '>') break;
            if (!is_space(c)) prev = c;
            ++j;
        }
        if (j >= s.size()) return s.size();   // the tag never ends: the rest is part of it
        tag.raw_attrs = std::string(s.substr(attrs_start, j - attrs_start));
        tag.self_closing = !tag.raw_attrs.empty() && tag.raw_attrs.back() == '/';
        const size_t after = j + 1;
        handle(tag);
        if (!tag.closing && !tag.self_closing) {
            static const char* const kSkipped[] = {"script", "style", "svg", "template", "iframe", "object", "embed", "applet", "noscript", "canvas", "audio", "video", "math", "title", "textarea"};
            if (in_list(kSkipped, sizeof kSkipped / sizeof kSkipped[0], tag.name)) return skip_to(s, after, ("</" + tag.name).c_str()) < s.size() ? skip_to(s, skip_to(s, after, ("</" + tag.name).c_str()), ">") : s.size();
            if (tag.name == "head") {   // until </head>, or until <body when the head is not closed
                size_t e = skip_to(s, after, "</head");
                size_t b = after;
                for (; b + 5 <= s.size(); ++b)
                    if (ieq_prefix(s, b, "<body")) break;
                if (b + 5 > s.size()) b = s.size();
                if (e < s.size() && e <= b) return skip_to(s, e, ">");
                return b;
            }
        }
        return after;
    }

    void handle(const Tag& t) {
        static const char* const kGap[] = {"p", "h1", "h2", "h3", "h4", "h5", "h6", "pre", "address", "table", "fieldset"};
        static const char* const kLine[] = {"div", "section", "article", "header", "footer", "form", "center", "figure", "main", "nav", "aside", "details", "summary", "dl", "dt", "dd", "caption", "tbody", "thead", "tfoot"};
        const std::string& n = t.name;
        if (n == "br") { force_line(); return; }
        if (n == "hr") { gap(); cur_ = "-----"; finish_line(); gap(); return; }
        if (n == "img") {
            if (images_ && picture(t)) return;
            const std::string alt = attribute(t.raw_attrs, "alt");
            std::string trimmed;
            for (char c : alt) trimmed += is_space(c) ? ' ' : c;
            if (trimmed.find_first_not_of(' ') != std::string::npos) text("[" + trimmed + "]");
            return;
        }
        if (n == "a") { anchor(t); return; }
        if (n == "blockquote") {
            if (!t.closing) {   // the blank line before a quote belongs to the outer level, the one after it too
                gap();
                if (bq_ < kMaxDepth) ++bq_; else ++bq_over_;
            } else {
                finish_line();
                if (bq_over_ > 0) --bq_over_;
                else if (bq_ > 0) --bq_;
                gap();
            }
            return;
        }
        if (n == "ul" || n == "ol") {
            if (!t.closing) {
                if (list_depth_ == 0) gap(); else finish_line();
                if (static_cast<int>(lists_.size()) < kMaxDepth) lists_.push_back({n == "ol", 0});
                ++list_depth_;
            } else if (list_depth_ > 0) {
                finish_line();
                if (!lists_.empty()) lists_.pop_back();
                --list_depth_;
                in_item_ = false;
                if (list_depth_ == 0) gap();
            }
            return;
        }
        if (n == "li") {
            finish_line();
            if (!t.closing && list_depth_ > 0) {
                in_item_ = true;
                if (!lists_.empty() && lists_.back().ordered) marker_ = std::to_string(++lists_.back().counter) + ". ";
                else marker_ = "- ";
                // a marker on an empty item is dropped by finish_line
            }
            return;
        }
        if (n == "tr") { finish_line(); if (!t.closing) cells_ = 0; return; }
        if (n == "td" || n == "th") {
            if (!t.closing) {
                if (cells_++ > 0) { cur_ += " | "; pending_space_ = false; }
            }
            return;
        }
        if (n == "pre") {
            gap();
            in_pre_ = !t.closing;
            return;
        }
        if (in_list(kGap, sizeof kGap / sizeof kGap[0], n)) { gap(); return; }
        if (in_list(kLine, sizeof kLine / sizeof kLine[0], n)) { finish_line(); return; }
    }

    static int pixels(const std::string& v) {   // "200", "200px", "50%": a number of pixels or 0 (not given or not a pixel size)
        size_t i = 0;
        while (i < v.size() && is_space(v[i])) ++i;
        long n = 0;
        size_t digits = 0;
        while (i < v.size() && v[i] >= '0' && v[i] <= '9' && digits < 6) { n = n * 10 + (v[i] - '0'); ++i; ++digits; }
        if (digits == 0) return -1;                      // not given
        if (i < v.size() && v[i] == '%') return -1;      // a share of the width, not a size
        return static_cast<int>(n);
    }

    // A picture that the reader may show: puts its marker line into the text and returns true; false leaves the alt text to the caller.
    bool picture(const Tag& t) {
        std::string src = attribute(t.raw_attrs, "src");
        size_t b = 0, e = src.size();
        while (b < e && is_space(src[b])) ++b;
        while (e > b && is_space(src[e - 1])) --e;
        src = src.substr(b, e - b);
        const bool cid = ieq_prefix(src, 0, "cid:") && src.size() > 4;
        const bool https = ieq_prefix(src, 0, "https://") && src.size() > 8;
        if (!cid && !https) return false;
        if (src.size() > kMaxImageSrc) return true;     // hostile: neither listed nor shown
        for (unsigned char c : src)
            if (c <= 0x20 || c == 0x7F) return true;
        if (https) {
            const size_t host_end = src.find_first_of("/?#", 8);
            const std::string authority = src.substr(8, host_end == std::string::npos ? std::string::npos : host_end - 8);
            if (authority.find('@') != std::string::npos) return true;   // credentials in an address are never used
        }
        const int w = pixels(attribute(t.raw_attrs, "width")), h = pixels(attribute(t.raw_attrs, "height"));
        if ((w >= 0 && w <= 3) || (h >= 0 && h <= 3)) return true;       // a tracking pixel or a spacer: not shown, not even by its alt text
        size_t index = 0;
        for (; index < images_->size(); ++index)
            if ((*images_)[index].src == src) break;
        if (index == images_->size()) {
            if (images_->size() >= kMaxImages) return true;
            HtmlImage im;
            im.src = cid ? "cid:" + src.substr(4) : src;
            std::string alt = attribute(t.raw_attrs, "alt");
            for (char& c : alt)
                if (is_space(c)) c = ' ';
            if (alt.size() > 200) alt.resize(200);
            im.alt = alt;
            im.width = w > 0 ? w : 0;
            im.height = h > 0 ? h : 0;
            images_->push_back(std::move(im));
        }
        finish_line();
        cur_ = "\xEF\xBF\xBC" + std::to_string(index + 1) + "\xEF\xBF\xBC";
        const std::string keep_marker = marker_;
        marker_.clear();
        finish_line();
        marker_ = keep_marker;
        return true;
    }

    void anchor(const Tag& t) {
        if (!t.closing) {
            link_ = 0;
            std::string href = attribute(t.raw_attrs, "href");
            if (!links_ || !safe_href(href)) return;
            for (size_t k = 0; k < links_->size(); ++k)
                if ((*links_)[k] == href) { link_ = static_cast<int>(k) + 1; return; }
            if (links_->size() >= kMaxLinks) return;
            links_->push_back(href);
            link_ = static_cast<int>(links_->size());
            return;
        }
        if (link_ > 0) {
            const std::string marker = "[" + std::to_string(link_) + "]";
            if (cur_.empty()) cur_ = marker;
            else cur_ += " " + marker;
            pending_space_ = false;
            link_ = 0;
        }
    }

    struct ListState {
        bool ordered;
        int counter;
    };

    const MimeLimits& lim_;
    std::vector<std::string>* links_;
    std::vector<HtmlImage>* images_;
    std::string out_, cur_, marker_;
    bool pending_space_ = false, last_blank_ = false, in_pre_ = false, in_item_ = false;
    int bq_ = 0, bq_over_ = 0, list_depth_ = 0, cells_ = 0, link_ = 0;
    std::vector<ListState> lists_;
};

}  // namespace

std::string html_to_text_continue(std::string_view html, std::vector<std::string>* links, const MimeLimits& limits) {
    Converter c(limits, links);
    return c.run(html);
}

std::string html_to_text_continue(std::string_view html, std::vector<std::string>* links, std::vector<HtmlImage>* images, const MimeLimits& limits) {
    Converter c(limits, links, images);
    return c.run(html);
}

std::string html_to_text(std::string_view html, std::vector<std::string>* links, const MimeLimits& limits) {
    if (links) links->clear();
    return html_to_text_continue(html, links, limits);
}

}  // namespace pbo
