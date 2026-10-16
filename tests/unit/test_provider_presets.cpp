// T122 (E1, FR-016): the sent-copy default by provider. Exact hosts only; the first save applies it, later saves keep the user's choice.
#include <string>

#include "pbo/provider_presets.h"
#include "test_support.h"

namespace {
pbo::MailAccount account(const std::string& host) {
    pbo::MailAccount a;
    a.imap_host = host;
    a.smtp_host = "smtp.example.org";
    a.username = "alice";
    a.address = "alice@example.org";
    return a;
}
}  // namespace

PB_TEST(the_known_providers_file_sent_mail_themselves_with_the_standard_ports) {
    for (const char* h : {"imap.gmail.com", "outlook.office365.com", "imap-mail.outlook.com"}) {
        const auto p = pbo::preset_for_host(h);
        PB_CHECK(p.has_value());
        PB_CHECK(p->sent_by_server);
        PB_CHECK_EQ(p->imap_port, 993);
        PB_CHECK_EQ(p->smtp_port, 465);
    }
}

PB_TEST(case_does_not_matter_and_suffix_tricks_and_other_hosts_are_not_recognised) {
    PB_CHECK(pbo::preset_for_host("IMAP.GMAIL.COM").has_value());
    PB_CHECK(pbo::preset_for_host("Outlook.Office365.com").has_value());
    PB_CHECK(pbo::preset_for_host("imap.gmail.com.").has_value());
    for (const char* h : {"imap.gmail.com.evil.example", "evil-imap.gmail.com", "myimap.gmail.com", "gmail.com", "imap.googlemail.com", "outlook.com", "mail.example.org",
                          "imap.gmail.com:993", " imap.gmail.com", "imap.gmail.com ", "imap.gmail.com\n", "", "imap.gmail.co", "outlook.office365.com.example.net"})
        if (pbo::preset_for_host(h).has_value()) PB_FAIL_AT(std::string("recognised: ") + h);
}

PB_TEST(the_first_save_applies_the_preset_and_every_other_host_defaults_to_off) {
    PB_CHECK(pbo::merge_for_save(account("imap.gmail.com"), std::nullopt).sent_by_server);
    PB_CHECK(pbo::merge_for_save(account("outlook.office365.com"), std::nullopt).sent_by_server);
    PB_CHECK(!pbo::merge_for_save(account("mail.example.org"), std::nullopt).sent_by_server);
    auto typed_on = account("mail.example.org");
    typed_on.sent_by_server = true;                              // whatever the form map carried, the first save decides by the host
    PB_CHECK(!pbo::merge_for_save(typed_on, std::nullopt).sent_by_server);
}

PB_TEST(a_later_save_keeps_the_users_choice_and_the_settings_that_are_not_on_the_form) {
    auto stored = account("imap.gmail.com");
    stored.sent_by_server = false;                               // the user switched it off in Settings
    stored.sync_days = 90;
    stored.attachment_limit = 20u << 20;
    stored.storage_limit = 500u << 20;
    stored.sent_folder_id = 7;
    stored.drafts_folder_id = 8;
    stored.trash_folder_id = 9;
    stored.capabilities = "imap4rev1 uidplus";
    stored.created = 1234;
    auto resaved = pbo::merge_for_save(account("imap.gmail.com"), stored);
    PB_CHECK(!resaved.sent_by_server);
    PB_CHECK_EQ(resaved.sync_days, 90);
    PB_CHECK_EQ(resaved.attachment_limit, int64_t(20u << 20));
    PB_CHECK_EQ(resaved.storage_limit, int64_t(500u << 20));
    PB_CHECK_EQ(resaved.sent_folder_id, int64_t(7));
    PB_CHECK_EQ(resaved.trash_folder_id, int64_t(9));
    PB_CHECK_EQ(resaved.capabilities, std::string("imap4rev1 uidplus"));
    PB_CHECK_EQ(resaved.created, int64_t(1234));
    // the fields of the form do change
    auto form = account("imap.gmail.com");
    form.username = "alice2";
    PB_CHECK_EQ(pbo::merge_for_save(form, stored).username, std::string("alice2"));
}

PB_TEST_MAIN()
