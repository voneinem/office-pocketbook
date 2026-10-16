#include "pbo/compose.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <set>

#include "pbo/account_input.h"
#include "pbo/civil.h"
#include "pbo/mime.h"

namespace pbo {

namespace {

constexpr size_t kMaxBody = 1u << 20;

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

bool blank(const std::string& s) { return trim(s).empty(); }

// control characters (line breaks included) become blanks: no header can be injected through a name or a subject
std::string one_line(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) out += (c < 0x20 || c == 0x7f) ? ' ' : static_cast<char>(c);
    return trim(out);
}

bool printable_ascii(const std::string& s) {
    for (unsigned char c : s)
        if (c < 0x20 || c > 0x7e) return false;
    return true;
}

std::string base64(const std::string& in) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        const unsigned v = (unsigned char)in[i] << 16 | (unsigned char)in[i + 1] << 8 | (unsigned char)in[i + 2];
        out += t[v >> 18]; out += t[(v >> 12) & 63]; out += t[(v >> 6) & 63]; out += t[v & 63];
    }
    if (i + 1 == in.size()) {
        const unsigned v = (unsigned char)in[i] << 16;
        out += t[v >> 18]; out += t[(v >> 12) & 63]; out += "==";
    } else if (i + 2 == in.size()) {
        const unsigned v = (unsigned char)in[i] << 16 | (unsigned char)in[i + 1] << 8;
        out += t[v >> 18]; out += t[(v >> 12) & 63]; out += t[(v >> 6) & 63]; out += '=';
    }
    return out;
}

// RFC 2047 encoded words of at most 75 characters each, cut between UTF-8 characters
std::vector<std::string> encoded_words(const std::string& utf8) {
    std::vector<std::string> words;
    size_t pos = 0;
    while (pos < utf8.size()) {
        size_t n = std::min<size_t>(42, utf8.size() - pos);
        if (pos + n < utf8.size())
            while (n > 1 && (static_cast<unsigned char>(utf8[pos + n]) & 0xC0) == 0x80) --n;
        words.push_back("=?UTF-8?B?" + base64(utf8.substr(pos, n)) + "?=");
        pos += n;
    }
    return words;
}

// "Name: tok tok tok" folded at blanks between tokens so that no line is longer than 76 characters
std::string fold_header(const std::string& name, const std::vector<std::string>& tokens, const char* separator = " ") {
    std::string out = name + ":";
    size_t line = out.size();
    bool first = true;
    for (const std::string& t : tokens) {
        const size_t sep = first ? 1 : std::string(separator).size();
        if (!first && line + sep + t.size() > 76) {
            out += "\r\n ";
            out += t;
            line = 1 + t.size();
        } else {
            out += first ? " " : separator;
            out += t;
            line += sep + t.size();
        }
        first = false;
    }
    return out + "\r\n";
}

std::vector<std::string> words_of(const std::string& s) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos < s.size()) {
        while (pos < s.size() && s[pos] == ' ') ++pos;
        const size_t e = s.find(' ', pos);
        if (pos >= s.size()) break;
        out.push_back(s.substr(pos, e == std::string::npos ? std::string::npos : e - pos));
        pos = e == std::string::npos ? s.size() : e;
    }
    return out;
}

std::string subject_header(const std::string& subject) {
    const std::string s = one_line(subject);
    if (printable_ascii(s)) return fold_header("Subject", words_of(s));
    return fold_header("Subject", encoded_words(s));
}

// a display name as a phrase: plain, quoted, or as encoded words
std::vector<std::string> phrase_tokens(const std::string& raw_name) {
    const std::string name = one_line(raw_name);
    if (name.empty()) return {};
    if (!printable_ascii(name)) return encoded_words(name);
    bool special = false;
    for (char c : name)
        if (std::string("()<>[]:;@\\,.\"").find(c) != std::string::npos) special = true;
    if (!special) return words_of(name);
    std::string q = "\"";
    for (char c : name) {
        if (c == '"' || c == '\\') q += '\\';
        q += c;
    }
    return {q + "\""};
}

struct Entry {
    std::string name, addr;
};

// names for the validated addresses (case-insensitive match); an address without a known name stays bare
std::vector<Entry> entries_for(const std::string& typed, const std::vector<std::string>& addresses) {
    std::vector<Entry> out;
    const std::vector<MailAddress> parsed = parse_mailboxes(typed);
    for (const std::string& a : addresses) {
        Entry e;
        e.addr = a;
        for (const MailAddress& m : parsed)
            if (lower(m.address) == lower(a)) {
                e.name = m.name;
                break;
            }
        out.push_back(e);
    }
    return out;
}

std::string address_header(const std::string& header_name, const std::vector<Entry>& entries) {
    std::vector<std::string> tokens;
    for (size_t i = 0; i < entries.size(); ++i) {
        const Entry& e = entries[i];
        const std::vector<std::string> phrase = phrase_tokens(e.name);
        std::vector<std::string> parts = phrase;
        parts.push_back(phrase.empty() ? e.addr : "<" + e.addr + ">");
        const bool last = i + 1 == entries.size();
        parts.back() += last ? "" : ",";
        for (std::string& p : parts) tokens.push_back(std::move(p));
    }
    return fold_header(header_name, tokens);
}

std::string rfc_date(int64_t epoch) {
    static const char* days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char* months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    const LocalDateTime t = civil::from_naive_epoch(epoch);
    char b[64];
    snprintf(b, sizeof b, "%s, %02d %s %04d %02d:%02d:%02d +0000", days[civil::weekday_from_days(civil::floor_div(epoch, 86400))], t.day, months[t.month - 1], t.year, t.hour, t.minute, t.second);
    return b;
}

bool valid_message_id(const std::string& id) {
    if (id.size() < 5 || id.size() > 200 || id.front() != '<' || id.back() != '>') return false;
    const size_t at = id.find('@');
    if (at == std::string::npos || at == 1 || at + 2 > id.size() - 1) return false;
    for (size_t i = 1; i + 1 < id.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(id[i]);
        if (c <= 0x20 || c >= 0x7f || c == '<' || c == '>') return false;
        if (c == '@' && i != at) return false;
    }
    return true;
}

// "<a@b> <c@d>": only well-formed ids are kept
std::string clean_id_list(const std::string& s) {
    std::string out;
    size_t pos = 0;
    while ((pos = s.find('<', pos)) != std::string::npos) {
        const size_t e = s.find('>', pos);
        if (e == std::string::npos) break;
        const std::string id = s.substr(pos, e - pos + 1);
        if (valid_message_id(id)) out += (out.empty() ? "" : " ") + id;
        pos = e + 1;
    }
    return out;
}

std::string quoted_printable_body(const std::string& text) {
    // clean text: "\n" line ends, no control characters except tab
    std::string clean;
    clean.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(text[i]);
        if (c == '\r') {
            if (i + 1 < text.size() && text[i + 1] == '\n') ++i;
            clean += '\n';
        } else if (c == '\n' || c == '\t' || (c >= 0x20 && c != 0x7f)) {
            clean += static_cast<char>(c);
        }
    }
    if (!clean.empty() && clean.back() != '\n') clean += '\n';
    std::string out;
    std::string line;   // the encoded text of the current line
    auto flush_line = [&](bool soft) {
        out += line;
        out += soft ? "=\r\n" : "\r\n";
        line.clear();
    };
    size_t i = 0;
    while (i < clean.size()) {
        // one source line
        const size_t e = clean.find('\n', i);
        const std::string src = clean.substr(i, e - i);
        i = e + 1;
        for (size_t k = 0; k < src.size(); ++k) {
            const unsigned char c = static_cast<unsigned char>(src[k]);
            const bool trailing_blank = (c == ' ' || c == '\t') && k + 1 == src.size();
            std::string token;
            if (c == '=' || c >= 0x80 || trailing_blank) {
                char b[4];
                snprintf(b, sizeof b, "=%02X", c);
                token = b;
            } else {
                token = std::string(1, static_cast<char>(c));
            }
            if (line.size() + token.size() > 75) flush_line(true);
            line += token;
        }
        flush_line(false);
    }
    return out;
}

}  // namespace

namespace {

Result<BuiltMessage> build_internal(const ComposeDraft& d, bool lenient) {
    Result<BuiltMessage> r;
    auto fail = [&](const char* key, const char* detail) {
        r.error = make_error(ErrorClass::Unsupported, key, detail);
        r.value = BuiltMessage();
        return r;
    };
    if (!is_valid_address(d.from_addr)) return fail("from_invalid", "from");
    if (!valid_message_id(d.message_id)) return fail("invalid_input", "message_id");
    if (d.date_utc <= 0) return fail("invalid_input", "date");
    if (d.body.size() > kMaxBody && !lenient) return fail("too_large", "body");

    struct Field {
        const char* name;
        const std::string* typed;
    };
    std::vector<std::string> to, cc, bcc;
    for (const Field& f : {Field{"to", &d.to}, Field{"cc", &d.cc}, Field{"bcc", &d.bcc}}) {
        if (blank(*f.typed)) continue;
        std::vector<std::string> list = parse_address_list(*f.typed);
        if (list.empty()) {
            if (lenient) continue;   // a draft may hold an address that is not finished yet
            return fail("recipient_invalid", f.name);
        }
        (std::string(f.name) == "to" ? to : std::string(f.name) == "cc" ? cc : bcc) = std::move(list);
    }
    if (to.empty() && cc.empty() && bcc.empty() && !lenient) return fail("no_recipient", "to");

    BuiltMessage m;
    m.envelope_from = d.from_addr;
    std::set<std::string> seen;
    for (const auto* list : {&to, &cc, &bcc})
        for (const std::string& a : *list)
            if (seen.insert(lower(a)).second) m.recipients.push_back(a);

    std::string head;
    head += "Date: " + rfc_date(d.date_utc) + "\r\n";
    head += address_header("From", {Entry{d.from_name, d.from_addr}});
    // a recipient that is also in an earlier list is not repeated in the later header
    auto unique_entries = [&](const std::string& typed, const std::vector<std::string>& list, std::set<std::string>* used) {
        std::vector<std::string> keep;
        for (const std::string& a : list)
            if (used->insert(lower(a)).second) keep.push_back(a);
        return entries_for(typed, keep);
    };
    std::set<std::string> used;
    const std::vector<Entry> to_entries = unique_entries(d.to, to, &used);
    const std::vector<Entry> cc_entries = unique_entries(d.cc, cc, &used);
    if (!to_entries.empty()) head += address_header("To", to_entries);
    if (!cc_entries.empty()) head += address_header("Cc", cc_entries);
    head += subject_header(d.subject);
    head += "Message-ID: " + d.message_id + "\r\n";
    const std::string irt = clean_id_list(d.in_reply_to);
    if (!irt.empty()) head += fold_header("In-Reply-To", words_of(irt));
    const std::string refs = clean_id_list(d.references);
    if (!refs.empty()) head += fold_header("References", words_of(refs));
    head += "MIME-Version: 1.0\r\n";
    head += "Content-Type: text/plain; charset=UTF-8\r\n";
    head += "Content-Transfer-Encoding: quoted-printable\r\n";
    m.raw = head + "\r\n" + quoted_printable_body(lenient && d.body.size() > kMaxBody ? d.body.substr(0, kMaxBody) : d.body);
    r.value = std::move(m);
    return r;
}

}  // namespace

Result<BuiltMessage> build_message(const ComposeDraft& d) { return build_internal(d, false); }
Result<BuiltMessage> build_draft_message(const ComposeDraft& d) { return build_internal(d, true); }

std::string make_message_id(Random& random, const std::string& from_addr) {
    uint8_t b[16];
    random.bytes(b, sizeof b);
    std::string id = "<";
    char hex[3];
    for (uint8_t v : b) {
        snprintf(hex, sizeof hex, "%02x", v);
        id += hex;
    }
    std::string domain = "pboffice.invalid";
    const size_t at = from_addr.rfind('@');
    if (at != std::string::npos && is_valid_address(from_addr)) domain = lower(from_addr.substr(at + 1));
    return id + "@" + domain + ">";
}

namespace {

// true if the subject starts with one of the prefixes followed by an optional "[n]" and a colon
bool has_prefix(const std::string& subject, std::initializer_list<const char*> prefixes) {
    const std::string s = lower(trim(subject));
    for (const char* p : prefixes) {
        const std::string pre = p;
        if (s.compare(0, pre.size(), pre) != 0) continue;
        size_t i = pre.size();
        if (i < s.size() && s[i] == '[') {
            const size_t close = s.find(']', i);
            if (close == std::string::npos) continue;
            i = close + 1;
        }
        if (i < s.size() && s[i] == ':') return true;
    }
    return false;
}

}  // namespace

std::string reply_subject(const std::string& subject) {
    const std::string s = trim(subject);
    if (has_prefix(s, {"re", "aw", "antw", "sv"})) return s;
    return "Re: " + s;
}

std::string forward_subject(const std::string& subject) {
    const std::string s = trim(subject);
    if (has_prefix(s, {"fwd", "fw", "wg", "tr"})) return s;
    return "Fwd: " + s;
}

namespace {

std::string format_entry(const MailAddress& a) {
    if (a.name.empty()) return a.address;
    bool special = false;
    for (char c : a.name)
        if (std::string("()<>[]:;@\\,.\"").find(c) != std::string::npos) special = true;
    std::string name = a.name;
    if (special) {
        std::string q = "\"";
        for (char c : a.name) {
            if (c == '"' || c == '\\') q += '\\';
            q += c;
        }
        name = q + "\"";
    }
    return name + " <" + a.address + ">";
}

std::string join(const std::vector<MailAddress>& v) {
    std::string out;
    for (const MailAddress& a : v) out += (out.empty() ? "" : ", ") + format_entry(a);
    return out;
}

}  // namespace

ReplyTarget reply_recipients(const ReplySource& s, bool all) {
    const std::string own = lower(s.own_address);
    std::set<std::string> used;
    auto take = [&](const std::vector<MailAddress>& in, bool drop_own) {
        std::vector<MailAddress> out;
        for (const MailAddress& a : in) {
            const std::string key = lower(a.address);
            if (key.empty() || (drop_own && key == own)) continue;
            if (used.insert(key).second) out.push_back(a);
        }
        return out;
    };
    std::vector<MailAddress> first;
    if (!blank(s.reply_to)) first = parse_mailboxes(s.reply_to);
    else if (!s.from_addr.empty()) first.push_back({s.from_name, s.from_addr});
    const bool replying_to_myself = lower(s.from_addr) == own && blank(s.reply_to);

    ReplyTarget t;
    std::vector<MailAddress> to;
    if (replying_to_myself) {
        to = take(parse_mailboxes(s.to), false);   // my own message: the answer goes to whom I wrote to
        if (all) {
            std::vector<MailAddress> cc = take(parse_mailboxes(s.cc), true);
            t.cc = join(cc);
        }
        t.to = join(to);
        return t;
    }
    to = take(first, false);
    if (all) {
        for (const MailAddress& a : take(parse_mailboxes(s.to), true)) to.push_back(a);
        t.cc = join(take(parse_mailboxes(s.cc), true));
    }
    t.to = join(to);
    return t;
}

std::string quote_for_reply(const std::string& text) {
    std::string out;
    size_t i = 0;
    std::string clean;
    for (size_t k = 0; k < text.size(); ++k) {
        if (text[k] == '\r') {
            if (k + 1 < text.size() && text[k + 1] == '\n') ++k;
            clean += '\n';
        } else {
            clean += text[k];
        }
    }
    if (clean.empty()) return out;
    while (i < clean.size()) {
        const size_t e = clean.find('\n', i);
        const std::string line = clean.substr(i, e == std::string::npos ? std::string::npos : e - i);
        out += line.empty() ? ">" : (line[0] == '>' ? ">" + line : "> " + line);   // earlier quotes: ">> "
        out += '\n';
        if (e == std::string::npos) break;
        i = e + 1;
    }
    return out;
}

std::string attribution_line(const std::string& from_name, const std::string& from_addr, const std::string& when_text) {
    return "On " + when_text + ", " + (from_name.empty() ? from_addr : from_name) + " wrote:";
}

std::string forward_body(const std::string& from, const std::string& when_text, const std::string& subject, const std::string& to, const std::string& text) {
    return "---------- Forwarded message ----------\nFrom: " + from + "\nDate: " + when_text + "\nSubject: " + subject + "\nTo: " + to + "\n\n" + text;
}

std::string references_for_reply(const std::string& parent_references, const std::string& parent_message_id) {
    std::vector<std::string> ids;
    auto add = [&](const std::string& list) {
        std::string cleaned = clean_id_list(list);
        size_t pos = 0;
        while (pos < cleaned.size()) {
            const size_t e = cleaned.find(' ', pos);
            const std::string id = cleaned.substr(pos, e == std::string::npos ? std::string::npos : e - pos);
            if (std::find(ids.begin(), ids.end(), id) == ids.end()) ids.push_back(id);
            if (e == std::string::npos) break;
            pos = e + 1;
        }
    };
    add(parent_references);
    add(parent_message_id);
    auto total = [&] {
        size_t n = 0;
        for (const std::string& id : ids) n += id.size() + 1;
        return n;
    };
    while (total() > 900 && ids.size() > 2) ids.erase(ids.begin() + 1);   // the root and the newest stay
    std::string out;
    for (const std::string& id : ids) out += (out.empty() ? "" : " ") + id;
    return out;
}

}  // namespace pbo
