// T175: dates and weekday names follow the language of the interface (English, or German when the German texts are on).
#include <QCoreApplication>
#include <QTest>

#include "calendar_text.h"
#include "test_support.h"

namespace {
int g_argc = 1;
char g_arg0[] = "test_calendar_text";
char* g_argv[] = {g_arg0, nullptr};
QCoreApplication g_app(g_argc, g_argv);

struct English {   // every test starts and ends in English
    English() { caltext::set_german(false); }
    ~English() { caltext::set_german(false); }
};
}  // namespace

PB_TEST(english_dates_are_the_default) {
    English guard;
    PB_CHECK_EQ(caltext::long_date(2026, 10, 14).toStdString(), std::string("Wednesday, 14 October 2026"));
    PB_CHECK_EQ(caltext::short_date(2026, 10, 14).toStdString(), std::string("Wed 14 Oct"));
    PB_CHECK_EQ(caltext::short_month(3).toStdString(), std::string("Mar"));
}

PB_TEST(german_dates_use_german_names_and_the_german_order) {
    English guard;
    caltext::set_german(true);
    PB_CHECK_EQ(caltext::long_date(2026, 10, 14).toStdString(), std::string("Mittwoch, 14. Oktober 2026"));
    PB_CHECK_EQ(caltext::short_date(2026, 10, 14).toStdString(), std::string("Mi 14. Okt"));
    PB_CHECK_EQ(caltext::long_date(2026, 3, 1).toStdString(), std::string("Sonntag, 1. M\xC3\xA4rz 2026"));
    PB_CHECK_EQ(caltext::short_month(3).toStdString(), std::string("M\xC3\xA4r"));
    PB_CHECK_EQ(caltext::short_month(9).toStdString(), std::string("Sep"));
    PB_CHECK_EQ(caltext::month_title(2026, 12).toStdString(), std::string("Dezember 2026"));
}

PB_TEST(english_month_titles_keep_their_form) {
    English guard;
    PB_CHECK_EQ(caltext::month_title(2026, 10).toStdString(), std::string("October 2026"));
}

PB_TEST_MAIN()
