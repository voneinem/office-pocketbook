// T169: the limits of the settings, at the core (the bridge test covers the page). Every value at its edge, and a refused value leaves the stored account as it was.
#include <memory>
#include <string>

#include "pbo/account_input.h"
#include "pbo/store.h"
#include "pbo/tz.h"
#include "test_support.h"
#include "tmp_dir.h"

namespace {

struct Clock : pbo::Clock {
    int64_t now() const override { return 1791979200; }
};

pbo::MailAccount valid_account() {
    pbo::MailAccount a;
    a.imap_host = "mail.example.org";
    a.smtp_host = "smtp.example.org";
    a.username = "alice";
    a.address = "alice@example.org";
    return a;
}

bool refused_for(const pbo::MailAccount& a, const std::string& field) {
    for (const auto& e : pbo::validate_mail_account(a))
        if (e.field == field) return e.key == "value_out_of_range";
    return false;
}

}  // namespace

PB_TEST(the_days_of_mail_kept_run_from_1_to_365) {
    pbo::MailAccount a = valid_account();
    for (int ok : {1, 2, 30, 364, 365}) {
        a.sync_days = ok;
        PB_CHECK(pbo::validate_mail_account(a).empty());
    }
    for (int bad : {-1, 0, 366, 100000}) {
        a.sync_days = bad;
        PB_CHECK(refused_for(a, "sync_days"));
    }
}

PB_TEST(the_largest_attachment_runs_from_1_to_100_mib) {
    pbo::MailAccount a = valid_account();
    for (int64_t ok : {int64_t(1) << 20, int64_t(10) << 20, int64_t(100) << 20}) {
        a.attachment_limit = ok;
        PB_CHECK(pbo::validate_mail_account(a).empty());
    }
    for (int64_t bad : {int64_t(0), (int64_t(1) << 20) - 1, (int64_t(100) << 20) + 1, int64_t(-5)}) {
        a.attachment_limit = bad;
        PB_CHECK(refused_for(a, "attachment_limit"));
    }
}

PB_TEST(the_space_for_mail_is_at_least_20_mib) {
    pbo::MailAccount a = valid_account();
    for (int64_t ok : {int64_t(20) << 20, int64_t(200) << 20, int64_t(4000) << 20}) {
        a.storage_limit = ok;
        PB_CHECK(pbo::validate_mail_account(a).empty());
    }
    for (int64_t bad : {int64_t(0), (int64_t(20) << 20) - 1, int64_t(-1)}) {
        a.storage_limit = bad;
        PB_CHECK(refused_for(a, "storage_limit"));
    }
}

PB_TEST(a_refused_value_leaves_the_stored_account_as_it_was) {
    pbtest::TmpDir dir;
    Clock clock;
    auto store = std::move(pbo::Store::open(dir.path(), clock).value);
    pbo::MailAccount a = valid_account();
    a.sync_days = 45;
    PB_CHECK(store->save_mail_account(a).ok());
    a.sync_days = 400;
    a.attachment_limit = 5;
    const pbo::Error e = store->save_mail_account(a);
    PB_CHECK(!e.ok());
    PB_CHECK_EQ(e.key, std::string("invalid_account"));
    const auto stored = store->mail_account();
    PB_CHECK(stored.has_value());
    PB_CHECK_EQ(stored->sync_days, 45);
    PB_CHECK_EQ(stored->attachment_limit, valid_account().attachment_limit);
}

PB_TEST(the_switch_for_sent_mail_is_stored_and_read_back) {
    pbtest::TmpDir dir;
    Clock clock;
    auto store = std::move(pbo::Store::open(dir.path(), clock).value);
    pbo::MailAccount a = valid_account();
    a.sent_by_server = true;
    PB_CHECK(store->save_mail_account(a).ok());
    PB_CHECK(store->mail_account()->sent_by_server);
    a.sent_by_server = false;
    PB_CHECK(store->save_mail_account(a).ok());
    PB_CHECK(!store->mail_account()->sent_by_server);
}

PB_TEST(the_time_zone_override_must_be_a_bundled_zone) {
    auto tz = pbo::TzDatabase::open(std::string(PBO_SOURCE_DIR) + "/third_party/tzdata/zoneinfo");
    PB_CHECK(tz != nullptr);
    PB_CHECK_EQ(tz->canonical_name("Europe/Berlin"), std::string("Europe/Berlin"));
    PB_CHECK(tz->has_zone("America/New_York"));
    for (const char* bad : {"", "Mars/Olympus_Mons", "../../etc/passwd", "Europe/Berlin/../Paris", "Europe\\Berlin", "Not A Zone"}) PB_CHECK(!tz->has_zone(bad));
}

PB_TEST_MAIN()
