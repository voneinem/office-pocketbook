// T065 (FR-040, constitution Principle IV/privacy): reading never fetches anything remote. An HTML-only message with an image, a
// style sheet and a tracking pixel opens as text; the server sees no new connection while it is read; links are only text.
// UNVERIFIED against real servers (fake server).
#include <memory>
#include <string>
#include <vector>

#include "fake_imap_server.h"
#include "fixtures.h"
#include "pbo/imap_transport.h"
#include "pbo/store.h"
#include "pbo/sync.h"
#include "test_support.h"
#include "tls_test_server.h"
#include "tmp_dir.h"

using pbtest::FakeImapServer;

PBO_FAKE_SERVER_TEST();

namespace {

constexpr int64_t kNow = 1791979200;   // 2026-10-14 12:00:00 UTC

struct FixedClock : pbo::Clock {
    int64_t now() const override { return kNow; }
};
struct NeverCancel : pbo::Cancel {
    bool requested() const override { return false; }
};

struct Env {
    pbtest::TmpDir dir, pki_dir;
    pbtest::TestPki pki{pki_dir};
    pbtest::TestPki::Leaf leaf;
    FixedClock clock;
    NeverCancel cancel;
    std::unique_ptr<FakeImapServer> server;
    std::unique_ptr<pbo::Store> store;
    Env() {
        if (!pki.available()) throw pbtest::Skip{"the openssl command is not available"};
        leaf = pki.issue("srv", "DNS:localhost,IP:127.0.0.1");
        if (!leaf.ok) throw pbtest::Skip{"cannot create a test certificate"};
        FakeImapServer::Config c;
        c.cert = leaf.cert;
        c.key = leaf.key;
        server = std::make_unique<FakeImapServer>(c);
        server->set_capabilities("IMAP4rev1 UIDPLUS LITERAL+");
        server->add_folder("INBOX");
        store = std::move(pbo::Store::open(dir.path(), clock).value);
        pbo::MailAccount a;
        a.imap_host = "localhost";
        a.imap_port = server->port();
        a.smtp_host = "localhost";
        a.username = "alice";
        a.address = "alice@example.org";
        store->save_mail_account(a);
    }
    pbo::SyncReport sync() {
        auto transport = pbo::make_curl_imap_transport(pki.ca_cert());   // destroyed when the sync is over
        pbo::SyncDeps deps{*store, *transport, clock, cancel};
        pbo::Credentials creds;
        creds.mail_password = "pw-for-tests";
        return pbo::run_sync(deps, creds);
    }
};

}  // namespace

namespace {
const char* kTracking =
    "From: Shop <shop@example.org>\r\nTo: alice@example.org\r\nDate: Mon, 12 Oct 2026 10:00:00 +0000\r\nMessage-ID: <track@example.org>\r\n"
    "Subject: Offer\r\nMIME-Version: 1.0\r\nContent-Type: text/html; charset=utf-8\r\n\r\n"
    "<html><head><link rel=\"stylesheet\" href=\"http://css.example.net/style.css\"><style>@import url(http://css.example.net/x.css);</style></head>"
    "<body background=\"http://bg.example.net/b.png\"><p>Big sale <a href=\"https://shop.example.org/sale?u=1\">here</a></p>"
    "<img src=\"http://tracker.example.net/p.gif?id=77\" width=1 height=1>"
    "<img src=\"cid:logo\"><iframe src=\"http://frame.example.net/\"></iframe></body></html>";
}

PB_TEST(an_html_message_with_remote_content_opens_as_text_and_nothing_is_fetched) {
    Env env;
    env.server->add_message("INBOX", kTracking, {}, kNow - 86400);
    env.server->add_message("INBOX", pbtest::fixture("mail/html_only.eml"), {}, kNow - 2 * 86400);
    PB_CHECK(env.sync().first_problem.ok());
    const int connections = env.server->connections();
    const size_t commands = env.server->commands().size();

    const std::string ids = env.store->debug_query("SELECT group_concat(id) FROM message");
    PB_CHECK(!ids.empty());
    for (int round = 0; round < 3; ++round) {   // opened again and again
        size_t pos = 0;
        while (pos < ids.size()) {
            const size_t comma = ids.find(',', pos);
            const auto view = env.store->message(std::stoll(ids.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos)));
            PB_CHECK(view.has_value());
            if (view) {
                const std::string& t = view->body_text;
                for (const char* remote : {"tracker.example.net", "css.example.net", "bg.example.net", "frame.example.net", "tracker.example.org", "pixel.gif", "style.css"})
                    PB_CHECK(t.find(remote) == std::string::npos);   // no remote reference in the text
                for (const std::string& link : view->links) PB_CHECK(link.compare(0, 8, "https://") == 0);   // targets only: shown as text, never opened
                PB_CHECK(t.find("Hidden title") == std::string::npos && t.find("alert") == std::string::npos);   // title and script are not content
            }
            if (comma == std::string::npos) break;
            pos = comma + 1;
        }
    }
    PB_CHECK_EQ(env.server->connections(), connections);   // reading opened no connection
    PB_CHECK_EQ(env.server->commands().size(), commands);

    // the only link targets are the anchors
    const auto offer = env.store->message(std::stoll(env.store->debug_query("SELECT id FROM message WHERE subject = 'Offer'")));
    PB_CHECK(offer.has_value());
    if (offer) {
        PB_CHECK_EQ(offer->links.size(), 1u);
        PB_CHECK_EQ(offer->links[0], std::string("https://shop.example.org/sale?u=1"));
        PB_CHECK(offer->body_text.find("Big sale") != std::string::npos);
    }
}

PB_TEST_MAIN()
