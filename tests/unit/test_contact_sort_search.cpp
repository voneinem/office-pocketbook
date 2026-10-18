// T028 (spec 002, research R-11): the list of contacts of the shown address books, sorted by name, paged, and the search over name, organization,
// email addresses and phone numbers. The store works on cards made the way the pull makes them (contact_upsert_from_card).
#include <algorithm>
#include <string>
#include <vector>

#include "pbo/store.h"
#include "pbo/vcard.h"
#include "test_support.h"
#include "tmp_dir.h"

namespace {

pbo::SystemClock g_clock;

struct Env {
    pbtest::TmpDir dir;
    std::unique_ptr<pbo::Store> store;
    int64_t book = 0, other = 0;
    int n = 0;
    Env() {
        auto r = pbo::Store::open(dir.path(), g_clock);
        PB_CHECK(r.ok());
        store = std::move(r.value);
        pbo::AddressBook a;
        a.href = "/dav/addressbooks/alice/contacts/";
        a.name = "Contacts";
        a.writable = true;
        auto id = store->upsert_addressbook(a);
        PB_CHECK(id.ok());
        book = id.value;
        pbo::AddressBook b;
        b.href = "/dav/addressbooks/alice/company/";
        b.name = "Company";
        id = store->upsert_addressbook(b);
        PB_CHECK(id.ok());
        other = id.value;
    }
    int64_t add(const std::string& body, int64_t in_book = 0, const std::string& version = "3.0") {
        ++n;
        const std::string name = "c" + std::to_string(n) + ".vcf";
        const int64_t b = in_book ? in_book : book;
        std::string href = std::string(b == book ? "/dav/addressbooks/alice/contacts/" : "/dav/addressbooks/alice/company/") + name;
        const std::string card = "BEGIN:VCARD\r\nVERSION:" + version + "\r\nUID:uid-" + std::to_string(n) + "\r\n" + body + "END:VCARD\r\n";
        pbo::ContactPullBatch batch;
        batch.upserts.push_back(pbo::contact_upsert_from_card(href, "\"e" + std::to_string(n) + "\"", card));
        PB_CHECK(store->apply_contact_pull(b, batch).ok());
        return std::atoll(store->debug_query("SELECT id FROM contact WHERE href = '" + href + "'").c_str());
    }
    std::vector<std::string> names(const std::string& query = "", int offset = 0, int limit = 100) {
        std::vector<std::string> out;
        for (const auto& i : store->contacts_page(query, offset, limit)) out.push_back(i.display_name);
        return out;
    }
};

}  // namespace

PB_TEST(contacts_are_sorted_by_family_then_given_name_with_umlauts_folded) {
    Env e;
    e.add("FN:Bert Beispiel\r\nN:Beispiel;Bert;;;\r\n");
    e.add("FN:Anna Beispiel\r\nN:Beispiel;Anna;;;\r\n");
    e.add("FN:Uta Ärger\r\nN:Ärger;Uta;;;\r\n");
    e.add("FN:Zoe Aal\r\nN:Aal;Zoe;;;\r\n");
    e.add("FN:Mama\r\nN:;Mama;;;\r\n");
    e.add("ORG:Zeta Supplies\r\nTEL:+1 555 0100\r\n");
    e.add("EMAIL:nobody@example.org\r\n");
    e.add("TEL:+1 555 0999\r\n");
    const std::vector<std::string> want = {"Zoe Aal", "Uta Ärger", "Anna Beispiel", "Bert Beispiel", "Mama", "nobody@example.org", "+1 555 0999", "Zeta Supplies"};
    // nameless cards sort under their organization, email address or phone number: "nobody" < "mama"? no - check the real order below
    std::vector<std::string> got = e.names();
    PB_CHECK_EQ(got.size(), want.size());
    // the named ones keep their relative order
    auto at = [&](const std::string& n) { return std::find(got.begin(), got.end(), n) - got.begin(); };
    PB_CHECK(at("Zoe Aal") < at("Uta Ärger"));
    PB_CHECK(at("Uta Ärger") < at("Anna Beispiel"));
    PB_CHECK(at("Anna Beispiel") < at("Bert Beispiel"));
    PB_CHECK(at("Bert Beispiel") < at("Mama"));
    // nameless cards: "+1 555 0999" (phone) sorts by its text, "nobody@..." by the email address, "Zeta Supplies" by the organization
    PB_CHECK(at("Mama") < at("nobody@example.org"));
    PB_CHECK(at("nobody@example.org") < at("Zeta Supplies"));
}

PB_TEST(pages_are_stable_complete_and_capped_at_100_rows) {
    Env e;
    for (int i = 0; i < 250; ++i) {
        char fam[16];
        std::snprintf(fam, sizeof fam, "Family%03d", i);
        e.add(std::string("FN:Given ") + fam + "\r\nN:" + fam + ";Given;;;\r\n");
    }
    PB_CHECK_EQ(e.store->contacts_count(""), 250);
    std::vector<std::string> all;
    for (int off = 0; off < 250; off += 7) {
        const auto page = e.names("", off, 7);
        PB_CHECK(page.size() <= 7);
        all.insert(all.end(), page.begin(), page.end());
    }
    PB_CHECK_EQ(all.size(), size_t(250));
    PB_CHECK_EQ(all.front(), std::string("Given Family000"));
    PB_CHECK_EQ(all.back(), std::string("Given Family249"));
    PB_CHECK(std::is_sorted(all.begin(), all.end()));
    PB_CHECK_EQ(e.names("", 0, 1000).size(), size_t(100));   // a page never holds more than 100 rows
    PB_CHECK_EQ(e.names("", 240, 50).size(), size_t(10));
    PB_CHECK_EQ(e.names("", 250, 50).size(), size_t(0));
    PB_CHECK_EQ(e.names("", 0, 0).size(), size_t(0));
}

PB_TEST(search_matches_name_organization_email_and_phone_numbers_with_every_word) {
    Env e;
    e.add("FN:Anna Beispiel\r\nN:Beispiel;Anna;;;\r\nORG:Example Works GmbH\r\nTEL:+49 170 555 0120\r\nEMAIL:anna@example.org\r\n");
    e.add("FN:Bert Muster\r\nN:Muster;Bert;;;\r\nORG:Other Corp\r\nTEL:+1 555 0100\r\nEMAIL:bert.muster@other.example\r\n");
    e.add("FN:Carla Dritte\r\nN:Dritte;Carla;;;\r\nTEL:030 123456\r\n");
    auto q = [&](const std::string& query) { return e.names(query); };
    PB_CHECK((q("beispiel") == std::vector<std::string>{"Anna Beispiel"}));
    PB_CHECK((q("ANNA") == std::vector<std::string>{"Anna Beispiel"}));
    PB_CHECK((q("works") == std::vector<std::string>{"Anna Beispiel"}));                 // organization
    PB_CHECK((q("other.example") == std::vector<std::string>{"Bert Muster"}));           // email address
    PB_CHECK((q("0120") == std::vector<std::string>{"Anna Beispiel"}));                   // phone number
    PB_CHECK((q("0170") == std::vector<std::string>{}));                                   // "+49 170 ..." has no "0170"
    PB_CHECK((q("170") == std::vector<std::string>{"Anna Beispiel"}));
    PB_CHECK((q("+49 170") == std::vector<std::string>{"Anna Beispiel"}));
    PB_CHECK((q("555-0100") == std::vector<std::string>{"Bert Muster"}));                 // dashes and spaces do not matter in a number
    PB_CHECK((q("(030) 123456") == std::vector<std::string>{"Carla Dritte"}));
    PB_CHECK((q("anna works") == std::vector<std::string>{"Anna Beispiel"}));             // every word must match
    PB_CHECK((q("anna other") == std::vector<std::string>{}));
    PB_CHECK((q("zzz") == std::vector<std::string>{}));
    PB_CHECK_EQ(q("").size(), size_t(3));
    PB_CHECK_EQ(q("   ").size(), size_t(3));
    PB_CHECK_EQ(e.store->contacts_count("example"), 3 - 1 /* Carla has no "example" */);
}

PB_TEST(umlauts_case_and_sharp_s_are_folded_in_the_query_and_the_card) {
    Env e;
    e.add("FN:Jürgen Größe\r\nN:Größe;Jürgen;;;\r\n");
    e.add("FN:Zoë Ünal\r\nN:Ünal;Zoë;;;\r\n");
    PB_CHECK((e.names("grosse") == std::vector<std::string>{"Jürgen Größe"}));
    PB_CHECK((e.names("JURGEN") == std::vector<std::string>{"Jürgen Größe"}));
    PB_CHECK((e.names("Größe") == std::vector<std::string>{"Jürgen Größe"}));
    PB_CHECK((e.names("zoe unal") == std::vector<std::string>{"Zoë Ünal"}));
}

PB_TEST(percent_underscore_and_backslash_in_the_query_are_ordinary_characters) {
    Env e;
    e.add("FN:Hundert Prozent\r\nN:Prozent;Hundert;;;\r\nNOTE:x\r\nORG:100% Sure_Ltd\\\\Backslash\r\n");
    e.add("FN:Anna Beispiel\r\nN:Beispiel;Anna;;;\r\n");
    PB_CHECK((e.names("100%") == std::vector<std::string>{"Hundert Prozent"}));
    PB_CHECK((e.names("sure_ltd") == std::vector<std::string>{"Hundert Prozent"}));
    PB_CHECK((e.names("%").size() == 1));
    PB_CHECK((e.names("_").size() == 1));
    PB_CHECK((e.names("a_na").size() == 0));     // "_" is not a wildcard
    PB_CHECK((e.names("a%a").size() == 0));      // "%" is not a wildcard
    PB_CHECK((e.names("\\").size() == 1));
    PB_CHECK((e.names("'; DROP TABLE contact; --").size() == 0));
    PB_CHECK_EQ(e.store->contacts_count(""), 2);
}

PB_TEST(only_contacts_of_shown_books_and_not_those_waiting_for_deletion_are_listed) {
    Env e;
    e.add("FN:Anna Beispiel\r\nN:Beispiel;Anna;;;\r\n");
    const int64_t carla = e.add("FN:Carla Company\r\nN:Company;Carla;;;\r\n", e.other);
    PB_CHECK_EQ(e.store->contacts_count(""), 2);
    PB_CHECK(e.store->set_addressbook_shown(e.other, false).ok());
    PB_CHECK_EQ(e.store->contacts_count(""), 1);
    PB_CHECK((e.names() == std::vector<std::string>{"Anna Beispiel"}));
    PB_CHECK((e.names("carla").empty()));
    PB_CHECK_EQ(e.store->debug_query("SELECT count(*) FROM contact"), std::string("2"));   // hiding deletes nothing
    PB_CHECK(e.store->set_addressbook_shown(e.other, true).ok());
    PB_CHECK_EQ(e.store->contacts_count(""), 2);
    PB_CHECK(e.store->debug_exec("UPDATE contact SET sync_state = 'pending_delete', undo_state = 'synced' WHERE id = " + std::to_string(carla)).empty());
    PB_CHECK_EQ(e.store->contacts_count(""), 1);
    PB_CHECK(e.store->set_addressbook_shown(999, false).key == "not_found");
}

PB_TEST(a_list_item_carries_what_the_row_needs) {
    Env e;
    const int64_t id = e.add("FN:Anna Beispiel\r\nN:Beispiel;Anna;;;\r\nORG:Example Works\r\nEMAIL:anna@example.org\r\n");
    e.add("garbage that is not a card");   // not even a card: wrapped by the helper, so unreadable
    PB_CHECK(e.store->debug_exec("UPDATE contact SET sync_state = 'pending_update', error_key = 'refused' WHERE id = " + std::to_string(id)).empty());
    const auto items = e.store->contacts_page("beispiel", 0, 10);
    PB_CHECK_EQ(items.size(), size_t(1));
    PB_CHECK_EQ(items[0].id, id);
    PB_CHECK_EQ(items[0].display_name, std::string("Anna Beispiel"));
    PB_CHECK_EQ(items[0].subtitle, std::string("Example Works"));
    PB_CHECK_EQ(items[0].sync_state, std::string("pending_update"));
    PB_CHECK_EQ(items[0].error_key, std::string("refused"));
    PB_CHECK_EQ(items[0].addressbook_name, std::string("Contacts"));
    PB_CHECK_EQ(items[0].addressbook_id, e.book);
    PB_CHECK(!items[0].unreadable);
}

PB_TEST(the_detail_of_a_contact_gives_the_card_and_the_book) {
    Env e;
    const int64_t id = e.add("FN:Anna Beispiel\r\nN:Beispiel;Anna;;;\r\nTEL;TYPE=CELL:+1 555 0101\r\nEMAIL;TYPE=WORK:anna@example.org\r\nBDAY:1985-04-12\r\nX-CUSTOM:keep\r\n");
    const auto v = e.store->contact(id);
    PB_CHECK(v.has_value());
    PB_CHECK_EQ(v->display_name, std::string("Anna Beispiel"));
    PB_CHECK_EQ(v->addressbook_name, std::string("Contacts"));
    PB_CHECK(v->addressbook_writable);
    PB_CHECK(v->addressbook_present && v->addressbook_shown);
    PB_CHECK(v->partial);
    PB_CHECK(v->card.readable);
    PB_CHECK_EQ(v->card.phones.size(), size_t(1));
    PB_CHECK_EQ(v->card.phones[0].label, std::string("mobile"));
    PB_CHECK_EQ(v->card.emails[0].value, std::string("anna@example.org"));
    PB_CHECK(v->vcf.find("X-CUSTOM:keep") != std::string::npos);   // the unknown property is part of the stored card
    PB_CHECK(!e.store->contact(424242).has_value());
}

PB_TEST(address_books_keep_the_choice_of_the_user_when_the_server_refreshes_them) {
    Env e;
    PB_CHECK(e.store->set_addressbook_shown(e.book, false).ok());
    pbo::AddressBook again;
    again.href = "/dav/addressbooks/alice/contacts/";
    again.name = "Renamed";
    again.writable = false;
    auto id = e.store->upsert_addressbook(again);
    PB_CHECK(id.ok());
    PB_CHECK_EQ(id.value, e.book);
    for (const auto& b : e.store->addressbooks())
        if (b.id == e.book) {
            PB_CHECK_EQ(b.name, std::string("Renamed"));
            PB_CHECK(!b.writable);
            PB_CHECK(!b.shown);
        }
    auto names = e.store->addressbooks();
    PB_CHECK_EQ(names.size(), size_t(2));
    PB_CHECK(names[0].name <= names[1].name);   // in the order of their names
}

PB_TEST(a_book_that_is_gone_is_removed_with_its_synced_contacts_but_a_book_with_local_work_stays) {
    Env e;
    e.add("FN:Anna Beispiel\r\nN:Beispiel;Anna;;;\r\n");
    const int64_t carla = e.add("FN:Carla Company\r\nN:Company;Carla;;;\r\n", e.other);
    PB_CHECK(e.store->debug_exec("UPDATE contact SET sync_state = 'pending_update' WHERE id = " + std::to_string(carla)).empty());
    PB_CHECK(e.store->mark_addressbooks_present({"/dav/addressbooks/alice/contacts/"}).ok());
    PB_CHECK_EQ(e.store->addressbooks().size(), size_t(2));   // "Company" holds local work: it stays
    PB_CHECK_EQ(e.store->debug_query("SELECT present_on_server FROM addressbook WHERE id = " + std::to_string(e.other)), std::string("0"));
    PB_CHECK_EQ(e.store->debug_query("SELECT count(*) FROM contact"), std::string("2"));
    PB_CHECK(e.store->debug_exec("UPDATE contact SET sync_state = 'synced' WHERE id = " + std::to_string(carla)).empty());
    PB_CHECK(e.store->mark_addressbooks_present({"/dav/addressbooks/alice/contacts/"}).ok());
    PB_CHECK_EQ(e.store->addressbooks().size(), size_t(1));
    PB_CHECK_EQ(e.store->debug_query("SELECT count(*) FROM contact"), std::string("1"));
    PB_CHECK_EQ(e.store->debug_query("SELECT count(*) FROM contact_email"), std::string("0"));
}

PB_TEST(a_pull_never_overwrites_a_contact_with_local_work) {
    Env e;
    const int64_t id = e.add("FN:Anna Beispiel\r\nN:Beispiel;Anna;;;\r\n");
    PB_CHECK(e.store->debug_exec("UPDATE contact SET sync_state = 'pending_update', display_name = 'My Edit' WHERE id = " + std::to_string(id)).empty());
    const std::string href = e.store->debug_query("SELECT href FROM contact WHERE id = " + std::to_string(id));
    pbo::ContactPullBatch batch;
    batch.upserts.push_back(pbo::contact_upsert_from_card(href, "\"newer\"", "BEGIN:VCARD\r\nVERSION:3.0\r\nUID:uid-1\r\nFN:Server Version\r\nEND:VCARD\r\n"));
    PB_CHECK(e.store->apply_contact_pull(e.book, batch).ok());
    PB_CHECK_EQ(e.store->debug_query("SELECT display_name FROM contact WHERE id = " + std::to_string(id)), std::string("My Edit"));
    // a removal does not remove it either
    pbo::ContactPullBatch removal;
    removal.removed_hrefs.push_back(href);
    removal.full = true;
    PB_CHECK(e.store->apply_contact_pull(e.book, removal).ok());
    PB_CHECK_EQ(e.store->debug_query("SELECT count(*) FROM contact WHERE id = " + std::to_string(id)), std::string("1"));
}

PB_TEST_MAIN()
