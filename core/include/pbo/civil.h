#pragma once
// Calendar arithmetic without time zones (proleptic Gregorian calendar): days since 1970-01-01 and back, weekday, month
// lengths. The base of the time-zone, iCalendar and recurrence code. Pure and header-only.

#include <cstdint>
#include <cstdio>
#include <string>

namespace pbo {

struct LocalDateTime {
    int year = 1970, month = 1, day = 1, hour = 0, minute = 0, second = 0;
    friend bool operator==(const LocalDateTime& a, const LocalDateTime& b) {
        return a.year == b.year && a.month == b.month && a.day == b.day && a.hour == b.hour && a.minute == b.minute && a.second == b.second;
    }
    friend bool operator!=(const LocalDateTime& a, const LocalDateTime& b) { return !(a == b); }
};

namespace civil {

inline bool is_leap(int64_t y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

inline int days_in_month(int64_t y, int m) {
    static const int d[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return m == 2 && is_leap(y) ? 29 : d[m - 1];
}

inline bool valid_date(int64_t y, int m, int d) { return m >= 1 && m <= 12 && d >= 1 && d <= days_in_month(y, m); }

// Days since 1970-01-01 (Howard Hinnant's algorithm)
inline int64_t days_from_civil(int64_t y, int m, int d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yoe = y - era * 400;
    const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

inline void civil_from_days(int64_t z, int* y, int* m, int* d) {
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const int64_t doe = z - era * 146097;
    const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t yy = yoe + era * 400;
    const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const int64_t mp = (5 * doy + 2) / 153;
    *d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
    *m = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
    *y = static_cast<int>(yy + (*m <= 2));
}

// 0 = Sunday ... 6 = Saturday
inline int weekday_from_days(int64_t days) {
    const int64_t w = (days + 4) % 7;   // 1970-01-01 was a Thursday
    return static_cast<int>(w >= 0 ? w : w + 7);
}

inline int64_t floor_div(int64_t a, int64_t b) { return a / b - ((a % b != 0) && ((a < 0) != (b < 0)) ? 1 : 0); }

// The wall-clock reading as if it were UTC
inline int64_t naive_epoch(const LocalDateTime& t) {
    return days_from_civil(t.year, t.month, t.day) * 86400 + t.hour * 3600 + t.minute * 60 + t.second;
}

inline LocalDateTime from_naive_epoch(int64_t e) {
    const int64_t days = floor_div(e, 86400);
    const int64_t rem = e - days * 86400;
    LocalDateTime t;
    civil_from_days(days, &t.year, &t.month, &t.day);
    t.hour = static_cast<int>(rem / 3600);
    t.minute = static_cast<int>((rem % 3600) / 60);
    t.second = static_cast<int>(rem % 60);
    return t;
}

inline std::string date_text(int y, int m, int d) {
    char buf[16];
    snprintf(buf, sizeof buf, "%04d-%02d-%02d", y, m, d);
    return buf;
}

}  // namespace civil
}  // namespace pbo
