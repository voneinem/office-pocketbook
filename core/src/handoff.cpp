#include "pbo/handoff.h"

#include "pbo/account_input.h"

namespace pbo {

const char* const kHandoffMarker = "pbo-mail-v1";

namespace {

const char kFffd[] = "\xef\xbf\xbd";   // U+FFFD

bool is_blank(char c) { return c == ' ' || c == '\t' || c == '\n'; }

bool all_blank(const std::string& s) {
    for (char c : s)
        if (!is_blank(c)) return false;
    return true;
}

bool is_cont(unsigned char c) { return (c & 0xc0) == 0x80; }

// Length in bytes of the valid UTF-8 sequence that starts at text[i], or 0 when there is none (overlong forms, surrogates, above U+10FFFF and
// truncated sequences are invalid).
size_t valid_sequence(std::string_view t, size_t i) {
    const unsigned char b = static_cast<unsigned char>(t[i]);
    auto at = [&](size_t k) { return static_cast<unsigned char>(t[i + k]); };
    const size_t left = t.size() - i;
    if (b >= 0xc2 && b <= 0xdf) return left >= 2 && is_cont(at(1)) ? 2 : 0;
    if (b >= 0xe0 && b <= 0xef) {
        if (left < 3 || !is_cont(at(1)) || !is_cont(at(2))) return 0;
        if (b == 0xe0 && at(1) < 0xa0) return 0;   // overlong
        if (b == 0xed && at(1) > 0x9f) return 0;   // a UTF-16 surrogate
        return 3;
    }
    if (b >= 0xf0 && b <= 0xf4) {
        if (left < 4 || !is_cont(at(1)) || !is_cont(at(2)) || !is_cont(at(3))) return 0;
        if (b == 0xf0 && at(1) < 0x90) return 0;   // overlong
        if (b == 0xf4 && at(1) > 0x8f) return 0;   // above U+10FFFF
        return 4;
    }
    return 0;
}

size_t count_characters(const std::string& valid_utf8) {
    size_t n = 0;
    for (char c : valid_utf8)
        if (!is_cont(static_cast<unsigned char>(c))) ++n;
    return n;
}

// "123" -> 123; anything else (empty, sign, blank, letter, overflow) -> false
bool parse_decimal(std::string_view s, size_t* out) {
    if (s.empty() || s.size() > 18) return false;
    size_t v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + static_cast<size_t>(c - '0');
    }
    *out = v;
    return true;
}

// one plain address: no blank, control character, comma, semicolon, bracket or quote before is_valid_address looks at it
bool plain_address(const std::string& a) {
    for (unsigned char c : a)
        if (c <= 0x20 || c == 0x7f || c == ',' || c == ';' || c == '<' || c == '>' || c == '"' || c == '(' || c == ')') return false;
    return a.size() <= kHandoffMaxAddressBytes && is_valid_address(a);
}

}  // namespace

bool Handoff::empty() const { return all_blank(text) && all_blank(subject) && all_blank(to); }

std::string clean_handoff_text(std::string_view t) {
    std::string out;
    out.reserve(t.size());
    for (size_t i = 0; i < t.size();) {
        const unsigned char b = static_cast<unsigned char>(t[i]);
        if (b < 0x80) {
            if (b == '\r') {
                out += '\n';
                i += (i + 1 < t.size() && t[i + 1] == '\n') ? 2 : 1;
                continue;
            }
            if (b == '\n' || b == '\t' || (b >= 0x20 && b != 0x7f)) out += static_cast<char>(b);
            else out += kFffd;   // C0 control characters and DEL
            ++i;
            continue;
        }
        const size_t n = valid_sequence(t, i);
        if (n == 0) {   // an invalid byte: one replacement for it alone, the bytes after it are judged on their own
            out += kFffd;
            ++i;
            continue;
        }
        if (n == 2 && b == 0xc2 && static_cast<unsigned char>(t[i + 1]) <= 0x9f) out += kFffd;   // U+0080..U+009F (C1 controls)
        else out.append(t.data() + i, n);
        i += n;
    }
    return out;
}

std::string clean_handoff_subject(std::string_view t) {
    std::string s = clean_handoff_text(t);
    for (char& c : s)
        if (c == '\n' || c == '\t') c = ' ';
    return s;
}

HandoffResult parse_handoff(const std::vector<std::string>& args) {
    HandoffResult r;
    size_t marker = args.size();
    for (size_t i = 0; i < args.size(); ++i)
        if (args[i] == kHandoffMarker) {
            marker = i;
            break;
        }
    if (marker == args.size()) return r;
    r.is_handoff = true;

    const std::string* text = nullptr;
    const std::string* textlen = nullptr;
    const std::string* subject = nullptr;
    const std::string* to = nullptr;
    std::string text_value, textlen_value, subject_value, to_value;   // the part after the first '='
    for (size_t i = marker + 1; i < args.size(); ++i) {
        const std::string& a = args[i];
        const size_t eq = a.find('=');
        if (eq == std::string::npos) continue;
        const std::string_view key(a.data(), eq);
        const std::string* slot = nullptr;
        std::string* value = nullptr;
        if (key == "text") { slot = text; value = &text_value; }
        else if (key == "textlen") { slot = textlen; value = &textlen_value; }
        else if (key == "subject") { slot = subject; value = &subject_value; }
        else if (key == "to") { slot = to; value = &to_value; }
        else continue;   // unknown keys are ignored
        if (slot) {      // a known key twice: a bug or an attack
            r.refusal = HandoffRefusal::Malformed;
            return r;
        }
        *value = a.substr(eq + 1);
        if (key == "text") text = &a;
        else if (key == "textlen") textlen = &a;
        else if (key == "subject") subject = &a;
        else to = &a;
    }

    if (text) {
        if (text_value.size() > kHandoffMaxTextBytes) {
            r.refusal = HandoffRefusal::TooLong;
            return r;
        }
        size_t announced = 0;
        if (!textlen || !parse_decimal(textlen_value, &announced) || announced != text_value.size()) {
            r.refusal = HandoffRefusal::Incomplete;   // cut or changed on the way, or not announced
            return r;
        }
        r.handoff.text = clean_handoff_text(text_value);
    }

    if (subject) {
        // 300 characters are at most 1200 bytes; anything longer is too long without cleaning it first
        if (subject_value.size() > kHandoffMaxSubjectChars * 4) {
            r.handoff.notices.push_back(HandoffNotice::SubjectTooLong);
        } else {
            std::string s = clean_handoff_subject(subject_value);
            if (count_characters(s) > kHandoffMaxSubjectChars) r.handoff.notices.push_back(HandoffNotice::SubjectTooLong);
            else r.handoff.subject = std::move(s);
        }
    }

    if (to) {
        size_t b = 0, e = to_value.size();
        while (b < e && (to_value[b] == ' ' || to_value[b] == '\t')) ++b;
        while (e > b && (to_value[e - 1] == ' ' || to_value[e - 1] == '\t')) --e;
        if (e > b) {
            std::string address = to_value.substr(b, e - b);
            if (plain_address(address)) r.handoff.to = std::move(address);
            else r.handoff.notices.push_back(HandoffNotice::RecipientIgnored);
        }
    }
    return r;
}

}  // namespace pbo
