#include <algorithm>
#include <cctype>

#include "mime_internal.h"
#include "pbo/charset.h"
#include "pbo/html_text.h"

namespace pbo {
namespace mime_internal {

namespace {

char lc(char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }

int b64v(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

int hexv(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// "\n" line ends, no NUL or other control characters, lines cut at max_line.
std::string normalize_text(std::string_view s, size_t max_line, size_t* removed_controls) {
    std::string out;
    out.reserve(s.size());
    size_t line_len = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == '\r') {
            if (i + 1 < s.size() && s[i + 1] == '\n') ++i;
            out += '\n';
            line_len = 0;
        } else if (c == '\n') {
            out += '\n';
            line_len = 0;
        } else if (c == 0xEF && i + 2 < s.size() && static_cast<unsigned char>(s[i + 1]) == 0xBF && static_cast<unsigned char>(s[i + 2]) == 0xBC) {
            i += 2;   // U+FFFC marks a picture in the reader: never from the mail's own text
        } else if (c == '\t' || (c >= 0x20 && c != 0x7F)) {
            if (++line_len <= max_line) out += static_cast<char>(c);
        } else {
            ++*removed_controls;
        }
    }
    return out;
}

// Leading blank lines and trailing blank space go; the indentation of the first text line stays.
std::string trim_text(std::string s) {
    size_t first = 0;
    for (size_t i = 0; i < s.size() && (s[i] == '\n' || s[i] == ' ' || s[i] == '\t'); ++i)
        if (s[i] == '\n') first = i + 1;
    s.erase(0, first);
    size_t e = s.size();
    while (e > 0 && (s[e - 1] == '\n' || s[e - 1] == ' ' || s[e - 1] == '\t')) --e;
    s.resize(e);
    return s;
}

std::string cut_utf8(std::string s, size_t limit) {
    if (s.size() <= limit) return s;
    size_t cut = limit;
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;
    s.resize(cut);
    return s;
}

// A Content-ID or the target of a "cid:" address without the angle brackets and case-folded, so that the two can be compared.
std::string lower_id(std::string_view v) {
    while (!v.empty() && (v.front() == '<' || v.front() == ' ')) v.remove_prefix(1);
    while (!v.empty() && (v.back() == '>' || v.back() == ' ')) v.remove_suffix(1);
    std::string out(v);
    for (char& c : out)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return out;
}

struct Sel {
    std::vector<const PartNode*> plain, html, attachments;
    std::vector<const PartNode*> inline_images;   // pictures of a multipart/related that the HTML may name by Content-ID
    bool saw_text = false;
    bool saw_attachment = false;
};

bool is_signature(const std::string& type) {
    return type == "application/pgp-signature" || type == "application/pkcs7-signature" || type == "application/x-pkcs7-signature";
}

// Does this leaf count as an attachment? Text parts do not, unless they carry a file name or an attachment disposition.
bool leaf_is_attachment(const PartNode& n, const std::string& parent_type) {
    const bool text = n.type == "text/plain" || n.type == "text/html";
    if (n.disposition == "attachment" || !n.filename.empty()) return true;
    if (text) return false;
    // an inline picture of an html body (multipart/related) is not listed
    if (parent_type == "multipart/related") return false;
    return true;
}

Sel select(const PartNode& n, const std::string& parent_type) {
    Sel s;
    if (!n.multipart()) {
        if (is_signature(n.type) && n.disposition != "attachment") return s;
        if (parent_type == "multipart/related" && n.disposition != "attachment" && n.type.compare(0, 6, "image/") == 0 && !n.content_id.empty()) {
            s.inline_images.push_back(&n);   // listed as an attachment later only if the HTML does not use it (and it has a name)
            return s;
        }
        if (n.type == "message/rfc822" || n.type == "message/global" || leaf_is_attachment(n, parent_type)) {
            s.attachments.push_back(&n);
            s.saw_attachment = true;
            return s;
        }
        s.saw_text = true;
        (n.type == "text/html" ? s.html : s.plain).push_back(&n);
        return s;
    }
    std::vector<Sel> kids;
    for (const PartNode& c : n.children) kids.push_back(select(c, n.type));
    if (n.type == "multipart/alternative") {
        const Sel* plain = nullptr;
        const Sel* html = nullptr;
        for (const Sel& k : kids) {
            s.saw_text = s.saw_text || k.saw_text;
            if (!plain && !k.plain.empty()) plain = &k;
            if (!html && !k.html.empty()) html = &k;
            s.attachments.insert(s.attachments.end(), k.attachments.begin(), k.attachments.end());
            s.inline_images.insert(s.inline_images.end(), k.inline_images.begin(), k.inline_images.end());
            s.saw_attachment = s.saw_attachment || k.saw_attachment;
        }
        if (plain) s.plain = plain->plain;
        if (html) s.html = html->html;
        return s;
    }
    // mixed, related, signed and anything else: text before the first attachment is the body
    const bool related = n.type == "multipart/related";
    bool attachment_before = false;
    for (size_t i = 0; i < kids.size(); ++i) {
        const Sel& k = kids[i];
        s.saw_text = s.saw_text || k.saw_text;
        if (!attachment_before && !(related && i > 0)) {
            s.plain.insert(s.plain.end(), k.plain.begin(), k.plain.end());
            s.html.insert(s.html.end(), k.html.begin(), k.html.end());
        }
        s.attachments.insert(s.attachments.end(), k.attachments.begin(), k.attachments.end());
        s.inline_images.insert(s.inline_images.end(), k.inline_images.begin(), k.inline_images.end());
        if (k.saw_attachment) {
            s.saw_attachment = true;
            attachment_before = true;
        }
    }
    return s;
}

}  // namespace

std::string decode_base64(std::string_view s, size_t limit) {
    std::string out;
    uint32_t acc = 0;
    int bits = 0;
    for (char c : s) {
        const int v = b64v(c);
        if (v < 0) continue;
        acc = (acc << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += static_cast<char>((acc >> bits) & 0xFF);
            if (out.size() >= limit) break;
        }
    }
    return out;
}

std::string decode_qp(std::string_view s, size_t limit) {
    std::string out;
    for (size_t i = 0; i < s.size() && out.size() < limit; ++i) {
        if (s[i] != '=') { out += s[i]; continue; }
        if (i + 1 < s.size() && (s[i + 1] == '\n' || s[i + 1] == '\r')) {   // soft line break
            ++i;
            if (s[i] == '\r' && i + 1 < s.size() && s[i + 1] == '\n') ++i;
            continue;
        }
        const int h = i + 1 < s.size() ? hexv(s[i + 1]) : -1;
        const int l = i + 2 < s.size() ? hexv(s[i + 2]) : -1;
        if (h >= 0 && l >= 0) { out += static_cast<char>(h * 16 + l); i += 2; }
        else out += '=';   // not an escape: kept as written
    }
    return out;
}

std::string decode_transfer(std::string_view body, const std::string& cte, size_t limit) {
    if (cte == "base64") return decode_base64(body, limit);
    if (cte == "quoted-printable") return decode_qp(body, limit);
    return std::string(body.substr(0, limit));
}

uint64_t decoded_size_of(std::string_view body, const std::string& cte) {
    if (cte == "base64") {
        uint64_t chars = 0;
        for (char c : body)
            if (b64v(c) >= 0) ++chars;
        return chars * 3 / 4;
    }
    if (cte == "quoted-printable") {
        uint64_t n = 0;
        for (size_t i = 0; i < body.size(); ++i) {
            if (body[i] == '=' && i + 2 < body.size() && hexv(body[i + 1]) >= 0 && hexv(body[i + 2]) >= 0) i += 2;
            ++n;
        }
        return n;
    }
    return body.size();
}

uint64_t decoded_size_from_transfer(uint64_t transfer_size, const std::string& cte) {
    return cte == "base64" ? transfer_size * 3 / 4 : transfer_size;
}

std::vector<const PartNode*> choose_text_parts(const PartNode& root, const MimeLimits&) {
    const Sel s = select(root, "");
    return !s.plain.empty() ? s.plain : s.html;
}

std::vector<const PartNode*> choose_html_for_pictures(const PartNode& root) {
    const Sel s = select(root, "");
    if (s.plain.empty() || s.html.empty()) return {};
    uint64_t total = 0;
    for (const PartNode* p : s.html) total += p->has_body ? decoded_size_of(p->body, p->cte) : p->decoded_size;
    if (total > (512u << 10)) return {};
    if (s.inline_images.empty() && total < 8000) return {};
    return s.html;
}

ParsedMessage assemble(const PartNode& root, const MimeLimits& lim) {
    ParsedMessage m;
    const Sel s = select(root, "");

    for (const PartNode* a : s.attachments) {
        AttachmentInfo info;
        info.section = a->section;
        info.name = a->filename.empty() ? std::string() : safe_file_name(a->filename);
        info.mime_type = a->type.find('/') == std::string::npos ? "application/octet-stream" : a->type;
        info.encoding = a->cte.empty() ? "7bit" : a->cte;
        info.size = a->has_body ? decoded_size_of(a->body, a->cte) : a->decoded_size;
        m.attachments.push_back(std::move(info));
        if (m.attachments.size() >= static_cast<size_t>(lim.max_parts)) break;
    }

    bool truncated = false, charset_unsupported = false;
    size_t removed_controls = 0, text_chars = 0;
    auto decode = [&](const PartNode& p, bool* any) {
        if (!p.has_body) return std::string();
        *any = true;
        const bool over = p.body.size() > lim.max_text_part * 2 + 1024;
        const std::string bytes = decode_transfer(p.body, p.cte, lim.max_text_part * 2 + 1024);
        if (over) truncated = true;
        bool supported = true;
        const auto it = p.params.find("charset");
        std::string text = to_utf8(bytes, it == p.params.end() ? "" : it->second, &supported);
        if (!supported) charset_unsupported = true;
        return text;
    };

    // The HTML body is converted first (when its bytes are here): a message whose HTML has pictures is shown from the HTML, with its pictures; otherwise
    // the plain-text alternative stays the body, as before. An HTML-only message is shown from the HTML in any case.
    bool got_plain = false, got_html = false;
    std::string html_joined;
    std::vector<std::string> html_links;
    std::vector<HtmlImage> html_images;
    if (!s.html.empty()) {
        for (const PartNode* p : s.html) {
            bool any = false;
            const std::string html = decode(*p, &any);
            if (!any) continue;
            got_html = true;
            const std::string text = html_to_text_continue(html, &html_links, &html_images, lim);
            if (!html_joined.empty() && !text.empty()) html_joined += "\n\n";
            html_joined += text;
        }
    }
    const bool show_html = got_html && (s.plain.empty() || !html_images.empty());
    std::vector<const PartNode*> used_inline;   // the inline pictures the shown HTML names
    if (!show_html && !s.plain.empty()) {
        std::string joined;
        for (const PartNode* p : s.plain) {
            bool any = false;
            std::string text = decode(*p, &any);
            if (!any) continue;
            got_plain = true;
            text = trim_text(normalize_text(text, lim.max_line, &removed_controls));
            text_chars += text.size();
            if (text.size() > lim.max_text_part) {
                text = cut_utf8(std::move(text), lim.max_text_part);
                truncated = true;
            }
            if (!joined.empty() && !text.empty()) joined += "\n\n";
            joined += text;
        }
        m.text = std::move(joined);
    } else if (show_html) {
        if (html_joined.size() >= lim.max_text_part) truncated = true;
        m.text = std::move(html_joined);
        m.links = std::move(html_links);
        m.note = "html_converted";
        for (const HtmlImage& hi : html_images) {
            MessageImage mi;
            mi.src = hi.src;
            mi.alt = hi.alt;
            mi.width = hi.width;
            mi.height = hi.height;
            if (hi.src.compare(0, 4, "cid:") == 0) {
                mi.kind = "cid";
                const std::string want = lower_id(hi.src.substr(4));
                for (const PartNode* ip : s.inline_images)
                    if (lower_id(ip->content_id) == want) {
                        mi.section = ip->section;
                        mi.mime_type = ip->type;
                        mi.encoding = ip->cte.empty() ? "7bit" : ip->cte;
                        mi.size = ip->has_body ? decoded_size_of(ip->body, ip->cte) : ip->decoded_size;
                        used_inline.push_back(ip);
                        break;
                    }
            } else {
                mi.kind = "remote";
            }
            m.images.push_back(std::move(mi));
        }
    }
    // inline pictures that the shown text does not name are not hidden: with a file name they are attachments
    for (const PartNode* ip : s.inline_images) {
        if (std::find(used_inline.begin(), used_inline.end(), ip) != used_inline.end() || ip->filename.empty()) continue;
        AttachmentInfo info;
        info.section = ip->section;
        info.name = safe_file_name(ip->filename);
        info.mime_type = ip->type;
        info.encoding = ip->cte.empty() ? "7bit" : ip->cte;
        info.size = ip->has_body ? decoded_size_of(ip->body, ip->cte) : ip->decoded_size;
        if (m.attachments.size() < static_cast<size_t>(lim.max_parts)) m.attachments.push_back(std::move(info));
    }
    if (m.text.size() > lim.max_text_part) {
        m.text = cut_utf8(std::move(m.text), lim.max_text_part);
        truncated = true;
    }

    if (got_plain || got_html) m.body_kind = ParsedMessage::Body::Text;
    else m.body_kind = ParsedMessage::Body::NoReadableText;
    if (s.plain.empty() && s.html.empty() && s.saw_text) m.body_kind = ParsedMessage::Body::NoReadableText;
    // text that is mostly binary is not readable
    if (got_plain && removed_controls > 64 && removed_controls * 3 > text_chars + removed_controls) {
        m.body_kind = ParsedMessage::Body::Unreadable;
        m.text.clear();
        m.links.clear();
        m.images.clear();
        return m;
    }
    if (truncated) m.note = "truncated";
    else if (charset_unsupported) m.note = "charset_unsupported";
    return m;
}

}  // namespace mime_internal
}  // namespace pbo
