// vCard reader for contacts (vcard.h). Total and bounded: no exceptions, no recursion, every loop is limited by the size of its input and the
// limits of contact_types.h. See specs/002-server-contacts/research.md R-05/R-06 and contracts/carddav-vcard-subset.md section 3.

#include "pbo/vcard.h"

#include <algorithm>
#include <cctype>
#include <set>
#include <cstdio>

#include "mime_internal.h"
#include "pbo/charset.h"
#include "pbo/civil.h"
#include "pbo/utf8.h"

namespace pbo {

namespace {

// ---- lines ------------------------------------------------------------------------------------------------------------------------

struct Logical {
    std::string raw;       // the physical lines exactly as written
    std::string unf;       // unfolded text (continuation marks removed, quoted-printable soft breaks joined)
    bool qp = false;       // a quoted-printable property
    bool qp_open = false;  // the last physical line ended with a soft break
};

bool header_is_qp(std::string_view line) {
    // the part before the first colon holds "QUOTED-PRINTABLE" (2.1: a bare parameter, or ENCODING=QUOTED-PRINTABLE)
    const size_t colon = line.find(':');
    std::string head(line.substr(0, colon == std::string_view::npos ? line.size() : colon));
    for (char& c : head) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return head.find("QUOTED-PRINTABLE") != std::string::npos;
}

// Splits `text` into logical lines. Physical line ends: CRLF, LF, lone CR. A physical line that starts with a space or a tab continues the
// previous logical line; a quoted-printable line that ends with "=" is continued by the next physical line whatever it starts with.
std::vector<Logical> split_logical(std::string_view text, size_t* physical_count = nullptr) {
    std::vector<Logical> out;
    size_t physical = 0;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = pos;
        while (end < text.size() && text[end] != '\r' && text[end] != '\n') ++end;
        const std::string_view content = text.substr(pos, end - pos);
        size_t next = end;
        if (next < text.size()) {
            if (text[next] == '\r' && next + 1 < text.size() && text[next + 1] == '\n') next += 2;
            else next += 1;
        }
        const std::string_view raw = text.substr(pos, next - pos);
        ++physical;
        pos = next;
        if (!out.empty() && out.back().qp_open) {
            Logical& cur = out.back();
            cur.raw.append(raw);
            cur.unf.pop_back();   // the soft break
            cur.unf.append(content);
            cur.qp_open = !cur.unf.empty() && cur.unf.back() == '=';
            continue;
        }
        if (!out.empty() && !content.empty() && (content[0] == ' ' || content[0] == '\t')) {
            Logical& cur = out.back();
            cur.raw.append(raw);
            cur.unf.append(content.substr(1));
            cur.qp_open = cur.qp && !cur.unf.empty() && cur.unf.back() == '=';
            continue;
        }
        Logical l;
        l.raw.assign(raw);
        l.unf.assign(content);
        l.qp = header_is_qp(content);
        l.qp_open = l.qp && !l.unf.empty() && l.unf.back() == '=';
        out.push_back(std::move(l));
    }
    if (physical_count) *physical_count = physical;
    return out;
}

std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(const std::string& s) {
    size_t b = 0;
    while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    size_t e = s.size();
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

// the first ':' outside double quotes
size_t find_colon(const std::string& s) {
    bool quoted = false;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '"') quoted = !quoted;
        else if (s[i] == ':' && !quoted) return i;
    }
    return std::string::npos;
}

std::vector<std::string> split_outside_quotes(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    bool quoted = false;
    for (char c : s) {
        if (c == '"') quoted = !quoted;
        if (c == sep && !quoted) {
            out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    out.push_back(cur);
    return out;
}

bool name_char(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_'; }

VCardLine parse_line(const Logical& l) {
    VCardLine v;
    v.raw = l.raw;
    const size_t colon = find_colon(l.unf);
    if (colon == std::string::npos) return v;   // not a property: kept, never interpreted
    const std::string head = l.unf.substr(0, colon);
    const size_t semi = [&] {
        bool quoted = false;
        for (size_t i = 0; i < head.size(); ++i) {
            if (head[i] == '"') quoted = !quoted;
            else if (head[i] == ';' && !quoted) return i;
        }
        return std::string::npos;
    }();
    std::string first = head.substr(0, semi);
    std::string params = semi == std::string::npos ? std::string() : head.substr(semi + 1);
    std::string group, name = first;
    const size_t dot = first.rfind('.');
    if (dot != std::string::npos) {
        group = first.substr(0, dot);
        name = first.substr(dot + 1);
    }
    if (name.empty() || !std::all_of(name.begin(), name.end(), name_char)) return v;
    v.group = group;
    v.name = upper(name);
    v.params = params;
    v.value = l.unf.substr(colon + 1);
    return v;
}

// ---- values -----------------------------------------------------------------------------------------------------------------------

struct Params {
    std::vector<std::string> types;   // lower case, quotes removed
    std::string encoding;             // upper case: QUOTED-PRINTABLE, BASE64, B, ...
    std::string charset;
    std::string value_type;           // lower case
};

Params parse_params(const std::string& params) {
    Params p;
    if (params.empty()) return p;
    for (const std::string& part : split_outside_quotes(params, ';')) {
        if (part.empty()) continue;
        const size_t eq = part.find('=');
        if (eq == std::string::npos) {   // vCard 2.1: a bare word is a type or an encoding
            const std::string w = upper(trim(part));
            if (w == "QUOTED-PRINTABLE" || w == "BASE64" || w == "7BIT" || w == "8BIT") p.encoding = w;
            else if (!w.empty()) p.types.push_back(lower(w));
            continue;
        }
        const std::string key = upper(trim(part.substr(0, eq)));
        std::string val = part.substr(eq + 1);
        if (key == "ENCODING") {
            p.encoding = upper(trim(val));
        } else if (key == "CHARSET") {
            p.charset = trim(val);
        } else if (key == "VALUE") {
            p.value_type = lower(trim(val));
        } else if (key == "TYPE") {
            val.erase(std::remove(val.begin(), val.end(), '"'), val.end());
            for (std::string t : split_outside_quotes(val, ',')) {
                t = trim(t);
                if (!t.empty()) p.types.push_back(lower(t));
            }
        }
    }
    return p;
}

// the value of a property as UTF-8 text: quoted-printable and the character set of vCard 2.1 are decoded, NUL bytes dropped, line ends = "\n"
std::string decode_value(const VCardLine& l) {
    const Params p = parse_params(l.params);
    std::string bytes = l.value;
    if (p.encoding == "QUOTED-PRINTABLE") bytes = mime_internal::decode_qp(bytes, bytes.size() + 1);
    std::string text = to_utf8(bytes, p.charset);
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '\0') continue;
        if (c == '\r') {
            out += '\n';
            if (i + 1 < text.size() && text[i + 1] == '\n') ++i;
            continue;
        }
        out += c;
    }
    return out;
}

std::string unescape_text(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '\\' || i + 1 >= s.size()) {
            out += s[i];
            continue;
        }
        const char n = s[++i];
        if (n == 'n' || n == 'N') out += '\n';
        else out += n;   // \\ \, \; \: and any other escaped character
    }
    return out;
}

// the components of a structured value (N, ADR, ORG), split at ';' that is not escaped, each one unescaped
std::vector<std::string> components(const std::string& s, size_t max_parts) {
    std::vector<std::string> out;
    std::string cur;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            cur += s[i];
            cur += s[++i];
        } else if (s[i] == ';') {
            if (out.size() + 1 >= max_parts) break;   // the rest is ignored
            out.push_back(unescape_text(cur));
            cur.clear();
        } else {
            cur += s[i];
        }
    }
    out.push_back(unescape_text(cur));
    return out;
}

// ---- labels -----------------------------------------------------------------------------------------------------------------------

// "_$!<Mother>!$_" (Apple) -> "Mother"; the known words become the editor's labels
std::string normalize_custom_label(std::string text) {
    text = trim(text);
    if (text.size() > 7 && text.compare(0, 3, "_$!") == 0 && text.compare(text.size() - 3, 3, "!$_") == 0) {
        std::string inner = text.substr(3, text.size() - 6);
        if (inner.size() >= 2 && inner.front() == '<' && inner.back() == '>') inner = inner.substr(1, inner.size() - 2);
        text = trim(inner);
    }
    const std::string l = lower(text);
    if (l == "home" || l == "work" || l == "other") return l;
    if (l == "mobile" || l == "iphone" || l == "cell") return "mobile";
    return text;
}

std::string label_from_types(const std::vector<std::string>& types, bool phone) {
    static const char* ignored[] = {"voice", "pref", "internet", "text", "msg", "video", "x400", "textphone"};
    std::vector<std::string> rest;
    for (const std::string& t : types) {
        if (std::find_if(std::begin(ignored), std::end(ignored), [&](const char* i) { return t == i; }) != std::end(ignored)) continue;
        rest.push_back(t);
    }
    auto has = [&](const char* w) { return std::find(rest.begin(), rest.end(), w) != rest.end(); };
    if (phone) {
        if (has("fax")) return "fax";
        if (has("pager")) return "pager";
        if (has("cell")) return "mobile";
    }
    if (has("work")) return "work";
    if (has("home")) return "home";
    if (!rest.empty()) return rest.front();
    return "other";
}

// ---- the card ---------------------------------------------------------------------------------------------------------------------

bool is_blank(const VCardLine& l) { return trim(l.raw).empty(); }

std::string combine_street(const std::vector<std::string>& c) {
    std::string out;
    for (size_t i = 0; i < 3 && i < c.size(); ++i) {
        const std::string t = trim(c[i]);
        if (t.empty()) continue;
        if (!out.empty()) out += ' ';
        out += t;
    }
    return out;
}

}  // namespace

ParsedContact parse_vcard(std::string_view text) {
    ParsedContact card;
    if (text.size() > kMaxCardBytes) {
        card.problem = "too_large";
        return card;
    }
    size_t physical = 0;
    const std::vector<Logical> logical = split_logical(text, &physical);
    if (physical > kMaxCardLines) {
        card.problem = "too_large";
        return card;
    }
    auto too_large = [&card]() {
        card = ParsedContact();
        card.problem = "too_large";
        return card;
    };
    card.lines.reserve(logical.size());
    for (const Logical& l : logical) {
        if (l.unf.size() > kMaxCardLineBytes) return too_large();
        card.lines.push_back(parse_line(l));
    }

    // the structure: BEGIN:VCARD ... END:VCARD, exactly one card
    auto is_marker = [&](const VCardLine& l, const char* what) { return l.name == what && upper(trim(l.value)) == "VCARD"; };
    int first = -1;
    for (size_t i = 0; i < card.lines.size(); ++i) {
        if (is_blank(card.lines[i])) continue;
        first = static_cast<int>(i);
        break;
    }
    if (first < 0 || !is_marker(card.lines[static_cast<size_t>(first)], "BEGIN")) {
        card.problem = "malformed";
        return card;
    }
    card.begin_line = first;
    int end = -1;
    for (size_t i = static_cast<size_t>(first) + 1; i < card.lines.size(); ++i) {
        if (is_marker(card.lines[i], "BEGIN")) {
            card.problem = "malformed";   // nested
            return card;
        }
        if (is_marker(card.lines[i], "END")) {
            end = static_cast<int>(i);
            break;
        }
    }
    if (end < 0) {
        card.problem = "malformed";   // truncated
        return card;
    }
    card.end_line = end;
    for (size_t i = static_cast<size_t>(end) + 1; i < card.lines.size(); ++i) {
        if (is_marker(card.lines[i], "BEGIN")) {
            card.problem = "multiple_cards";
            return card;
        }
    }

    // groups with an X-ABLabel (Apple): the label of the entry that shares the group
    std::vector<std::pair<std::string, std::string>> group_labels;
    for (int i = first + 1; i < end; ++i) {
        const VCardLine& l = card.lines[static_cast<size_t>(i)];
        if (l.name == "X-ABLABEL" && !l.group.empty()) {
            const std::string t = normalize_custom_label(unescape_text(decode_value(l)));
            if (!t.empty()) group_labels.emplace_back(l.group, t);
        }
    }
    auto group_label = [&](const std::string& group) -> std::string {
        if (group.empty()) return std::string();
        for (const auto& g : group_labels)
            if (g.first == group) return g.second;
        return std::string();
    };

    for (int i = first + 1; i < end; ++i) {
        const VCardLine& l = card.lines[static_cast<size_t>(i)];
        if (l.name.empty()) continue;
        const Params params = parse_params(l.params);
        if (l.name == "VERSION") {
            if (card.version_line < 0) {
                card.version_line = i;
                card.version = trim(decode_value(l));
            }
        } else if (l.name == "UID") {
            if (card.uid.empty()) card.uid = trim(unescape_text(decode_value(l)));
        } else if (l.name == "FN") {
            if (card.fn_line < 0) {
                card.fn_line = i;
                card.fn = trim(unescape_text(decode_value(l)));
            }
        } else if (l.name == "N") {
            if (card.n_line < 0) {
                card.n_line = i;
                const std::vector<std::string> c = components(decode_value(l), 5);
                auto at = [&](size_t k) { return k < c.size() ? trim(c[k]) : std::string(); };
                card.family = at(0);
                card.given = at(1);
                card.additional = at(2);
                card.prefix = at(3);
                card.suffix = at(4);
            }
        } else if (l.name == "ORG") {
            if (card.org_line < 0) {
                card.org_line = i;
                const std::vector<std::string> c = components(decode_value(l), 1);
                card.org = trim(c.empty() ? std::string() : c[0]);
            }
        } else if (l.name == "NOTE") {
            if (card.note_line < 0) {
                card.note_line = i;
                card.note = unescape_text(decode_value(l));
            }
        } else if (l.name == "REV") {
            if (card.rev_line < 0) card.rev_line = i;
        } else if (l.name == "TEL" || l.name == "EMAIL") {
            const bool phone = l.name == "TEL";
            std::string value = trim(unescape_text(decode_value(l)));
            const std::string low = lower(value);
            if (phone && low.compare(0, 4, "tel:") == 0) value = trim(value.substr(4));
            if (!phone && low.compare(0, 7, "mailto:") == 0) value = trim(value.substr(7));
            if (value.empty()) continue;
            LabeledValue e;
            e.value = value;
            e.label = group_label(l.group);
            if (e.label.empty()) e.label = sanitize_utf8(label_from_types(params.types, phone));
            auto& entries = phone ? card.phones : card.emails;
            auto& lines = phone ? card.phone_lines : card.email_lines;
            e.orig_index = static_cast<int>(entries.size());
            if (entries.size() >= kMaxCardEntries) return too_large();
            entries.push_back(std::move(e));
            lines.push_back(i);
        } else if (l.name == "ADR") {
            const std::vector<std::string> c = components(decode_value(l), 7);
            PostalAddress a;
            a.label = group_label(l.group);
            if (a.label.empty()) a.label = sanitize_utf8(label_from_types(params.types, false));
            auto at = [&](size_t k) { return k < c.size() ? trim(c[k]) : std::string(); };
            a.street = combine_street(c);
            a.city = at(3);
            a.region = at(4);
            a.postcode = at(5);
            a.country = at(6);
            if (a.street.empty() && a.city.empty() && a.region.empty() && a.postcode.empty() && a.country.empty()) continue;
            if (card.addresses.size() >= kMaxCardEntries) return too_large();
            a.orig_index = static_cast<int>(card.addresses.size());
            card.addresses.push_back(std::move(a));
            card.address_lines.push_back(i);
        }
    }

    if (card.uid.empty()) {
        card.problem = "no_uid";   // the fields above are still filled in
        return card;
    }
    card.readable = true;
    return card;
}

std::string strip_heavy_properties(const std::string& text, bool* removed) {
    if (removed) *removed = false;
    const std::vector<Logical> logical = split_logical(text);
    std::string out;
    out.reserve(text.size());
    bool any = false;
    for (const Logical& l : logical) {
        bool drop = l.unf.size() > kMaxCardLineBytes;
        if (!drop) {
            const VCardLine v = parse_line(l);
            drop = v.name == "PHOTO" || v.name == "LOGO" || v.name == "SOUND" || v.name == "KEY";
        }
        if (drop) {
            any = true;
            continue;
        }
        out += l.raw;
    }
    if (removed) *removed = any;
    return any ? out : text;
}

ContactFields contact_derive(const ParsedContact& card) {
    ContactFields f;
    f.family = card.family;
    f.given = card.given;
    f.org = card.org;
    std::string both = trim(card.given + " " + card.family);
    if (!card.fn.empty()) f.display_name = card.fn;
    else if (!both.empty()) f.display_name = both;
    else if (!card.org.empty()) f.display_name = card.org;
    else if (!card.emails.empty()) f.display_name = card.emails.front().value;
    else if (!card.phones.empty()) f.display_name = card.phones.front().value;
    else f.display_name = "(no name)";

    // the second line of the list: something that tells contacts of the same name apart
    for (const std::string* cand : {&card.org, card.emails.empty() ? &card.org : &card.emails.front().value, card.phones.empty() ? &card.org : &card.phones.front().value}) {
        if (!cand->empty() && *cand != f.display_name) {
            f.subtitle = *cand;
            break;
        }
    }

    std::string primary, secondary;
    if (!card.family.empty() || !card.given.empty()) {
        if (!card.family.empty()) {
            primary = card.family;
            secondary = card.given;
        } else {
            primary = card.given;
        }
    } else if (!card.fn.empty()) {
        primary = card.fn;
    } else if (!card.org.empty()) {
        primary = card.org;
    } else if (!card.emails.empty()) {
        primary = card.emails.front().value;
    } else if (!card.phones.empty()) {
        primary = card.phones.front().value;
    }
    f.sort_key = sort_key(primary) + "\x01" + sort_key(secondary);

    auto add = [&](const std::string& s) {
        if (s.empty()) return;
        if (!f.search_text.empty()) f.search_text += ' ';
        f.search_text += s;
    };
    add(sort_key(f.display_name));
    add(sort_key(card.family));
    add(sort_key(card.given));
    add(sort_key(card.org));
    for (const LabeledValue& e : card.emails) {
        ContactEmailRow row;
        row.addr = e.value;
        row.addr_key = lower(e.value);
        row.label = e.label;
        f.emails.push_back(row);
        add(sort_key(e.value));
    }
    for (const LabeledValue& p : card.phones) {
        std::string digits;
        for (char c : p.value)
            if (std::isdigit(static_cast<unsigned char>(c)) || c == '+') digits += c;
        add(digits);
    }
    return f;
}

ContactEdit contact_edit_of(const ParsedContact& card) {
    ContactEdit e;
    e.given = card.given;
    e.family = card.family;
    e.org = card.org;
    if (card.note.size() > kMaxNote) {
        e.note_locked = true;   // never loaded into an editable field
    } else {
        e.note = card.note;
    }
    for (LabeledValue p : card.phones) {
        p.locked = p.value.size() > kMaxPhone;
        e.phones.push_back(std::move(p));
    }
    for (LabeledValue m : card.emails) {
        m.locked = m.value.size() > kMaxEmail;
        e.emails.push_back(std::move(m));
    }
    for (PostalAddress a : card.addresses) {
        a.locked = a.street.size() > kMaxAddressPart || a.city.size() > kMaxAddressPart || a.region.size() > kMaxAddressPart ||
                   a.postcode.size() > kMaxAddressPart || a.country.size() > kMaxAddressPart;
        e.addresses.push_back(std::move(a));
    }
    return e;
}

// ---- writing ----------------------------------------------------------------------------------------------------------------------

namespace {

std::string escape_text(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '\\' || c == ',' || c == ';') {
            out += '\\';
            out += c;
        } else if (c == '\n') {
            out += "\\n";
        } else if (c != '\r') {
            out += c;
        }
    }
    return out;
}

// A content line folded at 75 octets (the continuation lines start with one space), never inside a UTF-8 character; ends with CRLF.
void put_line(std::string* out, const std::string& line, const char* eol = "\r\n") {
    size_t pos = 0;
    bool first = true;
    while (pos < line.size()) {
        const size_t room = first ? 75 : 74;
        size_t end = std::min(line.size(), pos + room);
        while (end < line.size() && end > pos && (static_cast<unsigned char>(line[end]) & 0xC0) == 0x80) --end;   // not inside a character
        if (end == pos) end = std::min(line.size(), pos + room);                                                  // (a malformed run: cut anyway)
        if (!first) *out += ' ';
        out->append(line, pos, end - pos);
        *out += eol;
        pos = end;
        first = false;
    }
    if (line.empty()) *out += eol;
}

bool text_blank(const std::string& s) { return s.find_first_not_of(" \t\r\n") == std::string::npos; }

const char* phone_type(const std::string& label) {
    if (label == "mobile") return ";TYPE=CELL";
    if (label == "home") return ";TYPE=HOME";
    if (label == "work") return ";TYPE=WORK";
    if (label == "fax") return ";TYPE=FAX";
    if (label == "pager") return ";TYPE=PAGER";
    return "";
}

const char* home_work_type(const std::string& label) {
    if (label == "home") return ";TYPE=HOME";
    if (label == "work") return ";TYPE=WORK";
    return "";
}

std::string rev_stamp(int64_t epoch) {
    const LocalDateTime t = civil::from_naive_epoch(epoch);
    char b[32];
    std::snprintf(b, sizeof b, "%04d%02d%02dT%02d%02d%02dZ", t.year, t.month, t.day, t.hour, t.minute, t.second);
    return b;
}

}  // namespace

std::string new_contact_vcf(const ContactEdit& e, const std::string& uid, int64_t now_epoch) {
    std::string name = e.given;
    if (!name.empty() && !e.family.empty()) name += ' ';
    name += e.family;
    if (name.empty()) name = e.org;
    if (name.empty())
        for (const LabeledValue& m : e.emails)
            if (!text_blank(m.value)) { name = m.value; break; }
    if (name.empty())
        for (const LabeledValue& p : e.phones)
            if (!text_blank(p.value)) { name = p.value; break; }

    std::string out;
    put_line(&out, "BEGIN:VCARD");
    put_line(&out, "VERSION:3.0");
    put_line(&out, "PRODID:-//office-pocketbook//EN");
    put_line(&out, "UID:" + uid);
    put_line(&out, "FN:" + escape_text(name));
    put_line(&out, "N:" + escape_text(e.family) + ";" + escape_text(e.given) + ";;;");
    if (!text_blank(e.org)) put_line(&out, "ORG:" + escape_text(e.org));
    for (const LabeledValue& p : e.phones)
        if (!text_blank(p.value)) put_line(&out, std::string("TEL") + phone_type(p.label) + ":" + escape_text(p.value));
    for (const LabeledValue& m : e.emails)
        if (!text_blank(m.value)) put_line(&out, std::string("EMAIL") + home_work_type(m.label) + ":" + escape_text(m.value));
    for (const PostalAddress& a : e.addresses) {
        if (text_blank(a.street) && text_blank(a.city) && text_blank(a.region) && text_blank(a.postcode) && text_blank(a.country)) continue;
        put_line(&out, std::string("ADR") + home_work_type(a.label) + ":;;" + escape_text(a.street) + ";" + escape_text(a.city) + ";" + escape_text(a.region) + ";" +
                           escape_text(a.postcode) + ";" + escape_text(a.country));
    }
    if (!text_blank(e.note)) put_line(&out, "NOTE:" + escape_text(e.note));
    put_line(&out, "REV:" + rev_stamp(now_epoch));
    put_line(&out, "END:VCARD");
    return out;
}

// ---- editing an existing card -----------------------------------------------------------------------------------------------------------------

namespace {

std::string without_eol(std::string raw) {
    while (!raw.empty() && (raw.back() == '\r' || raw.back() == '\n')) raw.pop_back();
    return raw;
}

// the parameters of a line without the ones that only describe how its old text was encoded
std::string params_without_encoding(const std::string& params) {
    std::string out;
    for (const std::string& part : split_outside_quotes(params, ';')) {
        const std::string up = upper(trim(part));
        if (up.empty() || up == "QUOTED-PRINTABLE" || up == "BASE64" || up == "7BIT" || up == "8BIT") continue;
        if (up.compare(0, 9, "ENCODING=") == 0 || up.compare(0, 8, "CHARSET=") == 0) continue;
        if (!out.empty()) out += ';';
        out += part;
    }
    return out;
}

bool has_qp(const std::string& params) { return upper(params).find("QUOTED-PRINTABLE") != std::string::npos; }

std::string type_param(const char* name, const std::string& label, bool phone) {
    const char* t = phone ? phone_type(label) : home_work_type(label);
    (void)name;
    return *t ? std::string(t + 1) : std::string();   // "TYPE=CELL" without the leading ';'
}

std::string make_line(const std::string& group, const std::string& name, const std::string& params, const std::string& value) {
    return (group.empty() ? std::string() : group + ".") + name + (params.empty() ? std::string() : ";" + params) + ":" + value;
}

std::string detect_eol(const std::vector<VCardLine>& lines) {
    for (const VCardLine& l : lines) {
        if (l.raw.size() >= 2 && l.raw.compare(l.raw.size() - 2, 2, "\r\n") == 0) return "\r\n";
        if (!l.raw.empty() && l.raw.back() == '\n') return "\n";
        if (!l.raw.empty() && l.raw.back() == '\r') return "\r";
    }
    return "\r\n";
}

bool same_labeled(const LabeledValue& a, const LabeledValue& b) { return a.label == b.label && a.value == b.value; }
bool same_address(const PostalAddress& a, const PostalAddress& b) {
    return a.label == b.label && a.street == b.street && a.city == b.city && a.region == b.region && a.postcode == b.postcode && a.country == b.country;
}

std::string address_value(const PostalAddress& a) {
    return ";;" + escape_text(a.street) + ";" + escape_text(a.city) + ";" + escape_text(a.region) + ";" + escape_text(a.postcode) + ";" + escape_text(a.country);
}

}  // namespace

Result<std::string> apply_contact_edit(const std::string& original_text, const ContactEdit& edit, int64_t now_epoch) {
    Result<std::string> out;
    const ParsedContact c = parse_vcard(original_text);
    if (!c.readable) {
        out.error = make_error(ErrorClass::Unsupported, "unreadable", "the card cannot be read");
        return out;
    }
    const ContactEdit orig = contact_edit_of(c);
    const std::string eol = detect_eol(c.lines);
    std::vector<std::string> text(c.lines.size());   // the new text of a line that is rewritten ("" with replaced[i] false: unchanged)
    std::vector<char> replaced(c.lines.size(), 0), removed(c.lines.size(), 0);
    std::string added;
    bool changed = false;

    auto remove_group = [&](size_t line) {
        removed[line] = 1;
        const std::string& group = c.lines[line].group;
        if (group.empty()) return;
        for (int i = c.begin_line + 1; i < c.end_line; ++i)
            if (c.lines[static_cast<size_t>(i)].group == group) removed[static_cast<size_t>(i)] = 1;
    };
    auto drop_group_label = [&](const std::string& group) {
        if (group.empty()) return;
        for (int i = c.begin_line + 1; i < c.end_line; ++i)
            if (c.lines[static_cast<size_t>(i)].group == group && c.lines[static_cast<size_t>(i)].name == "X-ABLABEL") removed[static_cast<size_t>(i)] = 1;
    };
    auto rewrite = [&](size_t line, const std::string& value, const std::string& params) {
        std::string t;
        put_line(&t, make_line(c.lines[line].group, c.lines[line].name, params, value), eol.c_str());
        text[line] = t;
        replaced[line] = 1;
    };
    auto add_line = [&](const std::string& logical) { put_line(&added, logical, eol.c_str()); };

    // phone numbers and email addresses
    auto labeled_kind = [&](const std::vector<LabeledValue>& now, const std::vector<LabeledValue>& was, const std::vector<int>& line_of, const char* name, bool phone) {
        std::vector<char> used(was.size(), 0);
        for (const LabeledValue& v : now) {
            const int k = v.orig_index;
            if (k >= 0 && static_cast<size_t>(k) < was.size() && !used[static_cast<size_t>(k)]) {
                used[static_cast<size_t>(k)] = 1;
                if (v.locked || same_labeled(v, was[static_cast<size_t>(k)])) continue;
                const size_t line = static_cast<size_t>(line_of[static_cast<size_t>(k)]);
                const bool label_changed = v.label != was[static_cast<size_t>(k)].label;
                std::string params;
                if (label_changed) {
                    drop_group_label(c.lines[line].group);
                    params = type_param(name, v.label, phone);
                } else {
                    params = params_without_encoding(c.lines[line].params);
                }
                rewrite(line, escape_text(v.value), params);
                changed = true;
            } else if (!v.locked && !text_blank(v.value)) {
                const std::string t = type_param(name, v.label, phone);
                add_line(std::string(name) + (t.empty() ? std::string() : ";" + t) + ":" + escape_text(v.value));
                changed = true;
            }
        }
        for (size_t k = 0; k < was.size(); ++k)
            if (!used[k]) {   // not in the form any more: removed with its group
                remove_group(static_cast<size_t>(line_of[k]));
                changed = true;
            }
    };
    labeled_kind(edit.phones, orig.phones, c.phone_lines, "TEL", true);
    labeled_kind(edit.emails, orig.emails, c.email_lines, "EMAIL", false);

    // postal addresses
    {
        std::vector<char> used(orig.addresses.size(), 0);
        for (const PostalAddress& a : edit.addresses) {
            const int k = a.orig_index;
            if (k >= 0 && static_cast<size_t>(k) < orig.addresses.size() && !used[static_cast<size_t>(k)]) {
                used[static_cast<size_t>(k)] = 1;
                if (a.locked || same_address(a, orig.addresses[static_cast<size_t>(k)])) continue;
                const size_t line = static_cast<size_t>(c.address_lines[static_cast<size_t>(k)]);
                const bool label_changed = a.label != orig.addresses[static_cast<size_t>(k)].label;
                std::string params;
                if (label_changed) {
                    drop_group_label(c.lines[line].group);
                    params = type_param("ADR", a.label, false);
                } else {
                    params = params_without_encoding(c.lines[line].params);
                }
                rewrite(line, address_value(a), params);
                changed = true;
            } else if (!a.locked && !(text_blank(a.street) && text_blank(a.city) && text_blank(a.region) && text_blank(a.postcode) && text_blank(a.country))) {
                const std::string t = type_param("ADR", a.label, false);
                add_line(std::string("ADR") + (t.empty() ? std::string() : ";" + t) + ":" + address_value(a));
                changed = true;
            }
        }
        for (size_t k = 0; k < orig.addresses.size(); ++k)
            if (!used[k]) {
                remove_group(static_cast<size_t>(c.address_lines[k]));
                changed = true;
            }
    }

    // names and organization
    const bool name_changed = edit.given != orig.given || edit.family != orig.family;
    const bool org_changed = edit.org != orig.org;
    if (name_changed) {
        const std::string value = escape_text(edit.family) + ";" + escape_text(edit.given) + ";" + escape_text(c.additional) + ";" + escape_text(c.prefix) + ";" + escape_text(c.suffix);
        if (c.n_line >= 0) rewrite(static_cast<size_t>(c.n_line), value, params_without_encoding(c.lines[static_cast<size_t>(c.n_line)].params));
        else add_line("N:" + value);
        changed = true;
    }
    if (org_changed) {
        if (text_blank(edit.org)) {
            if (c.org_line >= 0) removed[static_cast<size_t>(c.org_line)] = 1;
        } else if (c.org_line >= 0) {
            const VCardLine& l = c.lines[static_cast<size_t>(c.org_line)];
            std::string rest;   // the departments after the first component stay as they were
            if (!has_qp(l.params)) {
                for (size_t i = 0; i < l.value.size(); ++i) {
                    if (l.value[i] == '\\') { ++i; continue; }
                    if (l.value[i] == ';') { rest = l.value.substr(i); break; }
                }
            }
            rewrite(static_cast<size_t>(c.org_line), escape_text(edit.org) + rest, params_without_encoding(l.params));
        } else {
            add_line("ORG:" + escape_text(edit.org));
        }
        changed = true;
    }
    if (name_changed || (org_changed && (c.fn.empty() || c.fn == orig.org))) {
        std::string fn;
        auto join = [&](const std::string& part) {
            if (text_blank(part)) return;
            if (!fn.empty()) fn += ' ';
            fn += part;
        };
        const bool names = !text_blank(edit.given) || !text_blank(edit.family);
        if (names) {
            join(c.prefix);
            join(edit.given);
            join(c.additional);
            join(edit.family);
            join(c.suffix);
        }
        if (fn.empty()) fn = edit.org;
        if (fn.empty() && !edit.emails.empty()) fn = edit.emails.front().value;
        if (fn.empty() && !edit.phones.empty()) fn = edit.phones.front().value;
        if (c.fn_line >= 0) rewrite(static_cast<size_t>(c.fn_line), escape_text(fn), params_without_encoding(c.lines[static_cast<size_t>(c.fn_line)].params));
        else add_line("FN:" + escape_text(fn));
        changed = true;
    }

    // the note: replaced, added or removed unless it is kept as it is (locked)
    if (!edit.note_locked && (orig.note_locked || edit.note != orig.note)) {
        if (text_blank(edit.note)) {
            if (c.note_line >= 0) removed[static_cast<size_t>(c.note_line)] = 1;
        } else if (c.note_line >= 0) {
            rewrite(static_cast<size_t>(c.note_line), escape_text(edit.note), params_without_encoding(c.lines[static_cast<size_t>(c.note_line)].params));
        } else {
            add_line("NOTE:" + escape_text(edit.note));
        }
        changed = true;
    }

    if (!changed) {
        out.value = original_text;
        return out;
    }
    // the revision
    {
        const std::string rev = rev_stamp(now_epoch);
        if (c.rev_line >= 0) rewrite(static_cast<size_t>(c.rev_line), rev, params_without_encoding(c.lines[static_cast<size_t>(c.rev_line)].params));
        else add_line("REV:" + rev);
    }
    std::string result;
    result.reserve(original_text.size() + added.size() + 128);
    for (size_t i = 0; i < c.lines.size(); ++i) {
        if (static_cast<int>(i) == c.end_line) result += added;
        if (removed[i]) continue;
        result += replaced[i] ? text[i] : c.lines[i].raw;
    }
    out.value = result;
    return out;
}

std::string merge_into_full_card(const std::string& edited_stored, const std::string& full_card) {
    const ParsedContact e = parse_vcard(edited_stored);
    if (!e.readable) return edited_stored;
    // the server's card is read without the size limits: its giant lines are exactly what has to be carried over
    std::vector<VCardLine> f;
    for (const Logical& l : split_logical(full_card)) f.push_back(parse_line(l));
    int f_begin = -1, f_end = -1;
    for (size_t i = 0; i < f.size(); ++i) {
        if (f[i].name == "BEGIN" && f_begin < 0) f_begin = static_cast<int>(i);
        else if (f[i].name == "END" && f_begin >= 0) { f_end = static_cast<int>(i); break; }
    }
    if (f_begin < 0 || f_end < 0) return edited_stored;

    static const char* listed[] = {"VERSION", "UID", "FN", "N", "NICKNAME", "ORG", "TITLE", "TEL", "EMAIL", "ADR", "NOTE", "REV", "X-ABLABEL"};
    auto is_listed = [&](const std::string& name) {
        for (const char* l : listed)
            if (name == l) return true;
        return false;
    };
    // the groups the edited card still has, and the groups of the full card that hold an entry the app owns
    std::vector<std::string> edited_groups, owned_groups;
    std::multiset<std::string> present;
    for (int i = e.begin_line + 1; i < e.end_line; ++i) {
        const VCardLine& l = e.lines[static_cast<size_t>(i)];
        present.insert(without_eol(l.raw));
        if (!l.group.empty()) edited_groups.push_back(l.group);
    }
    for (int i = f_begin + 1; i < f_end; ++i) {
        const VCardLine& l = f[static_cast<size_t>(i)];
        if (!l.group.empty() && (l.name == "TEL" || l.name == "EMAIL" || l.name == "ADR")) owned_groups.push_back(l.group);
    }
    auto in = [](const std::vector<std::string>& v, const std::string& x) { return std::find(v.begin(), v.end(), x) != v.end(); };

    const std::string full_eol = detect_eol(f);
    std::string carry;
    for (int i = f_begin + 1; i < f_end; ++i) {
        const VCardLine& l = f[static_cast<size_t>(i)];
        if (!l.group.empty() && in(owned_groups, l.group) && !in(edited_groups, l.group)) continue;   // the user removed that entry: its whole group goes
        const std::string plain = without_eol(l.raw);
        const bool oversize = l.value.size() > kMaxCardLineBytes;
        if (!l.name.empty() && is_listed(l.name) && !oversize) continue;                              // the edited card is the authority for these
        auto hit = present.find(plain);
        if (hit != present.end()) {                                                                  // already in the edited card: not twice
            present.erase(hit);
            continue;
        }
        carry += l.raw;
        if (l.raw.empty() || (l.raw.back() != '\n' && l.raw.back() != '\r')) carry += full_eol;
    }
    std::string out;
    out.reserve(edited_stored.size() + carry.size());
    for (size_t i = 0; i < e.lines.size(); ++i) {
        if (static_cast<int>(i) == e.end_line) out += carry;
        out += e.lines[i].raw;
    }
    return out;
}

bool same_contact_content(const std::string& a, const std::string& b) {
    const ParsedContact x = parse_vcard(a), y = parse_vcard(b);
    if (!x.readable || !y.readable) return false;
    const ContactEdit ex = contact_edit_of(x), ey = contact_edit_of(y);
    auto same_list = [](const std::vector<LabeledValue>& l, const std::vector<LabeledValue>& r) {
        if (l.size() != r.size()) return false;
        for (size_t i = 0; i < l.size(); ++i)
            if (l[i].label != r[i].label || l[i].value != r[i].value) return false;
        return true;
    };
    if (ex.given != ey.given || ex.family != ey.family || ex.org != ey.org || ex.note != ey.note) return false;
    if (!same_list(ex.phones, ey.phones) || !same_list(ex.emails, ey.emails)) return false;
    if (ex.addresses.size() != ey.addresses.size()) return false;
    for (size_t i = 0; i < ex.addresses.size(); ++i)
        if (!same_address(ex.addresses[i], ey.addresses[i])) return false;
    return true;
}

std::string with_new_uid(const std::string& card, const std::string& uid) {
    const ParsedContact c = parse_vcard(card);
    if (c.lines.empty()) return card;
    std::string out;
    bool done = false;
    for (const VCardLine& l : c.lines) {
        if (!done && l.name == "UID") {
            std::string eol = "\r\n";
            if (l.raw.size() >= 2 && l.raw.compare(l.raw.size() - 2, 2, "\r\n") == 0) eol = "\r\n";
            else if (!l.raw.empty() && l.raw.back() == '\n') eol = "\n";
            else if (!l.raw.empty() && l.raw.back() == '\r') eol = "\r";
            put_line(&out, make_line(l.group, "UID", params_without_encoding(l.params), escape_text(uid)), eol.c_str());
            done = true;
        } else {
            out += l.raw;
        }
    }
    return done ? out : card;
}

ContactUpsert contact_upsert_from_card(const std::string& href, const std::string& etag, const std::string& raw_card) {
    ContactUpsert u;
    u.href = href;
    u.etag = etag;
    u.partial = true;   // the request named a property list: the app cannot know what the server left out (data-model.md)
    const std::string stripped = strip_heavy_properties(raw_card);
    const ParsedContact card = parse_vcard(stripped);
    u.fields = contact_derive(card);
    if (card.readable) {
        u.uid = card.uid;
        u.vcf = stripped;
        return u;
    }
    u.unreadable = true;
    u.uid = "href:" + href;
    u.vcf = (card.problem == "too_large" || stripped.size() > kMaxCardBytes) ? std::string() : stripped;
    // a card that gave nothing to show is listed at the end under its path
    if (u.fields.family.empty() && u.fields.given.empty() && u.fields.org.empty() && u.fields.emails.empty() && card.phones.empty() && card.fn.empty())
        u.fields.sort_key = "~" + sort_key(href) + "\x01";
    return u;
}

}  // namespace pbo
