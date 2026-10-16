#include "pbo/xml_min.h"

#include <map>
#include <utility>

namespace pbo {

const XmlNode* XmlNode::child(std::string_view ns_uri, std::string_view local) const {
    for (const XmlNode& c : children)
        if (c.name == local && c.ns == ns_uri) return &c;
    return nullptr;
}

std::vector<const XmlNode*> XmlNode::children_named(std::string_view ns_uri, std::string_view local) const {
    std::vector<const XmlNode*> out;
    for (const XmlNode& c : children)
        if (c.name == local && c.ns == ns_uri) out.push_back(&c);
    return out;
}

std::string XmlNode::attr(std::string_view attr_name) const {
    for (const auto& a : attrs)
        if (a.first == attr_name) return a.second;
    return std::string();
}

namespace {

struct Failure {
    const char* why;
};

using NsScope = std::map<std::string, std::string>;   // prefix ("" = default) -> URI

void append_utf8(std::string& out, unsigned long cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
bool is_name_start(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == ':' || (static_cast<unsigned char>(c) >= 0x80); }
bool is_name_char(char c) { return is_name_start(c) || (c >= '0' && c <= '9') || c == '-' || c == '.'; }

class Parser {
public:
    Parser(std::string_view s, const XmlLimits& l) : s_(s), lim_(l) {}

    XmlNode run() {
        if (s_.size() > lim_.max_bytes) throw Failure{"too_large"};
        if (s_.size() >= 3 && s_.substr(0, 3) == "\xEF\xBB\xBF") pos_ = 3;   // byte order mark
        skip_space();
        if (starts("<?xml") && pos_ + 5 < s_.size() && (is_space(s_[pos_ + 5]) || s_[pos_ + 5] == '?')) {
            const size_t end = s_.find("?>", pos_);
            if (end == std::string_view::npos) throw Failure{"unterminated_declaration"};
            pos_ = end + 2;
        }
        prolog_misc();
        if (pos_ >= s_.size() || s_[pos_] != '<') throw Failure{"no_root"};
        NsScope scope;
        XmlNode root = element(scope, 1);
        prolog_misc();
        if (pos_ != s_.size()) throw Failure{"content_after_root"};
        return root;
    }

private:
    std::string_view s_;
    const XmlLimits& lim_;
    size_t pos_ = 0;
    size_t nodes_ = 0;

    bool starts(std::string_view t) const { return s_.compare(pos_, t.size(), t) == 0; }
    void skip_space() { while (pos_ < s_.size() && is_space(s_[pos_])) ++pos_; }

    // whitespace and comments around the root element; DOCTYPE and processing instructions are refused
    void prolog_misc() {
        for (;;) {
            skip_space();
            if (starts("<!--")) {
                comment();
            } else if (starts("<!DOCTYPE") || starts("<!doctype")) {
                throw Failure{"doctype"};
            } else if (starts("<?")) {
                throw Failure{"processing_instruction"};
            } else {
                return;
            }
        }
    }

    void comment() {
        const size_t end = s_.find("-->", pos_ + 4);
        if (end == std::string_view::npos) throw Failure{"unterminated_comment"};
        pos_ = end + 3;
    }

    std::string name() {
        if (pos_ >= s_.size() || !is_name_start(s_[pos_])) throw Failure{"bad_name"};
        const size_t b = pos_;
        while (pos_ < s_.size() && is_name_char(s_[pos_])) ++pos_;
        return std::string(s_.substr(b, pos_ - b));
    }

    // the text after '&' up to and including ';' -> appended
    void reference(std::string& out) {
        const size_t semi = s_.find(';', pos_);
        if (semi == std::string_view::npos || semi - pos_ > 10) throw Failure{"bad_entity"};
        const std::string_view ref = s_.substr(pos_ + 1, semi - pos_ - 1);
        pos_ = semi + 1;
        if (ref == "lt") out += '<';
        else if (ref == "gt") out += '>';
        else if (ref == "amp") out += '&';
        else if (ref == "quot") out += '"';
        else if (ref == "apos") out += '\'';
        else if (!ref.empty() && ref[0] == '#') {
            unsigned long cp = 0;
            const bool hex = ref.size() > 1 && (ref[1] == 'x');
            const std::string_view digits = ref.substr(hex ? 2 : 1);
            if (digits.empty()) throw Failure{"bad_reference"};
            for (char c : digits) {
                int v;
                if (c >= '0' && c <= '9') v = c - '0';
                else if (hex && c >= 'a' && c <= 'f') v = c - 'a' + 10;
                else if (hex && c >= 'A' && c <= 'F') v = c - 'A' + 10;
                else throw Failure{"bad_reference"};
                cp = cp * (hex ? 16 : 10) + static_cast<unsigned long>(v);
                if (cp > 0x10FFFF) throw Failure{"bad_reference"};
            }
            if (cp == 0 || (cp >= 0xD800 && cp <= 0xDFFF)) throw Failure{"bad_reference"};
            append_utf8(out, cp);
        } else {
            throw Failure{"unknown_entity"};
        }
    }

    std::string attr_value() {
        if (pos_ >= s_.size() || (s_[pos_] != '"' && s_[pos_] != '\'')) throw Failure{"bad_attribute"};
        const char q = s_[pos_++];
        std::string out;
        for (;;) {
            if (pos_ >= s_.size()) throw Failure{"unterminated_attribute"};
            const char c = s_[pos_];
            if (c == q) { ++pos_; return out; }
            if (c == '<') throw Failure{"bad_attribute"};
            if (c == '&') { reference(out); continue; }
            out += c;
            ++pos_;
        }
    }

    static std::pair<std::string, std::string> split_qname(const std::string& q) {
        const size_t colon = q.find(':');
        if (colon == std::string::npos) return {std::string(), q};
        return {q.substr(0, colon), q.substr(colon + 1)};
    }

    XmlNode element(const NsScope& outer, int depth) {
        if (depth > lim_.max_depth) throw Failure{"too_deep"};
        if (++nodes_ > lim_.max_nodes) throw Failure{"too_many_nodes"};
        ++pos_;   // '<'
        const std::string qname = name();
        std::vector<std::pair<std::string, std::string>> raw_attrs;
        NsScope scope = outer;
        bool self_closing = false;
        for (;;) {
            const bool had_space = pos_ < s_.size() && is_space(s_[pos_]);
            skip_space();
            if (pos_ >= s_.size()) throw Failure{"unterminated_tag"};
            if (s_[pos_] == '>') { ++pos_; break; }
            if (s_[pos_] == '/') {
                if (pos_ + 1 >= s_.size() || s_[pos_ + 1] != '>') throw Failure{"bad_tag"};
                pos_ += 2;
                self_closing = true;
                break;
            }
            if (!had_space) throw Failure{"bad_tag"};
            const std::string an = name();
            skip_space();
            if (pos_ >= s_.size() || s_[pos_] != '=') throw Failure{"bad_attribute"};
            ++pos_;
            skip_space();
            std::string av = attr_value();
            for (const auto& a : raw_attrs)
                if (a.first == an) throw Failure{"duplicate_attribute"};
            raw_attrs.emplace_back(an, std::move(av));
        }
        XmlNode node;
        for (const auto& a : raw_attrs) {
            if (a.first == "xmlns") scope[""] = a.second;
            else if (a.first.compare(0, 6, "xmlns:") == 0) scope[a.first.substr(6)] = a.second;
        }
        const auto q = split_qname(qname);
        node.name = q.second;
        if (q.first.empty()) {
            auto it = scope.find("");
            node.ns = it == scope.end() ? std::string() : it->second;
        } else {
            auto it = scope.find(q.first);
            if (it == scope.end()) throw Failure{"unbound_prefix"};
            node.ns = it->second;
        }
        for (const auto& a : raw_attrs) {
            if (a.first == "xmlns" || a.first.compare(0, 6, "xmlns:") == 0) continue;
            if (a.first.find(':') != std::string::npos) continue;   // prefixed attributes are not used by the callers
            node.attrs.emplace_back(a.first, a.second);
        }
        if (self_closing) return node;

        for (;;) {
            if (pos_ >= s_.size()) throw Failure{"unterminated_element"};
            const char c = s_[pos_];
            if (c == '&') {
                reference(node.text);
            } else if (c != '<') {
                const size_t next = s_.find_first_of("<&", pos_);
                const size_t stop = next == std::string_view::npos ? s_.size() : next;
                node.text.append(s_.substr(pos_, stop - pos_));
                pos_ = stop;
            } else if (starts("</")) {
                pos_ += 2;
                const std::string closing = name();
                skip_space();
                if (pos_ >= s_.size() || s_[pos_] != '>') throw Failure{"bad_tag"};
                ++pos_;
                if (closing != qname) throw Failure{"mismatched_tag"};
                return node;
            } else if (starts("<!--")) {
                comment();
            } else if (starts("<![CDATA[")) {
                const size_t end = s_.find("]]>", pos_ + 9);
                if (end == std::string_view::npos) throw Failure{"unterminated_cdata"};
                node.text.append(s_.substr(pos_ + 9, end - pos_ - 9));
                pos_ = end + 3;
            } else if (starts("<?")) {
                throw Failure{"processing_instruction"};
            } else if (starts("<!")) {
                throw Failure{"doctype"};
            } else {
                node.children.push_back(element(scope, depth + 1));
            }
        }
    }
};

}  // namespace

Result<XmlNode> parse_xml(std::string_view xml, const XmlLimits& limits) {
    Result<XmlNode> r;
    try {
        r.value = Parser(xml, limits).run();
    } catch (const Failure& f) {
        r.value = XmlNode();
        r.error = make_error(ErrorClass::Protocol, "malformed_xml", f.why);
    }
    return r;
}

}  // namespace pbo
