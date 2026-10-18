// T026 (spec 002): the read requests of the CardDAV client (contracts/carddav-vcard-subset.md section 1): changes by sync-collection or ctag,
// multiget in batches of 30 with the address-data property list, per-card failures. The write requests are added with the stories that need them.
// Evidence is from the fake server and scripted answers only: UNVERIFIED against real servers.
#include <regex>
#include <string>

#include "fake_carddav_server.h"
#include "fake_transport.h"
#include "pbo/carddav.h"
#include "pbo/curl_transport.h"
#include "test_support.h"

using pbo::AddressBookInfo;
using pbo::CardDavClient;
using pbtest::FakeCardDavServer;

PBO_FAKE_SERVER_TEST();

namespace {

const char* kCard = "BEGIN:VCARD\r\nVERSION:3.0\r\nUID:a@example.org\r\nFN:Ada Alpha\r\nN:Alpha;Ada;;;\r\nEND:VCARD\r\n";

struct Env {
    pbtest::TmpDir dir;
    pbtest::TestPki pki{dir};
    pbtest::TestPki::Leaf leaf;
    std::unique_ptr<FakeCardDavServer> server;
    std::unique_ptr<pbo::CurlTransport> http;
    Env() {
        if (!pki.available()) PB_SKIP("openssl CLI not available");
        leaf = pki.issue("srv", "DNS:localhost,IP:127.0.0.1");
        PB_CHECK(leaf.ok);
        FakeCardDavServer::Config c;
        c.cert = leaf.cert;
        c.key = leaf.key;
        server = std::make_unique<FakeCardDavServer>(c);
        http = std::make_unique<pbo::CurlTransport>(pki.ca_cert());
        FakeCardDavServer::BookOptions main;
        main.name = "Contacts";
        server->add_book("contacts", main);
    }
    CardDavClient client() { return CardDavClient(*http, server->origin(), "alice", "pw-for-tests"); }
    AddressBookInfo book(const std::string& slug) {
        auto d = client().discover();
        PB_CHECK(d.ok());
        for (const auto& b : d.value)
            if (b.href == server->book_href(slug)) return b;
        PB_FAIL_AT("book not found: " + slug);
        return AddressBookInfo();
    }
};

std::string multistatus(const std::string& inner) { return "<d:multistatus xmlns:d=\"DAV:\" xmlns:card=\"urn:ietf:params:xml:ns:carddav\">" + inner + "</d:multistatus>"; }

}  // namespace

PB_TEST(sync_collection_gives_changes_and_removals_since_the_stored_token) {
    Env e;
    e.server->put_card("contacts", "a.vcf", kCard);
    e.server->put_card("contacts", "b.vcf", kCard);
    auto book = e.book("contacts");
    PB_CHECK(book.supports_sync_report);
    auto first = e.client().changes(book, "");
    PB_CHECK(first.ok());
    PB_CHECK(first.value.full);   // nothing stored: the complete list
    PB_CHECK_EQ(first.value.changed.size(), size_t(2));
    PB_CHECK(!first.value.new_sync_token.empty());
    for (const auto& r : first.value.changed) PB_CHECK(!r.etag.empty() && r.href.rfind("/dav/addressbooks/alice/contacts/", 0) == 0);

    e.server->remove_card("contacts", "a.vcf");
    e.server->put_card("contacts", "c.vcf", kCard);
    auto next = e.client().changes(book, first.value.new_sync_token);
    PB_CHECK(next.ok());
    PB_CHECK(!next.value.full);
    PB_CHECK_EQ(next.value.changed.size(), size_t(1));
    PB_CHECK(next.value.changed[0].href.find("c.vcf") != std::string::npos);
    PB_CHECK_EQ(next.value.removed.size(), size_t(1));
    PB_CHECK(next.value.removed[0].find("a.vcf") != std::string::npos);
    PB_CHECK(!next.value.new_sync_token.empty() && next.value.new_sync_token != first.value.new_sync_token);

    auto none = e.client().changes(book, next.value.new_sync_token);
    PB_CHECK(none.ok());
    PB_CHECK(none.value.changed.empty() && none.value.removed.empty() && !none.value.full);
}

PB_TEST(the_sync_collection_request_asks_for_level_1_and_the_etag_only) {
    Env e;
    e.server->put_card("contacts", "a.vcf", kCard);
    auto book = e.book("contacts");
    auto first = e.client().changes(book, "");
    PB_CHECK(first.ok());
    PB_CHECK(e.client().changes(book, first.value.new_sync_token).ok());
    int seen = 0;
    for (const auto& r : e.server->log()) {
        if (r.method != "REPORT") continue;
        ++seen;
        PB_CHECK_EQ(r.headers.at("depth"), std::string("1"));
        PB_CHECK(r.body.find("sync-collection") != std::string::npos);
        PB_CHECK(r.body.find("<d:sync-level>1</d:sync-level>") != std::string::npos);
        PB_CHECK(r.body.find("getetag") != std::string::npos);
        PB_CHECK(r.body.find("address-data") == std::string::npos);
    }
    PB_CHECK(seen >= 1);
}

PB_TEST(an_invalid_sync_token_means_a_full_listing) {
    Env e;
    e.server->put_card("contacts", "a.vcf", kCard);
    e.server->put_card("contacts", "b.vcf", kCard);
    auto book = e.book("contacts");
    auto first = e.client().changes(book, "");
    PB_CHECK(first.ok());
    e.server->reset_sync_tokens("contacts");
    auto again = e.client().changes(book, first.value.new_sync_token);
    PB_CHECK(again.ok());
    PB_CHECK(again.value.full);
    PB_CHECK_EQ(again.value.changed.size(), size_t(2));
    PB_CHECK(!again.value.new_sync_token.empty());
}

PB_TEST(a_book_without_sync_collection_uses_ctag_and_a_full_etag_list) {
    Env e;
    FakeCardDavServer::BookOptions nosync;
    nosync.name = "Plain";
    nosync.sync_collection = false;
    e.server->add_book("plain", nosync);
    e.server->put_card("plain", "a.vcf", kCard);
    auto book = e.book("plain");
    PB_CHECK(!book.supports_sync_report);
    auto first = e.client().changes(book, "");
    PB_CHECK(first.ok());
    PB_CHECK(first.value.full);
    PB_CHECK_EQ(first.value.changed.size(), size_t(1));
    PB_CHECK(first.value.changed[0].href.find("a.vcf") != std::string::npos);
    PB_CHECK(!first.value.new_ctag.empty());
    const int listings = e.server->count("PROPFIND", "plain/");
    auto same = e.client().changes(book, first.value.new_ctag);
    PB_CHECK(same.ok());
    PB_CHECK(same.value.unchanged);
    PB_CHECK_EQ(e.server->count("PROPFIND", "plain/"), listings + 1);   // one question for the ctag, no listing
    e.server->put_card("plain", "b.vcf", kCard);
    auto more = e.client().changes(book, first.value.new_ctag);
    PB_CHECK(more.ok());
    PB_CHECK(more.value.full && !more.value.unchanged);
    PB_CHECK_EQ(more.value.changed.size(), size_t(2));
    PB_CHECK(more.value.new_ctag != first.value.new_ctag);
}

PB_TEST(multiget_fetches_in_batches_of_30_and_sends_the_documented_property_list) {
    Env e;
    std::vector<std::string> hrefs;
    for (int i = 0; i < 65; ++i) {
        const std::string name = "c" + std::to_string(i) + ".vcf";
        e.server->put_card("contacts", name, kCard);
        hrefs.push_back(e.server->card_href("contacts", name));
    }
    auto book = e.book("contacts");
    auto r = e.client().fetch(book, hrefs);
    PB_CHECK(r.ok());
    PB_CHECK_EQ(r.value.cards.size(), size_t(65));
    PB_CHECK(r.value.gone.empty() && r.value.failed.empty());
    PB_CHECK(r.value.cards[0].vcf.find("BEGIN:VCARD") != std::string::npos);
    PB_CHECK(!r.value.cards[0].etag.empty());
    PB_CHECK((e.server->multiget_batch_sizes() == std::vector<size_t>{30, 30, 5}));
    for (const auto& req : e.server->log()) {
        if (req.method != "REPORT") continue;
        PB_CHECK(req.body.find("addressbook-multiget") != std::string::npos);
        for (const char* prop : {"VERSION", "UID", "FN", "N", "NICKNAME", "ORG", "TITLE", "TEL", "EMAIL", "ADR", "NOTE", "REV", "X-ABLabel"})
            PB_CHECK(req.body.find(std::string("<card:prop name=\"") + prop + "\"/>") != std::string::npos);
        PB_CHECK(req.body.find("PHOTO") == std::string::npos);
    }
}

PB_TEST(a_server_that_honours_the_list_leaves_the_photo_out_and_one_that_ignores_it_sends_it) {
    Env e;
    const std::string photo = "BEGIN:VCARD\r\nVERSION:3.0\r\nUID:p\r\nFN:Pia\r\nPHOTO;ENCODING=b;TYPE=JPEG:AAAA\r\n BBBB\r\nEND:VCARD\r\n";
    e.server->put_card("contacts", "p.vcf", photo);
    auto book = e.book("contacts");
    auto lean = e.client().fetch(book, {e.server->card_href("contacts", "p.vcf")});
    PB_CHECK(lean.ok());
    PB_CHECK(lean.value.cards[0].vcf.find("PHOTO") == std::string::npos);
    e.server->set_honours_property_list(false);
    auto full = e.client().fetch(book, {e.server->card_href("contacts", "p.vcf")});
    PB_CHECK(full.ok());
    PB_CHECK(full.value.cards[0].vcf.find("PHOTO;ENCODING=b") != std::string::npos);   // stripped later by the ingest (vcard.h)
}

PB_TEST(a_card_that_disappeared_is_reported_as_gone) {
    Env e;
    e.server->put_card("contacts", "a.vcf", kCard);
    auto book = e.book("contacts");
    auto r = e.client().fetch(book, {e.server->card_href("contacts", "a.vcf"), e.server->card_href("contacts", "missing.vcf")});
    PB_CHECK(r.ok());
    PB_CHECK_EQ(r.value.cards.size(), size_t(1));
    PB_CHECK_EQ(r.value.gone.size(), size_t(1));
}

PB_TEST(a_hostile_href_outside_the_server_host_is_not_requested) {
    Env e;
    auto book = e.book("contacts");
    auto r = e.client().fetch(book, {"https://evil.example.org/dav/x.vcf", "//evil.example.org/x.vcf"});
    PB_CHECK(r.ok());
    PB_CHECK(r.value.cards.empty());
    PB_CHECK_EQ(e.server->multiget_batch_sizes().size(), size_t(0));   // nothing was asked
}

PB_TEST(a_card_with_a_server_error_inside_the_answer_is_failed_and_the_others_are_applied) {
    pbtest::FakeTransport http;
    http.set_handler([](const pbo::Request&) {
        return pbtest::FakeTransport::status(207, multistatus(
            "<d:response><d:href>/b/ok.vcf</d:href><d:propstat><d:prop><d:getetag>\"1\"</d:getetag><card:address-data>BEGIN:VCARD&#13;&#10;END:VCARD&#13;&#10;</card:address-data></d:prop><d:status>HTTP/1.1 200 OK</d:status></d:propstat></d:response>"
            "<d:response><d:href>/b/bad.vcf</d:href><d:status>HTTP/1.1 500 Internal Server Error</d:status></d:response>"
            "<d:response><d:href>/b/gone.vcf</d:href><d:status>HTTP/1.1 404 Not Found</d:status></d:response>"));
    });
    CardDavClient c(http, "https://dav.example.org/dav/", "alice", "pw");
    AddressBookInfo book;
    book.href = "/b/";
    auto r = c.fetch(book, {"/b/ok.vcf", "/b/bad.vcf", "/b/gone.vcf"});
    PB_CHECK(r.ok());
    PB_CHECK_EQ(r.value.cards.size(), size_t(1));
    PB_CHECK_EQ(r.value.cards[0].href, std::string("/b/ok.vcf"));
    PB_CHECK_EQ(r.value.failed.size(), size_t(1));
    PB_CHECK_EQ(r.value.failed[0], std::string("/b/bad.vcf"));
    PB_CHECK_EQ(r.value.gone.size(), size_t(1));
}

PB_TEST(an_answer_over_the_cap_is_split_and_a_single_card_over_the_cap_is_failed) {
    pbtest::FakeTransport http;
    int too_big_for_one = 0;
    http.set_handler([&](const pbo::Request& r) {
        std::vector<std::string> hrefs;
        static const std::regex href("<(?:[A-Za-z0-9]+:)?href>([^<]*)<");
        for (std::sregex_iterator it(r.body.begin(), r.body.end(), href), end; it != end; ++it) hrefs.push_back((*it)[1]);
        const bool huge_member = std::find(hrefs.begin(), hrefs.end(), "/b/huge.vcf") != hrefs.end();
        if (hrefs.size() > 2 || huge_member) {
            pbo::Response big;
            big.status = 207;
            big.body_too_large = true;
            if (hrefs.size() == 1) ++too_big_for_one;
            return big;
        }
        std::string inner;
        for (const std::string& h : hrefs)
            inner += "<d:response><d:href>" + h + "</d:href><d:propstat><d:prop><d:getetag>\"1\"</d:getetag><card:address-data>BEGIN:VCARD&#13;&#10;END:VCARD&#13;&#10;</card:address-data></d:prop><d:status>HTTP/1.1 200 OK</d:status></d:propstat></d:response>";
        return pbtest::FakeTransport::status(207, multistatus(inner));
    });
    CardDavClient c(http, "https://dav.example.org/dav/", "alice", "pw");
    AddressBookInfo book;
    book.href = "/b/";
    std::vector<std::string> hrefs;
    for (int i = 0; i < 7; ++i) hrefs.push_back("/b/c" + std::to_string(i) + ".vcf");
    hrefs.push_back("/b/huge.vcf");
    auto r = c.fetch(book, hrefs);
    PB_CHECK(r.ok());
    PB_CHECK_EQ(r.value.cards.size(), size_t(7));
    PB_CHECK_EQ(r.value.failed.size(), size_t(1));
    PB_CHECK_EQ(r.value.failed[0], std::string("/b/huge.vcf"));
    PB_CHECK(too_big_for_one >= 1);
}

PB_TEST(a_failing_request_stops_the_fetch_with_the_error) {
    Env e;
    e.server->put_card("contacts", "a.vcf", kCard);
    auto book = e.book("contacts");
    FakeCardDavServer::Fault f;
    f.method = "REPORT";
    f.status = 503;
    e.server->inject(f);
    auto r = e.client().fetch(book, {e.server->card_href("contacts", "a.vcf")});
    PB_CHECK(!r.ok());
    PB_CHECK_EQ(r.error.key, std::string("server_error"));
}

// ---- the write requests: create (US2) --------------------------------------------------------------------------------------------------------

namespace {
struct CounterRandom : pbo::Random {
    uint8_t n = 1;
    void bytes(uint8_t* out, size_t len) override {
        for (size_t i = 0; i < len; ++i) out[i] = n++;
    }
};
}  // namespace

PB_TEST(a_new_card_goes_to_a_random_path_in_its_book) {
    CounterRandom r;
    const std::string href = pbo::new_card_href("/dav/addressbooks/alice/contacts/", r);
    PB_CHECK_EQ(href, std::string("/dav/addressbooks/alice/contacts/0102030405060708090a0b0c0d0e0f10.vcf"));
    PB_CHECK(pbo::new_card_href("/dav/addressbooks/alice/contacts/", r) != href);   // the next one differs
    PB_CHECK_EQ(pbo::new_card_href("/b", r).compare(0, 3, "/b/"), 0);                // a book path without the final slash gets one
}

PB_TEST(creating_a_card_sends_a_conditional_put_with_the_vcard_content_type) {
    Env e;
    CardDavClient c = e.client();
    const std::string href = e.server->card_href("contacts", "new.vcf");
    auto created = c.put(href, kCard, nullptr);
    PB_CHECK(created.kind == pbo::CardWriteResult::Kind::Created);
    PB_CHECK(!created.etag.empty());
    PB_CHECK_EQ(e.server->card("contacts", "new.vcf").etag, created.etag);
    PB_CHECK_EQ(e.server->card("contacts", "new.vcf").vcf, std::string(kCard));
    int puts = 0;
    for (const auto& r : e.server->log()) {
        if (r.method != "PUT") continue;
        ++puts;
        PB_CHECK_EQ(r.headers.at("if-none-match"), std::string("*"));
        PB_CHECK(r.headers.count("if-match") == 0);
        PB_CHECK_EQ(r.headers.at("content-type"), std::string("text/vcard; charset=utf-8"));
        PB_CHECK(r.path.find('@') == std::string::npos);   // no credentials in the address
    }
    PB_CHECK_EQ(puts, 1);
    // the same path again: the server says it exists
    PB_CHECK(c.put(href, kCard, nullptr).kind == pbo::CardWriteResult::Kind::PreconditionFailed);
}

PB_TEST(an_answer_without_an_etag_gives_an_empty_one_and_204_also_means_created) {
    pbtest::FakeTransport http;
    http.set_handler([](const pbo::Request& r) { return pbtest::FakeTransport::status(r.url.find("two") != std::string::npos ? 204 : 201); });
    CardDavClient c(http, "https://dav.example.org/dav/", "alice", "pw");
    auto one = c.put("/b/one.vcf", kCard, nullptr);
    PB_CHECK(one.kind == pbo::CardWriteResult::Kind::Created);
    PB_CHECK(one.etag.empty());
    auto two = c.put("/b/two.vcf", kCard, nullptr);
    PB_CHECK(two.kind == pbo::CardWriteResult::Kind::Created);
    // an address on another host is never written to
    auto evil = c.put("https://evil.example.org/b/x.vcf", kCard, nullptr);
    PB_CHECK(evil.kind == pbo::CardWriteResult::Kind::Failed);
    PB_CHECK_EQ(evil.error.key, std::string("invalid_input"));
    for (const pbo::Request& r : http.requests()) PB_CHECK(r.url.compare(0, 24, "https://dav.example.org/") == 0);
}

PB_TEST(refusals_and_failures_of_a_create_are_mapped_to_the_documented_keys) {
    struct Row { int status; pbo::CardWriteResult::Kind kind; const char* key; };
    for (const Row& row : {Row{403, pbo::CardWriteResult::Kind::Refused, "refused"}, Row{405, pbo::CardWriteResult::Kind::Refused, "refused"},
                           Row{413, pbo::CardWriteResult::Kind::Refused, "too_large"}, Row{507, pbo::CardWriteResult::Kind::Refused, "storage_full_server"},
                           Row{401, pbo::CardWriteResult::Kind::Failed, "auth_failed"}, Row{500, pbo::CardWriteResult::Kind::Failed, "server_error"},
                           Row{503, pbo::CardWriteResult::Kind::Failed, "server_error"}, Row{418, pbo::CardWriteResult::Kind::Failed, "protocol_error"}}) {
        pbtest::FakeTransport http;
        const int status = row.status;
        http.set_handler([status](const pbo::Request&) { return pbtest::FakeTransport::status(status); });
        CardDavClient c(http, "https://dav.example.org/dav/", "alice", "pw");
        const auto w = c.put("/b/x.vcf", kCard, nullptr);
        PB_CHECK(w.kind == row.kind);
        PB_CHECK_EQ(w.error.key, std::string(row.key));
    }
    // no answer at all
    pbtest::FakeTransport down;
    down.set_handler([](const pbo::Request&) {
        pbo::Response r;
        r.error = pbo::ErrorClass::Network;
        r.error_key = "server_unreachable";
        return r;
    });
    CardDavClient c(down, "https://dav.example.org/dav/", "alice", "pw");
    const auto w = c.put("/b/x.vcf", kCard, nullptr);
    PB_CHECK(w.kind == pbo::CardWriteResult::Kind::Failed);
    PB_CHECK_EQ(w.error.key, std::string("server_unreachable"));
}

PB_TEST(a_read_only_book_refuses_a_create) {
    Env e;
    FakeCardDavServer::BookOptions ro;
    ro.name = "Company";
    ro.writable = false;
    e.server->add_book("company", ro);
    const auto w = e.client().put(e.server->card_href("company", "x.vcf"), kCard, nullptr);
    PB_CHECK(w.kind == pbo::CardWriteResult::Kind::Refused);
    PB_CHECK_EQ(w.error.key, std::string("refused"));
    PB_CHECK(!e.server->has_card("company", "x.vcf"));
}

PB_TEST_MAIN()
