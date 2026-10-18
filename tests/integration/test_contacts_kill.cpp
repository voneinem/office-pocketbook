// Spec 002 SC-005 (Principle III): the app is killed (SIGKILL, like a battery pull) at seeded random moments while contacts are created and pushed to the
// fake CardDAV server and pulled back. After every kill the store opens, passes its integrity check, holds every contact the child reported as saved
// (once, whole), and one more sync (push, then pull) leaves exactly one card on the server for each, none lost, none doubled. A kill between the
// server taking a card and the reader noting it is covered because the child works against a real (fake) server. UNVERIFIED against real servers.
#include <map>
#include <set>
#include <sstream>

#include "contact_env.h"
#include "kill_harness.h"
#include "pbo/contact_edit.h"
#include "test_support.h"

using pbtest::KillRun;

PBO_FAKE_SERVER_TEST();

namespace {

pbo::LabeledValue lv(const std::string& label, const std::string& value) {
    pbo::LabeledValue v;
    v.label = label;
    v.value = value;
    return v;
}

pbo::ContactEdit person(int k) {
    pbo::ContactEdit e;
    e.given = "K" + std::to_string(k);
    e.family = "Kill";
    e.phones = {lv("mobile", "+1 555 " + std::to_string(1000 + k))};
    e.emails = {lv("work", "k" + std::to_string(k) + "@example.org")};
    e.note = std::string(300, 'n');
    return e;
}

}  // namespace

PB_TEST(killing_the_app_while_contacts_are_created_pushed_and_pulled_never_loses_or_doubles_a_contact) {
    pbtest::TmpDir pki_dir;
    pbtest::TestPki pki(pki_dir);
    if (!pki.available()) PB_SKIP("openssl CLI not available");
    auto leaf = pki.issue("srv", "DNS:localhost,IP:127.0.0.1");
    PB_CHECK(leaf.ok);
    pbtest::FakeCardDavServer::Config sc;
    sc.cert = leaf.cert;
    sc.key = leaf.key;
    pbtest::FakeCardDavServer server(sc);
    pbtest::FakeCardDavServer::BookOptions book;
    book.name = "Contacts";
    server.add_book("contacts", book);
    const std::string url = server.origin();

    const int n = KillRun::iterations(100);
    int total_reported = 0;
    for (int i = 0; i < n; ++i) {
        pbtest::TmpDir dir;
        pbtest::ContactClock clock;
        pbtest::ContactNoCancel cancel;
        pbtest::ContactCounterRandom random;
        const std::string path = dir.path();
        const auto reports = KillRun::run(
            [&](int fd) {
                auto opened = pbo::Store::open(path, clock);
                pbo::Store& s = *opened.value;
                pbo::CalendarAccount a;
                a.caldav_url = url;
                a.username = "alice";
                s.save_calendar_account(a);
                pbo::CurlTransport http(pki.ca_cert());
                pbo::ContactsSyncDeps deps{s, http, clock, cancel};
                pbo::ContactEditDeps edit{s, clock, random};
                if (!pbo::pull_contacts(deps, "pw-for-tests", nullptr).ok()) return;
                int64_t book_id = 0;
                for (const auto& b : s.addressbooks()) book_id = b.id;
                KillRun::report(fd, "ready 0");
                for (int k = 1;; ++k) {
                    // the card of this round is told apart from the cards of the other rounds by the iteration
                    if (pbo::create_contact(edit, book_id, person(i * 1000 + k)).ok()) KillRun::report(fd, "created " + std::to_string(i * 1000 + k));
                    if (k % 2 == 0) {
                        pbo::push_contacts(deps, "pw-for-tests", nullptr);
                        pbo::pull_contacts(deps, "pw-for-tests", nullptr);
                    }
                }
            },
            1, KillRun::delay_for(i, 250000));
        if (reports.empty()) continue;   // killed before it reported anything

        // the app starts again
        auto reopened = pbo::Store::open(path, clock);
        if (!reopened.ok()) PB_FAIL_AT("iteration " + std::to_string(i) + ": the store did not open after the kill: " + reopened.error.key + " / " + reopened.error.detail);
        pbo::Store& s = *reopened.value;
        PB_CHECK(s.check_integrity().ok());
        std::set<int> created;
        for (const std::string& r : reports) {
            std::istringstream is(r);
            std::string what;
            int k = 0;
            is >> what >> k;
            if (what == "created") created.insert(k);
        }
        total_reported += static_cast<int>(created.size());
        // everything that was reported as saved is on the reader, once and whole
        for (int k : created) {
            const std::string name = "K" + std::to_string(k) + " Kill";
            PB_CHECK_EQ(s.debug_query("SELECT count(*) FROM contact WHERE display_name = '" + name + "'"), std::string("1"));
            PB_CHECK_EQ(s.debug_query("SELECT count(*) FROM contact c JOIN contact_email e ON e.contact_id = c.id WHERE c.display_name = '" + name + "'"), std::string("1"));
            PB_CHECK(s.debug_query("SELECT vcf FROM contact WHERE display_name = '" + name + "'").find("END:VCARD") != std::string::npos);
        }
        // one more sync: push what waits, then pull
        pbo::CurlTransport http(pki.ca_cert());
        pbo::ContactsSyncDeps deps{s, http, clock, cancel};
        PB_CHECK(pbo::push_contacts(deps, "pw-for-tests", nullptr).ok());
        PB_CHECK(pbo::pull_contacts(deps, "pw-for-tests", nullptr).ok());
        PB_CHECK_EQ(s.pending_contacts().size(), size_t(0));
        for (int k : created) {
            const std::string name = "K" + std::to_string(k) + " Kill";
            PB_CHECK_EQ(s.debug_query("SELECT count(*) FROM contact WHERE display_name = '" + name + "'"), std::string("1"));
            int cards = 0;
            for (const std::string& card : server.card_names("contacts"))
                if (server.card("contacts", card).vcf.find("FN:" + name + "\r\n") != std::string::npos) ++cards;
            PB_CHECK_EQ(cards, 1);   // not lost, not doubled
        }
        // every card the reader holds is on the server as well (the server may hold more: the cards of earlier rounds)
        PB_CHECK(static_cast<int>(server.card_count("contacts")) >= std::atoi(s.debug_query("SELECT count(*) FROM contact").c_str()));
    }
    PB_CHECK(total_reported > 0);   // the kills did not all come before the first contact
}

PB_TEST(killing_the_app_while_contacts_are_changed_and_pushed_never_loses_an_edit_or_the_photo_on_the_server) {
    pbtest::TmpDir pki_dir;
    pbtest::TestPki pki(pki_dir);
    if (!pki.available()) PB_SKIP("openssl CLI not available");
    auto leaf = pki.issue("srv", "DNS:localhost,IP:127.0.0.1");
    PB_CHECK(leaf.ok);
    pbtest::FakeCardDavServer::Config sc;
    sc.cert = leaf.cert;
    sc.key = leaf.key;
    pbtest::FakeCardDavServer server(sc);
    pbtest::FakeCardDavServer::BookOptions book;
    book.name = "Contacts";
    server.add_book("contacts", book);
    const std::string photo = "PHOTO;ENCODING=b;TYPE=JPEG:" + std::string(2000, 'A') + "\r\n";
    constexpr int kCards = 6;
    for (int j = 0; j < kCards; ++j)
        server.put_card("contacts", "e" + std::to_string(j) + ".vcf", pbtest::contact_vcf("u" + std::to_string(j), "Given" + std::to_string(j), "Start", photo + "BDAY:1985-04-12\r\nX-KEEP:yes\r\n"));
    const std::string url = server.origin();

    const int n = KillRun::iterations(100);
    int total = 0;
    for (int i = 0; i < n; ++i) {
        pbtest::TmpDir dir;
        pbtest::ContactClock clock;
        pbtest::ContactNoCancel cancel;
        pbtest::ContactCounterRandom random;
        const std::string path = dir.path();
        const auto reports = KillRun::run(
            [&](int fd) {
                auto opened = pbo::Store::open(path, clock);
                pbo::Store& s = *opened.value;
                pbo::CalendarAccount a;
                a.caldav_url = url;
                a.username = "alice";
                s.save_calendar_account(a);
                pbo::CurlTransport http(pki.ca_cert());
                pbo::ContactsSyncDeps deps{s, http, clock, cancel};
                pbo::ContactEditDeps edit{s, clock, random};
                if (!pbo::pull_contacts(deps, "pw-for-tests", nullptr).ok()) return;
                KillRun::report(fd, "ready 0 0");
                for (int r = 1;; ++r) {
                    for (int j = 0; j < kCards; ++j) {
                        int64_t id = 0;
                        for (const auto& item : s.contacts_page("Given" + std::to_string(j), 0, 5)) id = item.id;
                        if (id == 0) continue;
                        auto view = s.contact(id);
                        if (!view || !view->card.readable) continue;
                        pbo::ContactEdit e = pbo::contact_edit_of(view->card);
                        e.family = "I" + std::to_string(i) + "R" + std::to_string(r);
                        if (pbo::change_contact(edit, id, e).ok()) KillRun::report(fd, "edited " + std::to_string(j) + " " + std::to_string(r));
                        if (j % 3 == 2) {
                            pbo::push_contacts(deps, "pw-for-tests", nullptr);
                            pbo::pull_contacts(deps, "pw-for-tests", nullptr);
                        }
                    }
                }
            },
            1, KillRun::delay_for(i, 250000));
        if (reports.empty()) continue;

        auto reopened = pbo::Store::open(path, clock);
        if (!reopened.ok()) PB_FAIL_AT("iteration " + std::to_string(i) + ": the store did not open after the kill: " + reopened.error.key);
        pbo::Store& s = *reopened.value;
        PB_CHECK(s.check_integrity().ok());
        std::map<int, int> last;   // card -> last reported round
        for (const std::string& r : reports) {
            std::istringstream is(r);
            std::string what;
            int j = 0, round = 0;
            is >> what >> j >> round;
            if (what == "edited") last[j] = round;
        }
        total += static_cast<int>(last.size());
        for (const auto& kv : last) {   // an edit that was reported is on the reader, whole, at least as new as reported
            const std::string family = s.debug_query("SELECT family FROM contact WHERE uid = 'u" + std::to_string(kv.first) + "'");
            PB_CHECK(family.compare(0, 1, "I") == 0);
            const int round = std::atoi(family.substr(family.find('R') + 1).c_str());
            const int iter = std::atoi(family.substr(1, family.find('R') - 1).c_str());
            PB_CHECK_EQ(iter, i);
            PB_CHECK(round >= kv.second);
        }
        pbo::CurlTransport http(pki.ca_cert());
        pbo::ContactsSyncDeps deps{s, http, clock, cancel};
        PB_CHECK(pbo::push_contacts(deps, "pw-for-tests", nullptr).ok());
        PB_CHECK(pbo::pull_contacts(deps, "pw-for-tests", nullptr).ok());
        PB_CHECK_EQ(s.pending_contacts().size(), size_t(0));
        PB_CHECK_EQ(server.card_count("contacts"), size_t(kCards));   // no card lost, none doubled
        for (const auto& kv : last) {
            const std::string family = s.debug_query("SELECT family FROM contact WHERE uid = 'u" + std::to_string(kv.first) + "'");
            const std::string vcf = server.card("contacts", "e" + std::to_string(kv.first) + ".vcf").vcf;
            PB_CHECK(vcf.find("N:" + family + ";Given" + std::to_string(kv.first) + ";;;") != std::string::npos);   // the edit reached the server
        }
        for (int j = 0; j < kCards; ++j) {   // and what the app never shows is still there on every card
            const std::string vcf = server.card("contacts", "e" + std::to_string(j) + ".vcf").vcf;
            PB_CHECK(vcf.find("PHOTO;ENCODING=b;TYPE=JPEG:") != std::string::npos);
            PB_CHECK(vcf.find("BDAY:1985-04-12") != std::string::npos);
            PB_CHECK(vcf.find("X-KEEP:yes") != std::string::npos);
        }
    }
    PB_CHECK(total > 0);
}

PB_TEST(killing_the_app_while_contacts_are_deleted_and_pushed_never_loses_or_repeats_a_deletion) {
    pbtest::TmpDir pki_dir;
    pbtest::TestPki pki(pki_dir);
    if (!pki.available()) PB_SKIP("openssl CLI not available");
    auto leaf = pki.issue("srv", "DNS:localhost,IP:127.0.0.1");
    PB_CHECK(leaf.ok);
    pbtest::FakeCardDavServer::Config sc;
    sc.cert = leaf.cert;
    sc.key = leaf.key;
    pbtest::FakeCardDavServer server(sc);
    pbtest::FakeCardDavServer::BookOptions book;
    book.name = "Contacts";
    server.add_book("contacts", book);
    constexpr int kCards = 8;
    const std::string url = server.origin();

    const int n = KillRun::iterations(100);
    int total = 0;
    for (int i = 0; i < n; ++i) {
        for (int j = 0; j < kCards; ++j)   // every round starts with the same cards on the server
            server.put_card("contacts", "d" + std::to_string(j) + ".vcf", pbtest::contact_vcf("u" + std::to_string(j), "Given" + std::to_string(j), "Start"));
        pbtest::TmpDir dir;
        pbtest::ContactClock clock;
        pbtest::ContactNoCancel cancel;
        pbtest::ContactCounterRandom random;
        const std::string path = dir.path();
        const auto reports = KillRun::run(
            [&](int fd) {
                auto opened = pbo::Store::open(path, clock);
                pbo::Store& s = *opened.value;
                pbo::CalendarAccount a;
                a.caldav_url = url;
                a.username = "alice";
                s.save_calendar_account(a);
                pbo::CurlTransport http(pki.ca_cert());
                pbo::ContactsSyncDeps deps{s, http, clock, cancel};
                if (!pbo::pull_contacts(deps, "pw-for-tests", nullptr).ok()) return;
                KillRun::report(fd, "ready 0");
                for (int j = 0; j < kCards; ++j) {
                    int64_t id = 0;
                    for (const auto& item : s.contacts_page("Given" + std::to_string(j), 0, 5)) id = item.id;
                    if (id == 0) continue;
                    if (pbo::delete_contact(s, id).ok()) KillRun::report(fd, "deleted " + std::to_string(j));
                    if (j % 2 == 1) {
                        pbo::push_contacts(deps, "pw-for-tests", nullptr);
                        pbo::pull_contacts(deps, "pw-for-tests", nullptr);
                    }
                }
                for (;;) pbo::push_contacts(deps, "pw-for-tests", nullptr);
            },
            1, KillRun::delay_for(i, 250000));
        if (reports.empty()) continue;

        auto reopened = pbo::Store::open(path, clock);
        if (!reopened.ok()) PB_FAIL_AT("iteration " + std::to_string(i) + ": the store did not open after the kill: " + reopened.error.key);
        pbo::Store& s = *reopened.value;
        PB_CHECK(s.check_integrity().ok());
        std::set<int> deleted;
        for (const std::string& r : reports) {
            std::istringstream is(r);
            std::string what;
            int j = 0;
            is >> what >> j;
            if (what == "deleted") deleted.insert(j);
        }
        total += static_cast<int>(deleted.size());
        // a deletion that was reported is either still waiting or finished; the contact is not in the list
        for (int j : deleted) {
            const std::string state = s.debug_query("SELECT sync_state FROM contact WHERE uid = 'u" + std::to_string(j) + "'");
            PB_CHECK(state.empty() || state == "pending_delete");
            PB_CHECK_EQ(s.contacts_page("Given" + std::to_string(j), 0, 5).size(), size_t(0));
        }
        pbo::CurlTransport http(pki.ca_cert());
        pbo::ContactsSyncDeps deps{s, http, clock, cancel};
        PB_CHECK(pbo::push_contacts(deps, "pw-for-tests", nullptr).ok());
        PB_CHECK(pbo::pull_contacts(deps, "pw-for-tests", nullptr).ok());
        PB_CHECK_EQ(s.pending_contacts().size(), size_t(0));
        for (int j : deleted) {
            PB_CHECK_EQ(s.debug_query("SELECT count(*) FROM contact WHERE uid = 'u" + std::to_string(j) + "'"), std::string("0"));
            bool on_server = false;
            for (const std::string& card : server.card_names("contacts"))
                if (card == "d" + std::to_string(j) + ".vcf") on_server = true;
            PB_CHECK(!on_server);   // not lost
        }
        // what the user did not delete is still on the reader and on the server
        for (int j = 0; j < kCards; ++j) {
            if (deleted.count(j)) continue;
            const bool on_reader = s.debug_query("SELECT count(*) FROM contact WHERE uid = 'u" + std::to_string(j) + "'") == "1";
            bool on_server = false;
            for (const std::string& card : server.card_names("contacts"))
                if (card == "d" + std::to_string(j) + ".vcf") on_server = true;
            PB_CHECK_EQ(on_reader, on_server);   // a delete that was done but not reported removes it on both sides, never on one only
        }
    }
    PB_CHECK(total > 0);
}

PB_TEST_MAIN()
