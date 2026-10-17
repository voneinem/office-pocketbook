#include <algorithm>
#include <cctype>
#include <cstring>

#include "pbo/charset.h"
#include "pbo/html_text.h"
#include "pbo/mail_headers.h"
#include "mime_internal.h"
#include "pbo/mime.h"

namespace pbo {

namespace {

char lc(char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
bool is_blank(char c) { return c == ' ' || c == '\t'; }

std::string lower(std::string s) {
    for (char& c : s) c = lc(c);
    return s;
}

std::string trim(std::string_view v) {
    size_t b = 0, e = v.size();
    while (b < e && (is_blank(v[b]) || v[b] == '\r' || v[b] == '\n')) ++b;
    while (e > b && (is_blank(v[e - 1]) || v[e - 1] == '\r' || v[e - 1] == '\n')) --e;
    return std::string(v.substr(b, e - b));
}

// ---------------------------------------------------------------------------------------------- lines and headers

// The line at `pos` (without its terminator) and the position after the terminator (CRLF, LF or a lone CR).
std::string_view next_line(std::string_view s, size_t pos, size_t* next) {
    size_t i = pos;
    while (i < s.size() && s[i] != '\n' && s[i] != '\r') ++i;
    const size_t end = i;
    if (i < s.size()) {
        if (s[i] == '\r' && i + 1 < s.size() && s[i + 1] == '\n') i += 2;
        else ++i;
    }
    *next = i;
    return s.substr(pos, end - pos);
}

struct Headers {
    std::vector<std::pair<std::string, std::string>> list;   // lower-case name, unfolded value
    size_t body_start = 0;                                    // offset of the first byte after the blank line
    std::string get(const char* name) const {
        for (const auto& h : list)
            if (h.first == name) return h.second;
        return "";
    }
};

constexpr size_t kMaxHeaders = 1000;

Headers read_headers(std::string_view s, const MimeLimits& lim) {
    Headers h;
    size_t pos = 0;
    const size_t limit = std::min(s.size(), lim.max_header_block);
    std::string name, value;
    bool have = false;
    auto flush = [&] {
        if (have && h.list.size() < kMaxHeaders) h.list.emplace_back(lower(name), trim(value));
        have = false;
        name.clear();
        value.clear();
    };
    while (pos < s.size()) {
        size_t next;
        std::string_view line = next_line(s, pos, &next);
        if (pos >= limit) {   // header block too long: what follows is not read as headers (and not as text either)
            flush();
            h.body_start = s.size();
            return h;
        }
        if (line.empty()) {   // the blank line ends the header block
            flush();
            h.body_start = next;
            return h;
        }
        if (is_blank(line[0])) {   // folded continuation
            if (have && value.size() < lim.max_line) value += line;   // unfolding removes the line break only: the blank that starts the line stays
        } else {
            flush();
            const size_t colon = line.find(':');
            if (colon != std::string_view::npos && colon > 0 && line.find_first_of(" \t") > colon) {
                name = std::string(line.substr(0, colon));
                value = std::string(line.substr(colon + 1, lim.max_line));
                have = true;
            }
            // a line that is no header: ignored
        }
        pos = next;
    }
    flush();
    h.body_start = s.size();   // no blank line: the message has headers only
    return h;
}

// ---------------------------------------------------------------------------------------------- the tree of parts

std::string cut_utf8(std::string s, size_t limit) {
    if (s.size() <= limit) return s;
    size_t cut = limit;
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;
    s.resize(cut);
    return s;
}

// Splits a multipart body at its boundary lines; each part is (offset, length) into `body`.
std::vector<std::pair<size_t, size_t>> split_parts(std::string_view body, const std::string& boundary, int max_parts) {
    std::vector<std::pair<size_t, size_t>> parts;
    const std::string delim = "--" + boundary;
    size_t pos = 0;
    long part_start = -1;
    while (pos <= body.size()) {
        size_t next;
        const std::string_view line = next_line(body, pos, &next);
        bool is_delim = line.size() >= delim.size() && line.compare(0, delim.size(), delim) == 0;
        bool closing = false;
        if (is_delim) {
            std::string_view rest = line.substr(delim.size());
            if (rest.size() >= 2 && rest.compare(0, 2, "--") == 0) { closing = true; rest = rest.substr(2); }
            while (!rest.empty() && is_blank(rest.back())) rest.remove_suffix(1);
            if (!rest.empty()) is_delim = false;   // "--BX": another boundary or text, not ours
        }
        if (is_delim) {
            if (part_start >= 0) {
                size_t end = pos;   // the line break before the delimiter belongs to the delimiter
                if (end > static_cast<size_t>(part_start) && body[end - 1] == '\n') --end;
                if (end > static_cast<size_t>(part_start) && body[end - 1] == '\r') --end;
                parts.emplace_back(static_cast<size_t>(part_start), end - static_cast<size_t>(part_start));
                if (static_cast<int>(parts.size()) >= max_parts) return parts;
            }
            part_start = closing ? -1 : static_cast<long>(next);
            if (closing) return parts;
        }
        if (next >= body.size()) break;
        pos = next;
    }
    if (part_start >= 0 && static_cast<size_t>(part_start) <= body.size())   // no closing boundary: the part runs to the end
        parts.emplace_back(static_cast<size_t>(part_start), body.size() - static_cast<size_t>(part_start));
    return parts;
}

using mime_internal::PartNode;

struct Builder {
    const MimeLimits& lim;
    int parts_seen = 0;

    PartNode empty_node(const std::string& section) {
        PartNode n;
        n.type = "multipart/mixed";   // nothing inside: contributes nothing
        n.section = section;
        return n;
    }

    PartNode build(std::string_view raw, const std::string& section, int depth) {
        if (++parts_seen > lim.max_parts) return empty_node(section);
        const Headers h = read_headers(raw, lim);
        const std::string_view body = h.body_start <= raw.size() ? raw.substr(h.body_start) : std::string_view();
        const HeaderParams ct = parse_header_params(h.get("content-type"));
        const HeaderParams cd = parse_header_params(h.get("content-disposition"));

        PartNode n;
        n.type = ct.value.find('/') == std::string::npos ? "text/plain" : ct.value;   // RFC 2045: text/plain is the default
        n.params = ct.params;
        n.disposition = cd.value;
        n.filename = cd.params.count("filename") ? cd.params.at("filename") : (ct.params.count("name") ? ct.params.at("name") : "");
        n.content_id = trim(h.get("content-id"));
        n.cte = lower(trim(h.get("content-transfer-encoding")));
        n.section = section;

        if (!n.multipart()) {
            n.body = body;
            n.has_body = true;
            return n;
        }
        if (depth >= lim.max_depth) return empty_node(section);   // deeper than allowed: not looked at
        const auto it = ct.params.find("boundary");
        if (it == ct.params.end() || it->second.empty() || it->second.size() > 200) {
            // no usable boundary: the body is read as text
            n.type = "text/plain";
            n.params.clear();
            n.cte.clear();
            n.body = body;
            n.has_body = true;
            return n;
        }
        int index = 0;
        for (const auto& span : split_parts(body, it->second, lim.max_parts)) {
            ++index;
            const std::string sub = section.empty() ? std::to_string(index) : section + "." + std::to_string(index);
            n.children.push_back(build(body.substr(span.first, span.second), sub, depth + 1));
            if (parts_seen > lim.max_parts) break;
        }
        return n;
    }
};

}  // namespace

std::string safe_file_name(std::string_view name) {
    std::string clean = sanitize_utf8(name);
    // only the last path component
    const size_t sep = clean.find_last_of("/\\");
    if (sep != std::string::npos) clean = clean.substr(sep + 1);
    std::string out;
    for (unsigned char c : clean) {
        if (c == '\t' || c == '\r' || c == '\n') out += ' ';
        else if (c < 0x20 || c == 0x7F) continue;
        else if (c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') out += '_';
        else out += static_cast<char>(c);
    }
    size_t b = 0;
    while (b < out.size() && (out[b] == '.' || out[b] == ' ')) ++b;
    size_t e = out.size();
    while (e > b && (out[e - 1] == ' ' || out[e - 1] == '.')) --e;
    out = out.substr(b, e - b);
    if (out.empty()) return "attachment";
    constexpr size_t kMax = 120;
    if (out.size() > kMax) {
        const size_t dot = out.rfind('.');
        const std::string ext = (dot != std::string::npos && out.size() - dot <= 12) ? out.substr(dot) : std::string();
        std::string stem = out.substr(0, ext.empty() ? out.size() : dot);
        stem = cut_utf8(stem, kMax - ext.size());
        out = stem + ext;
    }
    return sanitize_utf8(out);
}

ParsedMessage parse_message(std::string_view raw, const MimeLimits& limits) {
    ParsedMessage m;
    const Headers top = read_headers(raw.substr(0, std::min(raw.size(), limits.max_input)), limits);
    // headers first: they are useful even when the body is not
    m.subject = decode_header_words(top.get("subject"));
    const auto senders = parse_mailboxes(top.get("from"));
    if (!senders.empty()) {
        m.from_name = senders.front().name;
        m.from_addr = senders.front().address;
    }
    m.to = format_address_header(top.get("to"));
    m.cc = format_address_header(top.get("cc"));
    m.message_id = sanitize_utf8(trim(top.get("message-id")));
    m.in_reply_to = sanitize_utf8(trim(top.get("in-reply-to")));
    m.references = sanitize_utf8(trim(top.get("references")));
    {
        std::string date = top.get("date");
        // drop a trailing comment such as "(CEST)"
        const size_t paren = date.find('(');
        if (paren != std::string::npos) date.resize(paren);
        int64_t t = 0;
        if (parse_rfc5322_date(date, &t)) m.date_utc = t;
    }
    if (raw.size() > limits.max_input) {
        m.body_kind = ParsedMessage::Body::Unreadable;
        return m;
    }

    Builder b{limits};
    mime_internal::PartNode root = b.build(raw, "", 0);
    if (!root.multipart() && root.section.empty()) root.section = "1";   // a single-part message is section 1
    ParsedMessage body = mime_internal::assemble(root, limits);
    m.text = std::move(body.text);
    m.links = std::move(body.links);
    m.attachments = std::move(body.attachments);
    m.images = std::move(body.images);
    m.note = std::move(body.note);
    m.body_kind = body.body_kind;
    return m;
}

}  // namespace pbo
