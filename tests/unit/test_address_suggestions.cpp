// T154 (part): address suggestions for the recipient fields come from cached mail and sent mail, are bounded and pruned.
#include <string>

#include "pbo/store.h"
#include "pbo/vcard.h"
#include "test_support.h"
#include "tmp_dir.h"

namespace {

struct Clock : pbo::Clock {
    int64_t t = 1791979200;
    int64_t now() const override { return t; }
};

}  // namespace

PB_TEST(addresses_of_pulled_mail_are_suggested_by_prefix_of_the_address_or_the_name_most_used_first) {
    pbtest::TmpDir dir;
    Clock clock;
    auto store = std::move(pbo::Store::open(dir.path(), clock).value);
    pbo::FolderListItem i;
    i.raw_name = i.name = "INBOX";
    store->upsert_folders({i});
    const int64_t f = store->folders()[0].id;
    pbo::PullBatch b;
    auto add = [&](uint32_t uid, const char* name, const char* addr) {
        pbo::PulledMessage m;
        m.uid = uid;
        m.from_name = name;
        m.from_addr = addr;
        m.date_utc = 1790000000 + uid;
        b.messages.push_back(m);
    };
    add(1, "Anna Berg", "anna@example.org");
    add(2, "Anna Berg", "anna@example.org");
    add(3, "Andreas", "andreas@example.org");
    add(4, "Bob", "bob@example.org");
    PB_CHECK(store->apply_pull(f, b).ok());
    const auto an = store->address_suggestions("an", 10);
    PB_CHECK_EQ(an.size(), size_t(2));
    PB_CHECK_EQ(an[0].addr, std::string("anna@example.org"));         // seen twice
    PB_CHECK_EQ(an[0].name, std::string("Anna Berg"));
    PB_CHECK_EQ(store->address_suggestions("BER", 10).size(), size_t(0));   // a prefix of the name or the address, not a part
    PB_CHECK_EQ(store->address_suggestions("anna b", 10).size(), size_t(1));
    PB_CHECK_EQ(store->address_suggestions("", 10).size(), size_t(0));
    PB_CHECK_EQ(store->address_suggestions("an", 1).size(), size_t(1));
}

PB_TEST(sent_mail_counts_more_and_wildcards_in_the_prefix_are_literal) {
    pbtest::TmpDir dir;
    Clock clock;
    auto store = std::move(pbo::Store::open(dir.path(), clock).value);
    PB_CHECK(store->note_address("carl@example.org", "Carl", 1).ok());
    PB_CHECK(store->note_address("carla@example.org", "", 5).ok());
    const auto r = store->address_suggestions("carl", 10);
    PB_CHECK_EQ(r.size(), size_t(2));
    PB_CHECK_EQ(r[0].addr, std::string("carla@example.org"));
    PB_CHECK_EQ(store->address_suggestions("%", 10).size(), size_t(0));
    PB_CHECK_EQ(store->address_suggestions("c_rl", 10).size(), size_t(0));
    PB_CHECK_EQ(store->note_address("not an address", "x").key, std::string("invalid_address"));
}

PB_TEST(the_table_is_bounded_to_2000_rows_and_the_oldest_least_used_go_first) {
    pbtest::TmpDir dir;
    Clock clock;
    auto store = std::move(pbo::Store::open(dir.path(), clock).value);
    for (int i = 0; i < 2100; ++i) {
        clock.t += 1;
        PB_CHECK(store->note_address("u" + std::to_string(i) + "@example.org", "", 1).ok());
    }
    PB_CHECK(store->note_address("keep@example.org", "", 50).ok());       // heavily used: stays
    PB_CHECK_EQ(store->debug_query("SELECT count(*) FROM address_suggestion"), std::string("2000"));
    PB_CHECK_EQ(store->address_suggestions("keep", 5).size(), size_t(1));
    PB_CHECK_EQ(store->address_suggestions("u0@", 5).size(), size_t(0));   // the oldest of the weakly used went
    PB_CHECK_EQ(store->address_suggestions("u2099@", 5).size(), size_t(1));
}

// ---- contacts first (spec 002 US6) --------------------------------------------------------------------------------------------------------------

namespace {

struct Book {
    pbo::Store& store;
    int64_t id = 0;
    int n = 0;
    explicit Book(pbo::Store& s, const std::string& name = "Contacts", bool shown = true) : store(s) {
        pbo::AddressBook b;
        b.href = "/dav/ab/" + name + "/";
        b.name = name;
        b.writable = true;
        id = store.upsert_addressbook(b).value;
        if (!shown) PB_CHECK(store.set_addressbook_shown(id, false).ok());
    }
    // a contact with the given lines (FN, N, EMAIL ...)
    int64_t add(const std::string& lines) {
        const std::string uid = "u" + std::to_string(++n);
        pbo::ContactPullBatch batch;
        batch.upserts.push_back(pbo::contact_upsert_from_card("/dav/ab/" + std::to_string(id) + "/" + uid + ".vcf", "\"e\"", "BEGIN:VCARD\r\nVERSION:3.0\r\nUID:" + uid + "\r\n" + lines + "END:VCARD\r\n"));
        PB_CHECK(store.apply_contact_pull(id, batch).ok());
        return std::atoll(store.debug_query("SELECT id FROM contact WHERE uid = '" + uid + "' AND addressbook_id = " + std::to_string(id)).c_str());
    }
};

}  // namespace

PB_TEST(contact_addresses_come_first_then_mail_addresses_and_an_address_in_both_appears_once_as_the_contact) {
    pbtest::TmpDir dir;
    Clock clock;
    auto store = std::move(pbo::Store::open(dir.path(), clock).value);
    PB_CHECK(store->note_address("anna@example.org", "A. Berg", 9).ok());          // in the mail cache, used a lot
    PB_CHECK(store->note_address("andreas@example.org", "Andreas", 5).ok());
    Book book(*store);
    book.add("FN:Anna Berg\r\nN:Berg;Anna;;;\r\nEMAIL;TYPE=WORK:Anna@Example.org\r\n");
    book.add("FN:Anton Zed\r\nN:Zed;Anton;;;\r\nEMAIL;TYPE=HOME:anton@example.org\r\n");
    const auto r = store->address_suggestions("an", 10);
    PB_CHECK_EQ(r.size(), size_t(3));
    PB_CHECK(r[0].from_contact && r[1].from_contact);
    PB_CHECK(!r[2].from_contact);
    PB_CHECK_EQ(r[0].name, std::string("Anna Berg"));          // contacts by name
    PB_CHECK_EQ(r[0].addr, std::string("Anna@Example.org"));   // the address as the contact holds it
    PB_CHECK_EQ(r[0].label, std::string("work"));
    PB_CHECK_EQ(r[1].addr, std::string("anton@example.org"));
    PB_CHECK_EQ(r[1].label, std::string("home"));
    PB_CHECK_EQ(r[2].addr, std::string("andreas@example.org"));   // the mail address that is not a contact
    PB_CHECK_EQ(r[2].label, std::string());
    int annas = 0;
    for (const auto& s : r)
        if (s.addr.size() >= 4 && (s.addr[0] == 'a' || s.addr[0] == 'A') && s.addr.find("nna@") == 1) ++annas;
    PB_CHECK_EQ(annas, 1);                                      // once, not twice
}

PB_TEST(a_contact_is_found_by_the_start_of_any_word_of_its_name_or_organization_and_by_its_address) {
    pbtest::TmpDir dir;
    Clock clock;
    auto store = std::move(pbo::Store::open(dir.path(), clock).value);
    Book book(*store);
    book.add("FN:Anna Berg\r\nN:Berg;Anna;;;\r\nORG:Example Works\r\nEMAIL:a.berg@example.org\r\n");
    PB_CHECK_EQ(store->address_suggestions("ber", 10).size(), size_t(1));       // the family name
    PB_CHECK_EQ(store->address_suggestions("works", 10).size(), size_t(1));     // the organization
    PB_CHECK_EQ(store->address_suggestions("anna b", 10).size(), size_t(1));    // several words
    PB_CHECK_EQ(store->address_suggestions("a.be", 10).size(), size_t(1));      // the address
    PB_CHECK_EQ(store->address_suggestions("erg", 10).size(), size_t(0));       // the start of a word, not a part
    PB_CHECK_EQ(store->address_suggestions("%", 10).size(), size_t(0));
    PB_CHECK_EQ(store->address_suggestions("b_r", 10).size(), size_t(0));      // wildcards are ordinary characters
}

PB_TEST(a_contact_with_several_addresses_gives_one_entry_per_address_with_its_label) {
    pbtest::TmpDir dir;
    Clock clock;
    auto store = std::move(pbo::Store::open(dir.path(), clock).value);
    Book book(*store);
    book.add("FN:Clara Wolf\r\nN:Wolf;Clara;;;\r\nEMAIL;TYPE=WORK:clara@work.example.org\r\nEMAIL;TYPE=HOME:clara@home.example.org\r\nitem1.EMAIL:clara@other.example.org\r\nitem1.X-ABLabel:Club\r\n");
    const auto r = store->address_suggestions("clara", 10);
    PB_CHECK_EQ(r.size(), size_t(3));
    PB_CHECK_EQ(r[0].label, std::string("work"));
    PB_CHECK_EQ(r[1].label, std::string("home"));
    PB_CHECK_EQ(r[2].label, std::string("Club"));
    for (const auto& s : r) PB_CHECK_EQ(s.name, std::string("Clara Wolf"));
}

PB_TEST(contacts_of_a_hidden_book_and_deleted_contacts_are_not_suggested) {
    pbtest::TmpDir dir;
    Clock clock;
    auto store = std::move(pbo::Store::open(dir.path(), clock).value);
    Book shown(*store, "Shown");
    Book hidden(*store, "Hidden", false);
    shown.add("FN:Dora Shown\r\nN:Shown;Dora;;;\r\nEMAIL:dora@example.org\r\n");
    hidden.add("FN:Dirk Hidden\r\nN:Hidden;Dirk;;;\r\nEMAIL:dirk@example.org\r\n");
    const int64_t gone = shown.add("FN:Dana Gone\r\nN:Gone;Dana;;;\r\nEMAIL:dana@example.org\r\n");
    PB_CHECK(store->debug_exec("UPDATE contact SET sync_state = 'pending_delete', undo_state = 'synced' WHERE id = " + std::to_string(gone)).empty());
    const auto r = store->address_suggestions("d", 10);
    PB_CHECK_EQ(r.size(), size_t(1));
    PB_CHECK_EQ(r[0].addr, std::string("dora@example.org"));
    PB_CHECK(store->set_addressbook_shown(hidden.id, true).ok());
    PB_CHECK_EQ(store->address_suggestions("d", 10).size(), size_t(2));
    PB_CHECK(store->debug_exec("UPDATE contact SET sync_state = 'synced' WHERE id = " + std::to_string(gone)).empty());
    PB_CHECK_EQ(store->address_suggestions("d", 10).size(), size_t(3));   // (undone: it is back)
}

PB_TEST(umlauts_fold_and_the_bound_holds) {
    pbtest::TmpDir dir;
    Clock clock;
    auto store = std::move(pbo::Store::open(dir.path(), clock).value);
    Book book(*store);
    book.add("FN:Jürgen Müller\r\nN:Müller;Jürgen;;;\r\nEMAIL:j.mueller@example.org\r\n");
    PB_CHECK_EQ(store->address_suggestions("mü", 10).size(), size_t(1));
    PB_CHECK_EQ(store->address_suggestions("mu", 10).size(), size_t(1));
    PB_CHECK_EQ(store->address_suggestions("JÜR", 10).size(), size_t(1));
    for (int i = 0; i < 12; ++i) book.add("FN:Zora Z" + std::to_string(i) + "\r\nN:Z" + std::to_string(i) + ";Zora;;;\r\nEMAIL:zora" + std::to_string(i) + "@example.org\r\n");
    PB_CHECK_EQ(store->address_suggestions("zora", 8).size(), size_t(8));
    PB_CHECK_EQ(store->address_suggestions("zora", 3).size(), size_t(3));
}

PB_TEST_MAIN()
