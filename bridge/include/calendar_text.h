#pragma once
// Texts and small helpers shared by the calendar bridges (English, or German after set_german(true); the strings of the pages are translated in QML, but dates are
// formatted here because they need the user's zone and the bundled time-zone database). No Qt types in the signatures except QString.

#include <QCoreApplication>
#include <QDateTime>
#include <QString>

#include "pbo/calendar_sync.h"
#include "pbo/civil.h"
#include "pbo/store.h"
#include "pbo/tz.h"

namespace caltext {

// the language of the dates; set by the app when the interface language changes
inline bool& german_flag() {
    static bool on = false;
    return on;
}
inline void set_german(bool on) { german_flag() = on; }

inline const char* weekday_name(int wd) {   // 0 = Sunday
    static const char* en[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
    static const char* de[] = {"Sonntag", "Montag", "Dienstag", "Mittwoch", "Donnerstag", "Freitag", "Samstag"};
    return (german_flag() ? de : en)[((wd % 7) + 7) % 7];
}
inline const char* month_name(int m) {
    static const char* en[] = {"January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"};
    static const char* de[] = {"Januar", "Februar", "M\xC3\xA4rz", "April", "Mai", "Juni", "Juli", "August", "September", "Oktober", "November", "Dezember"};
    return (german_flag() ? de : en)[(m - 1 + 12) % 12];
}
// "Wed" / "Mi", "Oct" / "Okt": the short forms
inline QString short_weekday(int wd) {
    static const char* de[] = {"So", "Mo", "Di", "Mi", "Do", "Fr", "Sa"};
    return german_flag() ? QString::fromLatin1(de[((wd % 7) + 7) % 7]) : QString::fromLatin1(weekday_name(wd)).left(3);
}
inline QString short_month(int m) {
    static const char* de[] = {"Jan", "Feb", "M\xC3\xA4r", "Apr", "Mai", "Jun", "Jul", "Aug", "Sep", "Okt", "Nov", "Dez"};
    return german_flag() ? QString::fromUtf8(de[(m - 1 + 12) % 12]) : QString::fromLatin1(month_name(m)).left(3);
}
// "October 2026" / "Oktober 2026"
inline QString month_title(int y, int m) { return QStringLiteral("%1 %2").arg(QString::fromUtf8(month_name(m))).arg(y); }

inline QString two(int v) { return QStringLiteral("%1").arg(v, 2, 10, QLatin1Char('0')); }
inline QString clock_text(const pbo::LocalDateTime& t) { return two(t.hour) + QLatin1Char(':') + two(t.minute); }
inline QString date_key(const pbo::LocalDateTime& t) { return QStringLiteral("%1-%2-%3").arg(t.year, 4, 10, QLatin1Char('0')).arg(t.month, 2, 10, QLatin1Char('0')).arg(t.day, 2, 10, QLatin1Char('0')); }

// "Wednesday, 14 October 2026" / "Mittwoch, 14. Oktober 2026"
inline QString long_date(int y, int m, int d) {
    const int wd = pbo::civil::weekday_from_days(pbo::civil::days_from_civil(y, m, d));
    return QString::fromUtf8(weekday_name(wd)) + QStringLiteral(", ") + QString::number(d) + (german_flag() ? QStringLiteral(". ") : QStringLiteral(" ")) +
           QString::fromUtf8(month_name(m)) + QLatin1Char(' ') + QString::number(y);
}
// "Wed 14 Oct" / "Mi 14. Okt"
inline QString short_date(int y, int m, int d) {
    const int wd = pbo::civil::weekday_from_days(pbo::civil::days_from_civil(y, m, d));
    return short_weekday(wd) + QLatin1Char(' ') + QString::number(d) + (german_flag() ? QStringLiteral(". ") : QStringLiteral(" ")) + short_month(m);
}

// "5 Oct, 09:30" / "5. Okt, 09:30": a moment in the reader's local time, for lists of drafts
inline QString short_stamp(int64_t epoch) {
    const QDateTime t = QDateTime::fromSecsSinceEpoch(epoch).toLocalTime();
    return QString::number(t.date().day()) + (german_flag() ? QStringLiteral(". ") : QStringLiteral(" ")) + short_month(t.date().month()) + QStringLiteral(", ") +
           t.time().toString(QStringLiteral("HH:mm"));
}

// "YYYY-MM-DD" -> days since 1970; false if not a valid date
inline bool parse_date(const QString& s, int* y, int* m, int* d) {
    if (s.size() != 10 || s[4] != QLatin1Char('-') || s[7] != QLatin1Char('-')) return false;
    bool ok1, ok2, ok3;
    *y = s.mid(0, 4).toInt(&ok1);
    *m = s.mid(5, 2).toInt(&ok2);
    *d = s.mid(8, 2).toInt(&ok3);
    return ok1 && ok2 && ok3 && *y >= 1 && pbo::civil::valid_date(*y, *m, *d);
}

inline QString date_of_days(int64_t days) {
    int y, m, d;
    pbo::civil::civil_from_days(days, &y, &m, &d);
    return QStringLiteral("%1-%2-%3").arg(y, 4, 10, QLatin1Char('0')).arg(m, 2, 10, QLatin1Char('0')).arg(d, 2, 10, QLatin1Char('0'));
}

// The time of an occurrence for a list row: "All day" or "09:30 – 10:00" (the end carries its date when it is another day).
inline QString time_text(const pbo::TzDatabase& tz, const std::string& zone, int64_t start_utc, int64_t end_utc, bool all_day) {
    if (all_day) return QCoreApplication::translate("qml", "All day");
    const pbo::LocalDateTime s = tz.utc_to_local(zone, start_utc);
    const pbo::LocalDateTime e = tz.utc_to_local(zone, end_utc);
    if (end_utc <= start_utc) return clock_text(s);
    if (s.year == e.year && s.month == e.month && s.day == e.day) return clock_text(s) + QStringLiteral(" – ") + clock_text(e);
    return clock_text(s) + QStringLiteral(" – ") + QString::number(e.day) + (german_flag() ? QStringLiteral(". ") : QStringLiteral(" ")) + short_month(e.month) + QLatin1Char(' ') + clock_text(e);
}

// The user's zone, "" while unknown. `environment` (tests) replaces the real TZ and /etc/localtime.
inline std::string zone_of(const pbo::Store* store, const pbo::TzDatabase* tz, pbo::ZoneEnvironment (*environment)()) {
    if (!store || !tz) return std::string();
    if (!environment) return pbo::user_zone_name(*store, *tz);
    const pbo::ZoneEnvironment env = environment();
    return pbo::user_zone_name(*store, *tz, &env);
}

inline bool pending_state(const std::string& s) { return s == "pending_create" || s == "pending_update" || s == "pending_delete"; }
inline bool conflict_state(const std::string& s) { return s == "conflict" || s == "server_deleted"; }

}  // namespace caltext
