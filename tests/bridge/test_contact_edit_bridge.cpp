// Spec 002 (US2): creating a contact through ContactBridge: the list of address books to choose from, the validation message beside its field, the
// saved contact in the list marked as not synced, and no contact text in the log. Server behaviour is covered by the core tests.
#include <QCoreApplication>
#include <QSignalSpy>
#include <QTest>
#include <QVariantMap>

#include "conflicts_model.h"
#include "contact_bridge.h"
#include "contacts_model.h"
#include "pbo/contact_edit.h"
#include "pbo/log.h"
#include "pbo/vcard.h"
#include "test_support.h"
#include "tmp_dir.h"

namespace {

int g_argc = 1;
char g_arg0[] = "test_contact_edit_bridge";
char* g_argv[] = {g_arg0, nullptr};
QCoreApplication g_app(g_argc, g_argv);

constexpr int64_t kNow = 1791979200;

struct Clock : pbo::Clock {
    int64_t now() const override { return kNow; }
};
struct CounterRandom : pbo::Random {
    uint8_t n = 1;
    void bytes(uint8_t* out, size_t len) override {
        for (size_t i = 0; i < len; ++i) out[i] = n++;
    }
};

struct Env {
    pbtest::TmpDir dir;
    Clock clock;
    CounterRandom random;
    std::unique_ptr<pbo::Store> store;
    int64_t main = 0, work = 0, ro = 0, hidden = 0;
    Env() {
        store = std::move(pbo::Store::open(dir.path(), clock).value);
        pbo::CalendarAccount a;
        a.caldav_url = "https://dav.example.org/dav/";
        a.username = "alice";
        PB_CHECK(store->save_calendar_account(a).ok());
        main = add_book("contacts", "Contacts", true);
        work = add_book("work", "Work", true);
        ro = add_book("company", "Company", false);
        hidden = add_book("secret", "Hidden", true);
        PB_CHECK(store->set_addressbook_shown(hidden, false).ok());
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
    ContactBridge::Config config() {
        ContactBridge::Config c;
        c.store = store.get();
        c.clock = &clock;
        c.random = &random;
        return c;
    }
};

QVariantMap phone(const QString& label, const QString& value) {
    QVariantMap m;
    m["label"] = label;
    m["value"] = value;
    m["origIndex"] = -1;
    m["locked"] = false;
    return m;
}

QVariantMap form(const QString& given, const QString& family) {
    QVariantMap m;
    m["given"] = given;
    m["family"] = family;
    m["org"] = QString();
    m["note"] = QString();
    m["noteLocked"] = false;
    m["phones"] = QVariantList();
    m["emails"] = QVariantList();
    m["addresses"] = QVariantList();
    return m;
}

std::string str(const QVariant& v) { return v.toString().toStdString(); }

}  // namespace

PB_TEST(a_new_contact_offers_the_writable_shown_address_books_and_preselects_the_last_used_one) {
    Env e;
    ContactBridge b(e.config());
    PB_CHECK(b.newContact());
    PB_CHECK(b.editing());
    PB_CHECK_EQ(b.writableBooks().size(), 2);   // not the read-only one, not the hidden one
    PB_CHECK_EQ(str(b.writableBooks().at(0).toMap()["name"]), std::string("Contacts"));
    PB_CHECK_EQ(str(b.writableBooks().at(1).toMap()["name"]), std::string("Work"));
    PB_CHECK_EQ(b.edit()["bookId"].toLongLong(), static_cast<qlonglong>(e.main));   // the first, as none was used yet
    // saving into "Work" makes it the preselected book of the next new contact
    QVariantMap f = form(QStringLiteral("Anna"), QStringLiteral("Beispiel"));
    f["bookId"] = static_cast<qlonglong>(e.work);
    PB_CHECK(b.save(f));
    PB_CHECK(b.newContact());
    PB_CHECK_EQ(b.edit()["bookId"].toLongLong(), static_cast<qlonglong>(e.work));
    // a remembered book that became read-only is not preselected any more
    PB_CHECK(e.store->debug_exec("UPDATE addressbook SET writable = 0 WHERE id = " + std::to_string(e.work)).empty());
    PB_CHECK(b.newContact());
    PB_CHECK_EQ(b.edit()["bookId"].toLongLong(), static_cast<qlonglong>(e.main));
}

PB_TEST(without_a_writable_book_a_new_contact_says_why_not) {
    Env e;
    PB_CHECK(e.store->debug_exec("UPDATE addressbook SET writable = 0").empty());
    ContactBridge b(e.config());
    PB_CHECK(!b.newContact());
    PB_CHECK(!b.editing());
    PB_CHECK_EQ(str(b.createBlockedReason()), std::string("no_writable_book"));
    PB_CHECK(b.writableBooks().isEmpty());
    PB_CHECK(e.store->debug_exec("DELETE FROM addressbook").empty());
    PB_CHECK(!b.newContact());
    PB_CHECK_EQ(str(b.createBlockedReason()), std::string("no_addressbook"));
}

PB_TEST(saving_stores_the_contact_marks_it_and_tells_the_page) {
    Env e;
    ContactBridge b(e.config());
    ContactsModel list(ContactsModel::Config{e.store.get()});
    PB_CHECK(b.newContact());
    QSignalSpy saved(&b, &ContactBridge::saved);
    QVariantMap f = form(QStringLiteral("  Anna  "), QStringLiteral("Beispiel"));
    f["bookId"] = static_cast<qlonglong>(e.main);
    f["org"] = QStringLiteral("Example Works");
    f["phones"] = QVariantList{phone(QStringLiteral("mobile"), QStringLiteral("+1 555 0101"))};
    f["emails"] = QVariantList{phone(QStringLiteral("work"), QStringLiteral("anna@example.org"))};
    PB_CHECK(b.save(f));
    PB_CHECK_EQ(saved.count(), 1);
    const qlonglong id = saved.at(0).at(0).toLongLong();
    PB_CHECK(id > 0);
    PB_CHECK(b.validationErrors().isEmpty());
    list.refresh();
    PB_CHECK_EQ(list.total(), 1);
    PB_CHECK_EQ(str(list.rows().at(0).toMap()["displayName"]), std::string("Anna Beispiel"));
    PB_CHECK_EQ(str(list.rows().at(0).toMap()["pendingMark"]), std::string("notSynced"));
    b.open(id);
    PB_CHECK(b.pending());
    PB_CHECK_EQ(b.phones().size(), 1);
    PB_CHECK_EQ(str(b.phones().at(0).toMap()["label"]), std::string("mobile"));
    PB_CHECK_EQ(str(b.organization()), std::string("Example Works"));
    PB_CHECK_EQ(e.store->pending_contacts().size(), size_t(1));
}

PB_TEST(an_invalid_form_stores_nothing_and_names_the_field) {
    Env e;
    ContactBridge b(e.config());
    PB_CHECK(b.newContact());
    QSignalSpy saved(&b, &ContactBridge::saved);
    QVariantMap empty = form(QString(), QString());
    empty["bookId"] = static_cast<qlonglong>(e.main);
    PB_CHECK(!b.save(empty));
    PB_CHECK_EQ(saved.count(), 0);
    PB_CHECK_EQ(b.validationErrors().size(), 1);
    PB_CHECK_EQ(str(b.validationErrors().at(0).toMap()["key"]), std::string("empty"));

    QVariantMap bad = form(QStringLiteral("Anna"), QStringLiteral("Beispiel"));
    bad["bookId"] = static_cast<qlonglong>(e.main);
    bad["emails"] = QVariantList{phone(QStringLiteral("work"), QStringLiteral("anna@example.org")), phone(QStringLiteral("home"), QStringLiteral("no-at-sign"))};
    PB_CHECK(!b.save(bad));
    PB_CHECK_EQ(b.validationErrors().size(), 1);
    PB_CHECK_EQ(str(b.validationErrors().at(0).toMap()["field"]), std::string("emails[1]"));
    PB_CHECK_EQ(str(b.validationErrors().at(0).toMap()["key"]), std::string("email_invalid"));
    PB_CHECK_EQ(e.store->debug_query("SELECT count(*) FROM contact"), std::string("0"));

    // correcting it saves, and the old messages are gone
    bad["emails"] = QVariantList{phone(QStringLiteral("work"), QStringLiteral("anna@example.org"))};
    PB_CHECK(b.save(bad));
    PB_CHECK(b.validationErrors().isEmpty());
    PB_CHECK_EQ(e.store->debug_query("SELECT count(*) FROM contact"), std::string("1"));
}

PB_TEST(saving_into_a_book_that_cannot_take_it_reports_the_reason) {
    Env e;
    ContactBridge b(e.config());
    QVariantMap f = form(QStringLiteral("Anna"), QStringLiteral("Beispiel"));
    f["bookId"] = static_cast<qlonglong>(e.ro);
    PB_CHECK(!b.save(f));
    PB_CHECK_EQ(str(b.saveError()), std::string("read_only"));
    f["bookId"] = static_cast<qlonglong>(4242);
    PB_CHECK(!b.save(f));
    PB_CHECK_EQ(str(b.saveError()), std::string("not_found"));
    PB_CHECK_EQ(e.store->debug_query("SELECT count(*) FROM contact"), std::string("0"));
}

PB_TEST(a_failed_contact_can_be_retried_or_discarded_from_its_page) {
    Env e;
    ContactBridge b(e.config());
    QVariantMap f = form(QStringLiteral("Anna"), QStringLiteral("Beispiel"));
    f["bookId"] = static_cast<qlonglong>(e.main);
    QSignalSpy saved(&b, &ContactBridge::saved);
    PB_CHECK(b.save(f));
    const qlonglong id = saved.at(0).at(0).toLongLong();
    PB_CHECK(e.store->debug_exec("UPDATE contact SET error_key = 'refused' WHERE id = " + std::to_string(id)).empty());
    b.open(id);
    PB_CHECK_EQ(str(b.failedReason()), std::string("refused"));
    QSignalSpy changed(&b, &ContactBridge::localChange);
    PB_CHECK(b.retry());
    PB_CHECK_EQ(changed.count(), 1);   // a retry is a change that has to reach the server: the page tells the sync control
    PB_CHECK(b.failedReason().isEmpty());
    PB_CHECK(e.store->debug_exec("UPDATE contact SET error_key = 'refused' WHERE id = " + std::to_string(id)).empty());
    b.open(id);
    PB_CHECK(b.discardChange());
    PB_CHECK(!b.loaded());
    PB_CHECK_EQ(e.store->debug_query("SELECT count(*) FROM contact"), std::string("0"));
}

PB_TEST(a_saved_contact_tells_the_sync_control_about_the_change) {
    Env e;
    ContactBridge b(e.config());
    QSignalSpy changed(&b, &ContactBridge::localChange);
    QVariantMap f = form(QStringLiteral("Anna"), QStringLiteral("Beispiel"));
    f["bookId"] = static_cast<qlonglong>(e.main);
    PB_CHECK(b.save(f));
    PB_CHECK_EQ(changed.count(), 1);
    QVariantMap bad = form(QString(), QString());
    bad["bookId"] = static_cast<qlonglong>(e.main);
    PB_CHECK(!b.save(bad));
    PB_CHECK_EQ(changed.count(), 1);   // a refused form changes nothing
}

PB_TEST(the_saved_text_never_reaches_the_log) {
    Env e;
    ContactBridge b(e.config());
    std::vector<std::string> lines;
    pbo::log::set_sink([&](const std::string& line) { lines.push_back(line); });
    QVariantMap f = form(QStringLiteral("Zacharias"), QStringLiteral("Geheimnis"));
    f["bookId"] = static_cast<qlonglong>(e.main);
    f["note"] = QStringLiteral("Streng vertraulich");
    f["phones"] = QVariantList{phone(QStringLiteral("mobile"), QStringLiteral("+49 555 0199"))};
    f["emails"] = QVariantList{phone(QStringLiteral("work"), QStringLiteral("zacharias@example.org"))};
    PB_CHECK(b.save(f));
    f["emails"] = QVariantList{phone(QStringLiteral("work"), QStringLiteral("kaputt-ohne-at"))};
    PB_CHECK(!b.save(f));
    pbo::log::reset_sink();
    for (const std::string& l : lines)
        for (const char* needle : {"Zacharias", "Geheimnis", "0199", "zacharias@example.org", "vertraulich", "kaputt"})
            if (l.find(needle) != std::string::npos) PB_FAIL_AT("the log line \"" + l + "\" holds " + needle);
}

// ---- changing a contact, and the contacts that need a decision (US3) ----------------------------------------------------------------------------

namespace {

int64_t add_card(Env& e, int64_t book, const std::string& uid, const std::string& body) {
    const std::string href = e.store->debug_query("SELECT href FROM addressbook WHERE id = " + std::to_string(book)) + uid + ".vcf";
    pbo::ContactPullBatch batch;
    batch.upserts.push_back(pbo::contact_upsert_from_card(href, "\"e-" + uid + "\"", "BEGIN:VCARD\r\nVERSION:3.0\r\nUID:" + uid + "\r\n" + body + "END:VCARD\r\n"));
    PB_CHECK(e.store->apply_contact_pull(book, batch).ok());
    return std::atoll(e.store->debug_query("SELECT id FROM contact WHERE uid = '" + uid + "'").c_str());
}

QVariantMap form_of(const QVariantMap& edit) {
    QVariantMap f = edit;
    return f;
}

}  // namespace

PB_TEST(loading_a_contact_into_the_form_gives_its_values_labels_and_positions) {
    Env e;
    const int64_t id = add_card(e, e.main,
        "u-a", "FN:Anna Beispiel\r\nN:Beispiel;Anna;;;\r\nORG:Example Works\r\nTEL;TYPE=CELL:+1 555 0101\r\nitem1.TEL:+1 555 0102\r\nitem1.X-ABLabel:Mother\r\nEMAIL;TYPE=WORK:anna@example.org\r\n"
        "ADR;TYPE=HOME:;;1 Example Street;Exampletown;EX;12345;Exampleland\r\nNOTE:A note\r\n");
    ContactBridge b(e.config());
    b.open(id);
    PB_CHECK(b.loadEdit());
    PB_CHECK(b.editing());
    PB_CHECK(b.editBlockedReason().isEmpty());
    const QVariantMap f = b.edit();
    PB_CHECK_EQ(str(f["given"]), std::string("Anna"));
    PB_CHECK_EQ(str(f["family"]), std::string("Beispiel"));
    PB_CHECK_EQ(str(f["org"]), std::string("Example Works"));
    PB_CHECK_EQ(str(f["note"]), std::string("A note"));
    PB_CHECK(!f["noteLocked"].toBool());
    PB_CHECK_EQ(f["contactId"].toLongLong(), static_cast<qlonglong>(id));
    PB_CHECK_EQ(f["bookId"].toLongLong(), static_cast<qlonglong>(e.main));
    const QVariantList phones = f["phones"].toList();
    PB_CHECK_EQ(phones.size(), 2);
    PB_CHECK_EQ(str(phones[0].toMap()["label"]), std::string("mobile"));
    PB_CHECK_EQ(phones[0].toMap()["origIndex"].toInt(), 0);
    PB_CHECK_EQ(str(phones[1].toMap()["label"]), std::string("Mother"));   // another label is shown as written
    PB_CHECK_EQ(phones[1].toMap()["origIndex"].toInt(), 1);
    PB_CHECK(!phones[1].toMap()["locked"].toBool());
    const QVariantMap a = f["addresses"].toList().at(0).toMap();
    PB_CHECK_EQ(str(a["street"]), std::string("1 Example Street"));
    PB_CHECK_EQ(str(a["postcode"]), std::string("12345"));
}

PB_TEST(a_form_saved_without_a_change_writes_nothing) {
    Env e;
    const int64_t id = add_card(e, e.main, "u-a", "FN:Anna Beispiel\r\nN:Beispiel;Anna;;;\r\nTEL;TYPE=CELL:+1 555 0101\r\n");
    ContactBridge b(e.config());
    b.open(id);
    PB_CHECK(b.loadEdit());
    QSignalSpy saved(&b, &ContactBridge::saved);
    QSignalSpy changed(&b, &ContactBridge::localChange);
    PB_CHECK(b.save(form_of(b.edit())));
    PB_CHECK_EQ(saved.count(), 1);                    // the page leaves as for any save
    PB_CHECK_EQ(changed.count(), 0);                  // but nothing has to reach the server
    PB_CHECK_EQ(e.store->debug_query("SELECT sync_state FROM contact WHERE id = " + std::to_string(id)), std::string("synced"));
    PB_CHECK_EQ(e.store->pending_contacts().size(), size_t(0));
}

PB_TEST(a_changed_form_marks_the_contact_and_asks_for_a_sync) {
    Env e;
    const int64_t id = add_card(e, e.main, "u-a", "FN:Anna Beispiel\r\nN:Beispiel;Anna;;;\r\nTEL;TYPE=CELL:+1 555 0101\r\n");
    ContactBridge b(e.config());
    b.open(id);
    PB_CHECK(b.loadEdit());
    QVariantMap f = b.edit();
    f["family"] = QStringLiteral("Neu");
    QVariantList phones = f["phones"].toList();
    QVariantMap p0 = phones[0].toMap();
    p0["value"] = QStringLiteral("+1 555 0199");
    phones[0] = p0;
    f["phones"] = phones;
    QSignalSpy saved(&b, &ContactBridge::saved);
    QSignalSpy changed(&b, &ContactBridge::localChange);
    PB_CHECK(b.save(f));
    PB_CHECK_EQ(saved.count(), 1);
    PB_CHECK_EQ(saved.at(0).at(0).toLongLong(), static_cast<qlonglong>(id));
    PB_CHECK_EQ(changed.count(), 1);
    PB_CHECK_EQ(e.store->debug_query("SELECT sync_state FROM contact WHERE id = " + std::to_string(id)), std::string("pending_update"));
    b.open(id);
    PB_CHECK_EQ(str(b.displayName()), std::string("Anna Neu"));
    PB_CHECK(b.pending());
    PB_CHECK_EQ(str(b.phones().at(0).toMap()["value"]), std::string("+1 555 0199"));
}

PB_TEST(a_contact_that_cannot_be_changed_does_not_open_the_form_and_says_why) {
    Env e;
    const int64_t ro_contact = add_card(e, e.ro, "u-c", "FN:Carla Firma\r\nN:Firma;Carla;;;\r\n");
    const int64_t unreadable = add_card(e, e.main, "u-u", "FN:Broken\r\n");
    PB_CHECK(e.store->debug_exec("UPDATE contact SET unreadable = 1 WHERE id = " + std::to_string(unreadable)).empty());
    const int64_t in_conflict = add_card(e, e.main, "u-k", "FN:Konflikt Fall\r\nN:Fall;Konflikt;;;\r\n");
    PB_CHECK(e.store->debug_exec("UPDATE contact SET sync_state = 'conflict' WHERE id = " + std::to_string(in_conflict)).empty());
    ContactBridge b(e.config());
    struct Row { int64_t id; const char* reason; };
    for (const Row& r : {Row{ro_contact, "read_only"}, Row{unreadable, "unreadable"}, Row{in_conflict, "in_conflict"}}) {
        b.open(r.id);
        PB_CHECK(!b.loadEdit());
        PB_CHECK(!b.editing());
        PB_CHECK_EQ(str(b.editBlockedReason()), std::string(r.reason));
    }
    // saving a form for such a contact is refused as well
    QVariantMap f = form(QStringLiteral("X"), QStringLiteral("Y"));
    f["contactId"] = static_cast<qlonglong>(ro_contact);
    f["bookId"] = static_cast<qlonglong>(e.ro);
    PB_CHECK(!b.save(f));
    PB_CHECK_EQ(str(b.saveError()), std::string("read_only"));
}

PB_TEST(a_locked_entry_is_marked_in_the_form_refused_when_changed_and_removable) {
    Env e;
    const std::string long_phone(90, '9');
    const int64_t id = add_card(e, e.main, "u-l", "FN:Quin Rho\r\nN:Rho;Quin;;;\r\nTEL;TYPE=WORK:" + long_phone + "\r\nTEL;TYPE=CELL:+1 555 0213\r\nNOTE:" + std::string(9000, 'n') + "\r\n");
    ContactBridge b(e.config());
    b.open(id);
    PB_CHECK(b.loadEdit());
    QVariantMap f = b.edit();
    QVariantList phones = f["phones"].toList();
    PB_CHECK(phones[0].toMap()["locked"].toBool());
    PB_CHECK(!phones[1].toMap()["locked"].toBool());
    PB_CHECK(f["noteLocked"].toBool());
    PB_CHECK(f["note"].toString().isEmpty());
    // a changed value of the locked entry is refused with its field
    QVariantMap changed = f;
    QVariantMap p0 = phones[0].toMap();
    p0["value"] = QStringLiteral("123");
    QVariantList cp = phones;
    cp[0] = p0;
    changed["phones"] = cp;
    PB_CHECK(!b.save(changed));
    PB_CHECK_EQ(b.validationErrors().size(), 1);
    PB_CHECK_EQ(str(b.validationErrors().at(0).toMap()["field"]), std::string("phones[0]"));
    PB_CHECK_EQ(str(b.validationErrors().at(0).toMap()["key"]), std::string("locked_changed"));
    // another field can be changed while the locked ones travel along untouched
    QVariantMap other = f;
    other["family"] = QStringLiteral("Neu");
    PB_CHECK(b.save(other));
    PB_CHECK(e.store->contact(id)->vcf.find(long_phone) != std::string::npos);
    PB_CHECK(e.store->contact(id)->vcf.find("NOTE:") != std::string::npos);
    // removing them is allowed
    b.open(id);
    PB_CHECK(b.loadEdit());
    QVariantMap removal = b.edit();
    QVariantList left = removal["phones"].toList();
    left.removeFirst();
    removal["phones"] = left;
    removal["noteLocked"] = false;
    removal["note"] = QString();
    PB_CHECK(b.save(removal));
    PB_CHECK(e.store->contact(id)->vcf.find(long_phone) == std::string::npos);
    PB_CHECK(e.store->contact(id)->vcf.find("NOTE:") == std::string::npos);
}

PB_TEST(the_contacts_that_need_a_decision_are_listed_with_both_versions_and_can_be_resolved) {
    Env e;
    const int64_t id = add_card(e, e.main, "u-k", "FN:Anna Lokal\r\nN:Lokal;Anna;;;\r\nTEL:+1 555 0101\r\n");
    const std::string server_card = "BEGIN:VCARD\r\nVERSION:3.0\r\nUID:u-k\r\nFN:Anna Server\r\nN:Server;Anna;;;\r\nTEL:+1 555 0101\r\nEND:VCARD\r\n";
    PB_CHECK(e.store->debug_exec("UPDATE contact SET sync_state = 'pending_update' WHERE id = " + std::to_string(id)).empty());
    PB_CHECK(e.store->contact_conflicted(id, server_card, "\"srv-2\"", e.store->contact(id)->href).ok());
    const int64_t gone = add_card(e, e.main, "u-g", "FN:Bert Weg\r\nN:Weg;Bert;;;\r\n");
    PB_CHECK(e.store->debug_exec("UPDATE contact SET sync_state = 'pending_update' WHERE id = " + std::to_string(gone)).empty());
    PB_CHECK(e.store->contact_gone_on_server(gone).ok());

    ConflictsModel::Config cc;
    cc.store = e.store.get();
    cc.clock = &e.clock;
    cc.random = &e.random;
    ConflictsModel m(cc);
    m.refresh();
    PB_CHECK_EQ(m.total(), 2);
    QVariantMap conflict, deleted;
    for (const QVariant& v : m.rows()) {
        const QVariantMap r = v.toMap();
        PB_CHECK_EQ(str(r["kind"]), std::string("contact"));
        if (str(r["state"]) == "conflict") conflict = r;
        else deleted = r;
    }
    PB_CHECK_EQ(conflict["contactId"].toLongLong(), static_cast<qlonglong>(id));
    PB_CHECK_EQ(str(conflict["mineTitle"]), std::string("Anna Lokal"));
    PB_CHECK_EQ(str(conflict["serverTitle"]), std::string("Anna Server"));
    PB_CHECK_EQ(str(conflict["bookName"]), std::string("Contacts"));
    PB_CHECK(!conflict["deleting"].toBool());
    PB_CHECK_EQ(str(deleted["state"]), std::string("server_deleted"));
    PB_CHECK_EQ(deleted["contactId"].toLongLong(), static_cast<qlonglong>(gone));

    QSignalSpy results(&m, &ConflictsModel::actionResult);
    PB_CHECK(!m.resolveContact(id, QStringLiteral("nonsense")));
    PB_CHECK(m.resolveContact(id, QStringLiteral("server")));
    PB_CHECK_EQ(results.count(), 1);
    PB_CHECK_EQ(e.store->debug_query("SELECT sync_state FROM contact WHERE id = " + std::to_string(id)), std::string("synced"));
    PB_CHECK_EQ(m.total(), 1);
    PB_CHECK(m.recreateContact(gone));
    PB_CHECK_EQ(e.store->debug_query("SELECT sync_state FROM contact WHERE id = " + std::to_string(gone)), std::string("pending_create"));
    PB_CHECK_EQ(m.total(), 0);
    PB_CHECK(m.empty());
}

PB_TEST(a_kept_contact_that_the_server_removed_can_be_discarded_from_the_list) {
    Env e;
    const int64_t gone = add_card(e, e.main, "u-g", "FN:Bert Weg\r\nN:Weg;Bert;;;\r\n");
    PB_CHECK(e.store->debug_exec("UPDATE contact SET sync_state = 'pending_update' WHERE id = " + std::to_string(gone)).empty());
    PB_CHECK(e.store->contact_gone_on_server(gone).ok());
    ConflictsModel::Config cc;
    cc.store = e.store.get();
    cc.clock = &e.clock;
    cc.random = &e.random;
    ConflictsModel m(cc);
    m.refresh();
    PB_CHECK_EQ(m.total(), 1);
    PB_CHECK(m.discardContact(gone));
    PB_CHECK_EQ(e.store->debug_query("SELECT count(*) FROM contact"), std::string("0"));
    PB_CHECK(!m.discardContact(gone));
}

// ---- deleting (US4) -----------------------------------------------------------------------------------------------------------------------------

PB_TEST(removing_a_contact_marks_the_deletion_closes_the_page_and_undo_brings_it_back) {
    Env e;
    const int64_t id = add_card(e, e.main, "u-a", "FN:Anna Beispiel\r\nN:Beispiel;Anna;;;\r\nTEL;TYPE=CELL:+1 555 0101\r\n");
    add_card(e, e.main, "u-b", "FN:Bert Bleibt\r\nN:Bleibt;Bert;;;\r\n");
    ContactBridge b(e.config());
    ContactsModel::Config mc;
    mc.store = e.store.get();
    ContactsModel m(mc);
    b.open(id);
    QSignalSpy changed(&b, &ContactBridge::localChange);
    PB_CHECK(b.remove());   // (the page asked the user before)
    PB_CHECK_EQ(changed.count(), 1);
    PB_CHECK(!b.loaded());
    PB_CHECK_EQ(e.store->debug_query("SELECT sync_state FROM contact WHERE id = " + std::to_string(id)), std::string("pending_delete"));
    m.refresh();
    PB_CHECK_EQ(m.total(), 1);   // gone from the list
    PB_CHECK_EQ(m.deletions().size(), 1);
    const QVariantMap d = m.deletions().at(0).toMap();
    PB_CHECK_EQ(d["contactId"].toLongLong(), static_cast<qlonglong>(id));
    PB_CHECK_EQ(str(d["displayName"]), std::string("Anna Beispiel"));
    PB_CHECK(b.undoRemove(id));
    PB_CHECK_EQ(changed.count(), 2);
    PB_CHECK_EQ(e.store->debug_query("SELECT sync_state FROM contact WHERE id = " + std::to_string(id)), std::string("synced"));
    m.refresh();
    PB_CHECK_EQ(m.total(), 2);
    PB_CHECK_EQ(m.deletions().size(), 0);
    PB_CHECK(!b.undoRemove(id));   // nothing waits any more
}

PB_TEST(a_contact_that_cannot_be_deleted_says_why) {
    Env e;
    const int64_t ro_contact = add_card(e, e.ro, "u-c", "FN:Carla Firma\r\nN:Firma;Carla;;;\r\n");
    const int64_t in_conflict = add_card(e, e.main, "u-k", "FN:Konflikt Fall\r\nN:Fall;Konflikt;;;\r\n");
    PB_CHECK(e.store->debug_exec("UPDATE contact SET sync_state = 'conflict' WHERE id = " + std::to_string(in_conflict)).empty());
    ContactBridge b(e.config());
    b.open(ro_contact);
    QSignalSpy changed(&b, &ContactBridge::localChange);
    PB_CHECK(!b.remove());
    PB_CHECK_EQ(str(b.saveError()), std::string("read_only"));
    b.open(in_conflict);
    PB_CHECK(!b.remove());
    PB_CHECK_EQ(str(b.saveError()), std::string("in_conflict"));
    PB_CHECK_EQ(changed.count(), 0);
    PB_CHECK_EQ(e.store->debug_query("SELECT count(*) FROM contact WHERE sync_state = 'pending_delete'"), std::string("0"));
}

PB_TEST_MAIN()
