// T099 (part): feeds - conditional download, 304, the 8 MiB cap, https only, nothing but a calendar accepted. Fake server only (UNVERIFIED
// against real feed hosts).
#include <string>

#include "fake_caldav_server.h"
#include "pbo/caldav.h"
#include "pbo/curl_transport.h"
#include "test_support.h"

using pbo::FeedResult;
using pbtest::FakeCalDavServer;

PBO_FAKE_SERVER_TEST();

namespace {

const char* kFeed = "BEGIN:VCALENDAR\r\nVERSION:2.0\r\nBEGIN:VEVENT\r\nUID:h1@example.org\r\nDTSTART;VALUE=DATE:20261225\r\nSUMMARY:Holiday\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n";

struct Env {
    pbtest::TmpDir dir;
    pbtest::TestPki pki{dir};
    pbtest::TestPki::Leaf leaf;
    std::unique_ptr<FakeCalDavServer> server;
    std::unique_ptr<pbo::CurlTransport> http;
    Env() {
        if (!pki.available()) PB_SKIP("openssl CLI not available");
        leaf = pki.issue("srv", "DNS:localhost,IP:127.0.0.1");
        PB_CHECK(leaf.ok);
        FakeCalDavServer::Config c;
        c.cert = leaf.cert;
        c.key = leaf.key;
        server = std::make_unique<FakeCalDavServer>(c);
        http = std::make_unique<pbo::CurlTransport>(pki.ca_cert());
    }
};

}  // namespace

PB_TEST(a_feed_is_downloaded_and_a_repeat_with_its_etag_is_not_modified) {
    Env e;
    e.server->set_feed("/feeds/h.ics", kFeed);
    auto first = pbo::fetch_feed(*e.http, e.server->origin() + "/feeds/h.ics", "", "");
    PB_CHECK(first.state == FeedResult::State::Changed);
    PB_CHECK_EQ(first.ics, std::string(kFeed));
    PB_CHECK(!first.etag.empty() && !first.last_modified.empty());
    auto again = pbo::fetch_feed(*e.http, e.server->origin() + "/feeds/h.ics", first.etag, first.last_modified);
    PB_CHECK(again.state == FeedResult::State::NotModified);
    auto by_date = pbo::fetch_feed(*e.http, e.server->origin() + "/feeds/h.ics", "", first.last_modified);
    PB_CHECK(by_date.state == FeedResult::State::NotModified);
    e.server->set_feed("/feeds/h.ics", std::string(kFeed) + "\r\n");
    auto changed = pbo::fetch_feed(*e.http, e.server->origin() + "/feeds/h.ics", first.etag, first.last_modified);
    PB_CHECK(changed.state == FeedResult::State::Changed);
}

PB_TEST(the_request_carries_the_validators_and_no_credentials) {
    Env e;
    e.server->set_feed("/feeds/h.ics", kFeed);
    auto r = pbo::fetch_feed(*e.http, e.server->origin() + "/feeds/h.ics", "\"abc\"", "Wed, 14 Oct 2026 08:00:00 GMT");
    PB_CHECK(r.state == FeedResult::State::Changed);
    const auto log = e.server->log();
    PB_CHECK(log.back().headers.count("authorization") == 0);
    PB_CHECK_EQ(log.back().headers.at("if-none-match"), std::string("\"abc\""));
    PB_CHECK_EQ(log.back().headers.at("if-modified-since"), std::string("Wed, 14 Oct 2026 08:00:00 GMT"));
}

PB_TEST(status_codes_and_failures_are_mapped) {
    Env e;
    e.server->set_feed("/feeds/h.ics", kFeed);
    struct Row { int status; const char* key; };
    for (const Row& row : {Row{401, "feed_refused"}, Row{403, "feed_refused"}, Row{404, "feed_not_found"}, Row{410, "feed_not_found"}, Row{500, "server_error"}, Row{503, "server_error"}}) {
        FakeCalDavServer::Fault f;
        f.kind = FakeCalDavServer::Fault::Kind::Status;
        f.status = row.status;
        e.server->inject(f);
        auto r = pbo::fetch_feed(*e.http, e.server->origin() + "/feeds/h.ics", "", "");
        PB_CHECK(r.state == FeedResult::State::Failed);
        PB_CHECK_EQ(r.error.key, std::string(row.key));
    }
    FakeCalDavServer::Fault drop;
    drop.kind = FakeCalDavServer::Fault::Kind::Drop;
    e.server->inject(drop);
    PB_CHECK(pbo::fetch_feed(*e.http, e.server->origin() + "/feeds/h.ics", "", "").state == FeedResult::State::Failed);
}

PB_TEST(a_feed_over_8_mib_is_refused_and_something_that_is_not_a_calendar_too) {
    Env e;
    e.server->set_feed("/feeds/big.ics", std::string(kFeed) + std::string(9u << 20, ' '));
    auto r = pbo::fetch_feed(*e.http, e.server->origin() + "/feeds/big.ics", "", "");
    PB_CHECK(r.state == FeedResult::State::Failed);
    PB_CHECK_EQ(r.error.key, std::string("too_large"));
    e.server->set_feed("/feeds/html.ics", "<html><body>Please log in</body></html>");
    r = pbo::fetch_feed(*e.http, e.server->origin() + "/feeds/html.ics", "", "");
    PB_CHECK(r.state == FeedResult::State::Failed);
    PB_CHECK_EQ(r.error.key, std::string("not_a_calendar"));
    e.server->set_feed("/feeds/bom.ics", std::string("\xEF\xBB\xBF\r\n") + kFeed);
    PB_CHECK(pbo::fetch_feed(*e.http, e.server->origin() + "/feeds/bom.ics", "", "").state == FeedResult::State::Changed);
}

PB_TEST(plain_http_and_anything_but_a_calendar_are_refused) {
    Env e;
    auto r = pbo::fetch_feed(*e.http, "http://localhost:" + std::to_string(e.server->port()) + "/feeds/h.ics", "", "");
    PB_CHECK(r.state == FeedResult::State::Failed);
    PB_CHECK_EQ(r.error.key, std::string("scheme_not_allowed"));
    r = pbo::fetch_feed(*e.http, "", "", "");
    PB_CHECK(r.state == FeedResult::State::Failed);
}

PB_TEST_MAIN()
