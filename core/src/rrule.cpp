#include "pbo/rrule.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <set>

namespace pbo {

namespace {

std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

bool to_int(const std::string& s, long* out, long lo, long hi) {
    if (s.empty() || s.size() > 10) return false;
    size_t i = 0;
    if (s[0] == '+' || s[0] == '-') i = 1;
    if (i == s.size()) return false;
    for (size_t k = i; k < s.size(); ++k)
        if (!std::isdigit(static_cast<unsigned char>(s[k]))) return false;
    const long v = std::strtol(s.c_str(), nullptr, 10);
    if (v < lo || v > hi) return false;
    *out = v;
    return true;
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    size_t b = 0;
    for (;;) {
        const size_t e = s.find(sep, b);
        out.push_back(s.substr(b, e == std::string::npos ? std::string::npos : e - b));
        if (e == std::string::npos) break;
        b = e + 1;
    }
    return out;
}

int weekday_code(const std::string& s) {
    static const char* names[] = {"SU", "MO", "TU", "WE", "TH", "FR", "SA"};
    for (int i = 0; i < 7; ++i)
        if (s == names[i]) return i;
    return -1;
}

bool parse_until(const std::string& v, RRule::Until* u) {
    if (v.size() != 8 && v.size() != 15 && v.size() != 16) return false;
    for (size_t i = 0; i < v.size(); ++i) {
        if (i == 8 && v[i] == 'T') continue;
        if (i == 15 && v[i] == 'Z') continue;
        if (!std::isdigit(static_cast<unsigned char>(v[i]))) return false;
    }
    auto num = [&](size_t pos, size_t len) { return std::atoi(v.substr(pos, len).c_str()); };
    LocalDateTime t;
    t.year = num(0, 4);
    t.month = num(4, 2);
    t.day = num(6, 2);
    if (!civil::valid_date(t.year, t.month, t.day)) return false;
    u->date_only = v.size() == 8;
    u->utc = v.size() == 16;
    if (!u->date_only) {
        if (v[8] != 'T') return false;
        t.hour = num(9, 2);
        t.minute = num(11, 2);
        t.second = num(13, 2);
        if (t.hour > 23 || t.minute > 59 || t.second > 60) return false;
        if (t.second == 60) t.second = 59;
    }
    u->local = t;
    return true;
}

}  // namespace

RRule parse_rrule(const std::string& text) {
    RRule r;
    if (text.empty() || text.size() > 1024) return r;
    std::set<std::string> seen;
    bool ok = true;
    for (const std::string& part : split(text, ';')) {
        const size_t eq = part.find('=');
        if (eq == std::string::npos || eq == 0) return RRule();
        const std::string key = upper(part.substr(0, eq));
        const std::string val = upper(part.substr(eq + 1));
        if (!seen.insert(key).second) return RRule();
        if (val.empty()) return RRule();
        long n = 0;
        if (key == "FREQ") {
            if (val == "DAILY") r.freq = RRule::Freq::Daily;
            else if (val == "WEEKLY") r.freq = RRule::Freq::Weekly;
            else if (val == "MONTHLY") r.freq = RRule::Freq::Monthly;
            else if (val == "YEARLY") r.freq = RRule::Freq::Yearly;
            else ok = false;   // SECONDLY, MINUTELY, HOURLY, unknown
        } else if (key == "INTERVAL") {
            if (!to_int(val, &n, 1, 1000000)) return RRule();
            r.interval = static_cast<int>(n);
        } else if (key == "COUNT") {
            if (!to_int(val, &n, 1, 2147483647)) return RRule();
            r.count = static_cast<int>(n);
        } else if (key == "UNTIL") {
            RRule::Until u;
            if (!parse_until(val, &u)) return RRule();
            r.until = u;
        } else if (key == "WKST") {
            const int w = weekday_code(val);
            if (w < 0) return RRule();
            r.wkst = w;
        } else if (key == "BYDAY") {
            for (const std::string& d : split(val, ',')) {
                if (d.size() < 2) return RRule();
                const int wd = weekday_code(d.substr(d.size() - 2));
                if (wd < 0) return RRule();
                RRule::ByDay b;
                b.weekday = wd;
                if (d.size() > 2) {
                    if (!to_int(d.substr(0, d.size() - 2), &n, -53, 53) || n == 0) return RRule();
                    b.ordinal = static_cast<int>(n);
                }
                r.by_day.push_back(b);
            }
        } else if (key == "BYMONTHDAY") {
            for (const std::string& d : split(val, ',')) {
                if (!to_int(d, &n, -31, 31) || n == 0) return RRule();
                r.by_month_day.push_back(static_cast<int>(n));
            }
        } else if (key == "BYMONTH") {
            for (const std::string& d : split(val, ',')) {
                if (!to_int(d, &n, 1, 12)) return RRule();
                r.by_month.push_back(static_cast<int>(n));
            }
        } else if (key == "BYSETPOS") {
            for (const std::string& d : split(val, ',')) {
                if (!to_int(d, &n, -366, 366) || n == 0) return RRule();
                r.by_set_pos.push_back(static_cast<int>(n));
            }
        } else {
            ok = false;   // BYWEEKNO, BYYEARDAY, BYHOUR, BYMINUTE, BYSECOND, RSCALE, SKIP, X-...
        }
    }
    if (r.freq == RRule::Freq::None) ok = false;
    if (r.count > 0 && r.until) return RRule();   // RFC 5545: not both
    const bool ordinals = std::any_of(r.by_day.begin(), r.by_day.end(), [](const RRule::ByDay& b) { return b.ordinal != 0; });
    if (r.freq == RRule::Freq::Weekly && (!r.by_month_day.empty() || ordinals)) return RRule();
    if (r.freq == RRule::Freq::Daily && ordinals) return RRule();
    if (!ok) return RRule();
    r.supported = true;
    return r;
}

namespace {

constexpr int kMaxYear = 9999;

struct Gen {
    const RRule& r;
    const LocalDateTime& start;
    int64_t start_day;
    int tod;   // seconds after midnight of DTSTART

    bool month_ok(int m) const { return r.by_month.empty() || std::find(r.by_month.begin(), r.by_month.end(), m) != r.by_month.end(); }

    // days (since 1970) of the given weekday entries inside one month
    void byday_in_month(int y, int m, std::set<int64_t>* out) const {
        const int dim = civil::days_in_month(y, m);
        const int64_t first = civil::days_from_civil(y, m, 1);
        for (const auto& b : r.by_day) {
            std::vector<int64_t> all;
            for (int d = 1; d <= dim; ++d)
                if (civil::weekday_from_days(first + d - 1) == b.weekday) all.push_back(first + d - 1);
            if (b.ordinal == 0) out->insert(all.begin(), all.end());
            else if (b.ordinal > 0 && static_cast<size_t>(b.ordinal) <= all.size()) out->insert(all[b.ordinal - 1]);
            else if (b.ordinal < 0 && static_cast<size_t>(-b.ordinal) <= all.size()) out->insert(all[all.size() + b.ordinal]);
        }
    }

    void bymonthday_in_month(int y, int m, std::set<int64_t>* out) const {
        const int dim = civil::days_in_month(y, m);
        const int64_t first = civil::days_from_civil(y, m, 1);
        for (int v : r.by_month_day) {
            const int d = v > 0 ? v : dim + v + 1;
            if (d >= 1 && d <= dim) out->insert(first + d - 1);
        }
    }

    // candidates of one month for MONTHLY and for the months of YEARLY
    std::set<int64_t> month_days(int y, int m) const {
        std::set<int64_t> days, a, b;
        const bool has_md = !r.by_month_day.empty(), has_bd = !r.by_day.empty();
        if (has_md) bymonthday_in_month(y, m, &a);
        if (has_bd) byday_in_month(y, m, &b);
        if (has_md && has_bd) {
            std::set_intersection(a.begin(), a.end(), b.begin(), b.end(), std::inserter(days, days.begin()));
        } else if (has_md) {
            days = a;
        } else if (has_bd) {
            days = b;
        } else if (start.day <= civil::days_in_month(y, m)) {
            days.insert(civil::days_from_civil(y, m, start.day));
        }
        return days;
    }

    // candidates of one year when no BYMONTH is given
    std::set<int64_t> year_days(int y) const {
        std::set<int64_t> days;
        if (!r.by_month_day.empty()) {
            for (int m = 1; m <= 12; ++m) {
                std::set<int64_t> md = month_days(y, m);
                days.insert(md.begin(), md.end());
            }
            return days;
        }
        if (!r.by_day.empty()) {
            const int64_t first = civil::days_from_civil(y, 1, 1);
            const int diy = civil::is_leap(y) ? 366 : 365;
            for (const auto& b : r.by_day) {
                std::vector<int64_t> all;
                for (int d = 0; d < diy; ++d)
                    if (civil::weekday_from_days(first + d) == b.weekday) all.push_back(first + d);
                if (b.ordinal == 0) days.insert(all.begin(), all.end());
                else if (b.ordinal > 0 && static_cast<size_t>(b.ordinal) <= all.size()) days.insert(all[b.ordinal - 1]);
                else if (b.ordinal < 0 && static_cast<size_t>(-b.ordinal) <= all.size()) days.insert(all[all.size() + b.ordinal]);
            }
            return days;
        }
        if (start.day <= civil::days_in_month(y, start.month)) days.insert(civil::days_from_civil(y, start.month, start.day));
        return days;
    }

    bool weekday_listed(int wd) const {
        return std::any_of(r.by_day.begin(), r.by_day.end(), [&](const RRule::ByDay& b) { return b.weekday == wd; });
    }

    // candidate days of period k (k-th period after the first), sorted
    std::vector<int64_t> period(int64_t k, int64_t week0) const {
        std::set<int64_t> days;
        const int64_t step = k * r.interval;
        switch (r.freq) {
            case RRule::Freq::Daily: {
                const int64_t d = start_day + step;
                int y, m, dd;
                civil::civil_from_days(d, &y, &m, &dd);
                if (!month_ok(m)) break;
                if (!r.by_month_day.empty()) {
                    std::set<int64_t> md;
                    bymonthday_in_month(y, m, &md);
                    if (!md.count(d)) break;
                }
                if (!r.by_day.empty() && !weekday_listed(civil::weekday_from_days(d))) break;
                days.insert(d);
                break;
            }
            case RRule::Freq::Weekly: {
                const int64_t w = week0 + 7 * step;
                for (int i = 0; i < 7; ++i) {
                    const int64_t d = w + i;
                    const int wd = civil::weekday_from_days(d);
                    if (r.by_day.empty() ? wd != civil::weekday_from_days(start_day) : !weekday_listed(wd)) continue;
                    int y, m, dd;
                    civil::civil_from_days(d, &y, &m, &dd);
                    if (month_ok(m)) days.insert(d);
                }
                break;
            }
            case RRule::Freq::Monthly: {
                const int64_t months = static_cast<int64_t>(start.year) * 12 + (start.month - 1) + step;
                const int y = static_cast<int>(civil::floor_div(months, 12));
                const int m = static_cast<int>(months - static_cast<int64_t>(y) * 12) + 1;
                if (y > kMaxYear) break;
                if (month_ok(m)) days = month_days(y, m);
                break;
            }
            case RRule::Freq::Yearly: {
                const int64_t y64 = static_cast<int64_t>(start.year) + step;
                if (y64 > kMaxYear) break;
                const int y = static_cast<int>(y64);
                if (!r.by_month.empty()) {
                    for (int m : r.by_month) {
                        std::set<int64_t> md = month_days(y, m);
                        days.insert(md.begin(), md.end());
                    }
                } else {
                    days = year_days(y);
                }
                break;
            }
            default: break;
        }
        std::vector<int64_t> v(days.begin(), days.end());
        if (!r.by_set_pos.empty()) {
            std::set<int64_t> chosen;
            for (int p : r.by_set_pos) {
                const int64_t idx = p > 0 ? p - 1 : static_cast<int64_t>(v.size()) + p;
                if (idx >= 0 && idx < static_cast<int64_t>(v.size())) chosen.insert(v[static_cast<size_t>(idx)]);
            }
            v.assign(chosen.begin(), chosen.end());
        }
        return v;
    }
};

}  // namespace

RRuleResult expand_rrule(const RRule& rule, const LocalDateTime& dtstart, std::optional<int64_t> until, int64_t emit_from, int64_t stop,
                         const RRuleLimits& limits) {
    RRuleResult out;
    if (!rule.supported) return out;
    if (dtstart.year < 1 || dtstart.year > kMaxYear) return out;
    Gen g{rule, dtstart, civil::days_from_civil(dtstart.year, dtstart.month, dtstart.day), dtstart.hour * 3600 + dtstart.minute * 60 + dtstart.second};
    // the week (by WKST) that holds DTSTART
    const int sw = civil::weekday_from_days(g.start_day);
    const int64_t week0 = g.start_day - ((sw - rule.wkst + 7) % 7);

    // jump over periods that end before emit_from (only without COUNT: COUNT must see every instance)
    int64_t k = 0;
    if (rule.count == 0 && emit_from > civil::naive_epoch(dtstart)) {
        const int64_t emit_day = civil::floor_div(emit_from, 86400);
        int64_t periods = 0;
        switch (rule.freq) {
            case RRule::Freq::Daily: periods = (emit_day - g.start_day) / rule.interval; break;
            case RRule::Freq::Weekly: periods = (emit_day - week0) / 7 / rule.interval; break;
            case RRule::Freq::Monthly: {
                int y, m, d;
                civil::civil_from_days(emit_day, &y, &m, &d);
                periods = ((static_cast<int64_t>(y) * 12 + m - 1) - (static_cast<int64_t>(dtstart.year) * 12 + dtstart.month - 1)) / rule.interval;
                break;
            }
            case RRule::Freq::Yearly: {
                int y, m, d;
                civil::civil_from_days(emit_day, &y, &m, &d);
                periods = (y - dtstart.year) / rule.interval;
                break;
            }
            default: break;
        }
        k = std::max<int64_t>(0, periods - 1);
    }

    int64_t counted = 0;
    for (size_t steps = 0;; ++k) {
        if (++steps > limits.max_steps) {
            out.guard_hit = true;
            return out;
        }
        // earliest possible instant of the period: if it is after the stop, nothing later can matter
        int64_t earliest_day = 0;
        switch (rule.freq) {
            case RRule::Freq::Daily: earliest_day = g.start_day + k * rule.interval; break;
            case RRule::Freq::Weekly: earliest_day = week0 + 7 * k * rule.interval; break;
            case RRule::Freq::Monthly: {
                const int64_t months = static_cast<int64_t>(dtstart.year) * 12 + (dtstart.month - 1) + k * rule.interval;
                const int y = static_cast<int>(civil::floor_div(months, 12));
                if (y > kMaxYear) return out;
                earliest_day = civil::days_from_civil(y, static_cast<int>(months - static_cast<int64_t>(y) * 12) + 1, 1);
                break;
            }
            case RRule::Freq::Yearly: {
                const int64_t y = static_cast<int64_t>(dtstart.year) + k * rule.interval;
                if (y > kMaxYear) return out;
                earliest_day = civil::days_from_civil(y, 1, 1);
                break;
            }
            default: return out;
        }
        if (earliest_day > civil::days_from_civil(kMaxYear, 12, 31)) return out;
        if (earliest_day * 86400 > stop) return out;
        if (until && earliest_day * 86400 > *until) return out;

        for (int64_t day : g.period(k, week0)) {
            if (day < g.start_day) continue;
            const int64_t t = day * 86400 + g.tod;
            if (t < civil::naive_epoch(dtstart)) continue;
            if (until && t > *until) return out;
            if (t > stop) return out;
            if (rule.count > 0 && counted >= rule.count) return out;
            ++counted;
            if (t >= emit_from) {
                if (out.starts.size() >= limits.max_instances) {
                    out.capped = true;
                    return out;
                }
                out.starts.push_back(civil::from_naive_epoch(t));
            }
            if (rule.count > 0 && counted >= rule.count) return out;
        }
    }
}

}  // namespace pbo

// ---- the words for a rule -----------------------------------------------------------------------------------------------------

namespace pbo {

namespace {

const char* kWeekdayNamesEn[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
const char* kWeekdayNamesDe[] = {"Sonntag", "Montag", "Dienstag", "Mittwoch", "Donnerstag", "Freitag", "Samstag"};
const char* kMonthNamesEn[] = {"January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"};
const char* kMonthNamesDe[] = {"Januar", "Februar", "M\xC3\xA4rz", "April", "Mai", "Juni", "Juli", "August", "September", "Oktober", "November", "Dezember"};

// The words of one language for the sentence about a rule.
struct Words {
    bool de;
    const char* weekday(int i) const { return (de ? kWeekdayNamesDe : kWeekdayNamesEn)[i]; }
    const char* month(int i) const { return (de ? kMonthNamesDe : kMonthNamesEn)[i]; }
    std::string fallback() const { return de ? "wiederholt sich (Einzelheiten nicht gezeigt)" : "repeats (details not shown)"; }
    std::string and_() const { return de ? " und " : " and "; }
    // "every day" / "every 3 days"
    std::string every(RRule::Freq f, int interval) const {
        const int k = f == RRule::Freq::Daily ? 0 : f == RRule::Freq::Weekly ? 1 : f == RRule::Freq::Monthly ? 2 : 3;
        if (!de) {
            static const char* unit[] = {"day", "week", "month", "year"};
            return interval > 1 ? "every " + std::to_string(interval) + " " + unit[k] + "s" : std::string("every ") + unit[k];
        }
        static const char* one[] = {"jeden Tag", "jede Woche", "jeden Monat", "jedes Jahr"};
        static const char* many[] = {"Tage", "Wochen", "Monate", "Jahre"};
        return interval > 1 ? "alle " + std::to_string(interval) + " " + many[k] : std::string(one[k]);
    }
    std::string on_weekdays(const std::string& names) const { return (de ? " am " : " on ") + names; }
    std::string ordinal(int n) const {
        if (de) {
            switch (n) {
                case 1: return "ersten";
                case 2: return "zweiten";
                case 3: return "dritten";
                case 4: return "vierten";
                case 5: return "f\xC3\xBCnften";
                case -1: return "letzten";
                case -2: return "vorletzten";
                default: return std::string();
            }
        }
        switch (n) {
            case 1: return "first";
            case 2: return "second";
            case 3: return "third";
            case 4: return "fourth";
            case 5: return "fifth";
            case -1: return "last";
            case -2: return "second to last";
            default: return std::string();
        }
    }
    std::string on_ordinal_weekday(const std::string& ordinal_word, int weekday) const {
        return de ? " am " + ordinal_word + " " + this->weekday(weekday) : " on the " + ordinal_word + " " + this->weekday(weekday);
    }
    std::string on_last_day() const { return de ? " am letzten Tag" : " on the last day"; }
    std::string on_days(const std::vector<std::string>& parts, const std::string& joined) const {
        if (de) return std::string(parts.size() == 1 ? " am Tag " : " an den Tagen ") + joined;
        return std::string(" on ") + (parts.size() == 1 ? "day " : "days ") + joined;
    }
    std::string in_month(int i) const { return std::string(de ? " im " : " in ") + month(i); }
    std::string times(int n) const { return de ? ", " + std::to_string(n) + "-mal" : ", " + std::to_string(n) + (n == 1 ? " time" : " times"); }
    std::string until(int d, int m, int y) const {
        return de ? " bis " + std::to_string(d) + ". " + month(m - 1) + " " + std::to_string(y) : " until " + std::to_string(d) + " " + month(m - 1) + " " + std::to_string(y);
    }
};

std::string join_and(const std::vector<std::string>& parts, const Words& w) {
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) out += i + 1 == parts.size() ? w.and_() : ", ";
        out += parts[i];
    }
    return out;
}

}  // namespace

std::string describe_rrule(const std::string& text, Language language) {
    if (text.empty()) return std::string();
    const Words w{language == Language::German};
    const std::string fallback = w.fallback();
    const RRule r = parse_rrule(text);
    if (!r.supported || !r.by_set_pos.empty()) return fallback;

    std::string out = w.every(r.freq, r.interval);

    // "day 5" / "days 1 and 15" / "the last day"
    auto month_day_words = [&](const std::vector<int>& days, std::string* words) {
        std::vector<std::string> parts;
        for (int d : days) {
            if (d == -1) {
                if (days.size() != 1) return false;
                *words = w.on_last_day();
                return true;
            }
            if (d < 1) return false;
            parts.push_back(std::to_string(d));
        }
        *words = w.on_days(parts, join_and(parts, w));
        return true;
    };

    switch (r.freq) {
        case RRule::Freq::Daily:
            if (!r.by_day.empty() || !r.by_month_day.empty() || !r.by_month.empty()) return fallback;
            break;
        case RRule::Freq::Weekly: {
            if (!r.by_month.empty() || !r.by_month_day.empty()) return fallback;
            if (!r.by_day.empty()) {
                std::vector<RRule::ByDay> days = r.by_day;
                std::sort(days.begin(), days.end(), [&](const RRule::ByDay& a, const RRule::ByDay& b) {
                    return (a.weekday + 6) % 7 < (b.weekday + 6) % 7;   // Monday first
                });
                std::vector<std::string> names;
                for (const auto& d : days) names.push_back(w.weekday(d.weekday));
                out += w.on_weekdays(join_and(names, w));
            }
            break;
        }
        case RRule::Freq::Monthly: {
            if (!r.by_month.empty()) return fallback;
            if (!r.by_month_day.empty() && r.by_day.empty()) {
                std::string words;
                if (!month_day_words(r.by_month_day, &words)) return fallback;
                out += words;
            } else if (r.by_month_day.empty() && r.by_day.size() == 1 && r.by_day[0].ordinal != 0) {
                const std::string o = w.ordinal(r.by_day[0].ordinal);
                if (o.empty()) return fallback;
                out += w.on_ordinal_weekday(o, r.by_day[0].weekday);
            } else if (!r.by_month_day.empty() || !r.by_day.empty()) {
                return fallback;
            }
            break;
        }
        case RRule::Freq::Yearly: {
            if (r.by_month.size() > 1) return fallback;
            if (r.by_month.empty()) {
                if (!r.by_month_day.empty() || !r.by_day.empty()) return fallback;
                break;
            }
            out += w.in_month(r.by_month[0] - 1);
            if (!r.by_month_day.empty() && r.by_day.empty()) {
                std::string words;
                if (r.by_month_day.size() != 1 || !month_day_words(r.by_month_day, &words)) return fallback;
                out += words;
            } else if (r.by_month_day.empty() && r.by_day.size() == 1 && r.by_day[0].ordinal != 0) {
                const std::string o = w.ordinal(r.by_day[0].ordinal);
                if (o.empty()) return fallback;
                out += w.on_ordinal_weekday(o, r.by_day[0].weekday);
            } else if (!r.by_month_day.empty() || !r.by_day.empty()) {
                return fallback;
            }
            break;
        }
        default: return fallback;
    }
    if (r.count > 0) out += w.times(r.count);
    if (r.until) out += w.until(r.until->local.day, r.until->local.month, r.until->local.year);
    return out;
}

}  // namespace pbo
