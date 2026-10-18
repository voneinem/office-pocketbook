// T021 (spec 002): self-test of the fake CardDAV server harness. If this fails, contact tests built on it prove nothing.
#include <string>

#include "fake_carddav_server.h"
#include "pbo/curl_transport.h"
#include "pbo/xml_min.h"
#include "test_support.h"

using pbo::CurlTransport;
using pbo::Request;
using pbtest::FakeCardDavServer;

PBO_FAKE_SERVER_TEST();

namespace {

const char* kCard = "BEGIN:VCARD\r\nVERSION:3.0\r\nUID:a@example.org\r\nFN:Ada Alpha\r\nN:Alpha;Ada;;;\r\nEND:VCARD\r\n";
const char* kPhotoCard =
    "BEGIN:VCARD\r\nVERSION:3.0\r\nUID:p@example.org\r\nFN:Pia Foto\r\nPHOTO;ENCODING=b;TYPE=JPEG:AAAAAAAA\r\n BBBBBBBB\r\nLOGO:CCCC\r\nTEL:+1 555 0100\r\nEND:VCARD\r\n";

struct Env {
    pbtest::TmpDir dir;
    pbtest::TestPki pki{dir};
    pbtest::TestPki::Leaf leaf;
    std::unique_ptr<FakeCardDavServer> server;
    std::unique_ptr<CurlTransport> http;
    explicit Env(bool address_book_home = true) {
        if (!pki.available()) PB_SKIP("openssl CLI not available");
        leaf = pki.issue("srv", "DNS:localhost,IP:127.0.0.1");
        PB_CHECK(leaf.ok);
        FakeCardDavServer::Config c;
        c.cert = leaf.cert;
        c.key = leaf.key;
        c.address_book_home = address_book_home;
        server = std::make_unique<FakeCardDavServer>(c);
        http = std::make_unique<CurlTransport>(pki.ca_cert());
        FakeCardDavServer::BookOptions main;
        main.name = "Contacts";
        server->add_book("contacts", main);
        FakeCardDavServer::BookOptions ro;
        ro.name = "Company";
        ro.writable = false;
        server->add_book("company", ro);
    }
    pbo::Response call(const std::string& method, const std::string& url, const std::string& body = "", pbo::Headers h = {}, bool login = true) {
        Request r;
        r.method = method;
        r.url = url;
        r.body = body;
        r.headers = std::move(h);
        if (login) {
            r.username = "alice";
            r.password = "pw-for-tests";
        }
        return http->perform(r);
    }
    std::string url(const std::string& path) const { return server->origin() + path; }
};

std::string multiget(const std::string& hrefs, bool with_property_list) {
    return std::string("<card:addressbook-multiget xmlns:d=\"DAV:\" xmlns:card=\"urn:ietf:params:xml:ns:carddav\"><d:prop><d:getetag/><card:address-data>") +
           (with_property_list ? "<card:prop name=\"FN\"/><card:prop name=\"TEL\"/>" : "") + "</card:address-data></d:prop>" + hrefs + "</card:addressbook-multiget>";
}

}  // namespace

PB_TEST(a_wrong_login_is_refused_and_counted) {
    Env e;
    PB_CHECK_EQ(e.call("PROPFIND", e.server->base_url(), "<x/>", {{"Depth", "0"}}, false).status, 401L);
    PB_CHECK_EQ(e.server->rejected_logins(), 1);
}

PB_TEST(discovery_walks_from_the_well_known_address_to_the_address_books) {
    Env e;
    auto r = e.call("PROPFIND", e.url("/.well-known/carddav"), "<d:propfind xmlns:d=\"DAV:\"><d:prop><d:current-user-principal/></d:prop></d:propfind>", {{"Depth", "0"}});
    PB_CHECK_EQ(r.status, 207L);   // the redirect was followed
    PB_CHECK(r.body.find("/dav/principals/alice/") != std::string::npos);
    r = e.call("PROPFIND", e.url("/dav/principals/alice/"), "<d:propfind xmlns:d=\"DAV:\" xmlns:card=\"urn:ietf:params:xml:ns:carddav\"><d:prop><card:addressbook-home-set/></d:prop></d:propfind>", {{"Depth", "0"}});
    PB_CHECK(r.body.find("/dav/addressbooks/alice/") != std::string::npos);
    r = e.call("PROPFIND", e.url("/dav/addressbooks/alice/"), "<d:propfind xmlns:d=\"DAV:\"><d:allprop/></d:propfind>", {{"Depth", "1"}});
    PB_CHECK_EQ(r.status, 207L);
    auto x = pbo::parse_xml(r.body);
    PB_CHECK(x.ok());
    PB_CHECK_EQ(x.value.children_named("DAV:", "response").size(), size_t(3));   // the home and two address books
    PB_CHECK(r.body.find("<card:addressbook/>") != std::string::npos);
    PB_CHECK(r.body.find("<d:write-content/>") != std::string::npos);
    PB_CHECK(r.body.find("<cs:getctag>") != std::string::npos);
    PB_CHECK(r.body.find("<d:sync-collection/>") != std::string::npos);
    PB_CHECK(!e.server->saw_credentials_in_url());
}

PB_TEST(a_server_without_an_address_book_home_names_none) {
    Env e(false);
    auto r = e.call("PROPFIND", e.url("/dav/principals/alice/"), "<d:propfind xmlns:d=\"DAV:\" xmlns:card=\"urn:ietf:params:xml:ns:carddav\"><d:prop><card:addressbook-home-set/></d:prop></d:propfind>", {{"Depth", "0"}});
    PB_CHECK_EQ(r.status, 207L);
    PB_CHECK(r.body.find("/dav/addressbooks/alice/") == std::string::npos);
    PB_CHECK(r.body.find("404") != std::string::npos);   // the property is reported as not found
}

PB_TEST(sync_collection_lists_changes_since_a_token_and_rejects_a_stale_one) {
    Env e;
    e.server->put_card("contacts", "a.vcf", kCard);
    const std::string book = e.url(e.server->book_href("contacts"));
    const std::string req = "<d:sync-collection xmlns:d=\"DAV:\"><d:sync-token>%s</d:sync-token><d:sync-level>1</d:sync-level><d:prop><d:getetag/></d:prop></d:sync-collection>";
    auto body = [&](const std::string& token) { std::string b = req; b.replace(b.find("%s"), 2, token); return b; };
    auto r = e.call("REPORT", book, body(""), {{"Depth", "1"}});
    PB_CHECK_EQ(r.status, 207L);
    PB_CHECK(r.body.find("a.vcf") != std::string::npos);
    auto x = pbo::parse_xml(r.body);
    PB_CHECK(x.ok());
    const std::string token = x.value.child("DAV:", "sync-token")->text;
    PB_CHECK(!token.empty());
    // nothing changed: the same token gives an empty list
    r = e.call("REPORT", book, body(token), {{"Depth", "1"}});
    PB_CHECK(r.body.find("a.vcf") == std::string::npos);
    // a change and a removal appear
    e.server->put_card("contacts", "b.vcf", kCard);
    e.server->remove_card("contacts", "a.vcf");
    r = e.call("REPORT", book, body(token), {{"Depth", "1"}});
    PB_CHECK(r.body.find("b.vcf") != std::string::npos);
    PB_CHECK(r.body.find("a.vcf") != std::string::npos);
    PB_CHECK(r.body.find("404 Not Found") != std::string::npos);
    // a reset server invalidates every token
    e.server->reset_sync_tokens("contacts");
    r = e.call("REPORT", book, body(token), {{"Depth", "1"}});
    PB_CHECK_EQ(r.status, 409L);
    PB_CHECK(r.body.find("valid-sync-token") != std::string::npos);
}

PB_TEST(a_book_without_sync_collection_refuses_the_report_and_lists_etags_by_propfind) {
    Env e;
    FakeCardDavServer::BookOptions o;
    o.name = "Old";
    o.sync_collection = false;
    e.server->add_book("old", o);
    e.server->put_card("old", "a.vcf", kCard);
    const std::string book = e.url(e.server->book_href("old"));
    auto r = e.call("REPORT", book, "<d:sync-collection xmlns:d=\"DAV:\"><d:sync-token/></d:sync-collection>", {{"Depth", "1"}});
    PB_CHECK_EQ(r.status, 403L);
    r = e.call("PROPFIND", book, "<d:propfind xmlns:d=\"DAV:\"><d:prop><d:getetag/></d:prop></d:propfind>", {{"Depth", "1"}});
    PB_CHECK_EQ(r.status, 207L);
    PB_CHECK(r.body.find("a.vcf") != std::string::npos);
    PB_CHECK(r.body.find("etag-") != std::string::npos);
    r = e.call("PROPFIND", book, "<d:propfind xmlns:d=\"DAV:\" xmlns:cs=\"http://calendarserver.org/ns/\"><d:prop><cs:getctag/></d:prop></d:propfind>", {{"Depth", "0"}});
    PB_CHECK(r.body.find("<cs:getctag>ctag-") != std::string::npos);
}

PB_TEST(multiget_returns_the_cards_and_honours_the_property_list_unless_told_otherwise) {
    Env e;
    e.server->put_card("contacts", "p.vcf", kPhotoCard);
    const std::string book = e.url(e.server->book_href("contacts"));
    const std::string hrefs = "<d:href>" + e.server->card_href("contacts", "p.vcf") + "</d:href><d:href>" + e.server->card_href("contacts", "gone.vcf") + "</d:href>";
    auto r = e.call("REPORT", book, multiget(hrefs, true), {{"Depth", "1"}});
    PB_CHECK_EQ(r.status, 207L);
    PB_CHECK(r.body.find("FN:Pia Foto") != std::string::npos);
    PB_CHECK(r.body.find("PHOTO") == std::string::npos);   // the server leaves the photo out when the list is given
    PB_CHECK(r.body.find("LOGO") == std::string::npos);
    PB_CHECK(r.body.find("404 Not Found") != std::string::npos);
    e.server->set_honours_property_list(false);
    r = e.call("REPORT", book, multiget(hrefs, true), {{"Depth", "1"}});
    PB_CHECK(r.body.find("PHOTO;ENCODING=b") != std::string::npos);   // this server sends the full card
    PB_CHECK(r.body.find("BBBBBBBB") != std::string::npos);
    PB_CHECK_EQ(e.server->multiget_batch_sizes().size(), size_t(2));
    PB_CHECK_EQ(e.server->multiget_batch_sizes()[0], size_t(2));
}

PB_TEST(writes_are_conditional_and_answer_like_a_real_server) {
    Env e;
    const std::string href = e.url(e.server->card_href("contacts", "n.vcf"));
    auto r = e.call("PUT", href, kCard, {{"Content-Type", "text/vcard; charset=utf-8"}, {"If-None-Match", "*"}});
    PB_CHECK_EQ(r.status, 201L);
    const std::string etag1 = pbo::header_value(r.headers, "ETag");
    PB_CHECK(!etag1.empty());
    PB_CHECK(e.server->has_card("contacts", "n.vcf"));
    // creating again with If-None-Match: * fails
    PB_CHECK_EQ(e.call("PUT", href, kCard, {{"If-None-Match", "*"}}).status, 412L);
    // an update needs the current ETag
    PB_CHECK_EQ(e.call("PUT", href, "BEGIN:VCARD\r\nEND:VCARD\r\n", {{"If-Match", "\"stale\""}}).status, 412L);
    r = e.call("PUT", href, "BEGIN:VCARD\r\nFN:New\r\nEND:VCARD\r\n", {{"If-Match", etag1}});
    PB_CHECK_EQ(r.status, 204L);
    const std::string etag2 = pbo::header_value(r.headers, "ETag");
    PB_CHECK(etag2 != etag1);
    PB_CHECK(e.server->card("contacts", "n.vcf").vcf.find("FN:New") != std::string::npos);
    // GET returns the full card with its ETag
    r = e.call("GET", href);
    PB_CHECK_EQ(r.status, 200L);
    PB_CHECK_EQ(pbo::header_value(r.headers, "ETag"), etag2);
    // an update of a missing card is 404, a delete needs the ETag too
    PB_CHECK_EQ(e.call("PUT", e.url(e.server->card_href("contacts", "missing.vcf")), kCard, {{"If-Match", "\"x\""}}).status, 404L);
    PB_CHECK_EQ(e.call("DELETE", href, "", {{"If-Match", etag1}}).status, 412L);
    PB_CHECK_EQ(e.call("DELETE", href, "", {{"If-Match", etag2}}).status, 204L);
    PB_CHECK(!e.server->has_card("contacts", "n.vcf"));
    PB_CHECK_EQ(e.call("DELETE", href, "", {{"If-Match", etag2}}).status, 404L);
    // a read-only book refuses writes
    PB_CHECK_EQ(e.call("PUT", e.url(e.server->card_href("company", "x.vcf")), kCard, {{"If-None-Match", "*"}}).status, 403L);
}

PB_TEST(faults_are_injected_per_request_and_spent_after_their_count) {
    Env e;
    e.server->put_card("contacts", "a.vcf", kCard);
    const std::string href = e.url(e.server->card_href("contacts", "a.vcf"));
    FakeCardDavServer::Fault f;
    f.method = "GET";
    f.status = 503;
    f.times = 2;
    e.server->inject(f);
    PB_CHECK_EQ(e.call("GET", href).status, 503L);
    PB_CHECK_EQ(e.call("GET", href).status, 503L);
    PB_CHECK_EQ(e.call("GET", href).status, 200L);
    FakeCardDavServer::Fault drop;
    drop.kind = FakeCardDavServer::Fault::Kind::Drop;
    drop.method = "GET";
    e.server->inject(drop);
    PB_CHECK_EQ(e.call("GET", href).status, 0L);   // the connection was dropped
    FakeCardDavServer::Fault bad;
    bad.kind = FakeCardDavServer::Fault::Kind::MalformedXml;
    bad.method = "PROPFIND";
    e.server->inject(bad);
    auto r = e.call("PROPFIND", e.url("/dav/addressbooks/alice/"), "<x/>", {{"Depth", "1"}});
    PB_CHECK(!pbo::parse_xml(r.body).ok());
    FakeCardDavServer::Fault pe;
    pe.method = "PUT";
    pe.status = 507;
    e.server->inject(pe);
    PB_CHECK_EQ(e.call("PUT", e.url(e.server->card_href("contacts", "z.vcf")), kCard, {{"If-None-Match", "*"}}).status, 507L);
    PB_CHECK(!e.server->has_card("contacts", "z.vcf"));
}

PB_TEST(a_redirect_to_another_host_is_served_and_gets_no_credentials) {
    Env e;
    FakeCardDavServer::Fault f;
    f.kind = FakeCardDavServer::Fault::Kind::RedirectOtherHost;
    f.location = "https://other.invalid/dav/";
    f.method = "PROPFIND";
    e.server->inject(f);
    const auto r = e.call("PROPFIND", e.url("/dav/"), "<x/>", {{"Depth", "0"}});
    PB_CHECK(r.status != 207);
    PB_CHECK_EQ(e.server->count("PROPFIND", "/dav/"), 1);
}

PB_TEST(the_before_write_hook_edits_the_server_copy_between_the_apps_read_and_write) {
    Env e;
    const std::string etag = e.server->put_card("contacts", "a.vcf", kCard);
    const std::string href = e.url(e.server->card_href("contacts", "a.vcf"));
    e.server->before_write([](FakeCardDavServer& s) { s.put_card("contacts", "a.vcf", "BEGIN:VCARD\r\nFN:Changed Elsewhere\r\nEND:VCARD\r\n"); });
    PB_CHECK_EQ(e.call("PUT", href, kCard, {{"If-Match", etag}}).status, 412L);   // the app's version is stale now
    PB_CHECK(e.server->card("contacts", "a.vcf").vcf.find("Changed Elsewhere") != std::string::npos);
}

PB_TEST(the_server_reports_its_time_in_the_date_header) {
    Env e;
    e.server->set_server_time(1791979200);
    const auto r = e.call("GET", e.url(e.server->card_href("contacts", "none.vcf")));
    PB_CHECK_EQ(pbo::header_value(r.headers, "Date"), std::string("Wed, 14 Oct 2026 12:00:00 GMT"));
}

PB_TEST_MAIN()
