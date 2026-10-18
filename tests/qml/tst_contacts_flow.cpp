// Spec 002: the contact pages through real QML (offscreen): the list with paging and search, the detail page, the empty states and the sync status line.
// The bridges and the core are real; the data is written into the store directly as a pull would write it.
#include <QQmlContext>
#include <QQmlEngine>
#include <QtQuickTest>
#include <memory>

#include "conflicts_model.h"
#include "contact_bridge.h"
#include "contacts_model.h"
#include "pbo/store.h"
#include "pbo/vcard.h"
#include "sync_bridge.h"
#include "test_support.h"
#include "tmp_dir.h"
#include "worker.h"

namespace {

constexpr int64_t kNow = 1791979200;   // 2026-10-14 12:00:00 UTC

struct FixedClock : pbo::Clock {
    int64_t now() const override { return kNow; }
};

struct CountingRandom : pbo::Random {
    uint8_t n = 1;
    void bytes(uint8_t* out, size_t len) override {
        for (size_t i = 0; i < len; ++i) out[i] = n++;
    }
};

class FakeDevice : public QObject {
    Q_OBJECT
public:
    Q_INVOKABLE bool wifiConnected() const { return true; }
    Q_INVOKABLE void connectNetwork() { emit networkResult(true); }
    Q_INVOKABLE void quit() {}
signals:
    void networkResult(bool connected);
};

class Harness : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString pagesDir READ pagesDir CONSTANT)
public:
    pbo::Store* store = nullptr;
    QString pages_;
    QString pagesDir() const { return pages_; }

    Q_INVOKABLE void reset() {
        store->debug_exec("DELETE FROM contact");
        store->debug_exec("DELETE FROM addressbook");
        store->debug_exec("DELETE FROM account_calendar");
        pbo::CalendarAccount a;
        a.caldav_url = "https://dav.example.org/dav/";
        a.username = "alice";
        store->save_calendar_account(a);
        pbo::SyncState s = store->sync_state();
        s.outcome = "success";
        s.problem_key.clear();
        s.problem_area.clear();
        s.last_success = kNow;
        store->write_sync_state(s);
        counter_ = 0;
    }
    Q_INVOKABLE void removeAccount() { store->debug_exec("DELETE FROM account_calendar"); }
    Q_INVOKABLE qlonglong addBook(const QString& name, bool writable = true) {
        pbo::AddressBook b;
        b.href = "/dav/addressbooks/alice/" + name.toLower().toStdString() + "/";
        b.name = name.toStdString();
        b.writable = writable;
        return store->upsert_addressbook(b).value;
    }
    // a card as a pull stores it; `body` holds the property lines (without BEGIN, VERSION, UID and END)
    Q_INVOKABLE qlonglong addContact(qlonglong book, const QString& body) {
        const int n = ++counter_;
        const std::string href = store->debug_query("SELECT href FROM addressbook WHERE id = " + std::to_string(book)) + "c" + std::to_string(n) + ".vcf";
        const std::string card = "BEGIN:VCARD\r\nVERSION:3.0\r\nUID:uid-" + std::to_string(n) + "\r\n" + body.toStdString() + "END:VCARD\r\n";
        pbo::ContactPullBatch batch;
        batch.upserts.push_back(pbo::contact_upsert_from_card(href, "\"e" + std::to_string(n) + "\"", card));
        store->apply_contact_pull(book, batch);
        return std::stoll(store->debug_query("SELECT id FROM contact WHERE href = '" + href + "'"));
    }
    Q_INVOKABLE void exec(const QString& sql) { store->debug_exec(sql.toStdString()); }
    Q_INVOKABLE QString query(const QString& sql) { return QString::fromStdString(store->debug_query(sql.toStdString())); }
    // two versions: the reader's (pending_update) and the server's (kept beside it)
    Q_INVOKABLE void makeContactConflict(qlonglong id, const QString& serverFamily) {
        store->debug_exec("UPDATE contact SET sync_state = 'pending_update' WHERE id = " + std::to_string(id));
        const auto view = store->contact(id);
        std::string theirs = view->vcf;
        const size_t at = theirs.find("FN:");
        const size_t end = theirs.find("\r\n", at);
        theirs.replace(at, end - at, "FN:Anna " + serverFamily.toStdString());
        store->contact_conflicted(id, theirs, "\"srv-2\"", view->href);
    }
    Q_INVOKABLE void makeServerDeleted(qlonglong id) {
        store->debug_exec("UPDATE contact SET sync_state = 'pending_update' WHERE id = " + std::to_string(id));
        store->contact_gone_on_server(id);
    }
    Q_INVOKABLE void failContactsSync(const QString& key) {
        pbo::SyncState s = store->sync_state();
        s.outcome = "partial";
        s.problem_key = key.toStdString();
        s.problem_area = "contacts";
        store->write_sync_state(s);
    }

private:
    int counter_ = 0;
};

class Setup : public QObject {
    Q_OBJECT
public:
    Setup() = default;

public slots:
    void qmlEngineAvailable(QQmlEngine* engine) {
        store_ = std::move(pbo::Store::open(data_.path(), clock_).value);
        worker_ = std::make_unique<Worker>();
        SyncBridge::Config sc;
        sc.store = store_.get();
        sc.data_dir = data_.path();
        sc.worker = worker_.get();
        sc.clock = &clock_;
        sync_ = std::make_unique<SyncBridge>(sc);
        ContactsModel::Config mc;
        mc.store = store_.get();
        contacts_ = std::make_unique<ContactsModel>(mc);
        ContactBridge::Config bc;
        bc.store = store_.get();
        bc.clock = &clock_;
        bc.random = &random_;
        contact_ = std::make_unique<ContactBridge>(bc);
        ConflictsModel::Config cfc;
        cfc.store = store_.get();
        cfc.clock = &clock_;
        cfc.random = &random_;
        conflicts_ = std::make_unique<ConflictsModel>(cfc);
        device_ = std::make_unique<FakeDevice>();
        harness_ = std::make_unique<Harness>();
        harness_->store = store_.get();
        harness_->pages_ = QUrl::fromLocalFile(QStringLiteral(PBO_SOURCE_DIR "/qml/pages/")).toString();

        QQmlContext* ctx = engine->rootContext();
        ctx->setContextProperty("sync", sync_.get());
        ctx->setContextProperty("contacts", contacts_.get());
        ctx->setContextProperty("contact", contact_.get());
        ctx->setContextProperty("conflicts", conflicts_.get());
        ctx->setContextProperty("device", device_.get());
        ctx->setContextProperty("harness", harness_.get());
    }

private:
    pbtest::TmpDir data_;
    FixedClock clock_;
    CountingRandom random_;
    std::unique_ptr<pbo::Store> store_;
    std::unique_ptr<Worker> worker_;
    std::unique_ptr<SyncBridge> sync_;
    std::unique_ptr<ContactsModel> contacts_;
    std::unique_ptr<ContactBridge> contact_;
    std::unique_ptr<ConflictsModel> conflicts_;
    std::unique_ptr<FakeDevice> device_;
    std::unique_ptr<Harness> harness_;
};

}  // namespace

QUICK_TEST_MAIN_WITH_SETUP(contacts_flow, Setup)

#include "tst_contacts_flow.moc"
