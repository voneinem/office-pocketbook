// T169/T171 (settings part): SettingsBridge reads and writes the options of the app - Auto Sync, the days of mail kept, the attachment limit, the
// storage limit, the "server files sent mail itself" switch - with the validation of the core. The real core, a throw-away store.
#include <QCoreApplication>
#include <QSignalSpy>
#include <QTest>

#include "pbo/store.h"
#include "pbo/vcard.h"
#include "settings_bridge.h"
#include "test_support.h"
#include "tmp_dir.h"

namespace {

int g_argc = 1;
char g_arg0[] = "test_settings_bridge";
char* g_argv[] = {g_arg0, nullptr};
QCoreApplication g_app(g_argc, g_argv);

struct Clock : pbo::Clock {
    int64_t now() const override { return 1791979200; }
};

struct Env {
    pbtest::TmpDir dir;
    Clock clock;
    std::unique_ptr<pbo::Store> store;
    Env(bool with_account = true) {
        store = std::move(pbo::Store::open(dir.path(), clock).value);
        if (with_account) {
            pbo::MailAccount a;
            a.imap_host = "mail.example.org";
            a.smtp_host = "smtp.example.org";
            a.username = "alice";
            a.address = "alice@example.org";
            PB_CHECK(store->save_mail_account(a).ok());
        }
    }
    SettingsBridge::Config config() {
        SettingsBridge::Config c;
        c.store = store.get();
        return c;
    }
};

}  // namespace

PB_TEST(the_defaults_are_shown_and_auto_sync_is_on_unless_switched_off) {
    Env e;
    SettingsBridge s(e.config());
    PB_CHECK(s.hasMailAccount());
    PB_CHECK(s.autoSync());
    PB_CHECK_EQ(s.syncDays(), 30);
    PB_CHECK_EQ(s.attachmentLimitMib(), 10);
    PB_CHECK_EQ(s.storageLimitMib(), 200);
    PB_CHECK(!s.sentByServer());
    s.setAutoSync(false);
    PB_CHECK_EQ(e.store->setting("auto_sync"), std::string("0"));
    PB_CHECK(!s.autoSync());
    SettingsBridge again(e.config());
    PB_CHECK(!again.autoSync());                               // kept
    again.setAutoSync(true);
    PB_CHECK_EQ(e.store->setting("auto_sync"), std::string("1"));
}

PB_TEST(a_valid_value_is_stored_in_the_account_and_leaves_the_rest_alone) {
    Env e;
    SettingsBridge s(e.config());
    QSignalSpy changed(&s, &SettingsBridge::changed);
    PB_CHECK(s.setSyncDays(90));
    PB_CHECK(s.setAttachmentLimitMib(25));
    PB_CHECK(s.setStorageLimitMib(500));
    s.setSentByServer(true);
    PB_CHECK(changed.count() >= 4);
    const auto a = e.store->mail_account();
    PB_CHECK_EQ(a->sync_days, 90);
    PB_CHECK_EQ(a->attachment_limit, int64_t(25) << 20);
    PB_CHECK_EQ(a->storage_limit, int64_t(500) << 20);
    PB_CHECK(a->sent_by_server);
    PB_CHECK_EQ(a->imap_host, std::string("mail.example.org"));   // untouched
    PB_CHECK_EQ(a->username, std::string("alice"));
}

PB_TEST(a_value_out_of_range_is_refused_with_its_name_and_nothing_changes) {
    Env e;
    SettingsBridge s(e.config());
    PB_CHECK(!s.setSyncDays(0));
    PB_CHECK_EQ(s.errorKey().toStdString(), std::string("sync_days"));
    PB_CHECK(!s.setSyncDays(366));
    PB_CHECK(!s.setAttachmentLimitMib(0));
    PB_CHECK_EQ(s.errorKey().toStdString(), std::string("attachment_limit"));
    PB_CHECK(!s.setAttachmentLimitMib(101));
    PB_CHECK(!s.setStorageLimitMib(19));
    PB_CHECK_EQ(s.errorKey().toStdString(), std::string("storage_limit"));
    PB_CHECK(s.setStorageLimitMib(20));                         // the smallest allowed
    PB_CHECK_EQ(e.store->mail_account()->sync_days, 30);
    PB_CHECK_EQ(e.store->mail_account()->attachment_limit, int64_t(10) << 20);
    PB_CHECK(s.errorKey().isEmpty());                           // a success clears the message
}

PB_TEST(without_a_mail_account_the_mail_options_are_unavailable_and_auto_sync_still_works) {
    Env e(false);
    SettingsBridge s(e.config());
    PB_CHECK(!s.hasMailAccount());
    PB_CHECK(!s.setSyncDays(10));
    PB_CHECK_EQ(s.errorKey().toStdString(), std::string("no_account"));
    s.setAutoSync(false);
    PB_CHECK_EQ(e.store->setting("auto_sync"), std::string("0"));
}

PB_TEST(the_choices_offered_are_all_valid_values) {
    Env e;
    SettingsBridge s(e.config());
    for (const QVariant& v : s.syncDaysChoices()) PB_CHECK(s.setSyncDays(v.toInt()));
    for (const QVariant& v : s.attachmentLimitChoices()) PB_CHECK(s.setAttachmentLimitMib(v.toInt()));
    for (const QVariant& v : s.storageLimitChoices()) PB_CHECK(s.setStorageLimitMib(v.toInt()));
    PB_CHECK(s.syncDaysChoices().size() >= 4);
}

PB_TEST(the_special_folders_show_what_is_used_and_can_be_chosen_among_the_folders_of_the_account) {
    Env e;
    pbo::FolderListItem inbox, sent, trash, archive;
    inbox.raw_name = inbox.name = "INBOX";
    sent.raw_name = sent.name = "Sent";
    sent.attributes = {"\\Sent"};
    trash.raw_name = trash.name = "Trash";
    trash.attributes = {"\\Trash"};
    archive.raw_name = archive.name = "Archive";
    e.store->upsert_folders({inbox, sent, trash, archive});
    SettingsBridge s(e.config());
    auto find = [&](const char* role) {
        for (const QVariant& v : s.specialFolders())
            if (v.toMap()["role"].toString() == QLatin1String(role)) return v.toMap();
        return QVariantMap();
    };
    PB_CHECK_EQ(find("sent")["name"].toString().toStdString(), std::string("Sent"));          // found by its attribute
    PB_CHECK(!find("sent")["chosen"].toBool());
    PB_CHECK_EQ(find("trash")["name"].toString().toStdString(), std::string("Trash"));
    PB_CHECK_EQ(find("drafts")["name"].toString().toStdString(), std::string(""));            // none known: the page says so
    PB_CHECK_EQ(s.folderChoices().size(), 4);
    qlonglong archive_id = 0;
    for (const QVariant& v : s.folderChoices())
        if (v.toMap()["name"].toString() == QLatin1String("Archive")) archive_id = v.toMap()["id"].toLongLong();
    PB_CHECK(s.setSpecialFolder(QStringLiteral("drafts"), archive_id));
    PB_CHECK_EQ(find("drafts")["name"].toString().toStdString(), std::string("Archive"));
    PB_CHECK(find("drafts")["chosen"].toBool());
    PB_CHECK_EQ(e.store->mail_account()->drafts_folder_id, int64_t(archive_id));
    PB_CHECK(s.setSpecialFolder(QStringLiteral("drafts"), 0));                                  // back to automatic
    PB_CHECK_EQ(e.store->mail_account()->drafts_folder_id, int64_t(0));
    PB_CHECK(!s.setSpecialFolder(QStringLiteral("drafts"), 99999));                            // a folder that is not the account's
    PB_CHECK_EQ(s.errorKey().toStdString(), std::string("bad_folder"));
    PB_CHECK(!s.setSpecialFolder(QStringLiteral("junk"), archive_id));
    PB_CHECK_EQ(s.errorKey().toStdString(), std::string("bad_role"));
}

PB_TEST(settings_report_the_custom_certificate_authority) {
    Env e;
    SettingsBridge none(e.config());
    PB_CHECK_EQ(none.customCaStatus().toStdString(), std::string("none"));
    PB_CHECK_EQ(none.customCaCount(), 0);
    SettingsBridge::Config c = e.config();
    c.custom_ca_status = "loaded";
    c.custom_ca_count = 2;
    SettingsBridge loaded(c);
    PB_CHECK_EQ(loaded.customCaStatus().toStdString(), std::string("loaded"));
    PB_CHECK_EQ(loaded.customCaCount(), 2);
    c.custom_ca_status = "invalid";
    c.custom_ca_count = 0;
    SettingsBridge invalid(c);
    PB_CHECK_EQ(invalid.customCaStatus().toStdString(), std::string("invalid"));
}

PB_TEST(the_language_is_automatic_until_chosen_and_only_known_languages_are_kept) {
    Env e;
    SettingsBridge s(e.config());
    QSignalSpy lang(&s, &SettingsBridge::languageChanged);
    PB_CHECK_EQ(s.language().toStdString(), std::string("auto"));
    PB_CHECK(s.setLanguage(QStringLiteral("de")));
    PB_CHECK_EQ(e.store->setting("ui_language"), std::string("de"));
    PB_CHECK_EQ(s.language().toStdString(), std::string("de"));
    PB_CHECK_EQ(lang.count(), 1);
    PB_CHECK(!s.setLanguage(QStringLiteral("fr")));                       // not offered: nothing changes
    PB_CHECK_EQ(e.store->setting("ui_language"), std::string("de"));
    PB_CHECK_EQ(lang.count(), 1);
    PB_CHECK(s.setLanguage(QStringLiteral("auto")));
    PB_CHECK_EQ(s.language().toStdString(), std::string("auto"));
    SettingsBridge again(e.config());                                      // kept across a restart
    PB_CHECK(again.setLanguage(QStringLiteral("en")));
    SettingsBridge third(e.config());
    PB_CHECK_EQ(third.language().toStdString(), std::string("en"));
    PB_CHECK_EQ(third.languageChoices().size(), 3);
}

PB_TEST(the_address_books_are_listed_and_hiding_one_deletes_nothing) {
    Env e;
    SettingsBridge none(e.config());
    PB_CHECK_EQ(none.addressBooks().size(), 0);
    pbo::AddressBook a, b;
    a.href = "/dav/ab/family/";
    a.name = "Family";
    a.writable = true;
    b.href = "/dav/ab/company/";
    b.name = "Company";
    b.writable = false;
    const auto ida = e.store->upsert_addressbook(a);
    const auto idb = e.store->upsert_addressbook(b);
    PB_CHECK(ida.ok() && idb.ok());
    pbo::ContactPullBatch batch;
    batch.upserts.push_back(pbo::contact_upsert_from_card("/dav/ab/company/c.vcf", "\"e\"", "BEGIN:VCARD\r\nVERSION:3.0\r\nUID:u-c\r\nFN:Carla Firma\r\nN:Firma;Carla;;;\r\nEND:VCARD\r\n"));
    PB_CHECK(e.store->apply_contact_pull(idb.value, batch).ok());
    SettingsBridge s(e.config());
    s.refresh();
    const QVariantList books = s.addressBooks();
    PB_CHECK_EQ(books.size(), 2);
    PB_CHECK_EQ(books[0].toMap()["name"].toString().toStdString(), std::string("Company"));   // in the order of their names
    PB_CHECK(books[0].toMap()["shown"].toBool());
    PB_CHECK(!books[0].toMap()["writable"].toBool());
    PB_CHECK(books[1].toMap()["writable"].toBool());
    QSignalSpy changed(&s, &SettingsBridge::changed);
    PB_CHECK(s.setAddressBookShown(static_cast<qlonglong>(idb.value), false));
    PB_CHECK(changed.count() >= 1);
    PB_CHECK(!s.addressBooks()[0].toMap()["shown"].toBool());
    PB_CHECK_EQ(e.store->contacts_count(""), 0);                                      // not listed ...
    PB_CHECK_EQ(e.store->debug_query("SELECT count(*) FROM contact"), std::string("1"));   // ... and nothing deleted (FR-025)
    PB_CHECK(s.setAddressBookShown(static_cast<qlonglong>(idb.value), true));
    PB_CHECK_EQ(e.store->contacts_count(""), 1);
    PB_CHECK(!s.setAddressBookShown(4242, false));
    PB_CHECK_EQ(s.errorKey().toStdString(), std::string("not_found"));
}

PB_TEST_MAIN()
