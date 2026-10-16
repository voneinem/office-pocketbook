// T095 (part): the words for a repeating event (FR-022): "every week on Monday and Thursday until 30 June 2027".
#include <string>

#include "pbo/rrule.h"
#include "test_support.h"

namespace {
std::string d(const char* rule) { return pbo::describe_rrule(rule); }
}  // namespace

PB_TEST(the_common_rules_are_put_into_words) {
    PB_CHECK_EQ(d("FREQ=WEEKLY;BYDAY=MO,TH;UNTIL=20270630"), std::string("every week on Monday and Thursday until 30 June 2027"));
    PB_CHECK_EQ(d("FREQ=DAILY"), std::string("every day"));
    PB_CHECK_EQ(d("FREQ=DAILY;INTERVAL=3"), std::string("every 3 days"));
    PB_CHECK_EQ(d("FREQ=WEEKLY"), std::string("every week"));
    PB_CHECK_EQ(d("FREQ=WEEKLY;INTERVAL=2;BYDAY=MO"), std::string("every 2 weeks on Monday"));
    PB_CHECK_EQ(d("FREQ=WEEKLY;BYDAY=MO,WE,FR"), std::string("every week on Monday, Wednesday and Friday"));
    PB_CHECK_EQ(d("FREQ=MONTHLY"), std::string("every month"));
    PB_CHECK_EQ(d("FREQ=MONTHLY;BYMONTHDAY=15"), std::string("every month on day 15"));
    PB_CHECK_EQ(d("FREQ=MONTHLY;BYMONTHDAY=1,15"), std::string("every month on days 1 and 15"));
    PB_CHECK_EQ(d("FREQ=MONTHLY;BYMONTHDAY=-1"), std::string("every month on the last day"));
    PB_CHECK_EQ(d("FREQ=MONTHLY;BYDAY=2TU"), std::string("every month on the second Tuesday"));
    PB_CHECK_EQ(d("FREQ=MONTHLY;INTERVAL=3;BYDAY=-1FR"), std::string("every 3 months on the last Friday"));
    PB_CHECK_EQ(d("FREQ=YEARLY"), std::string("every year"));
    PB_CHECK_EQ(d("FREQ=YEARLY;BYMONTH=3;BYDAY=-1SU"), std::string("every year in March on the last Sunday"));
    PB_CHECK_EQ(d("FREQ=YEARLY;BYMONTH=11;BYMONTHDAY=5"), std::string("every year in November on day 5"));
}

PB_TEST(count_and_until_end_the_sentence) {
    PB_CHECK_EQ(d("FREQ=DAILY;COUNT=5"), std::string("every day, 5 times"));
    PB_CHECK_EQ(d("FREQ=DAILY;COUNT=1"), std::string("every day, 1 time"));
    PB_CHECK_EQ(d("FREQ=WEEKLY;BYDAY=TU;UNTIL=20271231T235959Z"), std::string("every week on Tuesday until 31 December 2027"));
    PB_CHECK_EQ(d("FREQ=DAILY;UNTIL=20260105"), std::string("every day until 5 January 2026"));
}

PB_TEST(rules_that_are_too_rich_for_a_short_sentence_or_outside_the_subset_say_so) {
    for (const char* rule : {"FREQ=MONTHLY;BYDAY=MO,TU,WE,TH,FR;BYSETPOS=-1", "FREQ=YEARLY;BYMONTH=1,7;BYMONTHDAY=10", "FREQ=YEARLY;BYWEEKNO=20",
                             "FREQ=HOURLY", "FREQ=WEEKLY;BYMONTH=6;BYDAY=MO", "garbage", "FREQ=MONTHLY;BYDAY=MO", "FREQ=YEARLY;BYDAY=20MO"})
        PB_CHECK_EQ(d(rule), std::string("repeats (details not shown)"));
    PB_CHECK_EQ(d(""), std::string(""));
}

PB_TEST(the_same_rules_are_put_into_german_words) {
    auto g = [](const char* rule) { return pbo::describe_rrule(rule, pbo::Language::German); };
    PB_CHECK_EQ(g("FREQ=WEEKLY;BYDAY=MO,TH;UNTIL=20270630"), std::string("jede Woche am Montag und Donnerstag bis 30. Juni 2027"));
    PB_CHECK_EQ(g("FREQ=DAILY"), std::string("jeden Tag"));
    PB_CHECK_EQ(g("FREQ=DAILY;INTERVAL=3"), std::string("alle 3 Tage"));
    PB_CHECK_EQ(g("FREQ=WEEKLY;INTERVAL=2;BYDAY=MO"), std::string("alle 2 Wochen am Montag"));
    PB_CHECK_EQ(g("FREQ=WEEKLY;BYDAY=MO,WE,FR"), std::string("jede Woche am Montag, Mittwoch und Freitag"));
    PB_CHECK_EQ(g("FREQ=MONTHLY"), std::string("jeden Monat"));
    PB_CHECK_EQ(g("FREQ=MONTHLY;BYMONTHDAY=15"), std::string("jeden Monat am Tag 15"));
    PB_CHECK_EQ(g("FREQ=MONTHLY;BYMONTHDAY=1,15"), std::string("jeden Monat an den Tagen 1 und 15"));
    PB_CHECK_EQ(g("FREQ=MONTHLY;BYMONTHDAY=-1"), std::string("jeden Monat am letzten Tag"));
    PB_CHECK_EQ(g("FREQ=MONTHLY;BYDAY=2TU"), std::string("jeden Monat am zweiten Dienstag"));
    PB_CHECK_EQ(g("FREQ=MONTHLY;INTERVAL=3;BYDAY=-1FR"), std::string("alle 3 Monate am letzten Freitag"));
    PB_CHECK_EQ(g("FREQ=YEARLY"), std::string("jedes Jahr"));
    PB_CHECK_EQ(g("FREQ=YEARLY;BYMONTH=3;BYDAY=-1SU"), std::string("jedes Jahr im M\xC3\xA4rz am letzten Sonntag"));
    PB_CHECK_EQ(g("FREQ=YEARLY;BYMONTH=11;BYMONTHDAY=5"), std::string("jedes Jahr im November am Tag 5"));
    PB_CHECK_EQ(g("FREQ=DAILY;COUNT=5"), std::string("jeden Tag, 5-mal"));
    PB_CHECK_EQ(g("FREQ=DAILY;UNTIL=20260105"), std::string("jeden Tag bis 5. Januar 2026"));
    PB_CHECK_EQ(g("FREQ=HOURLY"), std::string("wiederholt sich (Einzelheiten nicht gezeigt)"));
    PB_CHECK_EQ(g(""), std::string(""));
}

PB_TEST_MAIN()
