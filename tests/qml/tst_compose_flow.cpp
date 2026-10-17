// T134: the compose and outbox pages through real QML (offscreen) with the real bridges, the core and the in-process fake IMAP and SMTP servers.
// UNVERIFIED against real servers (Principle IX).
#include <QQmlContext>
#include <QQmlEngine>
#include <QtQuickTest>
#include <memory>

#include "account_bridge.h"
#include "compose_bridge.h"
#include "conflicts_model.h"
#include "fake_imap_server.h"
#include "fake_smtp_server.h"
#include "outbox_model.h"
#include "pbo/credentials_file.h"
#include "pbo/store.h"
#include "sync_bridge.h"
#include "test_support.h"
#include "tls_test_server.h"
#include "tmp_dir.h"
#include "worker.h"

PBO_FAKE_SERVER_TEST();

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

class Harness : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString pagesDir READ pagesDir CONSTANT)
public:
    pbo::Store* store = nullptr;
    pbtest::FakeSmtpServer* smtp = nullptr;
    pbtest::FakeImapServer* imap = nullptr;
    QString pages_;
    QString pagesDir() const { return pages_; }
    Q_INVOKABLE QString query(const QString& sql) { return QString::fromStdString(store->debug_query(sql.toStdString())); }
    Q_INVOKABLE void exec(const QString& sql) { store->debug_exec(sql.toStdString()); }
    Q_INVOKABLE int accepted() { return static_cast<int>(smtp->accepted().size()); }
    Q_INVOKABLE void refuseRecipients(bool on) { if (on) smtp->refuse_recipient("bob@example.org"); else smtp->refuse_recipient("nobody@example.org"); }
    Q_INVOKABLE int sentCopies() { return imap->message_count("Sent"); }
    Q_INVOKABLE void clearAll() { store->debug_exec("DELETE FROM outbox; DELETE FROM draft"); }
    Q_INVOKABLE void breakSmtpLogin(bool on) { smtp->require_auth = true; broken_ = on; }
    bool broken_ = false;
    // a queued message of the given state, for the outbox page
    Q_INVOKABLE qlonglong addOutbox(const QString& subject, const QString& state, const QString& reason) {
        const std::string mid = "<h" + std::to_string(++n_) + "@example.org>";
        store->debug_exec("INSERT INTO draft(kind, message_id, to_text, subject, created, updated, content_hash) VALUES('new', '" + mid + "', 'bob@example.org', '" + subject.toStdString() + "', 1, 1, 'x')");
        const std::string draft = store->debug_query("SELECT id FROM draft WHERE message_id = '" + mid + "'");
        store->debug_exec("INSERT INTO outbox(draft_id, message_id, envelope_from, envelope_to, subject_preview, raw_message, state, error_key, queued_at, state_at) VALUES(" + draft + ", '" + mid + "', 'alice@example.org', 'bob@example.org', '" + subject.toStdString() + "', x'41', '" + state.toStdString() + "', '" + reason.toStdString() + "', 1, 1)");
        return std::stoll(store->debug_query("SELECT id FROM outbox WHERE message_id = '" + mid + "'"));
    }
    int n_ = 0;
};

class Setup : public QObject {
    Q_OBJECT
public:
    Setup() = default;

public slots:
    void qmlEngineAvailable(QQmlEngine* engine) {
        pki_ = std::make_unique<pbtest::TestPki>(pki_dir_);
        if (!pki_->available()) qFatal("the openssl command is not available");
        leaf_ = pki_->issue("srv", "DNS:localhost,IP:127.0.0.1");
        pbtest::FakeImapServer::Config ic;
        ic.cert = leaf_.cert;
        ic.key = leaf_.key;
        imap_ = std::make_unique<pbtest::FakeImapServer>(ic);
        imap_->set_capabilities("IMAP4rev1 UIDPLUS LITERAL+");
        imap_->add_folder("INBOX");
        imap_->add_folder("Sent", {"\\Sent"});
        imap_->add_folder("Drafts", {"\\Drafts"});
        pbtest::FakeSmtpServer::Config sc;
        sc.cert = leaf_.cert;
        sc.key = leaf_.key;
        smtp_ = std::make_unique<pbtest::FakeSmtpServer>(sc);
        store_ = std::move(pbo::Store::open(data_.path(), clock_).value);
        worker_ = std::make_unique<Worker>();
        pbo::MailAccount a;
        a.imap_host = "localhost";
        a.imap_port = imap_->port();
        a.smtp_host = "localhost";
        a.smtp_port = smtp_->port();
        a.username = "alice";
        a.address = "alice@example.org";
        a.display_name = "Alice";
        store_->save_mail_account(a);
        pbo::Credentials creds;
        creds.mail_password = "pw-for-tests";
        pbo::save_credentials(data_.path(), creds);
        pbo::FolderListItem inbox, sent, drafts;
        inbox.raw_name = inbox.name = "INBOX";
        sent.raw_name = sent.name = "Sent";
        sent.attributes = {"\\Sent"};
        drafts.raw_name = drafts.name = "Drafts";
        drafts.attributes = {"\\Drafts"};
        inbox.delimiter = sent.delimiter = drafts.delimiter = "/";
        store_->upsert_folders({inbox, sent, drafts});

        SyncBridge::Config syc;
        syc.store = store_.get();
        syc.data_dir = data_.path();
        syc.ca_file = pki_->ca_cert();
        syc.worker = worker_.get();
        syc.clock = &clock_;
        syc.auto_sync_quiet_ms = 150;   // a short quiet period for the tests
        sync_ = std::make_unique<SyncBridge>(syc);
        ComposeBridge::Config cc;
        cc.store = store_.get();
        cc.clock = &clock_;
        cc.random = &random_;
        cc.autosave_ms = 300;
        compose_ = std::make_unique<ComposeBridge>(cc);
        outbox_ = std::make_unique<OutboxModel>(OutboxModel::Config{store_.get()});
        drafts_ = std::make_unique<DraftsModel>(DraftsModel::Config{store_.get()});
        {
            ConflictsModel::Config cfc;
            cfc.store = store_.get();
            conflicts_ = std::make_unique<ConflictsModel>(cfc);
        }
        device_ = std::make_unique<FakeDevice>();
        harness_ = std::make_unique<Harness>();
        harness_->store = store_.get();
        harness_->smtp = smtp_.get();
        harness_->imap = imap_.get();
        harness_->pages_ = QUrl::fromLocalFile(QStringLiteral(PBO_SOURCE_DIR "/qml/pages/")).toString();
        QQmlContext* ctx = engine->rootContext();
        ctx->setContextProperty("sync", sync_.get());
        ctx->setContextProperty("compose", compose_.get());
        ctx->setContextProperty("outbox", outbox_.get());
        ctx->setContextProperty("draftList", drafts_.get());
        ctx->setContextProperty("conflicts", conflicts_.get());
        ctx->setContextProperty("device", device_.get());
        ctx->setContextProperty("harness", harness_.get());
    }

private:
    pbtest::TmpDir data_, pki_dir_;
    FixedClock clock_;
    pbo::SystemRandom random_;
    std::unique_ptr<pbtest::TestPki> pki_;
    pbtest::TestPki::Leaf leaf_;
    std::unique_ptr<pbtest::FakeImapServer> imap_;
    std::unique_ptr<pbtest::FakeSmtpServer> smtp_;
    std::unique_ptr<pbo::Store> store_;
    std::unique_ptr<Worker> worker_;
    std::unique_ptr<SyncBridge> sync_;
    std::unique_ptr<ComposeBridge> compose_;
    std::unique_ptr<OutboxModel> outbox_;
    std::unique_ptr<DraftsModel> drafts_;
    std::unique_ptr<ConflictsModel> conflicts_;
    std::unique_ptr<FakeDevice> device_;
    std::unique_ptr<Harness> harness_;
};

}  // namespace

QUICK_TEST_MAIN_WITH_SETUP(compose_flow, Setup)

#include "tst_compose_flow.moc"
