#include "pbo/imap_parse.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string_view>

#include "pbo/mail_headers.h"

namespace pbo {

namespace {

char up(char c) { return static_cast<char>(std::toupper(static_cast<unsigned char>(c))); }
char low(char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }

std::string upper(std::string_view s) {
    std::string r(s);
    for (char& c : r) c = up(c);
    return r;
}

// A cursor over the response bytes. Every read checks the bounds; a failed read sets `bad` and moves nothing.
struct Reader {
    std::string_view s;
    size_t p = 0;
    bool bad = false;
    bool last_was_literal = false;   // the last value read was a literal (libcurl then omits the closing parenthesis)

    bool eof() const { return p >= s.size(); }
    char peek() const { return p < s.size() ? s[p] : '\0'; }
    bool at_eol() const { return p < s.size() && (s[p] == '\r' || s[p] == '\n'); }
    void skip_spaces() {
        while (p < s.size() && (s[p] == ' ' || s[p] == '\t')) ++p;
    }
    void skip_eol() {
        if (p < s.size() && s[p] == '\r') ++p;
        if (p < s.size() && s[p] == '\n') ++p;
    }
    // to the start of the next line; false if there is none
    void skip_line() {
        const size_t nl = s.find('\n', p);
        p = nl == std::string_view::npos ? s.size() : nl + 1;
    }
    bool starts_with_ci(std::string_view w) const {
        if (s.size() - p < w.size()) return false;
        for (size_t i = 0; i < w.size(); ++i)
            if (up(s[p + i]) != up(w[i])) return false;
        return true;
    }

    // unsigned decimal number, at most 20 digits; false if no digit or the value does not fit 64 bits
    bool number(uint64_t* out) {
        const size_t start = p;
        uint64_t v = 0;
        while (p < s.size() && std::isdigit(static_cast<unsigned char>(s[p]))) {
            const unsigned digit = static_cast<unsigned>(s[p] - '0');
            if (v > (UINT64_MAX - digit) / 10) {
                p = start;
                return false;
            }
            v = v * 10 + digit;
            ++p;
        }
        if (p == start) return false;
        *out = v;
        return true;
    }

    // Atom text; brackets belong to the atom (BODY[HEADER.FIELDS (FROM TO)]) up to the matching ']'.
    bool atom(std::string* out) {
        const size_t start = p;
        int bracket = 0;
        while (p < s.size()) {
            const char c = s[p];
            if (bracket > 0) {
                if (c == '\r' || c == '\n') break;
                if (c == '[') ++bracket;
                else if (c == ']') --bracket;
                ++p;
                continue;
            }
            if (c == '[') { ++bracket; ++p; continue; }
            if (c == ' ' || c == '\t' || c == '(' || c == ')' || c == '\r' || c == '\n' || c == '"' || c == '{') break;
            ++p;
        }
        if (bracket > 0 || p == start) {
            if (bracket > 0) bad = true;
            p = start;
            return false;
        }
        *out = std::string(s.substr(start, p - start));
        return true;
    }

    bool quoted(std::string* out) {
        if (peek() != '"') return false;
        size_t q = p + 1;
        std::string r;
        while (q < s.size()) {
            const char c = s[q];
            if (c == '"') {
                p = q + 1;
                *out = std::move(r);
                return true;
            }
            if (c == '\r' || c == '\n') break;
            if (c == '\\' && q + 1 < s.size()) ++q;
            r += s[q];
            ++q;
            if (r.size() > kMaxImapLine) break;
        }
        bad = true;
        return false;
    }

    // {n} CRLF n bytes. A literal bigger than `cap` is cut to `cap` bytes (truncated=true) and skipped.
    bool literal(std::string* out, size_t cap, bool* truncated) {
        if (peek() != '{') return false;
        size_t q = p + 1;
        uint64_t n = 0;
        const size_t digits_at = q;
        while (q < s.size() && std::isdigit(static_cast<unsigned char>(s[q]))) {
            if (n > kMaxImapInput) { bad = true; return false; }
            n = n * 10 + static_cast<uint64_t>(s[q] - '0');
            ++q;
        }
        if (q == digits_at || n > kMaxImapInput) { bad = true; return false; }
        if (q < s.size() && s[q] == '+') ++q;
        if (q >= s.size() || s[q] != '}') { bad = true; return false; }
        ++q;
        if (q < s.size() && s[q] == '\r') ++q;
        if (q >= s.size() || s[q] != '\n') { bad = true; return false; }
        ++q;
        if (s.size() - q < n) { bad = true; return false; }   // the literal is cut off
        const size_t take = static_cast<size_t>(std::min<uint64_t>(n, cap));
        *out = std::string(s.substr(q, take));
        if (take < n && truncated) *truncated = true;
        p = q + static_cast<size_t>(n);
        last_was_literal = true;
        return true;
    }

    // atom, quoted string or literal
    bool astring(std::string* out, size_t cap = kMaxImapLine, bool* truncated = nullptr) {
        last_was_literal = false;
        if (peek() == '"') return quoted(out);
        if (peek() == '{') return literal(out, cap, truncated);
        return atom(out);
    }

    // NIL, quoted string or literal; NIL gives an empty string and *is_nil
    bool nstring(std::string* out, bool* is_nil, size_t cap, bool* truncated) {
        last_was_literal = false;
        if (starts_with_ci("NIL") && (p + 3 >= s.size() || s[p + 3] == ' ' || s[p + 3] == ')' || s[p + 3] == '\r' || s[p + 3] == '\n')) {
            p += 3;
            out->clear();
            if (is_nil) *is_nil = true;
            return true;
        }
        if (is_nil) *is_nil = false;
        if (peek() == '"') return quoted(out);
        if (peek() == '{') return literal(out, cap, truncated);
        return atom(out);
    }

    // Captures a balanced "( ... )" list as text, skipping quoted strings and literals; nesting is limited.
    bool balanced_list(std::string* out) {
        if (peek() != '(') return false;
        const size_t start = p;
        int depth = 0;
        size_t q = p;
        while (q < s.size()) {
            const char c = s[q];
            if (c == '(') {
                if (++depth > kMaxImapNesting) { bad = true; return false; }
                ++q;
            } else if (c == ')') {
                --depth;
                ++q;
                if (depth == 0) {
                    *out = std::string(s.substr(start, q - start));
                    p = q;
                    return true;
                }
            } else if (c == '"') {
                Reader sub{s, q};
                std::string ignored;
                if (!sub.quoted(&ignored)) { bad = true; return false; }
                q = sub.p;
            } else if (c == '{') {
                Reader sub{s, q};
                std::string ignored;
                if (!sub.literal(&ignored, 0, nullptr)) { bad = true; return false; }
                q = sub.p;
            } else if (c == '\r' || c == '\n') {
                // a line break outside a literal ends the list: it is cut off
                bad = true;
                return false;
            } else {
                ++q;
            }
        }
        bad = true;
        return false;
    }
};

bool is_untagged(const Reader& r) { return r.s.size() - r.p >= 2 && r.s[r.p] == '*' && r.s[r.p + 1] == ' '; }

// Reads the keyword of an untagged response: "* LIST ...", "* 3 FETCH ..." (number first), "* OK [CAPABILITY ...]".
// On return the cursor is after the keyword. `number` is set for "* n KEYWORD".
bool untagged_keyword(Reader& r, std::string* keyword, uint64_t* number, bool* has_number) {
    r.p += 2;
    r.skip_spaces();
    *has_number = false;
    uint64_t n = 0;
    const size_t before = r.p;
    if (r.number(&n)) {
        *has_number = true;
        *number = n;
        r.skip_spaces();
    } else {
        r.p = before;
    }
    return r.atom(keyword);
}

int month_number(std::string_view m) {
    static const char* const names[] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
    if (m.size() != 3) return 0;
    const std::string u = upper(m);
    for (int i = 0; i < 12; ++i)
        if (u == names[i]) return i + 1;
    return 0;
}

// "05-Oct-2026 10:00:01 +0000" (also "5-Oct-2026")
bool parse_internaldate(const std::string& v, int64_t* epoch) {
    int day = 0, year = 0, hh = 0, mm = 0, ss = 0;
    char mon[4] = {0, 0, 0, 0};
    char sign = '+';
    int zone = 0;
    if (std::sscanf(v.c_str(), " %d-%3[A-Za-z]-%d %d:%d:%d %c%d", &day, mon, &year, &hh, &mm, &ss, &sign, &zone) != 8) return false;
    const int month = month_number(mon);
    if (!month || day < 1 || day > 31 || hh > 23 || mm > 59 || ss > 60 || year < 1970 || year > 2100) return false;
    const int off = (zone / 100) * 3600 + (zone % 100) * 60;
    *epoch = epoch_from_civil(year, month, day, hh, mm, ss) - (sign == '-' ? -off : off);
    return true;
}

bool parse_flag_list(Reader& r, std::vector<std::string>* flags) {
    if (r.peek() != '(') return false;
    ++r.p;
    for (;;) {
        r.skip_spaces();
        if (r.peek() == ')') {
            ++r.p;
            return true;
        }
        std::string f;
        // flag atoms may start with a backslash and may be "\*"
        const size_t start = r.p;
        while (r.p < r.s.size() && r.s[r.p] != ' ' && r.s[r.p] != ')' && r.s[r.p] != '\r' && r.s[r.p] != '\n') ++r.p;
        if (r.p == start) {
            r.bad = true;
            return false;
        }
        f = std::string(r.s.substr(start, r.p - start));
        flags->push_back(std::move(f));
        if (flags->size() > 64) {
            r.bad = true;
            return false;
        }
    }
}

constexpr size_t kLiteralCap = 2u << 20;   // 2 MiB: the largest part the app asks for in one command

// Parses the item list of one FETCH response, cursor after "(" . Returns true when a complete item was read.
bool parse_fetch_items(Reader& r, FetchItem* item) {
    for (;;) {
        r.skip_spaces();
        if (r.eof()) return r.last_was_literal;            // libcurl omits ")" after a literal at the very end
        if (r.peek() == ')') {
            ++r.p;
            return true;
        }
        if (r.at_eol()) return r.last_was_literal;         // ... or before the next response
        std::string name;
        if (!r.atom(&name)) return false;
        const std::string U = upper(name);
        r.skip_spaces();
        r.last_was_literal = false;
        if (U == "UID") {
            uint64_t v;
            if (!r.number(&v) || v > UINT32_MAX) return false;
            item->uid = static_cast<uint32_t>(v);
        } else if (U == "FLAGS") {
            std::vector<std::string> flags;
            if (!parse_flag_list(r, &flags)) return false;
            item->flags_bits = flags_to_bits(flags);
        } else if (U == "INTERNALDATE") {
            std::string v;
            if (!r.quoted(&v)) return false;
            int64_t t;
            if (parse_internaldate(v, &t)) item->internal_date = t;
        } else if (U == "RFC822.SIZE") {
            uint64_t v;
            if (!r.number(&v)) return false;
            item->size = v;
        } else if (U == "MODSEQ") {
            if (r.peek() != '(') return false;
            ++r.p;
            r.skip_spaces();
            uint64_t v;
            if (!r.number(&v)) return false;
            r.skip_spaces();
            if (r.peek() != ')') return false;
            ++r.p;
            item->modseq = v;
        } else if (U == "BODYSTRUCTURE" || U == "BODY") {
            std::string text;
            const size_t save = r.p;
            if (!r.balanced_list(&text)) {
                // Cut off at the end of the input: the item is incomplete. Otherwise the structure is garbled or too deeply
                // nested: it is skipped (with the rest of the line) and the item keeps what it has (the header must not be
                // lost because of a structure; the caller then reads the message in another way).
                if (r.p >= r.s.size() || r.s.size() - save < 2 || (r.bad && save + 1 >= r.s.size())) return false;
                bool at_end = false;
                for (size_t k = save; k < r.s.size(); ++k)
                    if (r.s[k] == '\n') { at_end = true; break; }
                if (!at_end) return false;
                r.bad = false;
                r.p = save;
                r.skip_line();
                item->structure_unreadable = true;
                return true;
            }
            item->bodystructure_text = std::move(text);
        } else if (U.compare(0, 5, "BODY[") == 0 || U.compare(0, 10, "BODY.PEEK[") == 0) {
            const size_t lb = name.find('['), rb = name.rfind(']');
            std::string key = rb == std::string::npos ? std::string() : name.substr(lb + 1, rb - lb - 1);
            std::string data;
            bool is_nil = false, truncated = false;
            if (!r.nstring(&data, &is_nil, kLiteralCap, &truncated)) return false;
            if (truncated) item->truncated = true;
            const std::string UK = upper(key);
            if (UK.compare(0, 6, "HEADER") == 0) item->header_bytes = data;
            item->sections[key] = std::move(data);
        } else {
            // an item this subset does not use: skip its value
            if (r.peek() == '(') {
                std::string ignored;
                if (!r.balanced_list(&ignored)) return false;
            } else {
                std::string ignored;
                bool trunc = false;
                if (!r.nstring(&ignored, nullptr, 0, &trunc)) return false;
            }
        }
        if (r.bad) return false;
    }
}

}  // namespace

// ---------------------------------------------------------------------------------------------- FETCH

Parsed<std::vector<FetchItem>> parse_fetch(const ImapRaw& raw) {
    Parsed<std::vector<FetchItem>> out;
    if (raw.data.size() > kMaxImapInput) {
        out.ok = false;
        return out;
    }
    Reader r{raw.data};
    while (!r.eof()) {
        if (r.at_eol()) { r.skip_eol(); continue; }
        if (!is_untagged(r)) { r.skip_line(); continue; }
        const size_t line_start = r.p;
        std::string kw;
        uint64_t seq = 0;
        bool has_seq = false;
        if (!untagged_keyword(r, &kw, &seq, &has_seq)) { r.bad = false; r.p = line_start; r.skip_line(); continue; }
        if (!has_seq || upper(kw) != "FETCH") { r.p = line_start; r.skip_line(); continue; }
        r.skip_spaces();
        if (r.peek() != '(') { out.ok = false; r.skip_line(); continue; }
        ++r.p;
        FetchItem item;
        item.seq = static_cast<uint32_t>(std::min<uint64_t>(seq, UINT32_MAX));
        const bool complete = parse_fetch_items(r, &item);
        if (!complete || r.bad) {
            out.ok = false;
            if (r.bad || r.eof()) break;    // cut off or hostile: nothing after this point can be trusted
            r.skip_line();
            continue;
        }
        if (item.uid != 0) out.value.push_back(std::move(item));
        r.skip_eol();
    }
    if (r.bad) out.ok = false;
    return out;
}

// ---------------------------------------------------------------------------------------------- LIST

Parsed<std::vector<FolderListItem>> parse_list(const ImapRaw& raw) {
    Parsed<std::vector<FolderListItem>> out;
    if (raw.data.size() > kMaxImapInput) {
        out.ok = false;
        return out;
    }
    Reader r{raw.data};
    while (!r.eof()) {
        if (r.at_eol()) { r.skip_eol(); continue; }
        if (!is_untagged(r)) { r.skip_line(); continue; }
        const size_t line_start = r.p;
        std::string kw;
        uint64_t n = 0;
        bool has_n = false;
        if (!untagged_keyword(r, &kw, &n, &has_n) || has_n || upper(kw) != "LIST") {
            r.bad = false;
            r.p = line_start;
            r.skip_line();
            continue;
        }
        FolderListItem item;
        r.skip_spaces();
        std::vector<std::string> attrs;
        if (!parse_flag_list(r, &attrs)) { out.ok = false; break; }
        for (const std::string& a : attrs) item.attributes.insert(a);
        r.skip_spaces();
        std::string delim;
        bool nil = false;
        if (!r.nstring(&delim, &nil, 8, nullptr)) { out.ok = false; break; }
        item.delimiter = nil ? std::string() : delim;
        r.skip_spaces();
        std::string name;
        if (!r.astring(&name, 4096)) { out.ok = false; break; }
        item.raw_name = name;
        item.name = decode_mutf7(name);
        out.value.push_back(std::move(item));
        r.skip_spaces();
        if (!r.at_eol() && !r.eof()) r.skip_line();
    }
    if (r.bad) out.ok = false;
    return out;
}

// ---------------------------------------------------------------------------------------------- STATUS

Parsed<StatusInfo> parse_status(const ImapRaw& raw) {
    Parsed<StatusInfo> out;
    out.ok = false;   // until a usable STATUS line has been read
    if (raw.data.size() > kMaxImapInput) return out;
    Reader r{raw.data};
    while (!r.eof()) {
        if (r.at_eol()) { r.skip_eol(); continue; }
        if (!is_untagged(r)) { r.skip_line(); continue; }
        const size_t line_start = r.p;
        std::string kw;
        uint64_t n = 0;
        bool has_n = false;
        if (!untagged_keyword(r, &kw, &n, &has_n) || has_n || upper(kw) != "STATUS") {
            r.bad = false;
            r.p = line_start;
            r.skip_line();
            continue;
        }
        r.skip_spaces();
        StatusInfo info;
        if (!r.astring(&info.mailbox, 4096)) return out;
        r.skip_spaces();
        if (r.peek() != '(') return out;
        ++r.p;
        bool all_ok = true;
        for (;;) {
            r.skip_spaces();
            if (r.peek() == ')') break;
            std::string item;
            uint64_t v = 0;
            if (!r.atom(&item)) { all_ok = false; break; }
            r.skip_spaces();
            if (!r.number(&v)) { all_ok = false; break; }
            const std::string U = upper(item);
            if (U == "UIDNEXT" || U == "UIDVALIDITY" || U == "MESSAGES" || U == "UNSEEN") {
                if (v > UINT32_MAX) { all_ok = false; continue; }
                if (U == "UIDNEXT") { info.uidnext = static_cast<uint32_t>(v); info.has_uidnext = true; }
                else if (U == "UIDVALIDITY") { info.uidvalidity = static_cast<uint32_t>(v); info.has_uidvalidity = true; }
                else if (U == "MESSAGES") { info.messages = static_cast<uint32_t>(v); info.has_messages = true; }
                else { info.unseen = static_cast<uint32_t>(v); info.has_unseen = true; }
            } else if (U == "HIGHESTMODSEQ") {
                info.highestmodseq = v;
                info.has_highestmodseq = true;
            }
        }
        out.value = std::move(info);
        out.ok = all_ok;
        return out;
    }
    return out;
}

// ---------------------------------------------------------------------------------------------- SEARCH

Parsed<std::vector<uint32_t>> parse_search(const ImapRaw& raw) {
    Parsed<std::vector<uint32_t>> out;
    out.ok = false;   // until a SEARCH line has been read
    if (raw.data.size() > kMaxImapInput) return out;
    Reader r{raw.data};
    bool seen_line = false;
    bool clean = true;
    while (!r.eof()) {
        if (r.at_eol()) { r.skip_eol(); continue; }
        if (!is_untagged(r)) { r.skip_line(); continue; }
        const size_t line_start = r.p;
        std::string kw;
        uint64_t n = 0;
        bool has_n = false;
        if (!untagged_keyword(r, &kw, &n, &has_n) || has_n || upper(kw) != "SEARCH") {
            r.bad = false;
            r.p = line_start;
            r.skip_line();
            continue;
        }
        seen_line = true;
        const size_t content_start = r.p;
        size_t eol = raw.data.find('\n', r.p);
        if (eol == std::string::npos) eol = raw.data.size();
        if (eol - content_start > kMaxImapLine) { clean = false; r.skip_line(); continue; }   // over-long line: the item is dropped
        for (;;) {
            r.skip_spaces();
            if (r.eof() || r.at_eol()) break;
            uint64_t v;
            if (!r.number(&v) || v > UINT32_MAX) { clean = false; break; }
            if (r.p < raw.data.size() && raw.data[r.p] != ' ' && raw.data[r.p] != '\r' && raw.data[r.p] != '\n') { clean = false; break; }
            out.value.push_back(static_cast<uint32_t>(v));
        }
        r.skip_line();
    }
    out.ok = seen_line && clean;
    return out;
}

// ---------------------------------------------------------------------------------------------- CAPABILITY

std::set<std::string> parse_capability(const ImapRaw& raw) {
    std::set<std::string> caps;
    if (raw.data.size() > kMaxImapInput) return caps;
    std::string_view s = raw.data;
    size_t pos = 0;
    while (pos < s.size()) {
        size_t eol = s.find('\n', pos);
        const size_t end = eol == std::string_view::npos ? s.size() : eol;
        std::string_view line = s.substr(pos, end - pos);
        pos = eol == std::string_view::npos ? s.size() : eol + 1;
        if (line.size() > kMaxImapLine) continue;
        size_t at = std::string_view::npos;
        if (line.size() >= 2 && line[0] == '*' && line[1] == ' ') {
            std::string u = upper(line);
            at = u.find("CAPABILITY");
            if (at != std::string::npos && at > 2 && u.compare(2, 10, "CAPABILITY") != 0 && u.find("[CAPABILITY") == std::string::npos) at = std::string_view::npos;
        }
        if (at == std::string_view::npos) continue;
        std::string token;
        for (size_t i = at + 10; i <= line.size(); ++i) {
            const char c = i < line.size() ? line[i] : ' ';
            if (c == ' ' || c == '\r' || c == ']' || c == '\t') {
                if (!token.empty()) {
                    for (char& t : token) t = low(t);
                    caps.insert(token);
                    token.clear();
                }
                if (c == ']') break;
            } else {
                token += c;
            }
        }
    }
    return caps;
}

// ---------------------------------------------------------------------------------------------- flags

uint32_t flags_to_bits(const std::vector<std::string>& flags) {
    uint32_t bits = 0;
    for (const std::string& f : flags) {
        const std::string u = upper(f);
        if (u == "\\SEEN") bits |= kFlagSeen;
        else if (u == "\\ANSWERED") bits |= kFlagAnswered;
        else if (u == "\\FLAGGED") bits |= kFlagFlagged;
        else if (u == "\\DELETED") bits |= kFlagDeleted;
        else if (u == "\\DRAFT") bits |= kFlagDraft;
        else if (u == "$FORWARDED") bits |= kFlagForwarded;
    }
    return bits;
}

std::string bits_to_flag_list(uint32_t bits) {
    static const struct { uint32_t bit; const char* name; } kFlags[] = {
        {kFlagSeen, "\\Seen"}, {kFlagAnswered, "\\Answered"}, {kFlagFlagged, "\\Flagged"},
        {kFlagDeleted, "\\Deleted"}, {kFlagDraft, "\\Draft"}, {kFlagForwarded, "$Forwarded"}};
    std::string r = "(";
    bool first = true;
    for (const auto& f : kFlags)
        if (bits & f.bit) {
            r += (first ? "" : " ");
            r += f.name;
            first = false;
        }
    return r + ")";
}

// ---------------------------------------------------------------------------------------------- modified UTF-7

namespace {

int b64_value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == ',') return 63;
    return -1;
}

void append_utf8(std::string* out, uint32_t cp) {
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

// Decodes the base64 run of one &...- section into UTF-8; false if it is not valid modified base64 / UTF-16.
bool decode_run(std::string_view b64, std::string* out) {
    std::vector<uint16_t> units;
    uint32_t acc = 0;
    int bits = 0;
    std::vector<uint8_t> bytes;
    for (char c : b64) {
        const int v = b64_value(c);
        if (v < 0) return false;
        acc = (acc << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            bytes.push_back(static_cast<uint8_t>((acc >> bits) & 0xFF));
        }
    }
    if (bits >= 6 || (acc & ((1u << bits) - 1)) != 0) return false;   // leftover bits must be zero padding
    if (bytes.empty() || bytes.size() % 2 != 0) return false;
    for (size_t i = 0; i < bytes.size(); i += 2) units.push_back(static_cast<uint16_t>((bytes[i] << 8) | bytes[i + 1]));
    std::string r;
    for (size_t i = 0; i < units.size(); ++i) {
        uint32_t u = units[i];
        if (u >= 0xD800 && u <= 0xDBFF) {
            if (i + 1 >= units.size() || units[i + 1] < 0xDC00 || units[i + 1] > 0xDFFF) return false;
            u = 0x10000 + ((u - 0xD800) << 10) + (units[i + 1] - 0xDC00);
            ++i;
        } else if (u >= 0xDC00 && u <= 0xDFFF) {
            return false;
        }
        append_utf8(&r, u);
    }
    *out += r;
    return true;
}

}  // namespace

std::string decode_mutf7(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size();) {
        if (s[i] != '&') {
            out += s[i++];
            continue;
        }
        const size_t dash = s.find('-', i + 1);
        if (dash == std::string::npos) {   // unterminated: keep the rest unchanged
            out.append(s, i, std::string::npos);
            break;
        }
        if (dash == i + 1) {   // "&-" is a literal '&'
            out += '&';
            i = dash + 1;
            continue;
        }
        std::string piece;
        if (decode_run(std::string_view(s).substr(i + 1, dash - i - 1), &piece)) out += piece;
        else out.append(s, i, dash - i + 1);   // invalid: keep the original text
        i = dash + 1;
    }
    return out;
}

std::string encode_mutf7(const std::string& utf8) {
    static const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+,";
    std::string out;
    std::vector<uint16_t> run;
    auto flush = [&] {
        if (run.empty()) return;
        std::vector<uint8_t> bytes;
        for (uint16_t u : run) {
            bytes.push_back(static_cast<uint8_t>(u >> 8));
            bytes.push_back(static_cast<uint8_t>(u & 0xFF));
        }
        out += '&';
        uint32_t acc = 0;
        int bits = 0;
        for (uint8_t b : bytes) {
            acc = (acc << 8) | b;
            bits += 8;
            while (bits >= 6) {
                bits -= 6;
                out += kB64[(acc >> bits) & 0x3F];
            }
        }
        if (bits > 0) out += kB64[(acc << (6 - bits)) & 0x3F];
        out += '-';
        run.clear();
    };
    for (size_t i = 0; i < utf8.size();) {
        const unsigned char c = static_cast<unsigned char>(utf8[i]);
        uint32_t cp;
        size_t len;
        if (c < 0x80) { cp = c; len = 1; }
        else if ((c & 0xE0) == 0xC0 && i + 1 < utf8.size()) { cp = ((c & 0x1Fu) << 6) | (utf8[i + 1] & 0x3F); len = 2; }
        else if ((c & 0xF0) == 0xE0 && i + 2 < utf8.size()) { cp = ((c & 0x0Fu) << 12) | ((utf8[i + 1] & 0x3Fu) << 6) | (utf8[i + 2] & 0x3F); len = 3; }
        else if ((c & 0xF8) == 0xF0 && i + 3 < utf8.size()) { cp = ((c & 0x07u) << 18) | ((utf8[i + 1] & 0x3Fu) << 12) | ((utf8[i + 2] & 0x3Fu) << 6) | (utf8[i + 3] & 0x3F); len = 4; }
        else { cp = 0xFFFD; len = 1; }
        i += len;
        if (cp >= 0x20 && cp <= 0x7E) {
            flush();
            out += static_cast<char>(cp);
            if (cp == '&') out += '-';
        } else if (cp >= 0x10000) {
            cp -= 0x10000;
            run.push_back(static_cast<uint16_t>(0xD800 + (cp >> 10)));
            run.push_back(static_cast<uint16_t>(0xDC00 + (cp & 0x3FF)));
        } else {
            run.push_back(static_cast<uint16_t>(cp));
        }
    }
    flush();
    return out;
}

}  // namespace pbo

namespace pbo {

namespace {

bool shape_keyword(const std::string& upper_word) {
    static const char* const kKeep[] = {
        "FETCH", "UID", "FLAGS", "INTERNALDATE", "RFC822.SIZE", "RFC822", "BODY", "BODY.PEEK", "BODYSTRUCTURE", "ENVELOPE", "MODSEQ",
        "HEADER", "HEADER.FIELDS", "TEXT", "FROM", "TO", "CC", "SUBJECT", "DATE", "MESSAGE-ID", "IN-REPLY-TO", "REFERENCES",
        "SEARCH", "STATUS", "LIST", "LSUB", "CAPABILITY", "EXISTS", "RECENT", "EXPUNGE", "OK", "NO", "BAD", "BYE", "PREAUTH",
        "NIL", "MESSAGES", "UIDNEXT", "UIDVALIDITY", "UNSEEN", "HIGHESTMODSEQ", "READ-WRITE", "READ-ONLY", "UIDPLUS", "CONDSTORE",
        "IMAP4REV1", "MOVE", "LITERAL+", "STARTTLS", "SPECIAL-USE"};
    for (const char* k : kKeep)
        if (upper_word == k) return true;
    return false;
}

// Words that may stay when they stand in quotes (media types, disposition and parameter names, encodings): none of them can
// identify a person or a message.
bool shape_quoted_keyword(const std::string& upper_word) {
    static const char* const kKeep[] = {
        "TEXT", "PLAIN", "HTML", "MULTIPART", "MIXED", "ALTERNATIVE", "RELATED", "SIGNED", "REPORT", "APPLICATION", "PDF", "OCTET-STREAM", "ZIP",
        "IMAGE", "JPEG", "PNG", "GIF", "AUDIO", "VIDEO", "MESSAGE", "RFC822", "DELIVERY-STATUS", "CALENDAR", "PKCS7-SIGNATURE", "PGP-SIGNATURE",
        "ATTACHMENT", "INLINE", "FILENAME", "NAME", "CHARSET", "BOUNDARY", "FORMAT", "FLOWED", "DELSP", "METHOD", "UTF-8", "US-ASCII",
        "ISO-8859-1", "ISO-8859-15", "WINDOWS-1252", "BASE64", "QUOTED-PRINTABLE", "7BIT", "8BIT", "BINARY"};
    for (const char* k : kKeep)
        if (upper_word == k) return true;
    return false;
}

bool shape_flag(const std::string& w) {
    static const char* const kFlags[] = {"\\Seen", "\\Answered", "\\Flagged", "\\Deleted", "\\Draft", "\\Recent", "\\HasNoChildren", "\\HasChildren",
                                         "\\Noselect", "\\Noinferiors", "\\Sent", "\\Drafts", "\\Trash", "\\Junk", "\\All", "\\Archive", "\\Marked",
                                         "\\Unmarked", "$Forwarded", "$MDNSent", "$Junk", "$NotJunk"};
    for (const char* f : kFlags)
        if (w == f) return true;
    return false;
}

}  // namespace

std::string describe_shape(const ImapRaw& raw, size_t max_chars) {
    const std::string& s = raw.data;
    std::string out;
    size_t i = 0;
    auto room = [&] { return out.size() < max_chars; };
    while (i < s.size() && room()) {
        const char c = s[i];
        if (c == '\r') { out += "\\r"; ++i; }
        else if (c == '\n') { out += "\\n"; ++i; }
        else if (c == '"') {   // a quoted string: only that there was one
            size_t j = i + 1;
            while (j < s.size() && s[j] != '"' && s[j] != '\r' && s[j] != '\n') j += (s[j] == '\\' && j + 1 < s.size()) ? 2 : 1;
            const std::string inner = upper(std::string_view(s).substr(i + 1, j - i - 1));
            if (j < s.size() && s[j] == '"' && shape_quoted_keyword(inner)) out += "\"" + inner + "\"";
            else out += "q";
            i = (j < s.size() && s[j] == '"') ? j + 1 : j;
        } else if (c == '{') {   // a literal: declared size, available size, content dropped
            size_t j = i + 1;
            uint64_t decl = 0;
            bool digits = false;
            while (j < s.size() && std::isdigit(static_cast<unsigned char>(s[j])) && decl < (1ull << 40)) { decl = decl * 10 + static_cast<uint64_t>(s[j] - '0'); ++j; digits = true; }
            if (j < s.size() && s[j] == '+') ++j;
            if (digits && j < s.size() && s[j] == '}') {
                ++j;
                if (j < s.size() && s[j] == '\r') ++j;
                if (j < s.size() && s[j] == '\n') ++j;
                const uint64_t avail = s.size() - j;
                out += "{L" + std::to_string(decl) + "/" + std::to_string(avail) + "}";
                i = j + static_cast<size_t>(std::min<uint64_t>(decl, avail));
            } else {
                out += "{";
                ++i;
            }
        } else if (c == '(' || c == ')' || c == '[' || c == ']' || c == '<' || c == '>' || c == ' ' || c == '*') {
            out += c;
            ++i;
        } else {   // a word: keep known keywords and standard flags, otherwise N (number), w (word)
            size_t j = i;
            while (j < s.size() && s[j] != ' ' && s[j] != '(' && s[j] != ')' && s[j] != '[' && s[j] != ']' && s[j] != '<' && s[j] != '>' &&
                   s[j] != '"' && s[j] != '{' && s[j] != '\r' && s[j] != '\n')
                ++j;
            const std::string word = s.substr(i, j - i);
            std::string u = upper(word);
            bool all_digits = !word.empty();
            for (char ch : word)
                if (!std::isdigit(static_cast<unsigned char>(ch))) all_digits = false;
            if (all_digits) out += "N";
            else if (shape_keyword(u)) out += u;
            else if (shape_flag(word)) out += word;
            else out += "w";
            i = j > i ? j : i + 1;
        }
    }
    if (out.size() > max_chars) out.resize(max_chars);
    return out;
}

}  // namespace pbo
