// Spec 002 (US1): the contact bridges - ContactsModel (paged list, search, states), ContactBridge (view side) and the problem area of SyncBridge - with
// the real core and a throw-away store. Server behaviour is covered by the core tests (fake CardDAV server).
#include <QCoreApplication>
#include <QSignalSpy>
#include <QTest>
#include <QVariantMap>

#include "contact_bridge.h"
#include "contacts_model.h"
#include "home_bridge.h"
#include "pbo/log.h"
#include "pbo/vcard.h"
#include "sync_bridge.h"
#include "test_support.h"
#include "tmp_dir.h"
#include "worker.h"

namespace {

int g_argc = 1;
char g_arg0[] = "test_contacts_bridge";
char* g_argv[] = {g_arg0, nullptr};
QCoreApplication g_app(g_argc, g_argv);

constexpr int64_t kNow = 1791979200;

struct Clock : pbo::Clock {
    int64_t now() const override { return kNow; }
};

struct Env {
    pbtest::TmpDir dir;
    Clock clock;
    std::unique_ptr<pbo::Store> store;
    Worker worker;
    int64_t book = 0, ro = 0;
    int n = 0;
    explicit Env(bool with_account = true) {
        store = std::move(pbo::Store::open(dir.path(), clock).value);
        if (with_account) {
            pbo::CalendarAccount a;
            a.caldav_url = "https://dav.example.org/dav/";
            a.username = "alice";
            PB_CHECK(store->save_calendar_account(a).ok());
        }
    }
    int64_t add_book(const std::string& slug, const std::string& name, bool writable) {
        pbo::AddressBook b;
        b.href = "/dav/addressbooks/alice/" + slug + "/";
        b.name = name;
        b.writable = writable;
        auto id = store->upsert_addressbook(b);
        PB_CHECK(id.ok());
        return id.value;
    }
    int64_t add(int64_t in_book, const std::string& body) {
        ++n;
        const std::string href = store->debug_query("SELECT href FROM addressbook WHERE id = " + std::to_string(in_book)) + "c" + std::to_string(n) + ".vcf";
        const std::string card = "BEGIN:VCARD\r\nVERSION:3.0\r\nUID:uid-" + std::to_string(n) + "\r\n" + body + "END:VCARD\r\n";
        pbo::ContactPullBatch batch;
        batch.upserts.push_back(pbo::contact_upsert_from_card(href, "\"e" + std::to_string(n) + "\"", card));
        PB_CHECK(store->apply_contact_pull(in_book, batch).ok());
        return std::atoll(store->debug_query("SELECT id FROM contact WHERE href = '" + href + "'").c_str());
    }
    ContactsModel::Config model_config() {
        ContactsModel::Config c;
        c.store = store.get();
        return c;
    }
    ContactBridge::Config bridge_config() {
        ContactBridge::Config c;
        c.store = store.get();
        return c;
    }
    SyncBridge::Config sync_config() {
        SyncBridge::Config c;
        c.store = store.get();
        c.data_dir = dir.path();
        c.worker = &worker;
        c.clock = &clock;
        return c;
    }
};

QVariantMap row(const QVariantList& l, int i) { return l.at(i).toMap(); }
std::string str(const QVariant& v) { return v.toString().toStdString(); }

}  // namespace

PB_TEST(the_state_tells_why_the_list_is_empty) {
    {
        Env e(false);
        ContactsModel m(e.model_config());
        m.refresh();
        PB_CHECK_EQ(str(m.state()), std::string("noAccount"));
        PB_CHECK(m.rows().isEmpty());
    }
    Env e;
    ContactsModel m(e.model_config());
    m.refresh();
    PB_CHECK_EQ(str(m.state()), std::string("noAddressbook"));
    e.book = e.add_book("contacts", "Contacts", true);
    m.refresh();
    PB_CHECK_EQ(str(m.state()), std::string("empty"));
    e.add(e.book, "FN:Anna Beispiel\r\nN:Beispiel;Anna;;;\r\n");
    m.refresh();
    PB_CHECK_EQ(str(m.state()), std::string("ok"));
    m.search(QStringLiteral("zzz"));
    PB_CHECK_EQ(str(m.state()), std::string("noMatch"));
    m.clearSearch();
    PB_CHECK_EQ(str(m.state()), std::string("ok"));
}

PB_TEST(the_list_is_paged_sorted_and_clamped) {
    Env e;
    e.book = e.add_book("contacts", "Contacts", true);
    for (int i = 0; i < 25; ++i) {
        char fam[16];
        std::snprintf(fam, sizeof fam, "Family%02d", i);
        e.add(e.book, std::string("FN:Given ") + fam + "\r\nN:" + fam + ";Given;;;\r\n");
    }
    ContactsModel m(e.model_config());
    m.setPageSize(9);
    m.refresh();
    PB_CHECK_EQ(m.total(), 25);
    PB_CHECK_EQ(m.pageCount(), 3);
    PB_CHECK_EQ(m.page(), 0);
    PB_CHECK_EQ(m.rows().size(), 9);
    PB_CHECK_EQ(str(row(m.rows(), 0)["displayName"]), std::string("Given Family00"));
    m.nextPage();
    PB_CHECK_EQ(m.page(), 1);
    PB_CHECK_EQ(str(row(m.rows(), 0)["displayName"]), std::string("Given Family09"));
    m.goToPage(99);
    PB_CHECK_EQ(m.page(), 2);
    PB_CHECK_EQ(m.rows().size(), 7);
    m.nextPage();
    PB_CHECK_EQ(m.page(), 2);   // the last page stays
    m.goToPage(-5);
    PB_CHECK_EQ(m.page(), 0);
    m.previousPage();
    PB_CHECK_EQ(m.page(), 0);
}

PB_TEST(search_narrows_the_list_from_the_first_page_and_clear_restores_it) {
    Env e;
    e.book = e.add_book("contacts", "Contacts", true);
    for (int i = 0; i < 20; ++i) e.add(e.book, "FN:Person " + std::to_string(i) + "\r\nN:Person" + std::to_string(i) + ";P;;;\r\nTEL:+1 555 01" + std::to_string(10 + i) + "\r\n");
    e.add(e.book, "FN:Anna Beispiel\r\nN:Beispiel;Anna;;;\r\nEMAIL:anna@example.org\r\n");
    ContactsModel m(e.model_config());
    m.setPageSize(5);
    m.refresh();
    m.goToPage(2);
    PB_CHECK_EQ(m.page(), 2);
    QSignalSpy changed(&m, &ContactsModel::changed);
    m.search(QStringLiteral("beispiel"));
    PB_CHECK(changed.count() >= 1);
    PB_CHECK_EQ(m.page(), 0);
    PB_CHECK_EQ(m.total(), 1);
    PB_CHECK_EQ(str(m.query()), std::string("beispiel"));
    PB_CHECK_EQ(str(row(m.rows(), 0)["displayName"]), std::string("Anna Beispiel"));
    m.search(QStringLiteral("555 0115"));
    PB_CHECK_EQ(m.total(), 1);
    m.clearSearch();
    PB_CHECK_EQ(m.total(), 21);
    PB_CHECK(m.query().isEmpty());
    PB_CHECK_EQ(m.page(), 0);
}

PB_TEST(a_row_carries_name_subtitle_book_marks_and_the_unreadable_flag) {
    Env e;
    e.book = e.add_book("contacts", "Contacts", true);
    const int64_t id = e.add(e.book, "FN:Anna Beispiel\r\nN:Beispiel;Anna;;;\r\nORG:Example Works\r\n");
    ContactsModel m(e.model_config());
    m.refresh();
    QVariantMap r = row(m.rows(), 0);
    PB_CHECK_EQ(str(r["displayName"]), std::string("Anna Beispiel"));
    PB_CHECK_EQ(str(r["subtitle"]), std::string("Example Works"));
    PB_CHECK_EQ(r["contactId"].toLongLong(), static_cast<qlonglong>(id));
    PB_CHECK_EQ(str(r["bookName"]), std::string());      // one book shown: no book name on the row
    PB_CHECK_EQ(str(r["pendingMark"]), std::string());
    PB_CHECK_EQ(str(r["errorKey"]), std::string());
    PB_CHECK(!r["unreadable"].toBool());

    const struct { const char* sql; const char* mark; } marks[] = {
        {"sync_state = 'pending_update'", "notSynced"},
        {"sync_state = 'pending_create'", "notSynced"},
        {"sync_state = 'pending_update', error_key = 'refused'", "failed"},
        {"sync_state = 'conflict', error_key = ''", "conflict"},
        {"sync_state = 'server_deleted', error_key = ''", "deletedOnServer"},
    };
    for (const auto& mk : marks) {
        PB_CHECK(e.store->debug_exec(std::string("UPDATE contact SET ") + mk.sql + " WHERE id = " + std::to_string(id)).empty());
        m.refresh();
        PB_CHECK_EQ(str(row(m.rows(), 0)["pendingMark"]), std::string(mk.mark));
    }
    // a second shown book: every row says which book it is from
    PB_CHECK(e.store->debug_exec("UPDATE contact SET sync_state = 'synced', error_key = '' WHERE id = " + std::to_string(id)).empty());
    e.ro = e.add_book("company", "Company", false);
    e.add(e.ro, "FN:Carla Company\r\nN:Company;Carla;;;\r\n");
    m.refresh();
    PB_CHECK_EQ(m.total(), 2);
    PB_CHECK(str(row(m.rows(), 0)["bookName"]).size() > 0);
    PB_CHECK(str(row(m.rows(), 1)["bookName"]).size() > 0);
    // an unreadable card is listed, flagged
    e.add(e.book, "garbage without a colon\r\n");
    PB_CHECK(e.store->debug_exec("UPDATE contact SET unreadable = 1 WHERE id = (SELECT max(id) FROM contact)").empty());
    m.refresh();
    int unreadable = 0;
    for (const QVariant& v : m.rows())
        if (v.toMap()["unreadable"].toBool()) ++unreadable;
    PB_CHECK_EQ(unreadable, 1);
}

PB_TEST(the_detail_shows_every_field_with_labels_and_the_book_and_survives_unknown_properties) {
    Env e;
    e.book = e.add_book("contacts", "Contacts", true);
    const int64_t id = e.add(e.book,
        "FN:Anna Beispiel\r\nN:Beispiel;Anna;;;\r\nORG:Example Works GmbH;Sales\r\nTEL;TYPE=CELL:+1 555 0101\r\nTEL;TYPE=WORK:+1 555 0100\r\nEMAIL;TYPE=WORK:anna@example.org\r\n"
        "ADR;TYPE=HOME:;;1 Example Street;Exampletown;EX;12345;Exampleland\r\nNOTE:Met at the fair.\\nSecond line.\r\nBDAY:1985-04-12\r\nX-CUSTOM:keep\r\nIMPP:xmpp:anna@example.org\r\n");
    ContactBridge b(e.bridge_config());
    PB_CHECK(!b.loaded());
    QSignalSpy changed(&b, &ContactBridge::changed);
    b.open(id);
    PB_CHECK(changed.count() >= 1);
    PB_CHECK(b.loaded());
    PB_CHECK_EQ(str(b.displayName()), std::string("Anna Beispiel"));
    PB_CHECK_EQ(str(b.organization()), std::string("Example Works GmbH"));
    PB_CHECK_EQ(b.phones().size(), 2);
    PB_CHECK_EQ(str(b.phones().at(0).toMap()["label"]), std::string("mobile"));
    PB_CHECK_EQ(str(b.phones().at(0).toMap()["value"]), std::string("+1 555 0101"));
    PB_CHECK_EQ(str(b.phones().at(1).toMap()["label"]), std::string("work"));
    PB_CHECK_EQ(b.emails().size(), 1);
    PB_CHECK_EQ(str(b.emails().at(0).toMap()["value"]), std::string("anna@example.org"));
    PB_CHECK_EQ(b.addresses().size(), 1);
    PB_CHECK_EQ(str(b.addresses().at(0).toMap()["label"]), std::string("home"));
    const std::string addr = str(b.addresses().at(0).toMap()["text"]);
    for (const char* part : {"1 Example Street", "Exampletown", "12345", "Exampleland"}) PB_CHECK(addr.find(part) != std::string::npos);
    PB_CHECK_EQ(str(b.note()), std::string("Met at the fair.\nSecond line."));
    PB_CHECK_EQ(str(b.bookName()), std::string("Contacts"));
    PB_CHECK(!b.readOnly());
    PB_CHECK_EQ(str(b.readOnlyReason()), std::string());
    PB_CHECK(!b.pending());
    PB_CHECK(!b.unreadable());
    b.close();
    PB_CHECK(!b.loaded());
    PB_CHECK(b.displayName().isEmpty());
    b.open(424242);   // an unknown id opens nothing
    PB_CHECK(!b.loaded());
}

PB_TEST(a_contact_of_a_read_only_book_says_why_it_cannot_be_changed) {
    Env e;
    e.ro = e.add_book("company", "Company", false);
    const int64_t id = e.add(e.ro, "FN:Carla Company\r\nN:Company;Carla;;;\r\n");
    ContactBridge b(e.bridge_config());
    b.open(id);
    PB_CHECK(b.loaded());
    PB_CHECK(b.readOnly());
    PB_CHECK_EQ(str(b.readOnlyReason()), std::string("book"));
    PB_CHECK_EQ(str(b.bookName()), std::string("Company"));
}

PB_TEST(an_unreadable_contact_opens_as_unreadable_and_read_only) {
    Env e;
    e.book = e.add_book("contacts", "Contacts", true);
    const int64_t id = e.add(e.book, "FN:Broken\r\n");
    PB_CHECK(e.store->debug_exec("UPDATE contact SET unreadable = 1 WHERE id = " + std::to_string(id)).empty());
    ContactBridge b(e.bridge_config());
    b.open(id);
    PB_CHECK(b.loaded());
    PB_CHECK(b.unreadable());
    PB_CHECK(b.readOnly());
    PB_CHECK_EQ(str(b.readOnlyReason()), std::string("unreadable"));
}

PB_TEST(the_sync_bridge_reports_the_area_of_the_last_problem) {
    Env e;
    SyncBridge sync(e.sync_config());
    PB_CHECK(sync.problemArea().isEmpty());
    pbo::SyncState s = e.store->sync_state();
    s.outcome = "partial";
    s.problem_key = "server_unreachable";
    s.problem_area = "contacts";
    PB_CHECK(e.store->write_sync_state(s).ok());
    sync.refreshState();
    PB_CHECK_EQ(str(sync.problemArea()), std::string("contacts"));
    PB_CHECK_EQ(str(sync.problemKey()), std::string("server_unreachable"));
    for (const char* key : {"auth_failed", "server_unreachable", "cert_expired", "cert_untrusted", "server_error"}) {
        s.problem_key = key;
        PB_CHECK(e.store->write_sync_state(s).ok());
        PB_CHECK_EQ(str(sync.problemKey()), std::string(key));   // the page turns the key into the words; the area says which part failed
    }
}

PB_TEST(opening_the_list_and_a_contact_writes_nothing_personal_to_the_log) {
    Env e;
    e.book = e.add_book("secret", "Kontakte-Geheim", true);
    const int64_t id = e.add(e.book, "FN:Zacharias Geheimnis\r\nN:Geheimnis;Zacharias;;;\r\nTEL:+49 555 0199\r\nEMAIL:zacharias@example.org\r\nNOTE:Streng vertraulich\r\n");
    std::vector<std::string> lines;
    pbo::log::set_sink([&](const std::string& line) { lines.push_back(line); });
    ContactsModel m(e.model_config());
    m.refresh();
    m.search(QStringLiteral("zacharias"));
    ContactBridge b(e.bridge_config());
    b.open(id);
    pbo::log::reset_sink();
    for (const std::string& l : lines)
        for (const char* needle : {"Zacharias", "Geheimnis", "0199", "zacharias@example.org", "vertraulich", "Kontakte-Geheim"})
            if (l.find(needle) != std::string::npos) PB_FAIL_AT("the log line \"" + l + "\" holds " + needle);
}

PB_TEST(the_home_offers_contacts_whenever_a_calendar_server_account_exists) {
    auto tz = pbo::TzDatabase::open(std::string(PBO_SOURCE_DIR) + "/third_party/tzdata/zoneinfo");
    PB_CHECK(tz != nullptr);
    auto make = [&](Env& e) {
        HomeBridge::Config c;
        c.store = e.store.get();
        c.tz = tz.get();
        c.clock = &e.clock;
        return c;
    };
    {   // no account at all
        Env e(false);
        HomeBridge h(make(e));
        h.refresh();
        PB_CHECK(!h.contactsAvailable());
    }
    {   // a calendar feed only: feeds have no contacts
        Env e(false);
        pbo::CalendarAccount feed;
        feed.feed_url = "https://feeds.example.org/holidays.ics";
        PB_CHECK(e.store->save_calendar_account(feed).ok());
        HomeBridge h(make(e));
        h.refresh();
        PB_CHECK(!h.contactsAvailable());
    }
    {   // a calendar server account, before any address book was found: the Contacts page explains the state
        Env e;
        HomeBridge h(make(e));
        h.refresh();
        PB_CHECK(h.contactsAvailable());
        e.add_book("contacts", "Contacts", true);
        h.refresh();
        PB_CHECK(h.contactsAvailable());
    }
}

PB_TEST_MAIN()
