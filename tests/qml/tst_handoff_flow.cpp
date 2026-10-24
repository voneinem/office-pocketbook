// T014 (spec 003): a hand-over through real QML (offscreen) with the real bridges and a throw-away store: the navigator opens the compose page with the
// handed-over text, subject and recipient, puts the cursor in the first empty field, and Back returns to the page below.
#include <QQmlContext>
#include <QQmlEngine>
#include <QtQuickTest>
#include <memory>
#include <string>
#include <vector>

#include "account_bridge.h"
#include "compose_bridge.h"
#include "handoff_bridge.h"
#include "outbox_model.h"
#include "pbo/store.h"
#include "test_support.h"
#include "tmp_dir.h"
#include "worker.h"

namespace {

constexpr int64_t kNow = 1791979200;

struct FixedClock : pbo::Clock {
    int64_t now() const override { return kNow; }
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

// Stands in for the sync bridge: a sync is running, and any call that would start or ask for one is counted (a hand-over must make none).
class FakeSync : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool syncing READ syncing CONSTANT)
public:
    bool syncing() const { return true; }
    Q_INVOKABLE void noteChange(bool) { ++calls; }
    Q_INVOKABLE void startupSync(bool) { ++calls; }
    int calls = 0;
};

class Harness : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString pagesDir READ pagesDir CONSTANT)
    Q_PROPERTY(QString qmlDir READ qmlDir CONSTANT)
public:
    pbo::Store* store = nullptr;
    HandoffBridge* handoff = nullptr;
    FakeSync* sync = nullptr;
    AccountBridge* account = nullptr;
    // the reader without a mail account (first start, or a calendar-only account), and the moment the user saves one in the set-up
    Q_INVOKABLE void removeMail() {
        store->debug_exec("DELETE FROM account_mail");
        emit account->mailConfiguredChanged();
    }
    Q_INVOKABLE void configureMail() {
        pbo::MailAccount a;
        a.imap_host = "mail.example.org";
        a.smtp_host = "smtp.example.org";
        a.username = "alice";
        a.address = "alice@example.org";
        a.display_name = "Alice";
        store->save_mail_account(a);
        emit account->mailConfiguredChanged();
    }
    Q_INVOKABLE int syncCalls() const { return sync->calls; }
    QString pagesDir() const { return QUrl::fromLocalFile(QStringLiteral(PBO_SOURCE_DIR "/qml/pages/")).toString(); }
    QString qmlDir() const { return QUrl::fromLocalFile(QStringLiteral(PBO_SOURCE_DIR "/qml/")).toString(); }
    Q_INVOKABLE QString query(const QString& sql) { return QString::fromStdString(store->debug_query(sql.toStdString())); }
    Q_INVOKABLE void exec(const QString& sql) { store->debug_exec(sql.toStdString()); }
    // what another app does: the marker, the text, its length, and optionally a subject and a recipient (the text is given as UTF-8 text here)
    Q_INVOKABLE void deliver(const QString& text, const QString& subject = QString(), const QString& to = QString()) {
        const std::string t = text.toStdString();
        std::vector<std::string> a = {"pbo-mail-v1", "text=" + t, "textlen=" + std::to_string(t.size())};
        if (!subject.isEmpty()) a.push_back("subject=" + subject.toStdString());
        if (!to.isEmpty()) a.push_back("to=" + to.toStdString());
        handoff->deliver(std::move(a), "listener");
    }
    // an invalid hand-over: an announced length that does not match
    Q_INVOKABLE void deliverIncomplete() { handoff->deliver({"pbo-mail-v1", "text=abc", "textlen=99"}, "listener"); }
    Q_INVOKABLE void deliverRaw(const QStringList& args) {
        std::vector<std::string> a;
        for (const QString& s : args) a.push_back(s.toStdString());
        handoff->deliver(std::move(a), "listener");
    }
};

class Setup : public QObject {
    Q_OBJECT
public:
    Setup() = default;

public slots:
    void qmlEngineAvailable(QQmlEngine* engine) {
        store_ = std::move(pbo::Store::open(data_.path(), clock_).value);
        pbo::MailAccount a;
        a.imap_host = "mail.example.org";
        a.smtp_host = "smtp.example.org";
        a.username = "alice";
        a.address = "alice@example.org";
        a.display_name = "Alice";
        store_->save_mail_account(a);
        pbo::FolderListItem inbox;
        inbox.raw_name = inbox.name = "INBOX";
        inbox.delimiter = "/";
        store_->upsert_folders({inbox});

        ComposeBridge::Config cc;
        cc.store = store_.get();
        cc.clock = &clock_;
        cc.random = &random_;
        cc.autosave_ms = 300;
        compose_ = std::make_unique<ComposeBridge>(cc);
        drafts_ = std::make_unique<DraftsModel>(DraftsModel::Config{store_.get()});
        HandoffBridge::Config hc;
        hc.store = store_.get();
        hc.clock = &clock_;
        hc.random = &random_;
        hc.handoff_dir = data_.path() + "/handoff";
        handoff_ = std::make_unique<HandoffBridge>(hc);
        worker_ = std::make_unique<Worker>();
        AccountBridge::Config ac;
        ac.store = store_.get();
        ac.data_dir = data_.path();
        ac.ca_file = data_.path() + "/no-ca.pem";
        ac.worker = worker_.get();
        account_ = std::make_unique<AccountBridge>(ac);
        device_ = std::make_unique<FakeDevice>();
        sync_ = std::make_unique<FakeSync>();
        harness_ = std::make_unique<Harness>();
        harness_->sync = sync_.get();
        harness_->account = account_.get();
        harness_->store = store_.get();
        harness_->handoff = handoff_.get();
        QQmlContext* ctx = engine->rootContext();
        ctx->setContextProperty("compose", compose_.get());
        ctx->setContextProperty("draftList", drafts_.get());
        ctx->setContextProperty("handoff", handoff_.get());
        ctx->setContextProperty("device", device_.get());
        ctx->setContextProperty("sync", sync_.get());
        ctx->setContextProperty("account", account_.get());
        ctx->setContextProperty("harness", harness_.get());
    }

private:
    pbtest::TmpDir data_;
    FixedClock clock_;
    pbo::SystemRandom random_;
    std::unique_ptr<pbo::Store> store_;
    std::unique_ptr<ComposeBridge> compose_;
    std::unique_ptr<DraftsModel> drafts_;
    std::unique_ptr<HandoffBridge> handoff_;
    std::unique_ptr<FakeDevice> device_;
    std::unique_ptr<FakeSync> sync_;
    std::unique_ptr<Worker> worker_;
    std::unique_ptr<AccountBridge> account_;
    std::unique_ptr<Harness> harness_;
};

}  // namespace

QUICK_TEST_MAIN_WITH_SETUP(handoff_flow, Setup)

#include "tst_handoff_flow.moc"
