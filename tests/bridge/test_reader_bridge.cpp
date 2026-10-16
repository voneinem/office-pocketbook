// T082 (reader part): ReaderBridge turns a stored message into header texts, blocks with a quote flag, a link list and
// attachment rows.
#include <QCoreApplication>
#include <QSignalSpy>
#include <QVariantMap>

#include "fake_transport.h"
#include "pbo/store.h"
#include "mail_image_provider.h"
#include "reader_bridge.h"
#include "worker.h"
#include "test_support.h"
#include "tmp_dir.h"

namespace {

int g_argc = 1;
char g_arg0[] = "test_reader_bridge";
char* g_argv[] = {g_arg0, nullptr};
QCoreApplication g_app(g_argc, g_argv);

struct TestClock : pbo::Clock {
    int64_t now() const override { return 1791979200; }
};

struct Env {
    pbtest::TmpDir dir;
    TestClock clock;
    std::unique_ptr<pbo::Store> store;
    int64_t id = 0;
    Env() {
        store = std::move(pbo::Store::open(dir.path(), clock).value);
        pbo::FolderListItem inbox;
        inbox.raw_name = inbox.name = "INBOX";
        store->upsert_folders({inbox});
        pbo::PullBatch b;
        pbo::PulledMessage m;
        m.uid = 1;
        m.subject = "Re: Planung";
        m.from_name = "Anna Beispiel";
        m.from_addr = "anna@example.org";
        m.to_text = "Alice <alice@example.org>";
        m.cc_text = "ben@example.org";
        m.date_utc = 1791194400;
        m.flags = 0;
        b.messages.push_back(m);
        store->apply_pull(store->folders().at(0).id, b);
        id = std::stoll(store->debug_query("SELECT id FROM message"));
    }
};

}  // namespace

PB_TEST(a_message_is_shown_with_header_texts_and_blocks) {
    Env env;
    pbo::BodyUpdate u;
    u.text = "Yes, Thursday works.\n\nOn Mon, 05 Oct 2026, Ben wrote:\n> Can we meet on Thursday?\n> I could do 10:00.\n>> Earlier: the room is booked.\n\nAnna\nSecond line";
    u.links = {"https://www.example.org/a"};
    u.note = "html_converted";
    pbo::StoredAttachment a;
    a.section = "2";
    a.name = "report.pdf";
    a.mime_type = "application/pdf";
    a.size = 2500;
    a.encoding = "base64";
    pbo::StoredAttachment b = a;
    b.section = "3";
    b.name = "";
    b.size = 1300000;
    u.attachments = {a, b};
    env.store->apply_body(env.id, u);

    ReaderBridge::Config cfg;
    cfg.store = env.store.get();
    ReaderBridge r(cfg);
    PB_CHECK(!r.loaded());
    r.open(env.id);
    PB_CHECK(r.loaded());
    PB_CHECK(r.errorKey().isEmpty());
    PB_CHECK_EQ(r.subject().toStdString(), std::string("Re: Planung"));
    PB_CHECK_EQ(r.from().toStdString(), std::string("Anna Beispiel <anna@example.org>"));
    PB_CHECK_EQ(r.to().toStdString(), std::string("Alice <alice@example.org>"));
    PB_CHECK_EQ(r.cc().toStdString(), std::string("ben@example.org"));
    PB_CHECK(r.dateText().contains("2026"));
    PB_CHECK_EQ(r.bodyState().toStdString(), std::string("text"));
    PB_CHECK_EQ(r.bodyNote().toStdString(), std::string("html_converted"));
    PB_CHECK(r.unread());

    const QVariantList blocks = r.blocks();
    PB_CHECK_EQ(blocks.size(), 5);
    auto block = [&](int i) { return blocks.at(i).toMap(); };
    PB_CHECK_EQ(block(0).value("text").toString().toStdString(), std::string("Yes, Thursday works."));
    PB_CHECK(!block(0).value("quoted").toBool());
    PB_CHECK_EQ(block(1).value("text").toString().toStdString(), std::string("On Mon, 05 Oct 2026, Ben wrote:"));
    PB_CHECK(!block(1).value("quoted").toBool());
    PB_CHECK_EQ(block(2).value("text").toString().toStdString(), std::string("Can we meet on Thursday?\nI could do 10:00."));
    PB_CHECK(block(2).value("quoted").toBool());
    PB_CHECK_EQ(block(2).value("level").toInt(), 1);
    PB_CHECK_EQ(block(3).value("text").toString().toStdString(), std::string("Earlier: the room is booked."));
    PB_CHECK_EQ(block(3).value("level").toInt(), 2);
    PB_CHECK_EQ(block(4).value("text").toString().toStdString(), std::string("Anna\nSecond line"));
    PB_CHECK(!block(4).value("quoted").toBool());

    PB_CHECK_EQ(r.links().size(), 1);
    PB_CHECK_EQ(r.links().at(0).toString().toStdString(), std::string("https://www.example.org/a"));
    const QVariantList att = r.attachments();
    PB_CHECK_EQ(att.size(), 2);
    PB_CHECK_EQ(att.at(0).toMap().value("name").toString().toStdString(), std::string("report.pdf"));
    PB_CHECK_EQ(att.at(0).toMap().value("sizeText").toString().toStdString(), std::string("2.5 KB"));
    PB_CHECK_EQ(att.at(0).toMap().value("state").toString().toStdString(), std::string("remote"));
    PB_CHECK_EQ(att.at(1).toMap().value("sizeText").toString().toStdString(), std::string("1.3 MB"));
    PB_CHECK_EQ(att.at(1).toMap().value("section").toString().toStdString(), std::string("3"));
}

PB_TEST(a_message_without_a_fetched_body_says_so_and_an_unknown_id_is_not_found) {
    Env env;
    ReaderBridge::Config cfg;
    cfg.store = env.store.get();
    ReaderBridge r(cfg);
    r.open(env.id);
    PB_CHECK(r.loaded());
    PB_CHECK_EQ(r.bodyState().toStdString(), std::string("headers"));
    PB_CHECK(r.blocks().isEmpty());
    r.open(424242);
    PB_CHECK(!r.loaded());
    PB_CHECK_EQ(r.errorKey().toStdString(), std::string("not_found"));
    PB_CHECK(r.subject().isEmpty());
}

PB_TEST(opening_a_message_does_not_mark_it_read_and_close_clears_it) {
    Env env;
    ReaderBridge::Config cfg;
    cfg.store = env.store.get();
    ReaderBridge r(cfg);
    r.open(env.id);
    PB_CHECK_EQ(env.store->debug_query("SELECT flags_local FROM message"), std::string("0"));   // marking read is a separate, explicit action
    PB_CHECK_EQ(env.store->debug_query("SELECT last_opened IS NOT NULL FROM message"), std::string("1"));
    r.close();
    PB_CHECK(!r.loaded());
    PB_CHECK(r.blocks().isEmpty());
}

PB_TEST(sizes_use_decimal_units_with_one_decimal) {
    PB_CHECK_EQ(ReaderBridge::sizeText(0).toStdString(), std::string("0 B"));
    PB_CHECK_EQ(ReaderBridge::sizeText(999).toStdString(), std::string("999 B"));
    PB_CHECK_EQ(ReaderBridge::sizeText(1000).toStdString(), std::string("1.0 KB"));
    PB_CHECK_EQ(ReaderBridge::sizeText(1536).toStdString(), std::string("1.5 KB"));
    PB_CHECK_EQ(ReaderBridge::sizeText(2500000).toStdString(), std::string("2.5 MB"));
    PB_CHECK_EQ(ReaderBridge::sizeText(3500000000).toStdString(), std::string("3.5 GB"));
    PB_CHECK_EQ(ReaderBridge::sizeText(-5).toStdString(), std::string("0 B"));
}

// ---- pictures (T190) ----

namespace {
const std::string kMark = "\xEF\xBF\xBC";
const std::string kPng = std::string("\x89PNG\r\n\x1a\n", 8) + "rest-of-a-picture";

pbo::StoredImage picture(int idx, const std::string& kind, const std::string& src, const std::string& alt) {
    pbo::StoredImage im;
    im.idx = idx;
    im.kind = kind;
    im.src = src;
    im.alt = alt;
    if (kind == "cid") im.section = "1." + std::to_string(idx + 1);
    return im;
}
}  // namespace

PB_TEST(picture_markers_become_picture_blocks_between_the_text_blocks) {
    Env env;
    pbo::BodyUpdate u;
    u.text = "Hello Volker\n\n" + kMark + "1" + kMark + "\n\nSome text\n" + kMark + "2" + kMark + "\nlast line";
    u.images = {picture(1, "cid", "cid:logo", "Logo"), picture(2, "remote", "https://img.example.org/a.jpg", "Banner")};
    PB_CHECK(env.store->apply_body(env.id, u).ok());
    PB_CHECK(env.store->set_image_loaded(env.id, 1, "image/png", kPng).ok());
    ReaderBridge::Config cfg;
    cfg.store = env.store.get();
    ReaderBridge r(cfg);
    r.open(env.id);
    const QVariantList b = r.blocks();
    PB_CHECK_EQ(b.size(), 5);
    PB_CHECK_EQ(b.at(0).toMap().value("kind").toString().toStdString(), std::string("text"));
    PB_CHECK_EQ(b.at(0).toMap().value("text").toString().toStdString(), std::string("Hello Volker"));
    const QVariantMap first = b.at(1).toMap();
    PB_CHECK_EQ(first.value("kind").toString().toStdString(), std::string("image"));
    PB_CHECK_EQ(first.value("index").toInt(), 1);
    PB_CHECK_EQ(first.value("state").toString().toStdString(), std::string("loaded"));
    PB_CHECK_EQ(first.value("alt").toString().toStdString(), std::string("Logo"));
    PB_CHECK(first.value("source").toString().startsWith(QStringLiteral("image://mailimg/")));
    PB_CHECK(first.value("source").toString().contains(QString::number(env.id) + QStringLiteral("/1")));
    PB_CHECK_EQ(b.at(2).toMap().value("text").toString().toStdString(), std::string("Some text"));
    const QVariantMap second = b.at(3).toMap();
    PB_CHECK_EQ(second.value("state").toString().toStdString(), std::string("pending"));
    PB_CHECK_EQ(second.value("sourceKind").toString().toStdString(), std::string("remote"));
    PB_CHECK(second.value("source").toString().isEmpty());                       // nothing to show before the picture is loaded
    PB_CHECK_EQ(b.at(4).toMap().value("text").toString().toStdString(), std::string("last line"));
    PB_CHECK_EQ(r.imageCount(), 2);
    PB_CHECK_EQ(r.remotePending(), 1);                                           // "Load images" is offered for the one on the sender's server
}

PB_TEST(a_marker_without_a_picture_row_and_a_marker_inside_a_line_are_plain_text_or_nothing) {
    Env env;
    pbo::BodyUpdate u;
    u.text = "before\n" + kMark + "9" + kMark + "\nin a line " + kMark + "1" + kMark + " here";
    u.images = {picture(1, "cid", "cid:a", "A")};
    PB_CHECK(env.store->apply_body(env.id, u).ok());
    ReaderBridge::Config cfg;
    cfg.store = env.store.get();
    ReaderBridge r(cfg);
    r.open(env.id);
    for (const QVariant& v : r.blocks()) {
        const QVariantMap m = v.toMap();
        if (m.value("kind").toString() == QLatin1String("image")) PB_CHECK_EQ(m.value("index").toInt(), 1);   // the only picture that exists, and only as a line of its own
        PB_CHECK(!m.value("text").toString().contains(QString::fromUtf8("\xEF\xBF\xBC")));
    }
}

PB_TEST(loading_the_pictures_asks_for_the_remote_ones_once_and_the_blocks_then_show_them) {
    Env env;
    pbo::BodyUpdate u;
    u.text = kMark + "1" + kMark + "\n" + kMark + "2" + kMark;
    u.images = {picture(1, "remote", "https://img.example.org/a.png", "A"), picture(2, "remote", "https://img.example.org/b.png", "B")};
    PB_CHECK(env.store->apply_body(env.id, u).ok());
    Worker worker;
    pbtest::FakeTransport http;
    http.set_handler([](const pbo::Request& r) {
        pbo::Response resp;
        if (r.url.find("a.png") != std::string::npos) {
            resp.status = 200;
            resp.body = kPng;
        } else {
            resp.status = 404;
        }
        return resp;
    });
    ReaderBridge::Config cfg;
    cfg.store = env.store.get();
    cfg.worker = &worker;
    cfg.http_factory = [&http]() -> std::unique_ptr<pbo::HttpTransport> {
        struct Ref : pbo::HttpTransport {
            pbo::HttpTransport* t;
            explicit Ref(pbo::HttpTransport* x) : t(x) {}
            pbo::Response perform(const pbo::Request& q) override { return t->perform(q); }
        };
        return std::make_unique<Ref>(&http);
    };
    ReaderBridge r(cfg);
    r.open(env.id);
    PB_CHECK(http.requests().empty());                                  // opening a message loads nothing
    PB_CHECK_EQ(r.remotePending(), 2);
    QSignalSpy done(&r, &ReaderBridge::imagesLoaded);
    r.loadImages();
    PB_CHECK(r.loadingImages());
    PB_CHECK(done.wait(10000));
    PB_CHECK(!r.loadingImages());
    PB_CHECK_EQ(done.at(0).at(0).toBool(), true);
    PB_CHECK_EQ(done.at(0).at(2).toInt(), 1);                           // loaded
    PB_CHECK_EQ(done.at(0).at(3).toInt(), 1);                           // failed
    PB_CHECK_EQ(http.requests().size(), size_t(2));
    const QVariantList b = r.blocks();
    PB_CHECK_EQ(b.at(0).toMap().value("state").toString().toStdString(), std::string("loaded"));
    PB_CHECK_EQ(b.at(1).toMap().value("state").toString().toStdString(), std::string("failed"));
    PB_CHECK_EQ(r.remotePending(), 1);                                  // the failed one can be asked for again
    worker.wait_idle();
}

PB_TEST(a_second_tap_while_loading_does_nothing_and_without_a_worker_nothing_happens) {
    Env env;
    pbo::BodyUpdate u;
    u.text = kMark + "1" + kMark;
    u.images = {picture(1, "remote", "https://img.example.org/a.png", "A")};
    PB_CHECK(env.store->apply_body(env.id, u).ok());
    ReaderBridge::Config cfg;
    cfg.store = env.store.get();
    ReaderBridge r(cfg);
    r.open(env.id);
    r.loadImages();                    // no worker: nothing to run it on
    PB_CHECK(!r.loadingImages());
}

PB_TEST(the_picture_provider_hands_out_the_stored_bytes_as_an_image_and_nothing_for_the_rest) {
    Env env;
    pbo::BodyUpdate u;
    u.text = kMark + "1" + kMark;
    u.images = {picture(1, "cid", "cid:a", "A"), picture(2, "cid", "cid:b", "B")};
    PB_CHECK(env.store->apply_body(env.id, u).ok());
    // a real 4 x 3 PNG
    const QByteArray png = QByteArray::fromBase64("iVBORw0KGgoAAAANSUhEUgAAAAQAAAADCAIAAAA7ljmRAAAAEElEQVR4nGM4IScHRww4OQD1xwwxB+utmQAAAABJRU5ErkJggg==");
    PB_CHECK(env.store->set_image_loaded(env.id, 1, "image/png", std::string(png.constData(), static_cast<size_t>(png.size()))).ok());
    MailImageProvider provider(env.store.get());
    QSize size;
    const QImage ok = provider.requestImage(QString::number(env.id) + QStringLiteral("/1"), &size, QSize());
    PB_CHECK(!ok.isNull());
    PB_CHECK_EQ(size.width(), 4);
    PB_CHECK_EQ(size.height(), 3);
    const QImage scaled = provider.requestImage(QString::number(env.id) + QStringLiteral("/1?v=1"), &size, QSize(2, 2));
    PB_CHECK(scaled.width() <= 2 && scaled.height() <= 2 && !scaled.isNull());
    PB_CHECK(provider.requestImage(QString::number(env.id) + QStringLiteral("/2"), &size, QSize()).isNull());     // not loaded
    PB_CHECK(provider.requestImage(QStringLiteral("999/1"), &size, QSize()).isNull());                             // no such message
    PB_CHECK(provider.requestImage(QStringLiteral("nonsense"), &size, QSize()).isNull());
}

PB_TEST_MAIN()
