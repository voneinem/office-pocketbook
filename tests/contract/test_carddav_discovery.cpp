// T025 (spec 002): CardDAV discovery against the fake server (contracts/carddav-vcard-subset.md section 1 and the status table). Evidence is from
// the fake server only: UNVERIFIED against Nextcloud, OpenCloud or any other real server.
#include <set>
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

struct Env {
    pbtest::TmpDir dir;
    pbtest::TestPki pki{dir};
    pbtest::TestPki::Leaf leaf;
    std::unique_ptr<FakeCardDavServer> server;
    std::unique_ptr<pbo::CurlTransport> http;
    explicit Env(bool address_book_home = true) {
        if (!pki.available()) PB_SKIP("openssl CLI not available");
        leaf = pki.issue("srv", "DNS:localhost,IP:127.0.0.1");
        PB_CHECK(leaf.ok);
        FakeCardDavServer::Config c;
        c.cert = leaf.cert;
        c.key = leaf.key;
        c.address_book_home = address_book_home;
        server = std::make_unique<FakeCardDavServer>(c);
        http = std::make_unique<pbo::CurlTransport>(pki.ca_cert());
        FakeCardDavServer::BookOptions main;
        main.name = "Contacts";
        server->add_book("contacts", main);
        FakeCardDavServer::BookOptions ro;
        ro.name = "Company";
        ro.writable = false;
        server->add_book("company", ro);
    }
    CardDavClient client(const std::string& url = "", const std::string& pw = "pw-for-tests") {
        return CardDavClient(*http, url.empty() ? server->origin() : url, "alice", pw);
    }
};

}  // namespace

PB_TEST(discovery_from_the_server_address_follows_well_known_principal_and_home) {
    Env e;
    auto d = e.client().discover();
    PB_CHECK(d.ok());
    PB_CHECK_EQ(d.value.size(), size_t(2));
    std::set<std::string> names;
    for (const auto& b : d.value) names.insert(b.name);
    PB_CHECK(names.count("Contacts") && names.count("Company"));
    PB_CHECK(e.server->count("PROPFIND", "/.well-known/carddav") >= 1);   // a bare server address starts at the well-known address
}

PB_TEST(discovery_from_a_dav_address_works_too_and_asks_the_entered_address_first) {
    Env e;
    auto d = e.client(e.server->base_url()).discover();
    PB_CHECK(d.ok());
    PB_CHECK_EQ(d.value.size(), size_t(2));
    PB_CHECK_EQ(e.server->count("PROPFIND", "/.well-known/carddav"), 0);
}

PB_TEST(the_requests_carry_depth_and_content_type_and_ask_for_the_documented_properties) {
    Env e;
    PB_CHECK(e.client().discover().ok());
    bool principal = false, home_set = false, listing = false;
    for (const auto& r : e.server->log()) {
        if (r.method != "PROPFIND") continue;
        PB_CHECK(r.headers.count("depth") == 1);
        PB_CHECK(r.headers.at("content-type").find("application/xml") == 0);
        if (r.body.find("current-user-principal") != std::string::npos) { principal = true; PB_CHECK_EQ(r.headers.at("depth"), std::string("0")); }
        if (r.body.find("addressbook-home-set") != std::string::npos) { home_set = true; PB_CHECK_EQ(r.headers.at("depth"), std::string("0")); }
        if (r.body.find("supported-report-set") != std::string::npos) {
            listing = true;
            PB_CHECK_EQ(r.headers.at("depth"), std::string("1"));
            for (const char* prop : {"resourcetype", "displayname", "sync-token", "getctag", "current-user-privilege-set", "supported-report-set"})
                PB_CHECK(r.body.find(prop) != std::string::npos);
        }
    }
    PB_CHECK(principal && home_set && listing);
}

PB_TEST(writable_only_with_write_content_or_all_and_the_flags_are_read) {
    Env e;
    FakeCardDavServer::BookOptions all;
    all.name = "Shared";
    all.privilege_all = true;
    e.server->add_book("shared", all);
    FakeCardDavServer::BookOptions nosync;
    nosync.name = "Plain";
    nosync.sync_collection = false;
    e.server->add_book("plain", nosync);
    auto d = e.client().discover();
    PB_CHECK(d.ok());
    PB_CHECK_EQ(d.value.size(), size_t(4));
    for (const auto& b : d.value) {
        PB_CHECK(!b.href.empty() && b.href[0] == '/');
        PB_CHECK(!b.ctag.empty());
        if (b.name == "Contacts") { PB_CHECK(b.writable); PB_CHECK(b.supports_sync_report); PB_CHECK(!b.sync_token.empty()); }
        if (b.name == "Company") PB_CHECK(!b.writable);
        if (b.name == "Shared") PB_CHECK(b.writable);
        if (b.name == "Plain") PB_CHECK(!b.supports_sync_report);
    }
}

PB_TEST(plain_collections_are_not_address_books) {
    Env e;
    FakeCardDavServer::BookOptions folder;
    folder.name = "Folder";
    folder.is_addressbook = false;
    e.server->add_book("folder", folder);
    auto d = e.client().discover();
    PB_CHECK(d.ok());
    PB_CHECK_EQ(d.value.size(), size_t(2));
}

PB_TEST(a_server_without_an_address_book_home_gives_no_addressbook_and_not_a_failure_class) {
    Env e(false);
    auto d = e.client().discover();
    PB_CHECK(!d.ok());
    PB_CHECK_EQ(d.error.key, std::string("no_addressbook"));
    PB_CHECK(d.error.cls == pbo::ErrorClass::Protocol);
}

PB_TEST(a_home_that_holds_no_address_book_gives_no_addressbook) {
    Env e;
    e.server->remove_book("contacts");
    e.server->remove_book("company");
    auto d = e.client().discover();
    PB_CHECK(!d.ok());
    PB_CHECK_EQ(d.error.key, std::string("no_addressbook"));
}

PB_TEST(a_wrong_password_is_auth_failed_and_nothing_else_is_asked) {
    Env e;
    auto d = e.client("", "wrong").discover();
    PB_CHECK(!d.ok());
    PB_CHECK_EQ(d.error.key, std::string("auth_failed"));
    PB_CHECK(d.error.cls == pbo::ErrorClass::Auth);
    PB_CHECK(e.server->count("PROPFIND") <= 2);   // it stops at the first refusal
}

PB_TEST(credentials_are_never_part_of_a_url) {
    Env e;
    PB_CHECK(e.client().discover().ok());
    PB_CHECK(!e.server->saw_credentials_in_url());
}

PB_TEST(a_redirect_to_another_host_receives_no_credentials) {
    Env e;
    pbtest::TlsTestServer other(e.leaf.cert, e.leaf.key, [](const std::string&) { return pbtest::TlsTestServer::http_response(207, "<d:multistatus xmlns:d=\"DAV:\"/>"); });
    FakeCardDavServer::Fault f;
    f.kind = FakeCardDavServer::Fault::Kind::RedirectOtherHost;
    f.location = other.url("/dav/");
    f.times = 5;
    e.server->inject(f);
    auto d = e.client().discover();
    PB_CHECK(!other.saw_authorization());
    PB_CHECK(!d.ok());   // the other host's answer has no principal
}

PB_TEST(a_principal_and_a_book_on_another_host_are_ignored_and_never_asked) {
    pbtest::FakeTransport http;
    http.set_handler([](const pbo::Request& r) {
        if (r.body.find("current-user-principal") != std::string::npos)
            return pbtest::FakeTransport::status(207, "<d:multistatus xmlns:d=\"DAV:\"><d:response><d:href>/dav/</d:href><d:propstat><d:prop><d:current-user-principal><d:href>https://evil.example.org/p/</d:href></d:current-user-principal></d:prop><d:status>HTTP/1.1 200 OK</d:status></d:propstat></d:response></d:multistatus>");
        // the listing of the entered address itself names a book on another host and one on this host
        return pbtest::FakeTransport::status(207,
            "<d:multistatus xmlns:d=\"DAV:\" xmlns:card=\"urn:ietf:params:xml:ns:carddav\">"
            "<d:response><d:href>https://evil.example.org/dav/books/x/</d:href><d:propstat><d:prop><d:resourcetype><d:collection/><card:addressbook/></d:resourcetype></d:prop><d:status>HTTP/1.1 200 OK</d:status></d:propstat></d:response>"
            "<d:response><d:href>//evil.example.org/dav/books/y/</d:href><d:propstat><d:prop><d:resourcetype><d:collection/><card:addressbook/></d:resourcetype></d:prop><d:status>HTTP/1.1 200 OK</d:status></d:propstat></d:response>"
            "</d:multistatus>");
    });
    CardDavClient c(http, "https://dav.example.org/dav/", "alice", "pw");
    auto d = c.discover();
    PB_CHECK(!d.ok());
    PB_CHECK_EQ(d.error.key, std::string("no_addressbook"));
    for (const pbo::Request& r : http.requests()) PB_CHECK(r.url.compare(0, 24, "https://dav.example.org/") == 0);
}

PB_TEST(status_codes_are_mapped_to_the_core_results) {
    Env e;
    struct Row { int status; const char* key; pbo::ErrorClass cls; };
    for (const Row& row : {Row{401, "auth_failed", pbo::ErrorClass::Auth}, Row{500, "server_error", pbo::ErrorClass::Server}, Row{503, "server_error", pbo::ErrorClass::Server},
                           Row{403, "forbidden", pbo::ErrorClass::Server}}) {
        FakeCardDavServer::Fault f;
        f.kind = FakeCardDavServer::Fault::Kind::Status;
        f.status = row.status;
        f.method = "PROPFIND";
        e.server->inject(f);
        auto d = e.client().discover();
        PB_CHECK(!d.ok());
        PB_CHECK_EQ(d.error.key, std::string(row.key));
        PB_CHECK(d.error.cls == row.cls);
    }
}

PB_TEST(a_dropped_connection_and_malformed_xml_are_errors_not_crashes) {
    Env e;
    FakeCardDavServer::Fault drop;
    drop.kind = FakeCardDavServer::Fault::Kind::Drop;
    e.server->inject(drop);
    auto d = e.client().discover();
    PB_CHECK(!d.ok());
    FakeCardDavServer::Fault bad;
    bad.kind = FakeCardDavServer::Fault::Kind::MalformedXml;
    e.server->inject(bad);
    d = e.client().discover();
    PB_CHECK(!d.ok());
    PB_CHECK_EQ(d.error.key, std::string("malformed_xml"));
    PB_CHECK(e.client().discover().ok());   // the server is fine again
}

PB_TEST(the_server_time_of_the_latest_answer_is_remembered_for_the_clock_check) {
    Env e;
    e.server->set_server_time(1791979200);
    CardDavClient c = e.client();
    PB_CHECK(!c.last_server_time().has_value());
    PB_CHECK(c.discover().ok());
    PB_CHECK(c.last_server_time().has_value());
    PB_CHECK_EQ(*c.last_server_time(), 1791979200LL);
}

PB_TEST(a_server_that_knows_no_carddav_gives_no_addressbook_not_an_error) {
    // a calendar-only server answers 404 / 405 to every question about address books
    for (int status : {404, 405}) {
        pbtest::FakeTransport http;
        http.set_handler([status](const pbo::Request&) { return pbtest::FakeTransport::status(status); });
        CardDavClient c(http, "https://dav.example.org/dav/", "alice", "pw");
        auto d = c.discover();
        PB_CHECK(!d.ok());
        PB_CHECK_EQ(d.error.key, std::string("no_addressbook"));
    }
    // while a login problem, a server error and no answer at all stay errors
    for (int status : {401, 500, 503}) {
        pbtest::FakeTransport http;
        http.set_handler([status](const pbo::Request&) { return pbtest::FakeTransport::status(status); });
        CardDavClient c(http, "https://dav.example.org/dav/", "alice", "pw");
        PB_CHECK(c.discover().error.key != "no_addressbook");
    }
}

PB_TEST_MAIN()
