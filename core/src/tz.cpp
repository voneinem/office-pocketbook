#include "pbo/tz.h"

#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace pbo {

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Windows zone names -> IANA (the primary zone of each Windows name). Hand-written from the commonly published equivalences,
// not copied from a data file; a name that is missing here is simply unknown (the user picks a zone in Settings).
struct WindowsZone {
    const char* windows;
    const char* iana;
};
const WindowsZone kWindowsZones[] = {
    {"UTC", "UTC"},
    {"Dateline Standard Time", "Etc/GMT+12"},
    {"Hawaiian Standard Time", "Pacific/Honolulu"},
    {"Alaskan Standard Time", "America/Anchorage"},
    {"Pacific Standard Time", "America/Los_Angeles"},
    {"US Mountain Standard Time", "America/Phoenix"},
    {"Mountain Standard Time", "America/Denver"},
    {"Central Standard Time", "America/Chicago"},
    {"Eastern Standard Time", "America/New_York"},
    {"Atlantic Standard Time", "America/Halifax"},
    {"Newfoundland Standard Time", "America/St_Johns"},
    {"E. South America Standard Time", "America/Sao_Paulo"},
    {"Argentina Standard Time", "America/Argentina/Buenos_Aires"},
    {"SA Pacific Standard Time", "America/Bogota"},
    {"Mexico Standard Time", "America/Mexico_City"},
    {"Canada Central Standard Time", "America/Regina"},
    {"Greenwich Standard Time", "Atlantic/Reykjavik"},
    {"GMT Standard Time", "Europe/London"},
    {"W. Europe Standard Time", "Europe/Berlin"},
    {"Central Europe Standard Time", "Europe/Budapest"},
    {"Central European Standard Time", "Europe/Warsaw"},
    {"Romance Standard Time", "Europe/Paris"},
    {"E. Europe Standard Time", "Europe/Chisinau"},
    {"FLE Standard Time", "Europe/Kiev"},
    {"GTB Standard Time", "Europe/Bucharest"},
    {"Turkey Standard Time", "Europe/Istanbul"},
    {"Russian Standard Time", "Europe/Moscow"},
    {"Israel Standard Time", "Asia/Jerusalem"},
    {"Egypt Standard Time", "Africa/Cairo"},
    {"South Africa Standard Time", "Africa/Johannesburg"},
    {"W. Central Africa Standard Time", "Africa/Lagos"},
    {"E. Africa Standard Time", "Africa/Nairobi"},
    {"Arab Standard Time", "Asia/Riyadh"},
    {"Arabian Standard Time", "Asia/Dubai"},
    {"Iran Standard Time", "Asia/Tehran"},
    {"Pakistan Standard Time", "Asia/Karachi"},
    {"India Standard Time", "Asia/Kolkata"},
    {"Nepal Standard Time", "Asia/Kathmandu"},
    {"Bangladesh Standard Time", "Asia/Dhaka"},
    {"SE Asia Standard Time", "Asia/Bangkok"},
    {"China Standard Time", "Asia/Shanghai"},
    {"Singapore Standard Time", "Asia/Singapore"},
    {"Taipei Standard Time", "Asia/Taipei"},
    {"Tokyo Standard Time", "Asia/Tokyo"},
    {"Korea Standard Time", "Asia/Seoul"},
    {"W. Australia Standard Time", "Australia/Perth"},
    {"AUS Central Standard Time", "Australia/Darwin"},
    {"Cen. Australia Standard Time", "Australia/Adelaide"},
    {"E. Australia Standard Time", "Australia/Brisbane"},
    {"AUS Eastern Standard Time", "Australia/Sydney"},
    {"Tasmania Standard Time", "Australia/Hobart"},
    {"New Zealand Standard Time", "Pacific/Auckland"},
};

// ---- POSIX TZ footer of a TZif file ------------------------------------------------------------------------------------

struct PosixRule {
    bool valid = false;
    int std_off = 0;         // seconds east of UTC
    int dst_off = 0;
    bool has_dst = false;
    struct Date {
        char kind = 'M';     // 'M' month/week/day, 'J' Julian day without Feb 29, 'N' zero-based day with Feb 29
        int month = 0, week = 0, day = 0;
        int julian = 0;
        int time = 7200;     // seconds after local midnight (may be negative or above 86400)
    } start, end;
};

struct Cursor {
    const std::string& s;
    size_t i = 0;
    bool done() const { return i >= s.size(); }
    char peek() const { return i < s.size() ? s[i] : '\0'; }
};

bool parse_posix_name(Cursor& c) {
    if (c.peek() == '<') {
        const size_t e = c.s.find('>', c.i);
        if (e == std::string::npos) return false;
        c.i = e + 1;
        return true;
    }
    size_t n = 0;
    while (std::isalpha(static_cast<unsigned char>(c.peek()))) { ++c.i; ++n; }
    return n >= 3;
}

bool parse_number(Cursor& c, int* out) {
    if (!std::isdigit(static_cast<unsigned char>(c.peek()))) return false;
    long v = 0;
    while (std::isdigit(static_cast<unsigned char>(c.peek()))) {
        v = v * 10 + (c.peek() - '0');
        if (v > 100000) return false;
        ++c.i;
    }
    *out = static_cast<int>(v);
    return true;
}

// [+-]hh[:mm[:ss]] -> seconds (sign applied)
bool parse_hms(Cursor& c, int* seconds) {
    int sign = 1;
    if (c.peek() == '+') ++c.i;
    else if (c.peek() == '-') { sign = -1; ++c.i; }
    int h = 0, m = 0, s = 0;
    if (!parse_number(c, &h)) return false;
    if (c.peek() == ':') {
        ++c.i;
        if (!parse_number(c, &m)) return false;
        if (c.peek() == ':') {
            ++c.i;
            if (!parse_number(c, &s)) return false;
        }
    }
    if (h > 167 || m > 59 || s > 59) return false;
    *seconds = sign * (h * 3600 + m * 60 + s);
    return true;
}

bool parse_rule_date(Cursor& c, PosixRule::Date* d) {
    if (c.peek() == 'M') {
        ++c.i;
        d->kind = 'M';
        if (!parse_number(c, &d->month) || c.peek() != '.') return false;
        ++c.i;
        if (!parse_number(c, &d->week) || c.peek() != '.') return false;
        ++c.i;
        if (!parse_number(c, &d->day)) return false;
        if (d->month < 1 || d->month > 12 || d->week < 1 || d->week > 5 || d->day > 6) return false;
    } else if (c.peek() == 'J') {
        ++c.i;
        d->kind = 'J';
        if (!parse_number(c, &d->julian) || d->julian < 1 || d->julian > 365) return false;
    } else {
        d->kind = 'N';
        if (!parse_number(c, &d->julian) || d->julian > 365) return false;
    }
    if (c.peek() == '/') {
        ++c.i;
        if (!parse_hms(c, &d->time)) return false;
    }
    return true;
}

PosixRule parse_posix(const std::string& text) {
    PosixRule r;
    if (text.empty()) return r;
    Cursor c{text};
    if (!parse_posix_name(c)) return r;
    int off = 0;
    if (!parse_hms(c, &off)) return r;
    r.std_off = -off;
    if (c.done()) { r.valid = true; return r; }
    if (!parse_posix_name(c)) return r;
    r.has_dst = true;
    r.dst_off = r.std_off + 3600;
    if (c.peek() != ',' && !c.done()) {
        int d = 0;
        if (!parse_hms(c, &d)) return r;
        r.dst_off = -d;
    }
    if (c.done()) {   // no rule given: the traditional US rule
        r.start = {'M', 3, 2, 0, 0, 7200};
        r.end = {'M', 11, 1, 0, 0, 7200};
        r.valid = true;
        return r;
    }
    if (c.peek() != ',') return r;
    ++c.i;
    if (!parse_rule_date(c, &r.start)) return r;
    if (c.peek() != ',') return r;
    ++c.i;
    if (!parse_rule_date(c, &r.end)) return r;
    r.valid = c.done();
    return r;
}

// the local date (days since 1970) of a rule in a year
int64_t rule_day(const PosixRule::Date& d, int year) {
    if (d.kind == 'M') {
        const int64_t first = civil::days_from_civil(year, d.month, 1);
        const int wd_first = civil::weekday_from_days(first);
        int day = 1 + ((d.day - wd_first) + 7) % 7 + (d.week - 1) * 7;
        const int dim = civil::days_in_month(year, d.month);
        while (day > dim) day -= 7;
        return first + (day - 1);
    }
    const int64_t jan1 = civil::days_from_civil(year, 1, 1);
    if (d.kind == 'J') {
        int n = d.julian;   // 1..365, Feb 29 never counted
        if (civil::is_leap(year) && n >= 60) ++n;
        return jan1 + (n - 1);
    }
    return jan1 + d.julian;
}

int posix_offset_at(const PosixRule& r, int64_t utc) {
    if (!r.has_dst) return r.std_off;
    int year, m, d;
    civil::civil_from_days(civil::floor_div(utc + r.std_off, 86400), &year, &m, &d);
    const int64_t start = rule_day(r.start, year) * 86400 + r.start.time - r.std_off;   // in local standard time
    const int64_t end = rule_day(r.end, year) * 86400 + r.end.time - r.dst_off;         // in local daylight time
    const bool dst = start < end ? (utc >= start && utc < end) : !(utc >= end && utc < start);
    return dst ? r.dst_off : r.std_off;
}

}  // namespace

// ---- one zone ----------------------------------------------------------------------------------------------------------

struct TzDatabase::Zone {
    std::vector<int64_t> transitions;
    std::vector<uint8_t> type_of;      // per transition
    std::vector<int32_t> type_offset;  // per type
    PosixRule footer;

    int offset_at(int64_t utc) const {
        if (transitions.empty()) {
            if (footer.valid) return posix_offset_at(footer, utc);
            return type_offset.empty() ? 0 : type_offset[0];
        }
        if (utc < transitions.front()) return type_offset[0];
        if (utc >= transitions.back() && footer.valid) return posix_offset_at(footer, utc);
        const size_t i = static_cast<size_t>(std::upper_bound(transitions.begin(), transitions.end(), utc) - transitions.begin()) - 1;
        return type_offset[type_of[i]];
    }
};

namespace {

uint32_t be32(const unsigned char* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]); }
int64_t be64(const unsigned char* p) { return static_cast<int64_t>((uint64_t(be32(p)) << 32) | be32(p + 4)); }

// null if the data is not a usable TZif file
std::shared_ptr<TzDatabase::Zone> parse_tzif(const std::string& data) {
    const unsigned char* d = reinterpret_cast<const unsigned char*>(data.data());
    size_t pos = 0;
    auto header = [&](uint32_t* c) -> bool {   // 6 counts: isut, isstd, leap, time, type, char
        if (data.size() < pos + 44 || std::memcmp(d + pos, "TZif", 4) != 0) return false;
        for (int i = 0; i < 6; ++i) c[i] = be32(d + pos + 20 + 4 * i);
        return true;
    };
    uint32_t c[6];
    if (!header(c)) return nullptr;
    const char version = static_cast<char>(d[4]);
    int tsize = 4;
    if (version >= '2') {
        const uint64_t skip = 44 + uint64_t(c[3]) * 4 + c[3] + uint64_t(c[4]) * 6 + c[5] + uint64_t(c[2]) * 8 + c[1] + c[0];
        if (skip > data.size()) return nullptr;
        pos = static_cast<size_t>(skip);
        if (!header(c)) return nullptr;
        tsize = 8;
    }
    const uint32_t isut = c[0], isstd = c[1], leap = c[2], timecnt = c[3], typecnt = c[4], charcnt = c[5];
    if (typecnt == 0 || typecnt > 256 || timecnt > 100000) return nullptr;
    const uint64_t body = uint64_t(timecnt) * tsize + timecnt + uint64_t(typecnt) * 6 + charcnt + uint64_t(leap) * (tsize + 4) + isstd + isut;
    if (data.size() < pos + 44 + body) return nullptr;
    const unsigned char* p = d + pos + 44;

    auto z = std::make_shared<TzDatabase::Zone>();
    z->transitions.resize(timecnt);
    for (uint32_t i = 0; i < timecnt; ++i) z->transitions[i] = tsize == 8 ? be64(p + 8 * i) : int32_t(be32(p + 4 * i));
    p += uint64_t(timecnt) * tsize;
    z->type_of.assign(p, p + timecnt);
    p += timecnt;
    for (uint8_t t : z->type_of)
        if (t >= typecnt) return nullptr;
    z->type_offset.resize(typecnt);
    for (uint32_t i = 0; i < typecnt; ++i) z->type_offset[i] = int32_t(be32(p + 6 * i));
    for (size_t i = 1; i < z->transitions.size(); ++i)
        if (z->transitions[i] < z->transitions[i - 1]) return nullptr;
    if (tsize == 8) {
        size_t foot = pos + 44 + static_cast<size_t>(body);
        if (foot < data.size() && data[foot] == '\n') {
            const size_t e = data.find('\n', foot + 1);
            if (e != std::string::npos) z->footer = parse_posix(data.substr(foot + 1, e - foot - 1));
        }
    }
    return z;
}

bool plausible_name(const std::string& n) {
    if (n.empty() || n.size() > 64 || n.front() == '/' || n.back() == '/') return false;
    for (size_t i = 0; i < n.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(n[i]);
        const bool ok = std::isalnum(c) || c == '/' || c == '_' || c == '-' || c == '+';
        if (!ok) return false;
        if (c == '/' && i + 1 < n.size() && n[i + 1] == '/') return false;
    }
    return true;
}

std::string read_file(const std::string& path, size_t max) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return std::string();
    std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return s.size() > max ? std::string() : s;
}

std::string release_from(const std::string& path) {
    std::ifstream f(path);
    std::string line;
    for (int n = 0; n < 50 && std::getline(f, line); ++n)
        if (line.rfind("Release: ", 0) == 0) {
            std::string v = line.substr(9);
            while (!v.empty() && std::isspace(static_cast<unsigned char>(v.back()))) v.pop_back();
            return v;
        }
    return std::string();
}

}  // namespace

std::unique_ptr<TzDatabase> TzDatabase::open(const std::string& zoneinfo_dir) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::is_directory(zoneinfo_dir, ec)) return nullptr;
    std::unique_ptr<TzDatabase> db(new TzDatabase());
    db->dir_ = zoneinfo_dir;
    const fs::path root(zoneinfo_dir);
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        std::string rel = it->path().lexically_relative(root).generic_string();
        if (!plausible_name(rel) || rel == "VERSION") continue;
        db->names_.push_back(rel);
    }
    std::sort(db->names_.begin(), db->names_.end());
    for (const std::string& n : db->names_) db->by_lower_[lower(n)] = n;
    db->version_ = release_from(zoneinfo_dir + "/VERSION");
    if (db->version_.empty()) db->version_ = release_from(zoneinfo_dir + "/../VERSION");
    if (!db->load("UTC") && !db->load("Europe/Berlin")) return nullptr;
    return db;
}

std::shared_ptr<const TzDatabase::Zone> TzDatabase::load(const std::string& canonical) const {
    {
        std::lock_guard<std::mutex> l(mu_);
        auto it = cache_.find(canonical);
        if (it != cache_.end()) return it->second;
    }
    std::shared_ptr<const Zone> z;
    if (by_lower_.count(lower(canonical))) z = parse_tzif(read_file(dir_ + "/" + canonical, 1u << 20));
    std::lock_guard<std::mutex> l(mu_);
    cache_[canonical] = z;
    return z;
}

std::string TzDatabase::canonical_name(std::string_view name) const {
    const std::string n(name);
    if (n.empty() || n.size() > 64) return std::string();
    std::string iana;
    const std::string key = lower(n);
    auto it = by_lower_.find(key);
    if (it != by_lower_.end()) {
        iana = it->second;
    } else {
        for (const WindowsZone& w : kWindowsZones)
            if (key == lower(w.windows)) {
                auto m = by_lower_.find(lower(w.iana));
                if (m != by_lower_.end()) iana = m->second;
                break;
            }
    }
    if (iana.empty() || !load(iana)) return std::string();
    return iana;
}

int TzDatabase::offset_at(std::string_view zone, int64_t utc) const {
    const std::string c = canonical_name(zone);
    if (c.empty()) return 0;
    return load(c)->offset_at(utc);
}

LocalDateTime TzDatabase::utc_to_local(std::string_view zone, int64_t utc) const {
    return civil::from_naive_epoch(utc + offset_at(zone, utc));
}

int64_t TzDatabase::local_to_utc(std::string_view zone, const LocalDateTime& local) const {
    const int64_t naive = civil::naive_epoch(local);
    const std::string c = canonical_name(zone);
    if (c.empty()) return naive;
    const auto z = load(c);
    const int before = z->offset_at(naive - 86400);
    const int after = z->offset_at(naive + 86400);
    // The offset before the change is tried first: it gives the first occurrence of a repeated time, and the offset before the gap for
    // a time that does not exist (RFC 5545 section 3.3.5).
    for (int off : {before, after})
        if (z->offset_at(naive - off) == off) return naive - off;
    return naive - before;
}

// ---- the user's zone ---------------------------------------------------------------------------------------------------

ZoneEnvironment read_zone_environment() {
    ZoneEnvironment e;
    if (const char* tz = std::getenv("TZ")) e.tz_variable = tz;
    char buf[512];
    const ssize_t n = ::readlink("/etc/localtime", buf, sizeof buf - 1);
    if (n > 0) e.localtime_target.assign(buf, static_cast<size_t>(n));
    return e;
}

ZoneChoice resolve_user_zone(const std::optional<std::string>& stored_choice, const TzDatabase& db, const ZoneEnvironment& env) {
    ZoneChoice r;
    auto take = [&](const std::string& name) {
        if (name.empty() || name.find("..") != std::string::npos) return false;
        const std::string c = db.canonical_name(name);
        if (c.empty()) return false;
        r.state = ZoneChoice::State::Resolved;
        r.name = c;
        return true;
    };
    if (stored_choice && take(*stored_choice)) return r;
    std::string tz = env.tz_variable;
    if (!tz.empty() && tz[0] == ':') tz.erase(0, 1);
    if (take(tz)) return r;
    const std::string& t = env.localtime_target;
    const size_t at = t.rfind("zoneinfo/");
    if (at != std::string::npos) {
        std::string name = t.substr(at + 9);
        for (const char* prefix : {"posix/", "right/"})
            if (name.rfind(prefix, 0) == 0) name.erase(0, std::strlen(prefix));
        if (take(name)) return r;
    }
    return r;
}

}  // namespace pbo
