// T049: the walking skeleton through the real QML pages (offscreen): the account form is filled and tapped Test and
// Save, Sync is tapped, and the message list page shows the fixture subjects with unread rows marked. The bridges and
// the core are real; the IMAP server is the in-process fake (UNVERIFIED against real servers, Principle IX).
#include <QQmlContext>
#include <QQmlEngine>
#include <QtQuickTest>
#include <QTimer>
#include <memory>

#include <sys/stat.h>

#include "account_bridge.h"
#include "compose_bridge.h"
#include "conflicts_model.h"
#include "outbox_model.h"
#include "attachment_bridge.h"
#include "fixtures.h"
#include "fake_imap_server.h"
#include "fake_smtp_server.h"
#include "folder_model.h"
#include "message_list_model.h"
#include "mail_image_provider.h"
#include "reader_bridge.h"
#include "pbo/store.h"
#include "sync_bridge.h"
#include "test_support.h"
#include "tls_test_server.h"
#include "tmp_dir.h"
#include "worker.h"

PBO_FAKE_SERVER_TEST();

namespace {

constexpr int64_t kNow = 1791979200;   // 2026-10-14 12:00:00 UTC

struct FixedClock : pbo::Clock {
    int64_t now() const override { return kNow; }
};

std::string mail(int i, int64_t epoch) {
    char buf[64];
    time_t t = static_cast<time_t>(epoch);
    tm tm{};
    gmtime_r(&t, &tm);
    std::strftime(buf, sizeof buf, "%a, %d %b %Y %H:%M:%S +0000", &tm);
    return "From: Sender " + std::to_string(i) + " <sender" + std::to_string(i) + "@example.org>\r\nTo: alice@example.org\r\nSubject: Subject " +
           std::to_string(i) + "\r\nDate: " + buf + "\r\nMessage-ID: <m" + std::to_string(i) + "@example.org>\r\n\r\nBody\r\n";
}

// What the QML test needs to know about the harness.
class Harness : public QObject {
    Q_OBJECT
    Q_PROPERTY(int port READ port CONSTANT)
    Q_PROPERTY(int smtpPort READ smtpPort CONSTANT)
    Q_PROPERTY(QString pagesDir READ pagesDir CONSTANT)
public:
    pbo::Store* store = nullptr;
    pbtest::FakeImapServer* server = nullptr;
    std::string data_dir;

    // A mail of tests/fixtures/mail on the fake server (the test then runs a sync to bring it in).
    Q_INVOKABLE void addServerMessage(const QString& fixtureName) {
        server->add_message("INBOX", pbtest::fixture("mail/" + fixtureName.toStdString() + ".eml"), {}, kNow - 4000);
    }
    Q_INVOKABLE qlonglong messageIdBySubject(const QString& subject) {
        return std::stoll(store->debug_query("SELECT id FROM message WHERE subject = '" + subject.toStdString() + "' ORDER BY id LIMIT 1"));
    }
    std::string downloads_dir;
    // is there a file of this name in the Downloads folder?
    Q_INVOKABLE void exec(const QString& sql) { store->debug_exec(sql.toStdString()); }
    Q_INVOKABLE QString query(const QString& sql) { return QString::fromStdString(store->debug_query(sql.toStdString())); }
    Q_INVOKABLE bool fileExists(const QString& name) {
        struct stat st;
        return ::stat((downloads_dir + "/" + name.toStdString()).c_str(), &st) == 0;
    }
    Q_INVOKABLE void setAttachmentSize(qlonglong attachmentId, qlonglong bytes) {
        store->debug_exec("UPDATE attachment SET size = " + std::to_string(bytes) + " WHERE id = " + std::to_string(attachmentId));
    }
    Q_INVOKABLE qlonglong attachmentIdOf(qlonglong messageId, int index) {
        return std::stoll(store->debug_query("SELECT id FROM attachment WHERE message_id = " + std::to_string(messageId) + " ORDER BY id LIMIT 1 OFFSET " + std::to_string(index)));
    }

    // A stored message with a long body, quoted text, a link and an attachment, for the reader page. Returns its id.
    // A message whose body has not been fetched (as after a failed sync)
    Q_INVOKABLE qlonglong addHeaderOnlyMessage() {
        pbo::PullBatch batch;
        pbo::PulledMessage m;
        m.uid = 901;
        m.subject = "Header only";
        m.from_addr = "ben@example.org";
        m.date_utc = kNow - 2000;
        batch.messages.push_back(m);
        store->apply_pull(store->folders().at(0).id, batch);
        return std::stoll(store->debug_query("SELECT id FROM message WHERE uid = 901"));
    }

    Q_INVOKABLE qlonglong addRichMessage() {
        pbo::PullBatch batch;
        pbo::PulledMessage m;
        m.uid = 900;
        m.subject = "Re: Planung";
        m.from_name = "Anna Beispiel";
        m.from_addr = "anna@example.org";
        m.to_text = "Alice <alice@example.org>";
        m.date_utc = kNow - 1000;
        batch.messages.push_back(m);
        store->apply_pull(store->folders().at(0).id, batch);
        const long long id = std::stoll(store->debug_query("SELECT id FROM message WHERE uid = 900"));
        pbo::BodyUpdate u;
        u.text = "Yes, Thursday works for me [1].\n\nOn Mon, 05 Oct 2026, Ben wrote:\n> Can we meet on Thursday?\n> I could do 10:00.\n>> Earlier: the room is booked.\n\n";
        for (int i = 1; i <= 30; ++i) u.text += "Line " + std::to_string(i) + " of a long message that must be read page by page without cutting a line.\n";
        u.text += "\nAnna";
        u.links = {"https://www.example.org/a"};
        pbo::StoredAttachment a;
        a.section = "2";
        a.name = "report.pdf";
        a.mime_type = "application/pdf";
        a.size = 2500;
        a.encoding = "base64";
        u.attachments = {a};
        store->apply_body(id, u);
        return id;
    }

    // A mail of an HTML newsletter: text, a picture that is a part of the mail (loaded), a picture on the sender's server (not loaded), more text, and
    // three links, one of them very long (tracking).
    Q_INVOKABLE qlonglong addPictureMessage() {
        pbo::PullBatch batch;
        pbo::PulledMessage m;
        m.uid = 901;
        m.subject = "Offer";
        m.from_name = "Shop";
        m.from_addr = "news@example.org";
        m.to_text = "Alice <alice@example.org>";
        m.date_utc = kNow - 500;
        batch.messages.push_back(m);
        store->apply_pull(store->folders().at(0).id, batch);
        const long long id = std::stoll(store->debug_query("SELECT id FROM message WHERE uid = 901"));
        const std::string mark = "\xEF\xBF\xBC";
        pbo::BodyUpdate u;
        u.text = "Hello Volker\n\n" + mark + "1" + mark + "\n\nSome words about the offer [1].\n\n" + mark + "2" + mark + "\n\nGoodbye";
        u.links = {"https://shop.example.org/a", "https://www.shop.example.org/tracking/redirect?campaign=autumn&user=0123456789abcdef0123456789abcdef&url=https%3A%2F%2Fshop.example.org%2Fproducts%2Fheater",
                   "mailto:news@example.org"};
        pbo::StoredImage cid;
        cid.idx = 1;
        cid.kind = "cid";
        cid.src = "cid:logo";
        cid.alt = "Logo";
        cid.section = "1.2";
        pbo::StoredImage remote;
        remote.idx = 2;
        remote.kind = "remote";
        remote.src = "https://img.example.org/banner.png";
        remote.alt = "Banner";
        u.images = {cid, remote};
        store->apply_body(id, u);
        const QByteArray png = QByteArray::fromBase64("iVBORw0KGgoAAAANSUhEUgAAAAQAAAADCAIAAAA7ljmRAAAAEElEQVR4nGM4IScHRww4OQD1xwwxB+utmQAAAABJRU5ErkJggg==");
        store->set_image_loaded(id, 1, "image/png", std::string(png.constData(), static_cast<size_t>(png.size())));
        return id;
    }

    int port() const { return port_; }
    int smtpPort() const { return smtp_port_; }
    QString pagesDir() const { return pages_; }
    int port_ = 0;
    int smtp_port_ = 0;
    QString pages_;
};

// Stands in for the app's DeviceBridge: Wi-Fi can be down, and connectNetwork() succeeds or fails on demand.
class FakeDevice : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool wifiUp READ wifiUp WRITE setWifiUp NOTIFY wifiChanged)
    Q_PROPERTY(bool connectWorks READ connectWorks WRITE setConnectWorks NOTIFY wifiChanged)
    Q_PROPERTY(int connectCalls READ connectCalls NOTIFY wifiChanged)
    Q_PROPERTY(int quitCalls READ quitCalls NOTIFY wifiChanged)
public:
    int quitCalls() const { return quits_; }
    Q_INVOKABLE void quit() { ++quits_; emit wifiChanged(); }
    bool wifiUp() const { return up_; }
    void setWifiUp(bool v) { up_ = v; emit wifiChanged(); }
    bool connectWorks() const { return works_; }
    void setConnectWorks(bool v) { works_ = v; emit wifiChanged(); }
    int connectCalls() const { return calls_; }
    Q_INVOKABLE bool wifiConnected() const { return up_; }
    Q_INVOKABLE void connectNetwork() {
        ++calls_;
        // the answer comes later, like the system's connect dialog
        QTimer::singleShot(50, this, [this] {
            if (works_) up_ = true;
            emit networkResult(up_);
            emit wifiChanged();
        });
    }
signals:
    void networkResult(bool connected);
    void wifiChanged();

private:
    bool up_ = true, works_ = true;
    int calls_ = 0;
    int quits_ = 0;
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
        pbtest::FakeImapServer::Config c;
        c.cert = leaf_.cert;
        c.key = leaf_.key;
        server_ = std::make_unique<pbtest::FakeImapServer>(c);
        pbtest::FakeSmtpServer::Config smtp_config;
        smtp_config.cert = leaf_.cert;
        smtp_config.key = leaf_.key;
        smtp_server_ = std::make_unique<pbtest::FakeSmtpServer>(smtp_config);
        server_->set_capabilities("IMAP4rev1 UIDPLUS LITERAL+");
        server_->add_folder("INBOX");
        for (int i = 1; i <= 12; ++i) {
            const int64_t when = kNow - (13 - i) * 86400 + 3600;
            server_->add_message("INBOX", mail(i, when), i % 3 == 0 ? std::set<std::string>{"\\Seen"} : std::set<std::string>{}, when);
        }
        store_ = std::move(pbo::Store::open(data_.path(), clock_).value);
        worker_ = std::make_unique<Worker>();

        AccountBridge::Config ac;
        ac.store = store_.get();
        ac.data_dir = data_.path();
        ac.ca_file = pki_->ca_cert();
        ac.worker = worker_.get();
        account_ = std::make_unique<AccountBridge>(ac);
        SyncBridge::Config sc;
        sc.store = store_.get();
        sc.data_dir = data_.path();
        sc.ca_file = pki_->ca_cert();
        sc.worker = worker_.get();
        sc.clock = &clock_;
        sc.auto_sync_quiet_ms = 150;   // the tests switch Auto Sync on where they need it (setting auto_sync)
        sc.auto_sync_gap_ms = 0;
        store_->set_setting("auto_sync", "0");
        sync_ = std::make_unique<SyncBridge>(sc);
        MessageListModel::Config mc;
        mc.store = store_.get();
        messages_ = std::make_unique<MessageListModel>(mc);
        FolderModel::Config fc;
        fc.store = store_.get();
        folders_ = std::make_unique<FolderModel>(fc);
        device_ = std::make_unique<FakeDevice>();
        ComposeBridge::Config cc;
        cc.store = store_.get();
        cc.clock = &clock_;
        cc.random = &random_;
        compose_ = std::make_unique<ComposeBridge>(cc);
        outbox_ = std::make_unique<OutboxModel>(OutboxModel::Config{store_.get()});
        drafts_ = std::make_unique<DraftsModel>(DraftsModel::Config{store_.get()});
        {
            ConflictsModel::Config cfc;
            cfc.store = store_.get();
            conflicts_ = std::make_unique<ConflictsModel>(cfc);
        }
        {
            MessageListModel::Config smc;
            smc.store = store_.get();
            search_ = std::make_unique<MessageListModel>(smc);
        }
        ReaderBridge::Config rc;
        rc.store = store_.get();
        reader_ = std::make_unique<ReaderBridge>(rc);
        harness_ = std::make_unique<Harness>();
        harness_->store = store_.get();
        harness_->server = server_.get();
        harness_->data_dir = data_.path();
        harness_->downloads_dir = downloads_.path() + "/Downloads";
        AttachmentBridge::Config atc;
        atc.store = store_.get();
        atc.data_dir = data_.path();
        atc.ca_file = pki_->ca_cert();
        atc.downloads_dir = downloads_.path() + "/Downloads";
        atc.worker = worker_.get();
        attachments_ = std::make_unique<AttachmentBridge>(atc);
        harness_->port_ = server_->port();
        harness_->smtp_port_ = smtp_server_->port();
        harness_->pages_ = QUrl::fromLocalFile(QStringLiteral(PBO_SOURCE_DIR "/qml/pages/")).toString();

        engine->addImageProvider(QStringLiteral("mailimg"), new MailImageProvider(store_.get()));   // the engine owns it
        QQmlContext* ctx = engine->rootContext();
        ctx->setContextProperty("account", account_.get());
        ctx->setContextProperty("sync", sync_.get());
        ctx->setContextProperty("messages", messages_.get());
        ctx->setContextProperty("folders", folders_.get());
        ctx->setContextProperty("harness", harness_.get());
        ctx->setContextProperty("reader", reader_.get());
        ctx->setContextProperty("attachments", attachments_.get());
        ctx->setContextProperty("device", device_.get());
        ctx->setContextProperty("compose", compose_.get());
        ctx->setContextProperty("outbox", outbox_.get());
        ctx->setContextProperty("draftList", drafts_.get());
        ctx->setContextProperty("conflicts", conflicts_.get());
        ctx->setContextProperty("searchResults", search_.get());
    }

private:
    pbtest::TmpDir data_, pki_dir_, downloads_;
    FixedClock clock_;
    std::unique_ptr<pbtest::TestPki> pki_;
    pbtest::TestPki::Leaf leaf_;
    std::unique_ptr<pbtest::FakeImapServer> server_;
    std::unique_ptr<pbtest::FakeSmtpServer> smtp_server_;
    std::unique_ptr<pbo::Store> store_;
    std::unique_ptr<Worker> worker_;
    std::unique_ptr<AccountBridge> account_;
    std::unique_ptr<SyncBridge> sync_;
    std::unique_ptr<MessageListModel> messages_;
    std::unique_ptr<FolderModel> folders_;
    std::unique_ptr<ReaderBridge> reader_;
    std::unique_ptr<AttachmentBridge> attachments_;
    std::unique_ptr<Harness> harness_;
    std::unique_ptr<FakeDevice> device_;
    pbo::SystemRandom random_;
    std::unique_ptr<ComposeBridge> compose_;
    std::unique_ptr<OutboxModel> outbox_;
    std::unique_ptr<DraftsModel> drafts_;
    std::unique_ptr<ConflictsModel> conflicts_;
    std::unique_ptr<MessageListModel> search_;
};

}  // namespace

QUICK_TEST_MAIN_WITH_SETUP(skeleton_flow, Setup)

#include "tst_skeleton_flow.moc"
