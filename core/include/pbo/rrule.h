#pragma once
// Recurrence rules (RFC 5545 section 3.3.10, the subset of contracts/caldav-ical-subset.md section 4). Pure wall-clock arithmetic: no
// time zones here. The caller converts instances to instants.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "pbo/civil.h"

namespace pbo {

struct RRule {
    enum class Freq { None, Daily, Weekly, Monthly, Yearly };
    Freq freq = Freq::None;
    int interval = 1;
    int count = 0;                       // 0 = no COUNT
    struct Until {
        LocalDateTime local;             // the value as written (a date has 00:00:00)
        bool date_only = false;
        bool utc = false;                // written with Z
    };
    std::optional<Until> until;
    int wkst = 1;                        // 0 = Sunday ... 6 = Saturday; default Monday
    struct ByDay {
        int ordinal = 0;                 // 0 = every, 1..53 = from the start, -1..-53 = from the end
        int weekday = 0;                 // 0 = Sunday ... 6 = Saturday
    };
    std::vector<ByDay> by_day;
    std::vector<int> by_month_day;       // 1..31 or -31..-1
    std::vector<int> by_month;           // 1..12
    std::vector<int> by_set_pos;         // 1..366 or -366..-1
    // false if the rule uses a part outside the subset (SECONDLY/MINUTELY/HOURLY, BYWEEKNO, BYYEARDAY, BYHOUR, BYMINUTE, BYSECOND,
    // RSCALE, SKIP, ...) or is malformed; the event then has a single occurrence at DTSTART and the note "rrule_unsupported"
    bool supported = false;
};

RRule parse_rrule(const std::string& text);   // total: never throws; `supported` says whether it can be expanded

struct RRuleLimits {
    size_t max_instances = 5000;     // returned per call
    size_t max_steps = 100000;       // periods looked at: a hostile rule cannot hang the sync
};

struct RRuleResult {
    std::vector<LocalDateTime> starts;   // wall-clock starts in time order, DTSTART included when the rule produces it
    bool capped = false;                 // stopped at max_instances
    bool guard_hit = false;              // stopped at max_steps
};

// Instances of `rule` starting at `dtstart`, in order, with these bounds (all as wall-clock seconds, civil::naive_epoch):
//  - COUNT counts every instance from DTSTART on; instances before `emit_from` are counted but not returned
//  - nothing after `until` (inclusive) or after `stop`
RRuleResult expand_rrule(const RRule& rule, const LocalDateTime& dtstart, std::optional<int64_t> until, int64_t emit_from, int64_t stop,
                         const RRuleLimits& limits = RRuleLimits());

enum class Language { English, German };

// "every week on Monday and Thursday until 30 June 2027" for a supported rule, else "repeats (details not shown)"; German: "jede Woche am Montag und
// Donnerstag bis 30. Juni 2027" / "wiederholt sich (Einzelheiten nicht gezeigt)"
std::string describe_rrule(const std::string& rrule, Language language = Language::English);

}  // namespace pbo
