#include "pbo/mail_headers.h"

#include <cctype>
#include <cstring>

namespace pbo {

namespace {

char lower(char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (lower(a[i]) != lower(b[i])) return false;
    return true;
}

std::string trim(std::string_view v) {
    size_t b = 0, e = v.size();
    while (b < e && (v[b] == ' ' || v[b] == '\t')) ++b;
    while (e > b && (v[e - 1] == ' ' || v[e - 1] == '\t')) --e;
    return std::string(v.substr(b, e - b));
}

const char* const kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

// days from 1970-01-01 to y-m-d (Howard Hinnant's algorithm)
int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

void civil_from_days(int64_t z, int* y, unsigned* m, unsigned* d) {
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t yy = static_cast<int64_t>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y = static_cast<int>(yy + (*m <= 2));
}

}  // namespace

int64_t epoch_from_civil(int year, int month, int day, int hour, int minute, int second) {
    return days_from_civil(year, static_cast<unsigned>(month), static_cast<unsigned>(day)) * 86400 + hour * 3600 + minute * 60 + second;
}

std::string header_value(std::string_view block, std::string_view name) {
    size_t pos = 0;
    while (pos < block.size()) {
        size_t eol = block.find('\n', pos);
        const size_t line_end = eol == std::string_view::npos ? block.size() : eol;
        std::string_view line = block.substr(pos, line_end - pos);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.empty()) return "";   // end of the header block
        const size_t colon = line.find(':');
        if (colon != std::string_view::npos && iequals(line.substr(0, colon), name)) {
            std::string value(line.substr(colon + 1));
            size_t next = eol == std::string_view::npos ? block.size() : eol + 1;
            while (next < block.size() && (block[next] == ' ' || block[next] == '\t')) {   // folded continuation lines
                size_t e2 = block.find('\n', next);
                const size_t end2 = e2 == std::string_view::npos ? block.size() : e2;
                std::string_view cont = block.substr(next, end2 - next);
                if (!cont.empty() && cont.back() == '\r') cont.remove_suffix(1);
                value += ' ';
                value += trim(cont);
                next = e2 == std::string_view::npos ? block.size() : e2 + 1;
            }
            // blanks and tabs inside the value (also where lines were folded) count as one space
            std::string squeezed;
            for (char c : value) {
                if ((c == ' ' || c == '\t') && !squeezed.empty() && squeezed.back() == ' ') continue;
                squeezed += c == '\t' ? ' ' : c;
            }
            return trim(squeezed);
        }
        pos = eol == std::string_view::npos ? block.size() : eol + 1;
    }
    return "";
}

bool parse_rfc5322_date(std::string_view text, int64_t* epoch) {
    // [day-of-week ","] day month year hour ":" minute [":" second] zone
    std::string t(text);
    size_t i = 0;
    auto skip = [&] { while (i < t.size() && (t[i] == ' ' || t[i] == '\t')) ++i; };
    auto number = [&](int* out, int max_digits) {
        const size_t start = i;
        long v = 0;
        while (i < t.size() && std::isdigit(static_cast<unsigned char>(t[i])) && i - start < static_cast<size_t>(max_digits)) v = v * 10 + (t[i++] - '0');
        *out = static_cast<int>(v);
        return i > start;
    };
    skip();
    if (i + 3 < t.size() && std::isalpha(static_cast<unsigned char>(t[i]))) {   // day of week
        while (i < t.size() && t[i] != ',') ++i;
        if (i == t.size()) return false;
        ++i;
        skip();
    }
    int day = 0, year = 0, hour = 0, minute = 0, second = 0;
    if (!number(&day, 2)) return false;
    skip();
    int month = 0;
    if (i + 3 > t.size()) return false;
    for (int m = 0; m < 12; ++m)
        if (iequals(std::string_view(t).substr(i, 3), kMonths[m])) month = m + 1;
    if (!month) return false;
    i += 3;
    skip();
    if (!number(&year, 4)) return false;
    if (year < 100) year += year < 50 ? 2000 : 1900;
    skip();
    if (!number(&hour, 2) || i >= t.size() || t[i] != ':') return false;
    ++i;
    if (!number(&minute, 2)) return false;
    if (i < t.size() && t[i] == ':') {
        ++i;
        if (!number(&second, 2)) return false;
    }
    skip();
    int offset = 0;   // seconds east of UTC
    if (i < t.size() && (t[i] == '+' || t[i] == '-')) {
        const int sign = t[i] == '-' ? -1 : 1;
        ++i;
        int hhmm = 0;
        if (!number(&hhmm, 4)) return false;
        offset = sign * ((hhmm / 100) * 3600 + (hhmm % 100) * 60);
    } else if (i < t.size()) {   // GMT, UT, Z, EST, ... : only the UTC names are understood, others count as UTC
        offset = 0;
    }
    if (day < 1 || day > 31 || hour > 23 || minute > 59 || second > 60) return false;
    *epoch = epoch_from_civil(year, month, day, hour, minute, second) - offset;
    return true;
}

void split_first_address(std::string_view text, std::string* name, std::string* address) {
    name->clear();
    address->clear();
    std::string t(text);
    // the first address ends at a comma outside quotes and angle brackets
    size_t end = t.size();
    bool quoted = false, angle = false;
    for (size_t i = 0; i < t.size(); ++i) {
        const char c = t[i];
        if (c == '\\' && quoted) { ++i; continue; }
        if (c == '"') quoted = !quoted;
        else if (!quoted && c == '<') angle = true;
        else if (!quoted && c == '>') angle = false;
        else if (!quoted && !angle && c == ',') { end = i; break; }
    }
    t = t.substr(0, end);
    const size_t lt = t.rfind('<');
    const size_t gt = t.find('>', lt == std::string::npos ? 0 : lt);
    if (lt != std::string::npos && gt != std::string::npos && gt > lt) {
        *address = trim(std::string_view(t).substr(lt + 1, gt - lt - 1));
        std::string n = trim(std::string_view(t).substr(0, lt));
        if (n.size() >= 2 && n.front() == '"' && n.back() == '"') {
            std::string unq;
            for (size_t i = 1; i + 1 < n.size(); ++i) {
                if (n[i] == '\\' && i + 2 < n.size()) ++i;
                unq += n[i];
            }
            n = unq;
        }
        *name = n;
    } else {
        *address = trim(t);
    }
}

std::string imap_date(int64_t epoch) {
    int64_t days = epoch / 86400;
    if (epoch % 86400 < 0) --days;
    int y;
    unsigned m, d;
    civil_from_days(days, &y, &m, &d);
    return std::to_string(d) + "-" + kMonths[m - 1] + "-" + std::to_string(y);
}

}  // namespace pbo
