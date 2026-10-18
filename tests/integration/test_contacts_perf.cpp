// T030 (spec 002, research R-15): host proxies of the contact budgets with 2,000 contacts. The figures that count are those of the InkPad One (page turn
// < 1 s, search < 2 s, suggestions < 1 s, <= 20 MB): they stay UNVERIFIED until measured there (docs/device-test-checklist.md DT-CT-2/5).
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include "pbo/store.h"
#include "pbo/vcard.h"
#include "test_support.h"
#include "tmp_dir.h"

namespace {

pbo::SystemClock g_clock;

double seconds_since(std::chrono::steady_clock::time_point t) { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count(); }

std::string card(int i) {
    char buf[1024];
    std::snprintf(buf, sizeof buf,
                  "BEGIN:VCARD\r\nVERSION:3.0\r\nPRODID:-//example.org//perf//EN\r\nUID:perf-%05d\r\nFN:Given%04d Family%04d\r\nN:Family%04d;Given%04d;;;\r\nORG:Organization %d GmbH\r\n"
                  "TEL;TYPE=CELL:+49 170 %07d\r\nTEL;TYPE=WORK:+49 30 %07d\r\nEMAIL;TYPE=WORK:given%04d@example.org\r\nEMAIL;TYPE=HOME:home%04d@example.org\r\n"
                  "ADR;TYPE=HOME:;;%d Example Street;Exampletown;EX;%05d;Exampleland\r\nNOTE:A note for contact number %d with some more words to make it realistic.\r\n"
                  "BDAY:1980-01-01\r\nURL:https://www.example.org/%d\r\nREV:20260101T000000Z\r\nEND:VCARD\r\n",
                  i, i, i, i, i, i % 97, i, i + 5000000, i, i, i % 300, 10000 + i % 90000, i, i);
    return buf;
}

struct Env {
    pbtest::TmpDir dir;
    std::unique_ptr<pbo::Store> store;
    int64_t book = 0;
    Env() {
        auto r = pbo::Store::open(dir.path(), g_clock);
        PB_CHECK(r.ok());
        store = std::move(r.value);
        pbo::AddressBook b;
        b.href = "/dav/addressbooks/alice/contacts/";
        b.name = "Contacts";
        b.writable = true;
        auto id = store->upsert_addressbook(b);
        PB_CHECK(id.ok());
        book = id.value;
    }
    void fill(int n) {
        pbo::ContactPullBatch batch;
        for (int i = 0; i < n; ++i)
            batch.upserts.push_back(pbo::contact_upsert_from_card("/dav/addressbooks/alice/contacts/c" + std::to_string(i) + ".vcf", "\"e" + std::to_string(i) + "\"", card(i)));
        PB_CHECK(store->apply_contact_pull(book, batch).ok());
    }
};

}  // namespace

PB_TEST(a_book_of_2000_contacts_pages_and_searches_fast_and_stays_small) {
    Env e;
    const int64_t before = e.store->usage().database_bytes;
    auto t0 = std::chrono::steady_clock::now();
    e.fill(2000);
    const double fill_s = seconds_since(t0);
    const int64_t grown = e.store->usage().database_bytes - before;
    std::printf("fill %.2f s, database grown by %lld bytes\n", fill_s, static_cast<long long>(grown));
    PB_CHECK(fill_s < 60.0);
    PB_CHECK(grown < (8ll << 20));      // the proxy for the budget: expected about 5 MB, the budget is 20 MB (SC-012)
    PB_CHECK(grown < (20ll << 20));
    PB_CHECK_EQ(e.store->contacts_count(""), 2000);

    // paging: every page of 9 rows, the way the list shows them
    t0 = std::chrono::steady_clock::now();
    double worst = 0;
    for (int off = 0; off < 2000; off += 9) {
        const auto t = std::chrono::steady_clock::now();
        const auto page = e.store->contacts_page("", off, 9);
        worst = std::max(worst, seconds_since(t));
        PB_CHECK(!page.empty());
    }
    std::printf("worst page %.4f s\n", worst);
    PB_CHECK(worst < 0.1);

    // searching: a name, a number, an organization, an email address, and a word that is nowhere
    for (const char* query : {"family1234", "0000042", "organization 17", "home1999@example.org", "nowhere-at-all", "given1 family1"}) {
        t0 = std::chrono::steady_clock::now();
        const int n = e.store->contacts_count(query);
        const auto page = e.store->contacts_page(query, 0, 9);
        const double s = seconds_since(t0);
        std::printf("search '%s': %d hits in %.4f s\n", query, n, s);
        PB_CHECK(s < 0.1);
        PB_CHECK(static_cast<int>(page.size()) == std::min(n, 9));
    }
    PB_CHECK_EQ(e.store->contacts_count("family1234"), 1);
    PB_CHECK_EQ(e.store->contacts_count("nowhere-at-all"), 0);
}

PB_TEST(opening_the_store_does_not_read_the_contacts) {
    Env e;
    e.fill(2000);
    const std::string dir = e.dir.path();
    e.store.reset();
    auto t0 = std::chrono::steady_clock::now();
    auto again = pbo::Store::open(dir, g_clock);
    const double open_s = seconds_since(t0);
    PB_CHECK(again.ok());
    std::printf("open with 2000 contacts: %.4f s\n", open_s);
    PB_CHECK(open_s < 0.5);   // the cold start budget of spec 001 stays: contacts are not read at start
}

PB_TEST(suggestions_for_2000_contacts_with_several_addresses_are_fast_and_find_every_name) {
    Env e;
    e.fill(2000);
    PB_CHECK(e.store->note_address("given0007@example.org", "Mail Only", 3).ok());
    // names with umlauts and contacts with several addresses
    pbo::ContactPullBatch batch;
    const char* names[] = {"Jürgen Müller", "Zoë Ångström", "Çelik Öztürk", "Björn Äpfel", "Łukasz Żółć", "Renée Dupont", "Søren Ærø", "Anaïs Nin", "Hélène Fürst", "Günther Groß",
                           "Maria Schmidt", "Hans Meier", "Klaus Fischer", "Petra Weber", "Uwe Wagner", "Sabine Becker", "Jörg Hoffmann", "Ute Schäfer", "Åsa Lindqvist", "Noël Müller-Lüdenscheid"};
    for (int i = 0; i < 20; ++i) {
        std::string given = names[i], family;
        const size_t sp = given.find(' ');
        family = given.substr(sp + 1);
        given = given.substr(0, sp);
        batch.upserts.push_back(pbo::contact_upsert_from_card("/dav/addressbooks/alice/contacts/n" + std::to_string(i) + ".vcf", "\"n\"",
            "BEGIN:VCARD\r\nVERSION:3.0\r\nUID:name-" + std::to_string(i) + "\r\nFN:" + given + " " + family + "\r\nN:" + family + ";" + given + ";;;\r\nEMAIL;TYPE=WORK:work" + std::to_string(i) +
            "@example.org\r\nEMAIL;TYPE=HOME:home" + std::to_string(i) + "@example.org\r\nEND:VCARD\r\n"));
    }
    PB_CHECK(e.store->apply_contact_pull(e.book, batch).ok());
    int found = 0;
    double worst = 0;
    const char* typed[] = {"jür", "jur", "zoë", "zoe", "çel", "cel", "björn", "bjorn", "łuk", "ren", "søren", "soren", "anaï", "anai", "hélène", "helene", "günther", "gunther", "maria", "hans"};
    for (int i = 0; i < 20; ++i) {
        const auto t = std::chrono::steady_clock::now();
        const auto r = e.store->address_suggestions(typed[i], 8).size();
        worst = std::max(worst, seconds_since(t));
        if (r >= 2) ++found;   // both addresses of the person
    }
    std::printf("worst suggestion query %.4f s, found %d of 20\n", worst, found);
    PB_CHECK_EQ(found, 20);   // 100 %
    PB_CHECK(worst < 0.1);
    const auto t = std::chrono::steady_clock::now();
    PB_CHECK_EQ(e.store->address_suggestions("given00", 8).size(), size_t(8));
    PB_CHECK(seconds_since(t) < 0.1);
}

PB_TEST_MAIN()
