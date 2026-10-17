// T035: validation of the mail account form (data-model.md: account_mail, "validated on entry"). A rejected value is
// reported with a stable key and never reaches the store.
#include <algorithm>
#include <string>

#include "pbo/account_input.h"
#include "pbo/store.h"
#include "test_support.h"
#include "tmp_dir.h"

namespace {

pbo::MailAccountInput good() {
    pbo::MailAccountInput in;
    in.account.imap_host = "mail.example.org";
    in.account.imap_port = 993;
    in.account.smtp_host = "smtp.example.org";
    in.account.smtp_port = 465;
    in.account.username = "alice";
    in.account.address = "alice@example.org";
    in.imap_password = "pw-for-tests";
    return in;
}

bool has_error(const std::vector<pbo::FieldError>& errors, const std::string& field, const std::string& key) {
    return std::any_of(errors.begin(), errors.end(), [&](const pbo::FieldError& e) { return e.field == field && e.key == key; });
}

struct FixedClock : pbo::Clock {
    int64_t now() const override { return 1791201600; }
};

}  // namespace

PB_TEST(a_normal_account_is_valid) {
    PB_CHECK(pbo::validate_mail_account(good()).empty());
}

PB_TEST(hosts_must_be_plain_names_without_credentials_scheme_path_or_blanks) {
    for (const char* bad : {"", " ", "mail example.org", "imaps://mail.example.org", "alice:pw@mail.example.org", "mail.example.org/INBOX",
                            "mail.example.org:993", "mail.example.org?x=1", "mail\texample.org", "mail.example.org\n", "a@b",
                            "-bad.example.org", "bad..example.org", "m\xC3\xA4il.example.org"}) {
        auto in = good();
        in.account.imap_host = bad;
        if (!has_error(pbo::validate_mail_account(in), "imap_host", "host_invalid")) PB_FAIL_AT(std::string("accepted host: ") + bad);
        in = good();
        in.account.smtp_host = bad;
        if (!has_error(pbo::validate_mail_account(in), "smtp_host", "host_invalid")) PB_FAIL_AT(std::string("accepted smtp host: ") + bad);
    }
    for (const char* ok : {"mail.example.org", "imap-1.mail.example.org", "localhost", "192.0.2.10", "xn--mnchen-3ya.example.org"}) {
        auto in = good();
        in.account.imap_host = ok;
        if (has_error(pbo::validate_mail_account(in), "imap_host", "host_invalid")) PB_FAIL_AT(std::string("rejected host: ") + ok);
    }
    PB_CHECK(!pbo::is_valid_host(std::string(300, 'a')));
}

PB_TEST(ports_are_1_to_65535) {
    for (int p : {0, -1, 65536, 100000}) {
        auto in = good();
        in.account.imap_port = p;
        PB_CHECK(has_error(pbo::validate_mail_account(in), "imap_port", "port_invalid"));
        in = good();
        in.account.smtp_port = p;
        PB_CHECK(has_error(pbo::validate_mail_account(in), "smtp_port", "port_invalid"));
    }
    for (int p : {1, 143, 993, 65535}) {
        auto in = good();
        in.account.imap_port = p;
        PB_CHECK(!has_error(pbo::validate_mail_account(in), "imap_port", "port_invalid"));
    }
}

PB_TEST(starttls_is_accepted_only_where_the_upgrade_is_enforced) {
    auto in = good();
    in.account.imap_security = pbo::Security::StartTls;
    in.account.imap_port = 143;
    in.account.smtp_security = pbo::Security::StartTls;
    in.account.smtp_port = 587;
    PB_CHECK(pbo::validate_mail_account(in).empty());
    in.account.imap_port = 993;   // STARTTLS on the implicit-TLS port makes no sense
    PB_CHECK(has_error(pbo::validate_mail_account(in), "imap_security", "starttls_port_not_allowed"));
    in = good();
    in.account.smtp_security = pbo::Security::StartTls;
    in.account.smtp_port = 465;
    PB_CHECK(has_error(pbo::validate_mail_account(in), "smtp_security", "starttls_port_not_allowed"));
    in.account.smtp_port = 25;    // not one of the tested ports
    PB_CHECK(has_error(pbo::validate_mail_account(in), "smtp_security", "starttls_port_not_allowed"));
}

PB_TEST(user_names_addresses_and_passwords_are_checked) {
    auto in = good();
    in.account.username = "";
    PB_CHECK(has_error(pbo::validate_mail_account(in), "username", "username_invalid"));
    in.account.username = "ali\nce";
    PB_CHECK(has_error(pbo::validate_mail_account(in), "username", "username_invalid"));
    in = good();
    in.account.smtp_username = "bad\tname";
    PB_CHECK(has_error(pbo::validate_mail_account(in), "smtp_username", "username_invalid"));
    for (const char* bad : {"", "alice", "alice@", "@example.org", "a b@example.org", "alice@@example.org", "alice@example", "al<ice>@example.org",
                            "alice@example.org\r\nBcc: x@example.org"}) {
        in = good();
        in.account.address = bad;
        if (!has_error(pbo::validate_mail_account(in), "address", "address_invalid")) PB_FAIL_AT(std::string("accepted address: ") + bad);
    }
    in = good();
    in.imap_password = "";
    PB_CHECK(has_error(pbo::validate_mail_account(in), "imap_password", "password_empty"));
    in.imap_password = "pw\nsecond-line";
    PB_CHECK(has_error(pbo::validate_mail_account(in), "imap_password", "password_invalid"));
    in = good();
    in.smtp_password = std::string("x\0y", 3);
    PB_CHECK(has_error(pbo::validate_mail_account(in), "smtp_password", "password_invalid"));
    in = good();
    in.account.display_name = "Alice\r\nBcc: evil@example.org";
    PB_CHECK(has_error(pbo::validate_mail_account(in), "display_name", "display_name_invalid"));
}

PB_TEST(window_and_limits_stay_inside_the_data_model_ranges) {
    auto in = good();
    in.account.sync_days = 0;
    PB_CHECK(has_error(pbo::validate_mail_account(in), "sync_days", "value_out_of_range"));
    in.account.sync_days = 366;
    PB_CHECK(has_error(pbo::validate_mail_account(in), "sync_days", "value_out_of_range"));
    in = good();
    in.account.attachment_limit = 1000;
    PB_CHECK(has_error(pbo::validate_mail_account(in), "attachment_limit", "value_out_of_range"));
    in.account.attachment_limit = 200ll << 20;
    PB_CHECK(has_error(pbo::validate_mail_account(in), "attachment_limit", "value_out_of_range"));
    in = good();
    in.account.storage_limit = 1 << 20;
    PB_CHECK(has_error(pbo::validate_mail_account(in), "storage_limit", "value_out_of_range"));
}

PB_TEST(the_store_keeps_the_old_account_when_a_new_one_is_rejected) {
    pbtest::TmpDir dir;
    FixedClock clock;
    auto store = pbo::Store::open(dir.path(), clock);
    PB_CHECK(store.ok());
    PB_CHECK(!store.value->mail_account().has_value());
    auto account = good().account;
    PB_CHECK(store.value->save_mail_account(account).ok());
    auto saved = store.value->mail_account();
    PB_CHECK(saved.has_value());
    PB_CHECK_EQ(saved->imap_host, std::string("mail.example.org"));
    PB_CHECK_EQ(saved->created, static_cast<int64_t>(1791201600));

    auto bad = account;
    bad.imap_host = "evil host";
    bad.username = "mallory";
    const auto err = store.value->save_mail_account(bad);
    PB_CHECK(!err.ok());
    PB_CHECK_EQ(err.key, std::string("invalid_account"));
    PB_CHECK(err.detail.find("evil") == std::string::npos);   // the detail names the field, never the value
    auto after = store.value->mail_account();
    PB_CHECK_EQ(after->imap_host, std::string("mail.example.org"));
    PB_CHECK_EQ(after->username, std::string("alice"));

    account.imap_host = "imap2.example.org";   // a valid change replaces the values and keeps `created`
    PB_CHECK(store.value->save_mail_account(account).ok());
    PB_CHECK_EQ(store.value->mail_account()->imap_host, std::string("imap2.example.org"));
    PB_CHECK_EQ(store.value->mail_account()->created, static_cast<int64_t>(1791201600));
}

PB_TEST_MAIN()
