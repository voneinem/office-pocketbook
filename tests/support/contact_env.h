#pragma once
// Shared setup of the contact tests (spec 002): a fake CardDAV server, a store with the calendar server account, and the pull / push / edit calls.
// Everything runs against the fake server: UNVERIFIED against real servers.
#include <string>

#include "fake_carddav_server.h"
#include "pbo/contacts_sync.h"
#include "pbo/curl_transport.h"
#include "pbo/vcard.h"
#include "test_support.h"
#include "tmp_dir.h"

namespace pbtest {

constexpr int64_t kContactNow = 1791979200;   // 2026-10-14 12:00:00 UTC

struct ContactClock : pbo::Clock {
    int64_t now() const override { return kContactNow; }
};
struct ContactNoCancel : pbo::Cancel {
    bool requested() const override { return false; }
};
struct ContactCounterRandom : pbo::Random {
    uint8_t n = 1;
    void bytes(uint8_t* out, size_t len) override {
        for (size_t i = 0; i < len; ++i) out[i] = n++;
    }
};

// A small valid card. `extra` is inserted before END:VCARD (full lines with CRLF).
inline std::string contact_vcf(const std::string& uid, const std::string& given, const std::string& family, const std::string& extra = "") {
    return "BEGIN:VCARD\r\nVERSION:3.0\r\nPRODID:-//example.org//test//EN\r\nUID:" + uid + "\r\nFN:" + given + " " + family + "\r\nN:" + family + ";" + given + ";;;\r\n" + extra +
           "END:VCARD\r\n";
}

struct ContactEnv {
    TmpDir data, pki_dir;
    TestPki pki{pki_dir};
    TestPki::Leaf leaf;
    ContactClock clock;
    ContactNoCancel cancel;
    ContactCounterRandom random;
    std::unique_ptr<FakeCardDavServer> server;
    std::unique_ptr<pbo::Store> store;
    std::unique_ptr<pbo::CurlTransport> http;
    int64_t main_book = 0;   // the writable address book "Contacts"

    // with_read_only: a second, read-only book "Company"; start_pull: the books are discovered and downloaded at once
    explicit ContactEnv(bool with_read_only = false, bool start_pull = true) {
        if (!pki.available()) PB_SKIP("openssl CLI not available");
        leaf = pki.issue("srv", "DNS:localhost,IP:127.0.0.1");
        PB_CHECK(leaf.ok);
        FakeCardDavServer::Config c;
        c.cert = leaf.cert;
        c.key = leaf.key;
        server = std::make_unique<FakeCardDavServer>(c);
        auto opened = pbo::Store::open(data.path(), clock);
        PB_CHECK(opened.ok());
        store = std::move(opened.value);
        http = std::make_unique<pbo::CurlTransport>(pki.ca_cert());
        FakeCardDavServer::BookOptions o;
        o.name = "Contacts";
        server->add_book("contacts", o);
        if (with_read_only) {
            FakeCardDavServer::BookOptions ro;
            ro.name = "Company";
            ro.writable = false;
            server->add_book("company", ro);
        }
        pbo::CalendarAccount a;
        a.caldav_url = server->origin();
        a.username = "alice";
        PB_CHECK(store->save_calendar_account(a).ok());
        if (start_pull) {
            PB_CHECK(pull().ok());
            main_book = book_id("Contacts");
            PB_CHECK(main_book != 0);
        }
    }
    pbo::ContactsSyncDeps deps() { return pbo::ContactsSyncDeps{*store, *http, clock, cancel}; }
    pbo::Error pull(pbo::ContactsCounts* counts = nullptr) {
        auto d = deps();
        return pbo::pull_contacts(d, "pw-for-tests", counts);
    }
    std::string q(const std::string& sql) { return store->debug_query(sql); }
    int count(const std::string& sql) { return std::atoi(q(sql).c_str()); }
    int64_t book_id(const std::string& name) {
        for (const pbo::AddressBook& b : store->addressbooks())
            if (b.name == name) return b.id;
        return 0;
    }
    int64_t id_of_uid(const std::string& uid) { return std::atoll(q("SELECT id FROM contact WHERE uid = '" + uid + "'").c_str()); }
    int64_t id_of_href(const std::string& slug, const std::string& name) { return std::atoll(q("SELECT id FROM contact WHERE href = '" + server->card_href(slug, name) + "'").c_str()); }
    std::string state_of(int64_t id) { return q("SELECT sync_state FROM contact WHERE id = " + std::to_string(id)); }

    // a synced contact from the server, as the user sees it after a pull
    int64_t server_contact(const std::string& name, const std::string& uid, const std::string& given, const std::string& family, const std::string& extra = "",
                           const std::string& slug = "contacts") {
        server->put_card(slug, name, contact_vcf(uid, given, family, extra));
        PB_CHECK(pull().ok());
        return id_of_href(slug, name);
    }
};

}  // namespace pbtest
