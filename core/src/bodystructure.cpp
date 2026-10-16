#include "pbo/bodystructure.h"

#include <cctype>

#include "mime_internal.h"

namespace pbo {

namespace {

constexpr int kMaxNesting = 32;
constexpr size_t kMaxLiteral = 4096;
constexpr size_t kMaxParts = 400;
constexpr size_t kMaxParamPairs = 64;

char lc(char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
std::string lower(std::string s) {
    for (char& c : s) c = lc(c);
    return s;
}

struct Parser {
    std::string_view s;
    size_t i = 0;
    bool bad = false;
    size_t parts = 0;

    void ws() {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) ++i;
    }
    char peek() {
        ws();
        return i < s.size() ? s[i] : '\0';
    }
    bool eat(char c) {
        if (peek() == c) { ++i; return true; }
        return false;
    }
    bool nil() {
        ws();
        if (i + 3 <= s.size() && lc(s[i]) == 'n' && lc(s[i + 1]) == 'i' && lc(s[i + 2]) == 'l' && (i + 3 == s.size() || !std::isalnum(static_cast<unsigned char>(s[i + 3])))) {
            i += 3;
            return true;
        }
        return false;
    }

    // quoted string, literal or atom; NIL gives an empty string and *is_nil
    bool string(std::string* out, bool* is_nil = nullptr) {
        out->clear();
        if (is_nil) *is_nil = false;
        const char c = peek();
        if (c == '"') {
            ++i;
            while (i < s.size() && s[i] != '"') {
                if (s[i] == '\\' && i + 1 < s.size()) ++i;
                *out += s[i++];
                if (out->size() > kMaxLiteral) { bad = true; return false; }
            }
            if (i >= s.size()) { bad = true; return false; }
            ++i;
            return true;
        }
        if (c == '{') {
            ++i;
            uint64_t n = 0;
            const size_t digits = i;
            while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
                n = n * 10 + static_cast<uint64_t>(s[i++] - '0');
                if (n > kMaxLiteral) { bad = true; return false; }
            }
            if (i == digits || i >= s.size() || s[i] != '}') { bad = true; return false; }
            ++i;
            if (i < s.size() && s[i] == '\r') ++i;
            if (i >= s.size() || s[i] != '\n') { bad = true; return false; }
            ++i;
            if (s.size() - i < n) { bad = true; return false; }
            out->assign(s.substr(i, static_cast<size_t>(n)));
            i += static_cast<size_t>(n);
            return true;
        }
        if (nil()) {
            if (is_nil) *is_nil = true;
            return true;
        }
        const size_t start = i;
        while (i < s.size() && s[i] != ' ' && s[i] != '(' && s[i] != ')' && s[i] != '"' && s[i] != '\r' && s[i] != '\n') ++i;
        if (i == start) { bad = true; return false; }
        out->assign(s.substr(start, i - start));
        return true;
    }

    bool number(uint64_t* out) {
        ws();
        const size_t start = i;
        uint64_t v = 0;
        while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
            if (v > (UINT64_MAX - 9) / 10) { bad = true; return false; }
            v = v * 10 + static_cast<uint64_t>(s[i++] - '0');
        }
        if (i == start) return false;
        *out = v;
        return true;
    }

    // Skips one value of any kind: string, atom, number, NIL or a parenthesized list (nesting counted).
    void skip_value(int depth) {
        if (bad) return;
        const char c = peek();
        if (c == '(') {
            if (depth > kMaxNesting) { bad = true; return; }
            ++i;
            while (!bad && peek() != ')') {
                if (i >= s.size()) { bad = true; return; }
                skip_value(depth + 1);
            }
            if (!bad) ++i;
            return;
        }
        std::string ignored;
        string(&ignored);
    }

    // ("NAME" "value" ...) or NIL
    std::vector<std::pair<std::string, std::string>> param_list() {
        std::vector<std::pair<std::string, std::string>> out;
        if (nil()) return out;
        if (!eat('(')) { bad = true; return out; }
        while (!bad && peek() != ')') {
            if (i >= s.size()) { bad = true; return out; }
            std::string name, value;
            if (!string(&name)) return out;
            if (!string(&value)) return out;
            if (out.size() < kMaxParamPairs) out.emplace_back(lower(name), value);
        }
        if (!bad) ++i;
        return out;
    }

    // Parameter pairs -> resolved map (RFC 2231 continuations and extended values, RFC 2047 words)
    static std::map<std::string, std::string> resolve(const std::vector<std::pair<std::string, std::string>>& pairs) {
        std::string header = "x/y";
        for (const auto& p : pairs) {
            std::string quoted;
            for (char c : p.second) {
                if (c == '"' || c == '\\') quoted += '\\';
                quoted += c;
            }
            header += "; " + p.first + "=\"" + quoted + "\"";
        }
        return parse_header_params(header).params;
    }

    void skip_until_close(int depth) {
        while (!bad && peek() != ')') {
            if (i >= s.size()) { bad = true; return; }
            skip_value(depth + 1);
        }
    }

    BodyPart part(int depth) {
        BodyPart p;
        if (depth > kMaxNesting || ++parts > kMaxParts) { bad = true; return p; }
        if (!eat('(')) { bad = true; return p; }
        if (peek() == '(') {   // multipart
            p.type = "multipart";
            while (!bad && peek() == '(') p.children.push_back(part(depth + 1));
            if (bad) return p;
            std::string sub;
            if (!string(&sub)) return p;
            p.subtype = lower(sub);
            if (peek() != ')') p.params = resolve(param_list());
            if (!bad && peek() != ')') {
                if (!nil()) {   // disposition
                    if (eat('(')) {
                        std::string d;
                        string(&d);
                        p.disposition = lower(d);
                        if (peek() != ')') p.disposition_params = resolve(param_list());
                        skip_until_close(depth);
                        if (!bad) eat(')');
                    } else {
                        bad = true;
                    }
                }
            }
            skip_until_close(depth);
            if (!bad && !eat(')')) bad = true;
            return p;
        }
        std::string t, sub, id, desc, enc;
        if (!string(&t) || !string(&sub)) return p;
        p.type = lower(t);
        p.subtype = lower(sub);
        p.params = resolve(param_list());
        if (peek() != ')') string(&id);
        if (peek() != ')') string(&desc);
        if (peek() != ')') string(&enc);
        p.id = id;
        p.encoding = lower(enc);
        if (peek() != ')') {
            uint64_t n = 0;
            if (number(&n)) p.size = n;
            else if (!bad) bad = true;
        }
        if (!bad && peek() != ')') {
            if (p.type == "message" && p.subtype == "rfc822") {
                skip_value(depth + 1);   // envelope
                skip_value(depth + 1);   // the inner body structure
                uint64_t lines;
                number(&lines);
            } else if (p.type == "text") {
                uint64_t lines;
                number(&lines);
            }
        }
        if (!bad && peek() != ')') {   // md5
            std::string md5;
            string(&md5);
        }
        if (!bad && peek() != ')') {   // disposition
            if (!nil()) {
                if (eat('(')) {
                    std::string d;
                    string(&d);
                    p.disposition = lower(d);
                    if (peek() != ')') p.disposition_params = resolve(param_list());
                    skip_until_close(depth);
                    if (!bad) eat(')');
                } else {
                    bad = true;
                }
            }
        }
        skip_until_close(depth);
        if (!bad && !eat(')')) bad = true;
        return p;
    }
};

void number_sections(BodyPart& p, const std::string& section) {
    p.section = section;
    for (size_t k = 0; k < p.children.size(); ++k)
        number_sections(p.children[k], section.empty() ? std::to_string(k + 1) : section + "." + std::to_string(k + 1));
}

mime_internal::PartNode to_node(const BodyPart& p, const std::map<std::string, std::string>* bytes) {
    mime_internal::PartNode n;
    n.type = p.mime();
    n.params = p.params;
    n.disposition = p.disposition;
    n.filename = p.disposition_params.count("filename") ? p.disposition_params.at("filename") : (p.params.count("name") ? p.params.at("name") : "");
    n.content_id = p.id;
    n.cte = p.encoding;
    n.section = p.section;
    n.decoded_size = mime_internal::decoded_size_from_transfer(p.size, p.encoding);
    if (bytes) {
        const auto it = bytes->find(p.section);
        if (it != bytes->end()) {
            n.body = it->second;
            n.has_body = true;
        }
    }
    for (const BodyPart& c : p.children) n.children.push_back(to_node(c, bytes));
    return n;
}

}  // namespace

std::optional<BodyPart> parse_bodystructure(std::string_view text) {
    Parser ps{text};
    if (ps.peek() != '(') return std::nullopt;
    BodyPart root = ps.part(0);
    if (ps.bad) return std::nullopt;
    number_sections(root, root.multipart() ? "" : "1");
    return root;
}

std::vector<std::string> choose_text_sections(const BodyPart& root) {
    const mime_internal::PartNode node = to_node(root, nullptr);
    std::vector<std::string> out;
    for (const mime_internal::PartNode* p : mime_internal::choose_text_parts(node, MimeLimits{})) out.push_back(p->section);
    return out;
}

std::vector<std::string> choose_picture_html_sections(const BodyPart& root) {
    const mime_internal::PartNode node = to_node(root, nullptr);
    std::vector<std::string> out;
    for (const mime_internal::PartNode* p : mime_internal::choose_html_for_pictures(node)) out.push_back(p->section);
    return out;
}

ParsedMessage assemble_from_parts(const BodyPart& root, const std::map<std::string, std::string>& section_bytes, const MimeLimits& limits) {
    const mime_internal::PartNode node = to_node(root, &section_bytes);
    return mime_internal::assemble(node, limits);
}

}  // namespace pbo
