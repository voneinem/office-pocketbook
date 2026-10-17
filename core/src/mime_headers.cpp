#include <algorithm>
#include <cctype>
#include <cstdint>

#include "pbo/account_input.h"
#include "pbo/charset.h"
#include "pbo/mime.h"

namespace pbo {

namespace {

constexpr size_t kMaxHeaderInput = 1u << 20;   // a header value longer than this is cut
constexpr size_t kMaxWordText = 8192;          // encoded text longer than this is not treated as an encoded word
constexpr size_t kMaxParams = 64;
constexpr size_t kMaxParamTokens = 1000;
constexpr size_t kMaxSegments = 100;
constexpr size_t kMaxMailboxes = 200;
constexpr size_t kMaxRecipients = 100;

bool is_ws(char c) { return c == ' ' || c == '\t'; }

std::string trim(std::string_view v) {
    size_t b = 0, e = v.size();
    while (b < e && (is_ws(v[b]) || v[b] == '\r' || v[b] == '\n')) ++b;
    while (e > b && (is_ws(v[e - 1]) || v[e - 1] == '\r' || v[e - 1] == '\n')) --e;
    return std::string(v.substr(b, e - b));
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Unfolds (CRLF or LF followed by blanks become one blank) and turns control characters into blanks.
std::string clean_text(std::string_view v) {
    if (v.size() > kMaxHeaderInput) v = v.substr(0, kMaxHeaderInput);
    std::string out;
    out.reserve(v.size());
    for (size_t i = 0; i < v.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(v[i]);
        if (c == '\r' || c == '\n') {
            if (c == '\r' && i + 1 < v.size() && v[i + 1] == '\n') ++i;
            if (i + 1 < v.size() && is_ws(v[i + 1])) continue;   // folded: the blank that follows is kept
            out += ' ';
        } else if (c < 0x20 || c == 0x7F) {
            out += ' ';
        } else {
            out += static_cast<char>(c);
        }
    }
    return out;
}

int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int b64_value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

bool decode_b(std::string_view t, std::string* out) {
    uint32_t acc = 0;
    int bits = 0;
    size_t pad = 0;
    for (char c : t) {
        if (c == '=') { ++pad; continue; }
        if (pad > 0) return false;   // data after padding
        const int v = b64_value(c);
        if (v < 0) return false;
        acc = (acc << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            *out += static_cast<char>((acc >> bits) & 0xFF);
        }
    }
    return pad <= 2;
}

bool decode_q(std::string_view t, std::string* out) {
    for (size_t i = 0; i < t.size(); ++i) {
        if (t[i] == '_') {
            *out += ' ';
        } else if (t[i] == '=') {
            const int h = i + 1 < t.size() ? hex_value(t[i + 1]) : -1;
            const int l = i + 2 < t.size() ? hex_value(t[i + 2]) : -1;
            if (h < 0 || l < 0) return false;
            *out += static_cast<char>(h * 16 + l);
            i += 2;
        } else {
            *out += t[i];
        }
    }
    return true;
}

struct Segment {
    bool encoded = false;
    std::string text;      // plain text, or the decoded bytes of an encoded word
    std::string charset;   // lower case, without the language tag
};

// Tries to read an encoded word at s[pos..]; on success fills `seg` and sets *end after the closing "?=".
bool read_word(const std::string& s, size_t pos, Segment* seg, size_t* end) {
    size_t i = pos + 2;
    const size_t cs = i;
    while (i < s.size() && s[i] != '?' && !is_ws(s[i]) && i - cs <= 64) ++i;
    if (i >= s.size() || s[i] != '?' || i == cs) return false;
    std::string charset = s.substr(cs, i - cs);
    ++i;
    if (i + 1 >= s.size() || s[i + 1] != '?') return false;
    const char enc = static_cast<char>(std::tolower(static_cast<unsigned char>(s[i])));
    if (enc != 'b' && enc != 'q') return false;
    i += 2;
    const size_t ts = i;
    while (i < s.size() && s[i] != '?' && !is_ws(s[i])) {
        if (++i - ts > kMaxWordText) return false;
    }
    if (i + 1 >= s.size() || s[i] != '?' || s[i + 1] != '=') return false;
    std::string bytes;
    const std::string_view text(s.data() + ts, i - ts);
    if (enc == 'b' ? !decode_b(text, &bytes) : !decode_q(text, &bytes)) return false;
    const size_t star = charset.find('*');   // RFC 2231 language tag: =?charset*lang?...
    if (star != std::string::npos) charset.resize(star);
    seg->encoded = true;
    seg->text = std::move(bytes);
    seg->charset = lower(charset);
    *end = i + 2;
    return true;
}

}  // namespace

std::string decode_header_words(std::string_view value) {
    const std::string s = clean_text(value);
    std::vector<Segment> segs;
    std::string plain;
    auto flush_plain = [&] {
        if (!plain.empty()) {
            Segment p;
            p.text = plain;
            segs.push_back(std::move(p));
            plain.clear();
        }
    };
    size_t i = 0;
    while (i < s.size()) {
        if (s[i] == '=' && i + 1 < s.size() && s[i + 1] == '?') {
            Segment w;
            size_t end = 0;
            if (read_word(s, i, &w, &end)) {
                flush_plain();
                segs.push_back(std::move(w));
                i = end;
                continue;
            }
        }
        plain += s[i++];
    }
    flush_plain();

    // blanks between two encoded words are not part of the text
    std::vector<Segment> kept;
    for (size_t k = 0; k < segs.size(); ++k) {
        const bool blank = !segs[k].encoded && segs[k].text.find_first_not_of(" \t") == std::string::npos;
        if (blank && k > 0 && k + 1 < segs.size() && segs[k - 1].encoded && segs[k + 1].encoded) continue;
        kept.push_back(std::move(segs[k]));
    }
    // adjacent words of one charset are converted together: a multi-byte character may be split between them
    std::string out;
    for (size_t k = 0; k < kept.size();) {
        if (!kept[k].encoded) {
            out += to_utf8(kept[k].text, "");
            ++k;
            continue;
        }
        std::string bytes = kept[k].text;
        size_t j = k + 1;
        while (j < kept.size() && kept[j].encoded && kept[j].charset == kept[k].charset) bytes += kept[j++].text;
        out += to_utf8(bytes, kept[k].charset);
        k = j;
    }
    return out;
}

// ---------------------------------------------------------------------------------------------- parameters

namespace {

struct Piece {
    bool extended = false;   // name*N*= : percent-encoded, the first one carries charset'language'
    std::string text;
};

std::string percent_decode(std::string_view v) {
    std::string out;
    for (size_t i = 0; i < v.size(); ++i) {
        if (v[i] == '%') {
            const int h = i + 1 < v.size() ? hex_value(v[i + 1]) : -1;
            const int l = i + 2 < v.size() ? hex_value(v[i + 2]) : -1;
            if (h >= 0 && l >= 0) {
                out += static_cast<char>(h * 16 + l);
                i += 2;
                continue;
            }
        }
        out += v[i];
    }
    return out;
}

}  // namespace

HeaderParams parse_header_params(std::string_view header_value) {
    HeaderParams result;
    const std::string s = clean_text(header_value);
    size_t i = 0;
    const size_t semi = s.find(';');
    result.value = lower(trim(std::string_view(s).substr(0, semi == std::string::npos ? s.size() : semi)));
    if (semi == std::string::npos) return result;
    i = semi + 1;

    struct Param {
        std::map<int, Piece> pieces;
        bool plain = false;   // a value given without the RFC 2231 star
    };
    std::map<std::string, Param> found;
    std::vector<std::string> order;
    size_t tokens = 0;
    while (i < s.size() && tokens++ < kMaxParamTokens) {
        while (i < s.size() && (is_ws(s[i]) || s[i] == ';')) ++i;
        if (i >= s.size()) break;
        const size_t name_start = i;
        while (i < s.size() && s[i] != '=' && s[i] != ';') ++i;
        if (i >= s.size() || s[i] != '=') continue;   // a token without a value
        std::string name = lower(trim(std::string_view(s).substr(name_start, i - name_start)));
        ++i;
        while (i < s.size() && is_ws(s[i])) ++i;
        std::string val;
        if (i < s.size() && s[i] == '"') {
            ++i;
            while (i < s.size() && s[i] != '"') {
                if (s[i] == '\\' && i + 1 < s.size()) ++i;
                val += s[i++];
            }
            if (i < s.size()) ++i;
        } else {
            const size_t vs = i;
            while (i < s.size() && s[i] != ';') ++i;
            val = trim(std::string_view(s).substr(vs, i - vs));
        }
        if (name.empty() || val.empty()) continue;

        // name, name*, name*N, name*N*
        std::string base = name;
        int index = 0;
        bool ext = false, starred = false;
        const size_t star = name.find('*');
        if (star != std::string::npos) {
            starred = true;
            base = name.substr(0, star);
            std::string rest = name.substr(star + 1);
            if (!rest.empty() && rest.back() == '*') { ext = true; rest.pop_back(); }
            if (!rest.empty()) {
                if (rest.size() > 4 || rest.find_first_not_of("0123456789") != std::string::npos) continue;
                index = std::atoi(rest.c_str());
            } else {
                ext = true;   // "name*=": one extended value
            }
        }
        if (base.empty()) continue;
        if (!found.count(base)) {
            if (found.size() >= kMaxParams) continue;
            order.push_back(base);
        }
        Param& p = found[base];
        if (!starred) {
            p.plain = true;
            p.pieces[-1] = {false, val};
        } else if (p.pieces.size() < kMaxSegments) {
            p.pieces[index] = {ext, val};
        }
    }

    for (const std::string& base : order) {
        Param& p = found[base];
        bool any_extended = false;
        for (const auto& kv : p.pieces)
            if (kv.first >= 0) any_extended = true;
        if (!any_extended) {   // a plain parameter (RFC 2047 words are tolerated)
            const std::string& v = p.pieces[-1].text;
            result.params[base] = v.find("=?") != std::string::npos ? decode_header_words(v) : to_utf8(v, "");
            continue;
        }
        std::string charset;
        std::string bytes;
        bool first = true;
        for (const auto& kv : p.pieces) {
            if (kv.first < 0) continue;
            std::string text = kv.second.text;
            if (kv.second.extended) {
                if (first) {   // charset'language'value
                    const size_t q1 = text.find('\'');
                    const size_t q2 = q1 == std::string::npos ? std::string::npos : text.find('\'', q1 + 1);
                    if (q2 != std::string::npos) {
                        charset = text.substr(0, q1);
                        text = text.substr(q2 + 1);
                    }
                }
                text = percent_decode(text);
            }
            bytes += text;
            first = false;
        }
        result.params[base] = to_utf8(bytes, charset);
    }
    return result;
}

// ---------------------------------------------------------------------------------------------- addresses

namespace {

bool loose_address(const std::string& a) {
    if (a.empty() || a.size() > 320) return false;
    const size_t at = a.find('@');
    if (at == std::string::npos || at == 0 || at + 1 >= a.size() || a.find('@', at + 1) != std::string::npos) return false;
    for (unsigned char c : a)
        if (c <= 0x20 || c == 0x7F || c == '<' || c == '>' || c == '"' || c == ',' || c == ';' || c == ':' || c == '(' || c == ')') return false;
    return true;
}

std::string unquote(const std::string& v) {
    std::string t = trim(v);
    if (t.size() >= 2 && t.front() == '"' && t.back() == '"') {
        std::string out;
        for (size_t i = 1; i + 1 < t.size(); ++i) {
            if (t[i] == '\\' && i + 2 < t.size()) ++i;
            out += t[i];
        }
        return out;
    }
    return t;
}

// Reads one element ("Name <addr> (comment)" or "addr (comment)"): the pieces outside quotes.
struct Element {
    std::string before_angle;   // text before '<' (or the whole element when there is no '<')
    std::string angle;          // text inside <>
    std::string comment;        // first (comment)
    bool has_angle = false;
};

Element split_element(const std::string& e) {
    Element r;
    bool quoted = false;
    int comment_depth = 0;
    std::string comment;
    bool in_angle = false;
    for (size_t i = 0; i < e.size(); ++i) {
        const char c = e[i];
        if (quoted) {
            (in_angle ? r.angle : r.before_angle) += c;
            if (c == '\\' && i + 1 < e.size()) (in_angle ? r.angle : r.before_angle) += e[++i];
            else if (c == '"') quoted = false;
            continue;
        }
        if (comment_depth > 0) {
            if (c == '(') ++comment_depth;
            else if (c == ')' && --comment_depth == 0) { if (r.comment.empty()) r.comment = comment; comment.clear(); continue; }
            if (comment_depth > 0) comment += c;
            continue;
        }
        if (c == '"') { quoted = true; (in_angle ? r.angle : r.before_angle) += c; }
        else if (c == '(') { comment_depth = 1; comment.clear(); }
        else if (c == '<' && !in_angle) { in_angle = true; r.has_angle = true; }
        else if (c == '>' && in_angle) { in_angle = false; }
        else (in_angle ? r.angle : r.before_angle) += c;
    }
    return r;
}

// Splits a header value into elements at top-level commas and group ends, dropping group names.
std::vector<std::string> split_top_level(std::string_view v, bool semicolons_separate) {
    std::vector<std::string> out;
    std::string cur;
    bool quoted = false, angle = false;
    int comment = 0;
    for (size_t i = 0; i < v.size(); ++i) {
        const char c = v[i];
        if (quoted) {
            cur += c;
            if (c == '\\' && i + 1 < v.size()) cur += v[++i];
            else if (c == '"') quoted = false;
            continue;
        }
        if (comment > 0) {
            cur += c;
            if (c == '(') ++comment;
            else if (c == ')') --comment;
            continue;
        }
        if (c == '"') { quoted = true; cur += c; }
        else if (c == '(') { ++comment; cur += c; }
        else if (c == '<') { angle = true; cur += c; }
        else if (c == '>') { angle = false; cur += c; }
        else if (!angle && c == ':' && !semicolons_separate) { cur.clear(); }   // "Group name:" is dropped
        else if (!angle && (c == ',' || c == ';')) { out.push_back(cur); cur.clear(); }
        else cur += c;
    }
    out.push_back(cur);
    return out;
}

}  // namespace

std::vector<MailAddress> parse_mailboxes(std::string_view header_value) {
    std::vector<MailAddress> out;
    const std::string text = clean_text(header_value);
    for (const std::string& raw : split_top_level(text, false)) {
        if (out.size() >= kMaxMailboxes) break;
        const std::string e = trim(raw);
        if (e.empty()) continue;
        const Element el = split_element(e);
        MailAddress m;
        if (el.has_angle) {
            m.address = trim(el.angle);
            const size_t colon = m.address.rfind(':');   // an obsolete route "@a,@b:addr"
            if (colon != std::string::npos) m.address = trim(m.address.substr(colon + 1));
            m.name = decode_header_words(unquote(el.before_angle));
            if (m.name.empty()) m.name = decode_header_words(trim(el.comment));
        } else {
            m.address = trim(el.before_angle);
            m.name = decode_header_words(trim(el.comment));
        }
        if (!loose_address(m.address) || !is_valid_utf8(m.address)) continue;
        m.name = sanitize_utf8(m.name);
        out.push_back(std::move(m));
    }
    return out;
}

// "Name <addr>, addr2" for display; a name with a comma or angle bracket is quoted.
std::string format_address_header(std::string_view raw_header) {
    const std::vector<MailAddress> list = parse_mailboxes(raw_header);
    if (list.empty()) return sanitize_utf8(decode_header_words(raw_header));   // not parsable as addresses: show it as written
    std::string out;
    for (const MailAddress& a : list) {
        if (!out.empty()) out += ", ";
        if (a.name.empty()) {
            out += a.address;
        } else if (a.name.find_first_of(",<>\"") != std::string::npos) {
            std::string q;
            for (char c : a.name) q += (c == '"' || c == '\\') ? std::string("\\") + c : std::string(1, c);
            out += "\"" + q + "\" <" + a.address + ">";
        } else {
            out += a.name + " <" + a.address + ">";
        }
    }
    return out;
}

std::vector<std::string> parse_address_list(std::string_view text) {
    std::vector<std::string> none;
    if (text.size() > 64 * 1024) return none;
    for (char c : text)
        if (c == '\r' || c == '\n' || c == '\0') return none;
    std::vector<std::string> parts = split_top_level(text, true);
    // one trailing separator, as typed, is fine
    if (!parts.empty() && trim(parts.back()).empty()) parts.pop_back();
    std::vector<std::string> out;
    for (const std::string& raw : parts) {
        const std::string e = trim(raw);
        if (e.empty()) return none;
        std::string addr;
        const size_t lt = e.find('<');
        if (lt == std::string::npos) {
            addr = e;
        } else {
            const size_t gt = e.find('>', lt);
            if (gt == std::string::npos || !trim(e.substr(gt + 1)).empty()) return none;
            addr = trim(e.substr(lt + 1, gt - lt - 1));
            const std::string name = trim(e.substr(0, lt));
            for (unsigned char c : name)
                if (c < 0x20 || c == 0x7F) return none;
        }
        if (!is_valid_address(addr)) return none;
        bool dup = false;
        for (const std::string& o : out)
            if (lower(o) == lower(addr)) dup = true;
        if (!dup) out.push_back(addr);
        if (out.size() > kMaxRecipients) return none;
    }
    return out;
}

}  // namespace pbo
