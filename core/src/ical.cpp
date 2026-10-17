#include "pbo/ical.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <map>
#include <set>

#include "pbo/charset.h"
#include "pbo/rrule.h"

namespace pbo {

namespace {

// ---- content lines ------------------------------------------------------------------------------------------------------------

struct Prop {
    std::string name;                           // upper case
    std::map<std::string, std::string> params;  // upper-case name -> first value (quotes removed)
    std::string value;
};

std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

// index of the first `target` outside double quotes, or npos
size_t find_unquoted(const std::string& s, char target, size_t from = 0) {
    bool quoted = false;
    for (size_t i = from; i < s.size(); ++i) {
        if (s[i] == '"') quoted = !quoted;
        else if (!quoted && s[i] == target) return i;
    }
    return std::string::npos;
}

bool parse_line(const std::string& line, Prop* p) {
    const size_t colon = find_unquoted(line, ':');
    if (colon == std::string::npos || colon == 0) return false;
    const std::string head = line.substr(0, colon);
    p->value = line.substr(colon + 1);
    size_t semi = find_unquoted(head, ';');
    p->name = upper(head.substr(0, semi));
    p->params.clear();
    while (semi != std::string::npos) {
        const size_t next = find_unquoted(head, ';', semi + 1);
        const std::string part = head.substr(semi + 1, next == std::string::npos ? std::string::npos : next - semi - 1);
        const size_t eq = part.find('=');
        if (eq != std::string::npos) {
            std::string v = part.substr(eq + 1);
            const size_t comma = find_unquoted(v, ',');
            if (comma != std::string::npos) v = v.substr(0, comma);
            if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
            const std::string key = upper(part.substr(0, eq));
            if (!p->params.count(key)) p->params[key] = v;
        }
        semi = next;
    }
    return !p->name.empty();
}

std::string unescape_text(const std::string& v) {
    std::string out;
    out.reserve(v.size());
    for (size_t i = 0; i < v.size(); ++i) {
        if (v[i] == '\\' && i + 1 < v.size()) {
            const char n = v[++i];
            if (n == 'n' || n == 'N') out += '\n';
            else out += n;   // \, \; \\ and any other escaped character
        } else {
            out += v[i];
        }
    }
    return out;
}

// valid UTF-8, at most `max` bytes, cut on a character boundary
std::string clean_text(const std::string& raw, size_t max, bool* truncated) {
    std::string s = sanitize_utf8(unescape_text(raw));
    if (s.size() > max) {
        size_t cut = max;
        while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;
        s.resize(cut);
        *truncated = true;
    }
    return s;
}

// ---- date and time values -------------------------------------------------------------------------------------------------------

struct DtValue {
    LocalDateTime local;
    bool date_only = false;
    bool utc = false;
    std::string tzid_param;
};

bool parse_dt(const std::string& v, DtValue* out) {
    const size_t n = v.size();
    if (n != 8 && n != 15 && n != 16) return false;
    for (size_t i = 0; i < n; ++i) {
        if (i == 8 && v[i] == 'T') continue;
        if (i == 15 && v[i] == 'Z') continue;
        if (!std::isdigit(static_cast<unsigned char>(v[i]))) return false;
    }
    auto num = [&](size_t pos, size_t len) { return std::atoi(v.substr(pos, len).c_str()); };
    LocalDateTime t;
    t.year = num(0, 4);
    t.month = num(4, 2);
    t.day = num(6, 2);
    if (t.year < 1 || !civil::valid_date(t.year, t.month, t.day)) return false;
    out->date_only = n == 8;
    out->utc = n == 16;
    if (!out->date_only) {
        t.hour = num(9, 2);
        t.minute = num(11, 2);
        t.second = num(13, 2);
        if (t.hour > 23 || t.minute > 59 || t.second > 60) return false;
        if (t.second == 60) t.second = 59;
    }
    out->local = t;
    return true;
}

std::string key_of(const LocalDateTime& t, bool date_only) {
    char b[32];
    if (date_only) snprintf(b, sizeof b, "%04d%02d%02d", t.year, t.month, t.day);
    else snprintf(b, sizeof b, "%04d%02d%02dT%02d%02d%02d", t.year, t.month, t.day, t.hour, t.minute, t.second);
    return b;
}

bool parse_key(const std::string& k, LocalDateTime* t, bool* date_only) {
    DtValue v;
    if (k.size() != 8 && k.size() != 15) return false;
    if (!parse_dt(k, &v)) return false;
    *t = v.local;
    *date_only = v.date_only;
    return true;
}

// seconds of an RFC 5545 duration; false if malformed
bool parse_duration(const std::string& v, int64_t* seconds) {
    size_t i = 0;
    int sign = 1;
    if (i < v.size() && (v[i] == '+' || v[i] == '-')) sign = v[i++] == '-' ? -1 : 1;
    if (i >= v.size() || v[i] != 'P') return false;
    ++i;
    int64_t total = 0;
    bool in_time = false, any = false;
    while (i < v.size()) {
        if (v[i] == 'T') { in_time = true; ++i; continue; }
        int64_t n = 0;
        size_t digits = 0;
        while (i < v.size() && std::isdigit(static_cast<unsigned char>(v[i]))) {
            n = n * 10 + (v[i] - '0');
            if (n > 100000000) return false;
            ++i;
            ++digits;
        }
        if (digits == 0 || i >= v.size()) return false;
        const char u = v[i++];
        if (!in_time && u == 'W') total += n * 7 * 86400;
        else if (!in_time && u == 'D') total += n * 86400;
        else if (in_time && u == 'H') total += n * 3600;
        else if (in_time && u == 'M') total += n * 60;
        else if (in_time && u == 'S') total += n;
        else return false;
        any = true;
    }
    if (!any) return false;
    *seconds = sign * total;
    return true;
}

// Resolves a TZID parameter. "" if the zone is not known. Tolerates "/vendor/path/Area/City" forms.
std::string resolve_tzid(const TzDatabase& tz, const std::string& raw) {
    std::string name = raw;
    const std::string direct = tz.canonical_name(name);
    if (!direct.empty()) return direct;
    for (size_t slash = name.find('/'); slash != std::string::npos; slash = name.find('/', slash + 1)) {
        const std::string c = tz.canonical_name(name.substr(slash + 1));
        if (!c.empty()) return c;
    }
    return std::string();
}

// ---- one event -----------------------------------------------------------------------------------------------------------------

struct Builder {
    const TzDatabase& tz;
    const IcalLimits& lim;
    std::vector<Prop> props;

    const Prop* first(const char* name) const {
        for (const Prop& p : props)
            if (p.name == name) return &p;
        return nullptr;
    }
};

struct Zoned {
    bool ok = false;
    DtValue v;
};

void set_note(VEvent& e, const char* n) {
    if (e.note.empty()) e.note = n;
}

// A value in the zone context of the event, as a wall clock key. Z values and values with another zone are converted.
bool value_key(const TzDatabase& tz, const VEvent& e, const std::string& raw, const std::map<std::string, std::string>& params, std::string* key) {
    std::string v = raw;
    const size_t slash = v.find('/');
    if (slash != std::string::npos) v = v.substr(0, slash);   // PERIOD: the start is what counts
    DtValue d;
    if (!parse_dt(v, &d)) return false;
    if (d.date_only) {
        *key = key_of(d.local, true);
        return true;
    }
    LocalDateTime wall = d.local;
    if (!e.all_day && !e.floating && !e.tzid.empty()) {
        std::string from_zone;
        if (d.utc) from_zone = "UTC";
        else {
            auto it = params.find("TZID");
            if (it != params.end()) from_zone = resolve_tzid(tz, it->second);
        }
        if (!from_zone.empty() && from_zone != e.tzid) wall = tz.utc_to_local(e.tzid, tz.local_to_utc(from_zone, d.local));
    }
    *key = key_of(wall, e.all_day ? false : false);
    if (e.all_day) *key = key_of(wall, true);
    return true;
}

bool build_event(const Builder& b, VEvent* out, bool* ok_to_keep) {
    VEvent e;
    *ok_to_keep = false;
    bool truncated = false;
    const Prop* uid = b.first("UID");
    if (!uid || uid->value.empty()) return false;
    e.uid = sanitize_utf8(uid->value);
    if (e.uid.size() > 1024) e.uid.resize(1024);

    for (const Prop& p : b.props) {
        if (p.name == "SUMMARY" && e.summary.empty()) e.summary = clean_text(p.value, b.lim.max_text, &truncated);
        else if (p.name == "DESCRIPTION" && e.description.empty()) e.description = clean_text(p.value, b.lim.max_text, &truncated);
        else if (p.name == "LOCATION" && e.location.empty()) e.location = clean_text(p.value, b.lim.max_text, &truncated);
        else if (p.name == "STATUS" && e.status.empty()) e.status = upper(p.value.substr(0, 20));
        else if (p.name == "TRANSP" && e.transp.empty()) e.transp = upper(p.value.substr(0, 20));
        else if (p.name == "CLASS" && e.klass.empty()) e.klass = upper(p.value.substr(0, 20));
        else if (p.name == "SEQUENCE" && e.sequence == 0) e.sequence = std::max(0, std::atoi(p.value.substr(0, 9).c_str()));
        else if (p.name == "RRULE" && e.rrule.empty()) e.rrule = sanitize_utf8(p.value.substr(0, 1024));
    }
    auto person = [&](const Prop& p) {
        std::string addr = p.value;
        if (upper(addr.substr(0, 7)) == "MAILTO:") addr = addr.substr(7);
        addr = sanitize_utf8(addr.substr(0, 320));
        auto cn = p.params.find("CN");
        if (cn != p.params.end() && !cn->second.empty()) return sanitize_utf8(cn->second.substr(0, 200)) + " <" + addr + ">";
        return addr;
    };
    if (const Prop* o = b.first("ORGANIZER")) e.organizer = person(*o);
    for (const Prop& p : b.props)
        if (p.name == "ATTENDEE" && e.attendees.size() < b.lim.max_attendees) e.attendees.push_back(person(p));

    // DTSTART
    const Prop* ds = b.first("DTSTART");
    if (!ds) return false;
    DtValue start;
    if (!parse_dt(ds->value, &start)) return false;
    auto vt = ds->params.find("VALUE");
    if (vt != ds->params.end() && upper(vt->second) == "DATE" && !start.date_only) return false;
    e.all_day = start.date_only;
    e.start = start.local;
    if (!e.all_day) {
        if (start.utc) {
            e.tzid = "UTC";
        } else {
            auto z = ds->params.find("TZID");
            if (z != ds->params.end() && !z->second.empty()) {
                e.tzid = resolve_tzid(b.tz, z->second);
                if (e.tzid.empty()) {
                    e.floating = true;
                    set_note(e, "tz_unknown");
                }
            } else {
                e.floating = true;
            }
        }
        if (!e.tzid.empty()) e.start_utc = b.tz.local_to_utc(e.tzid, start.local);
    }

    // DTEND or DURATION
    const Prop* de = b.first("DTEND");
    const Prop* dur = b.first("DURATION");
    DtValue end;
    int64_t duration = 0;
    bool have_end = false;
    if (de && parse_dt(de->value, &end)) {
        have_end = true;
    } else if (dur && parse_duration(dur->value, &duration)) {
        have_end = true;
    }
    if (!have_end) set_note(e, "no_end");

    if (e.all_day) {
        int64_t days = 1;
        if (de && have_end && !dur) days = civil::days_from_civil(end.local.year, end.local.month, end.local.day) - civil::days_from_civil(start.local.year, start.local.month, start.local.day);
        else if (have_end && duration > 0) days = std::max<int64_t>(1, duration / 86400);
        if (days < 1) days = 1;
        if (days > 36500) days = 36500;
        e.end = civil::from_naive_epoch((civil::days_from_civil(start.local.year, start.local.month, start.local.day) + days) * 86400);
    } else if (!e.tzid.empty()) {
        int64_t end_utc = *e.start_utc;
        if (de && have_end && !end.date_only) {
            std::string z = end.utc ? "UTC" : e.tzid;
            auto ez = de->params.find("TZID");
            if (!end.utc && ez != de->params.end()) {
                const std::string r = resolve_tzid(b.tz, ez->second);
                if (!r.empty()) z = r;
            }
            end_utc = b.tz.local_to_utc(z, end.local);
        } else if (de && have_end) {
            end_utc = b.tz.local_to_utc(e.tzid, end.local);
        } else if (have_end) {
            end_utc = *e.start_utc + duration;
        }
        if (end_utc < *e.start_utc) end_utc = *e.start_utc;
        e.end_utc = end_utc;
        e.end = b.tz.utc_to_local(e.tzid, end_utc);
    } else {
        int64_t secs = 0;
        if (de && have_end) secs = civil::naive_epoch(end.local) - civil::naive_epoch(start.local);
        else if (have_end) secs = duration;
        if (secs < 0) secs = 0;
        e.end = civil::from_naive_epoch(civil::naive_epoch(start.local) + secs);
    }

    // recurrence
    if (!e.rrule.empty() && !parse_rrule(e.rrule).supported) set_note(e, "rrule_unsupported");
    for (const Prop& p : b.props) {
        if (p.name != "EXDATE" && p.name != "RDATE") continue;
        size_t pos = 0;
        while (pos <= p.value.size()) {
            const size_t comma = p.value.find(',', pos);
            const std::string one = p.value.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
            std::string key;
            if (!one.empty() && value_key(b.tz, e, one, p.params, &key)) {
                auto& list = p.name == "EXDATE" ? e.exdates : e.rdates;
                if (list.size() < 5000) list.push_back(key);
            }
            if (comma == std::string::npos) break;
            pos = comma + 1;
        }
    }
    if (const Prop* r = b.first("RECURRENCE-ID")) {
        std::string key;
        if (!value_key(b.tz, e, r->value, r->params, &key)) return false;
        e.recurrence_id = key;
        auto rg = r->params.find("RANGE");
        if (rg != r->params.end()) {
            e.recurrence_range = upper(rg->second.substr(0, 20));
            if (e.recurrence_range == "THISANDFUTURE" || e.recurrence_range == "THISANDPRIOR") set_note(e, "range_unsupported");
        }
    }
    if (truncated) set_note(e, "truncated");
    *out = std::move(e);
    *ok_to_keep = true;
    return true;
}

}  // namespace

namespace {

// Logical lines of an iCalendar text. `note` gets "too_many_lines" when the input has more physical lines than the limit (the lines read so
// far are returned). A line longer than the limit is dropped.
void unfold_lines(std::string_view ics, const IcalLimits& limits, std::vector<std::string>* lines, std::string* note) {
    std::string cur;
    bool have = false, overflow = false;
    size_t physical = 0;
    auto flush = [&] {
        if (have && !overflow) lines->push_back(std::move(cur));
        cur.clear();
        have = overflow = false;
    };
    size_t i = 0;
    while (i < ics.size()) {
        size_t e = i;
        while (e < ics.size() && ics[e] != '\n' && ics[e] != '\r') ++e;
        if (++physical > limits.max_lines) {
            flush();
            *note = "too_many_lines";
            return;
        }
        const std::string_view line = ics.substr(i, e - i);
        if (!line.empty() && (line[0] == ' ' || line[0] == '\t') && have) {
            if (cur.size() + line.size() > limits.max_line_bytes) overflow = true;
            else cur.append(line.substr(1));
        } else {
            flush();
            if (line.size() > limits.max_line_bytes) {
                overflow = true;
                have = true;
            } else {
                cur.assign(line);
                have = true;
            }
        }
        // line break: CRLF, LF or CR
        if (e < ics.size() && ics[e] == '\r' && e + 1 < ics.size() && ics[e + 1] == '\n') e += 2;
        else if (e < ics.size()) e += 1;
        i = e;
    }
    flush();
}

}  // namespace

std::string fold_ical_line(std::string_view line) {
    std::string out;
    size_t pos = 0, budget = 75;
    while (pos < line.size()) {
        size_t n = std::min(budget, line.size() - pos);
        if (pos + n < line.size())   // never cut inside a UTF-8 character
            while (n > 0 && (static_cast<unsigned char>(line[pos + n]) & 0xC0) == 0x80) --n;
        if (n == 0) n = std::min(budget, line.size() - pos);
        out.append(line.substr(pos, n));
        pos += n;
        if (pos < line.size()) out += "\r\n ";
        budget = 74;
    }
    out += "\r\n";
    return out;
}

std::vector<IcalPart> split_ical_by_uid(std::string_view ics, const IcalLimits& limits) {
    std::vector<std::string> lines;
    std::string note;
    if (ics.size() > limits.max_bytes) return {};
    unfold_lines(ics, limits, &lines, &note);
    std::string header;
    std::vector<IcalPart> parts;
    std::vector<std::string> block;
    int depth = 0;           // inside VEVENT (1) or another top-level component (>1 or skipped)
    bool in_event = false, in_other = false;
    int other_depth = 0;
    std::string uid;
    for (const std::string& line : lines) {
        std::string up;
        for (size_t i = 0; i < line.size() && line[i] != ':' && line[i] != ';'; ++i) up += static_cast<char>(std::toupper(static_cast<unsigned char>(line[i])));
        const size_t colon = line.find(':');
        std::string value = colon == std::string::npos ? std::string() : line.substr(colon + 1);
        std::string value_up;
        for (char c : value) value_up += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        if (up == "BEGIN") {
            if (value_up == "VCALENDAR" && !in_event && !in_other) continue;
            if (!in_event && !in_other) {
                if (value_up == "VEVENT") {
                    in_event = true;
                    depth = 1;
                    block.clear();
                    uid.clear();
                    block.push_back(line);
                } else {
                    in_other = true;
                    other_depth = 1;
                }
                continue;
            }
            if (in_event) { ++depth; block.push_back(line); }
            else ++other_depth;
            continue;
        }
        if (up == "END") {
            if (in_event) {
                block.push_back(line);
                if (--depth == 0) {
                    in_event = false;
                    if (!uid.empty()) {
                        IcalPart* part = nullptr;
                        for (IcalPart& p : parts)
                            if (p.uid == uid) part = &p;
                        if (!part) {
                            parts.push_back({uid, std::string()});
                            part = &parts.back();
                        }
                        for (const std::string& b : block) part->ics += fold_ical_line(b);
                    }
                }
            } else if (in_other) {
                if (--other_depth == 0) in_other = false;
            }
            continue;
        }
        if (in_event) {
            block.push_back(line);
            if (depth == 1 && up == "UID" && uid.empty()) uid = value;
        } else if (!in_other) {
            header += fold_ical_line(line);   // VERSION, PRODID, CALSCALE ...
        }
    }
    for (IcalPart& p : parts) p.ics = "BEGIN:VCALENDAR\r\n" + header + p.ics + "END:VCALENDAR\r\n";
    return parts;
}

ParsedCalendar parse_ical(std::string_view ics, const TzDatabase& tz, const IcalLimits& limits) {
    ParsedCalendar result;
    if (ics.size() > limits.max_bytes) {
        result.ok = false;
        result.note = "too_large";
        return result;
    }
    std::vector<std::string> lines;
    {
        std::string note;
        unfold_lines(ics, limits, &lines, &note);
        if (!note.empty()) {
            result.ok = false;
            result.note = note;
        }
    }

    std::vector<std::string> stack;
    Builder builder{tz, limits, {}};
    bool saw_calendar = false;
    size_t components = 0;
    bool stop = false;
    for (const std::string& line : lines) {
        if (line.empty()) continue;
        Prop p;
        if (!parse_line(line, &p)) continue;
        if (p.name == "BEGIN") {
            const std::string name = upper(p.value);
            if (++components > limits.max_components) {
                result.ok = false;
                result.note = "too_many_components";
                stop = true;
                break;
            }
            if (name == "VCALENDAR") saw_calendar = true;
            stack.push_back(name);
            if (name == "VEVENT") builder.props.clear();
            continue;
        }
        if (p.name == "END") {
            const std::string name = upper(p.value);
            while (!stack.empty() && stack.back() != name) stack.pop_back();   // tolerate a missing END
            if (stack.empty()) continue;
            if (name == "VEVENT" && stack.size() >= 1) {
                VEvent e;
                bool keep = false;
                build_event(builder, &e, &keep);
                if (keep) result.events.push_back(std::move(e));
                else ++result.skipped;
                builder.props.clear();
            }
            stack.pop_back();
            continue;
        }
        if (!stack.empty() && stack.back() == "VEVENT") {
            if (builder.props.size() < 20000) builder.props.push_back(std::move(p));
        }
    }
    (void)stop;
    if (!saw_calendar && result.ok) {
        result.ok = false;
        result.note = "not_a_calendar";
    }
    return result;
}

// ---- expansion ---------------------------------------------------------------------------------------------------------------------

namespace {

constexpr size_t kMaxPerSeries = 5000;

int64_t day_of(const LocalDateTime& t) { return civil::days_from_civil(t.year, t.month, t.day); }

struct Expander {
    const ParsedCalendar& res;
    const TzDatabase& tz;
    const std::string& user;
    int64_t from, to;
    std::vector<Occurrence>& out;

    bool overlaps(int64_t s, int64_t e) const { return s < to && (e > from || (e == s && s >= from)); }

    // an occurrence for the event `idx` starting at the wall clock `wall` (the event's own start for an override)
    bool make(size_t idx, const LocalDateTime& wall, const std::string& rid, Occurrence* o) const {
        const VEvent& e = res.events[idx];
        o->event_index = idx;
        o->recurrence_id = rid;
        o->all_day = e.all_day;
        if (e.all_day) {
            const int64_t days = std::max<int64_t>(1, day_of(*e.end) - day_of(*e.start));
            const LocalDateTime end = civil::from_naive_epoch((day_of(wall) + days) * 86400);
            LocalDateTime s = wall;
            s.hour = s.minute = s.second = 0;
            o->start_utc = tz.local_to_utc(user, s);
            o->end_utc = tz.local_to_utc(user, end);
            o->local_date = civil::date_text(wall.year, wall.month, wall.day);
            return true;
        }
        if (!e.tzid.empty()) {
            const int64_t dur = *e.end_utc - *e.start_utc;
            o->start_utc = tz.local_to_utc(e.tzid, wall);
            o->end_utc = o->start_utc + dur;
        } else {
            const int64_t dur = civil::naive_epoch(*e.end) - civil::naive_epoch(*e.start);
            o->start_utc = tz.local_to_utc(user, wall);
            o->end_utc = o->start_utc + dur;
        }
        const LocalDateTime l = tz.utc_to_local(user, o->start_utc);
        o->local_date = civil::date_text(l.year, l.month, l.day);
        return true;
    }

    // wall clocks of the master's instances inside the window (a little wider: exact filtering follows)
    std::vector<LocalDateTime> instances(const VEvent& m, bool* series) const {
        std::vector<LocalDateTime> v;
        const RRule rule = parse_rrule(m.rrule);
        *series = (!m.rrule.empty() && rule.supported) || !m.rdates.empty();
        if (!m.rrule.empty() && rule.supported) {
            const int64_t dur = m.all_day ? (day_of(*m.end) - day_of(*m.start)) * 86400
                                          : (m.end_utc ? *m.end_utc - *m.start_utc : civil::naive_epoch(*m.end) - civil::naive_epoch(*m.start));
            const int64_t emit_from = from - std::max<int64_t>(0, dur) - 2 * 86400;
            const int64_t stop = to + 2 * 86400;
            std::optional<int64_t> until;
            if (rule.until) {
                LocalDateTime u = rule.until->local;
                if (rule.until->date_only) {
                    u.hour = 23; u.minute = 59; u.second = 59;
                } else if (rule.until->utc && !m.tzid.empty() && !m.all_day) {
                    u = tz.utc_to_local(m.tzid, civil::naive_epoch(u));
                }
                until = civil::naive_epoch(u);
            }
            v = expand_rrule(rule, *m.start, until, emit_from, stop).starts;
        } else {
            v.push_back(*m.start);
        }
        for (const std::string& r : m.rdates) {
            LocalDateTime t;
            bool date_only = false;
            if (!parse_key(r, &t, &date_only)) continue;
            if (date_only && !m.all_day) { t.hour = m.start->hour; t.minute = m.start->minute; t.second = m.start->second; }
            v.push_back(t);
        }
        std::sort(v.begin(), v.end(), [](const LocalDateTime& a, const LocalDateTime& b) { return civil::naive_epoch(a) < civil::naive_epoch(b); });
        v.erase(std::unique(v.begin(), v.end()), v.end());
        return v;
    }
};

}  // namespace

std::vector<Occurrence> expand(const ParsedCalendar& resource, const TzDatabase& tz, const std::string& user_zone, int64_t from_utc, int64_t to_utc) {
    std::vector<Occurrence> out;
    const std::string user = tz.canonical_name(user_zone);
    if (user.empty()) return out;
    Expander x{resource, tz, user, from_utc, to_utc, out};

    std::map<std::string, std::vector<size_t>> by_uid;
    std::vector<std::string> order;
    for (size_t i = 0; i < resource.events.size(); ++i) {
        auto& v = by_uid[resource.events[i].uid];
        if (v.empty()) order.push_back(resource.events[i].uid);
        v.push_back(i);
    }
    for (const std::string& uid : order) {
        const auto& idxs = by_uid[uid];
        const VEvent* master = nullptr;
        size_t master_idx = 0;
        std::vector<size_t> overrides;
        for (size_t i : idxs) {
            if (resource.events[i].recurrence_id.empty()) {
                if (!master) { master = &resource.events[i]; master_idx = i; }
            } else {
                overrides.push_back(i);
            }
        }
        // the instances the overrides replace, as keys in the master's wall clock
        std::set<std::string> replaced;
        std::map<size_t, std::string> override_key;
        for (size_t i : overrides) {
            const VEvent& o = resource.events[i];
            std::string key = o.recurrence_id;
            if (master && key.size() == 15 && !o.tzid.empty() && !master->tzid.empty() && o.tzid != master->tzid && !master->all_day) {
                LocalDateTime t;
                bool d = false;
                if (parse_key(key, &t, &d)) key = key_of(tz.utc_to_local(master->tzid, tz.local_to_utc(o.tzid, t)), false);
            }
            override_key[i] = key;
            replaced.insert(key);
        }
        auto is_replaced = [&](const std::string& k) {
            if (replaced.count(k)) return true;
            return replaced.count(k.substr(0, 8)) > 0;
        };

        size_t emitted = 0;
        if (master && master->start && master->status != "CANCELLED") {
            bool series = false;
            const std::vector<LocalDateTime> inst = x.instances(*master, &series);
            std::set<std::string> excluded(master->exdates.begin(), master->exdates.end());
            for (const LocalDateTime& wall : inst) {
                const std::string key = key_of(wall, master->all_day);
                if (excluded.count(key) || excluded.count(key.substr(0, 8)) || is_replaced(key)) continue;
                Occurrence o;
                if (!x.make(master_idx, wall, series ? key : std::string(), &o)) continue;
                if (!x.overlaps(o.start_utc, o.end_utc)) continue;
                if (++emitted > kMaxPerSeries) break;
                out.push_back(std::move(o));
            }
        }
        for (size_t i : overrides) {
            const VEvent& o = resource.events[i];
            if (o.status == "CANCELLED" || !o.start) continue;
            Occurrence occ;
            if (!x.make(i, *o.start, override_key[i], &occ)) continue;
            if (!x.overlaps(occ.start_utc, occ.end_utc)) continue;
            out.push_back(std::move(occ));
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const Occurrence& a, const Occurrence& b) { return a.start_utc < b.start_utc; });
    return out;
}


// ---- writing events ---------------------------------------------------------------------------------------------------------------

namespace {

std::string escape_text(const std::string& v) {
    std::string out;
    out.reserve(v.size() + 8);
    for (char c : v) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case ';': out += "\\;"; break;
            case ',': out += "\\,"; break;
            case '\n': out += "\\n"; break;
            case '\r': break;
            default: out += c;
        }
    }
    return out;
}

std::string trimmed(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

std::string utc_stamp(int64_t t) {
    const LocalDateTime l = civil::from_naive_epoch(t);
    char b[32];
    snprintf(b, sizeof b, "%04d%02d%02dT%02d%02d%02dZ", l.year, l.month, l.day, l.hour, l.minute, l.second);
    return b;
}

// DTSTART / DTEND line of an edit (the zone name is the editor's canonical IANA name)
std::string time_line(const char* name, const EventEdit& e, const LocalDateTime& t) {
    char b[48];
    if (e.all_day) {
        snprintf(b, sizeof b, "%04d%02d%02d", t.year, t.month, t.day);
        return std::string(name) + ";VALUE=DATE:" + b;
    }
    snprintf(b, sizeof b, "%04d%02d%02dT%02d%02d%02d", t.year, t.month, t.day, t.hour, t.minute, t.second);
    if (e.tzid.empty()) return std::string(name) + ":" + b;
    if (e.tzid == "UTC") return std::string(name) + ":" + b + "Z";
    return std::string(name) + ";TZID=" + e.tzid + ":" + b;
}

bool same_day(const LocalDateTime& a, const LocalDateTime& b) { return a.year == b.year && a.month == b.month && a.day == b.day; }

bool same_time(const EventEdit& a, const EventEdit& b) {
    if (a.all_day != b.all_day) return false;
    if (a.all_day) return same_day(a.start, b.start) && same_day(a.end, b.end);
    return a.start == b.start && a.end == b.end && a.tzid == b.tzid;
}

struct Block {
    size_t begin, end;
};
// the VEVENT blocks of a list of content lines (line index of BEGIN and END)
std::vector<Block> vevent_blocks(const std::vector<std::string>& lines) {
    std::vector<Block> out;
    size_t open = std::string::npos;
    for (size_t i = 0; i < lines.size(); ++i) {
        Prop p;
        if (!parse_line(lines[i], &p)) continue;
        if (p.name == "BEGIN" && upper(p.value) == "VEVENT" && open == std::string::npos) {
            open = i;
        } else if (p.name == "END" && upper(p.value) == "VEVENT" && open != std::string::npos) {
            out.push_back({open, i});
            open = std::string::npos;
        }
    }
    return out;
}

// "NAME;params" part of a content line (everything before the first unquoted colon)
std::string head_of(const std::string& line) {
    const size_t colon = find_unquoted(line, ':');
    return colon == std::string::npos ? line : line.substr(0, colon);
}

}  // namespace

EventEdit event_edit_of(const VEvent& e) {
    EventEdit x;
    x.title = e.summary;
    x.location = e.location;
    x.description = e.description;
    x.all_day = e.all_day;
    x.tzid = e.all_day || e.floating ? std::string() : e.tzid;
    if (e.start) x.start = *e.start;
    x.end = e.end ? *e.end : x.start;
    return x;
}

std::string validate_event_edit(const EventEdit& e, const TzDatabase& tz) {
    if (trimmed(e.title).empty()) return "title_missing";
    if (e.title.size() > 1000 || e.location.size() > 1000 || e.description.size() > (64u << 10)) return "too_long";
    auto bad = [](const LocalDateTime& t, bool date_only) {
        if (t.year < 1 || t.year > 9999 || !civil::valid_date(t.year, t.month, t.day)) return true;
        return !date_only && (t.hour < 0 || t.hour > 23 || t.minute < 0 || t.minute > 59 || t.second < 0 || t.second > 59);
    };
    if (bad(e.start, e.all_day) || bad(e.end, e.all_day)) return "date_invalid";
    if (!e.all_day && !e.tzid.empty() && e.tzid != "UTC" && !tz.has_zone(e.tzid)) return "zone_unknown";
    if (e.all_day) {
        if (civil::days_from_civil(e.end.year, e.end.month, e.end.day) <= civil::days_from_civil(e.start.year, e.start.month, e.start.day)) return "end_before_start";
    } else if (civil::naive_epoch(e.end) < civil::naive_epoch(e.start)) {
        return "end_before_start";
    }
    return "";
}

bool is_series(std::string_view raw_ics, const TzDatabase& tz) {
    const ParsedCalendar p = parse_ical(raw_ics, tz);
    if (p.events.size() > 1) return true;
    for (const VEvent& e : p.events)
        if (!e.rrule.empty() || !e.rdates.empty() || !e.recurrence_id.empty()) return true;
    return false;
}

std::string make_event_uid(Random& random) {
    uint8_t b[16];
    random.bytes(b, sizeof b);
    static const char* hex = "0123456789abcdef";
    std::string out;
    for (uint8_t c : b) {
        out += hex[c >> 4];
        out += hex[c & 15];
    }
    return out + "@pboffice.invalid";
}

std::string new_event_ics(const EventEdit& e, const std::string& uid, int64_t now_utc) {
    std::vector<std::string> lines = {"BEGIN:VCALENDAR", "VERSION:2.0", "PRODID:-//office-pocketbook//EN", "CALSCALE:GREGORIAN", "BEGIN:VEVENT",
                                      "UID:" + uid, "DTSTAMP:" + utc_stamp(now_utc), "CREATED:" + utc_stamp(now_utc),
                                      "LAST-MODIFIED:" + utc_stamp(now_utc), "SEQUENCE:0",
                                      time_line("DTSTART", e, e.start), time_line("DTEND", e, e.end), "SUMMARY:" + escape_text(e.title)};
    if (!e.location.empty()) lines.push_back("LOCATION:" + escape_text(e.location));
    if (!e.description.empty()) lines.push_back("DESCRIPTION:" + escape_text(e.description));
    lines.push_back("END:VEVENT");
    lines.push_back("END:VCALENDAR");
    std::string out;
    for (const std::string& l : lines) out += fold_ical_line(l);
    return out;
}

EditResult apply_edit(std::string_view raw_ics, const EventEdit& edit, const TzDatabase& tz, int64_t now_utc) {
    EditResult r;
    r.ics = std::string(raw_ics);
    IcalLimits limits;
    if (raw_ics.size() > limits.max_bytes) {
        r.error = "not_editable";
        return r;
    }
    std::vector<std::string> lines;
    std::string note;
    unfold_lines(raw_ics, limits, &lines, &note);
    const std::vector<Block> blocks = vevent_blocks(lines);
    if (!note.empty() || blocks.empty()) {
        r.error = "not_editable";
        return r;
    }
    if (blocks.size() > 1 || is_series(raw_ics, tz)) {
        r.error = "series_read_only";
        return r;
    }
    if (const std::string bad = validate_event_edit(edit, tz); !bad.empty()) {
        r.error = bad;
        return r;
    }
    const ParsedCalendar parsed = parse_ical(raw_ics, tz);
    if (parsed.events.size() != 1) {
        r.error = "not_editable";
        return r;
    }
    const EventEdit current = event_edit_of(parsed.events[0]);
    const bool title_changed = edit.title != current.title, location_changed = edit.location != current.location,
               description_changed = edit.description != current.description, time_changed = !same_time(edit, current);
    if (!title_changed && !location_changed && !description_changed && !time_changed) {
        r.ok = true;
        r.changed = false;
        return r;
    }

    const Block blk = blocks[0];
    std::vector<std::string> out(lines.begin(), lines.begin() + static_cast<std::ptrdiff_t>(blk.begin) + 1);
    bool did_title = false, did_location = false, did_description = false, did_time = false, did_seq = false, did_stamp = false, did_mod = false;
    int depth = 0;
    const std::string stamp = utc_stamp(now_utc);
    auto text_line = [&](const std::string& old, const char* name, const std::string& value) {
        // keep the parameters of the old line (LANGUAGE=...); an empty value removes the property
        if (value.empty()) return std::string();
        const std::string head = old.empty() ? std::string(name) : head_of(old);
        return head + ":" + escape_text(value);
    };
    for (size_t i = blk.begin + 1; i < blk.end; ++i) {
        const std::string& line = lines[i];
        Prop p;
        if (!parse_line(line, &p)) {
            out.push_back(line);
            continue;
        }
        if (p.name == "BEGIN") {
            ++depth;
            out.push_back(line);
            continue;
        }
        if (p.name == "END") {
            if (depth > 0) --depth;
            out.push_back(line);
            continue;
        }
        if (depth > 0) {   // inside VALARM and the like: not ours
            out.push_back(line);
            continue;
        }
        auto replace_text = [&](bool changed, bool& done, const std::string& value) {
            if (!changed) {
                out.push_back(line);
                return;
            }
            if (!done) {
                done = true;
                const std::string l = text_line(line, p.name.c_str(), value);
                if (!l.empty()) out.push_back(l);
            }   // a second SUMMARY/LOCATION/DESCRIPTION of an edited property is dropped
        };
        if (p.name == "SUMMARY") replace_text(title_changed, did_title, edit.title);
        else if (p.name == "LOCATION") replace_text(location_changed, did_location, edit.location);
        else if (p.name == "DESCRIPTION") replace_text(description_changed, did_description, edit.description);
        else if (p.name == "DTSTART" && time_changed) {
            if (!did_time) {
                did_time = true;
                out.push_back(time_line("DTSTART", edit, edit.start));
                out.push_back(time_line("DTEND", edit, edit.end));
            }
        } else if ((p.name == "DTEND" || p.name == "DURATION") && time_changed) {
            // replaced together with DTSTART
        } else if (p.name == "SEQUENCE") {
            if (!did_seq) {
                did_seq = true;
                out.push_back("SEQUENCE:" + std::to_string(std::max(0, std::atoi(p.value.substr(0, 9).c_str())) + 1));
            }
        } else if (p.name == "DTSTAMP") {
            if (!did_stamp) {
                did_stamp = true;
                out.push_back("DTSTAMP:" + stamp);
            }
        } else if (p.name == "LAST-MODIFIED") {
            if (!did_mod) {
                did_mod = true;
                out.push_back("LAST-MODIFIED:" + stamp);
            }
        } else {
            out.push_back(line);
        }
    }
    if (!did_title && title_changed) out.push_back("SUMMARY:" + escape_text(edit.title));
    if (!did_location && location_changed && !edit.location.empty()) out.push_back("LOCATION:" + escape_text(edit.location));
    if (!did_description && description_changed && !edit.description.empty()) out.push_back("DESCRIPTION:" + escape_text(edit.description));
    if (!did_seq) out.push_back("SEQUENCE:1");
    if (!did_stamp) out.push_back("DTSTAMP:" + stamp);
    if (!did_mod) out.push_back("LAST-MODIFIED:" + stamp);
    out.insert(out.end(), lines.begin() + static_cast<std::ptrdiff_t>(blk.end), lines.end());

    std::string text;
    for (const std::string& l : out) text += fold_ical_line(l);
    r.ok = true;
    r.changed = true;
    r.ics = std::move(text);
    return r;
}

std::string replace_ical_uid(std::string_view raw_ics, const std::string& new_uid) {
    IcalLimits limits;
    std::vector<std::string> lines;
    std::string note;
    unfold_lines(raw_ics, limits, &lines, &note);
    std::string out;
    int depth = 0;   // inside VEVENT only (a VALARM has no UID, but stay exact)
    bool in_event = false;
    for (const std::string& line : lines) {
        Prop p;
        if (parse_line(line, &p)) {
            if (p.name == "BEGIN") {
                if (upper(p.value) == "VEVENT") in_event = true;
                else if (in_event) ++depth;
            } else if (p.name == "END") {
                if (upper(p.value) == "VEVENT") in_event = false;
                else if (in_event && depth > 0) --depth;
            } else if (in_event && depth == 0 && p.name == "UID") {
                out += fold_ical_line("UID:" + new_uid);
                continue;
            }
        }
        out += fold_ical_line(line);
    }
    return out;
}

}  // namespace pbo
