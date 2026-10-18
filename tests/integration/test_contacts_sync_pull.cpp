// T027 (spec 002): the pull of address books and contacts against the fake CardDAV server: full download, changes by token, removals, the ctag
// fallback, photos, unreadable and duplicate cards, the cursor, interrupted runs, hostile addresses and logs without contact data (FR-002,
// FR-004, FR-026, FR-027, SC-001, SC-005). UNVERIFIED against real servers.
#include <set>
#include <string>

#include "contact_env.h"
#include "fake_transport.h"
#include "pbo/log.h"
#include "test_support.h"

using pbtest::ContactEnv;
using pbtest::FakeCardDavServer;
using pbtest::contact_vcf;

PBO_FAKE_SERVER_TEST();

PB_TEST(a_first_pull_downloads_every_book_and_marks_the_read_only_one) {
    ContactEnv e(true, false);
    e.server->put_card("contacts", "a.vcf", contact_vcf("u-a", "Anna", "Beispiel"));
    e.server->put_card("contacts", "b.vcf", contact_vcf("u-b", "Bert", "Beispiel"));
    e.server->put_card("company", "c.vcf", contact_vcf("u-c", "Carla", "Company"));
    pbo::ContactsCounts counts;
    PB_CHECK(e.pull(&counts).ok());
    PB_CHECK_EQ(counts.books, 2);
    PB_CHECK_EQ(counts.changed, 3);
    PB_CHECK(!counts.no_addressbook);
    const auto books = e.store->addressbooks();
    PB_CHECK_EQ(books.size(), size_t(2));
    for (const auto& b : books) {
        PB_CHECK(b.shown && b.present_on_server);
        if (b.name == "Contacts") PB_CHECK(b.writable);
        if (b.name == "Company") PB_CHECK(!b.writable);
        PB_CHECK(!b.sync_token.empty());
        PB_CHECK(b.last_sync_at == pbtest::kContactNow);
    }
    PB_CHECK_EQ(e.count("SELECT count(*) FROM contact"), 3);
    PB_CHECK_EQ(e.count("SELECT count(*) FROM contact WHERE sync_state = 'synced' AND partial = 1 AND unreadable = 0"), 3);
    PB_CHECK_EQ(e.q("SELECT display_name FROM contact WHERE uid = 'u-a'"), std::string("Anna Beispiel"));
    PB_CHECK_EQ(e.count("SELECT count(*) FROM contact_email"), 0);
}

PB_TEST(a_later_pull_applies_changes_by_token_and_removals) {
    ContactEnv e;
    e.server->put_card("contacts", "a.vcf", contact_vcf("u-a", "Anna", "Beispiel"));
    e.server->put_card("contacts", "b.vcf", contact_vcf("u-b", "Bert", "Beispiel"));
    PB_CHECK(e.pull().ok());
    PB_CHECK_EQ(e.count("SELECT count(*) FROM contact"), 2);
    const std::string token = e.q("SELECT sync_token FROM addressbook");

    e.server->put_card("contacts", "a.vcf", contact_vcf("u-a", "Anna", "Beispiel", "EMAIL:anna@example.org\r\n"));   // changed
    e.server->put_card("contacts", "c.vcf", contact_vcf("u-c", "Carla", "Neu"));                                  // new
    e.server->remove_card("contacts", "b.vcf");                                                                    // removed
    pbo::ContactsCounts counts;
    PB_CHECK(e.pull(&counts).ok());
    PB_CHECK_EQ(counts.changed, 2);
    PB_CHECK_EQ(counts.removed, 1);
    PB_CHECK_EQ(e.count("SELECT count(*) FROM contact"), 2);
    PB_CHECK_EQ(e.count("SELECT count(*) FROM contact WHERE uid = 'u-b'"), 0);
    PB_CHECK_EQ(e.q("SELECT addr FROM contact_email WHERE contact_id = " + std::to_string(e.id_of_uid("u-a"))), std::string("anna@example.org"));
    PB_CHECK(e.q("SELECT sync_token FROM addressbook") != token);
    // the second pull asked only for the changed cards
    PB_CHECK((e.server->multiget_batch_sizes() == std::vector<size_t>{2, 2}));   // first pull: two cards; second pull: the two that changed
}

PB_TEST(a_pull_without_changes_fetches_nothing) {
    ContactEnv e;
    e.server->put_card("contacts", "a.vcf", contact_vcf("u-a", "Anna", "Beispiel"));
    PB_CHECK(e.pull().ok());
    const auto before = e.server->multiget_batch_sizes().size();
    PB_CHECK(e.pull().ok());
    PB_CHECK_EQ(e.server->multiget_batch_sizes().size(), before);
}

PB_TEST(the_ctag_fallback_gives_the_same_rows) {
    ContactEnv e(false, false);
    FakeCardDavServer::BookOptions plain;
    plain.name = "Plain";
    plain.sync_collection = false;
    e.server->add_book("plain", plain);
    e.server->put_card("plain", "a.vcf", contact_vcf("p-a", "Pia", "Plain"));
    e.server->put_card("plain", "b.vcf", contact_vcf("p-b", "Paul", "Plain"));
    e.server->put_card("contacts", "a.vcf", contact_vcf("u-a", "Anna", "Beispiel"));
    PB_CHECK(e.pull().ok());
    PB_CHECK_EQ(e.count("SELECT count(*) FROM contact"), 3);
    PB_CHECK(!e.q("SELECT ctag FROM addressbook WHERE name = 'Plain'").empty());
    PB_CHECK(e.q("SELECT sync_token FROM addressbook WHERE name = 'Plain'").empty());
    e.server->put_card("plain", "c.vcf", contact_vcf("p-c", "Pit", "Plain"));
    e.server->remove_card("plain", "a.vcf");
    PB_CHECK(e.pull().ok());
    PB_CHECK_EQ(e.count("SELECT count(*) FROM contact WHERE addressbook_id = (SELECT id FROM addressbook WHERE name = 'Plain')"), 2);
    PB_CHECK_EQ(e.count("SELECT count(*) FROM contact WHERE uid = 'p-a'"), 0);   // absent from the complete list: removed
    PB_CHECK_EQ(e.count("SELECT count(*) FROM contact WHERE uid = 'p-c'"), 1);
    PB_CHECK_EQ(e.count("SELECT count(*) FROM contact WHERE uid = 'u-a'"), 1);   // the other book is untouched
}

PB_TEST(photos_are_not_stored_whether_the_server_honours_the_property_list_or_not) {
    for (const bool honours : {true, false}) {
        ContactEnv e(false, false);
        e.server->set_honours_property_list(honours);
        const std::string photo = "PHOTO;ENCODING=b;TYPE=JPEG:" + std::string(3000, 'A') + "\r\nLOGO;ENCODING=b:" + std::string(500, 'B') + "\r\nBDAY:2000-01-31\r\n";
        e.server->put_card("contacts", "p.vcf", contact_vcf("u-p", "Pia", "Foto", photo));
        e.server->put_card("contacts", "n.vcf", contact_vcf("u-n", "Nina", "Normal"));
        PB_CHECK(e.pull().ok());
        const int64_t p = e.id_of_uid("u-p"), n = e.id_of_uid("u-n");
        PB_CHECK(e.q("SELECT vcf FROM contact WHERE id = " + std::to_string(p)).find("PHOTO") == std::string::npos);
        PB_CHECK(e.q("SELECT vcf FROM contact WHERE id = " + std::to_string(p)).find("LOGO") == std::string::npos);
        // what is not heavy is kept (the fake server only leaves out the heavy properties when it honours the list)
        PB_CHECK(e.q("SELECT vcf FROM contact WHERE id = " + std::to_string(p)).find("BDAY:2000-01-31") != std::string::npos);
        // every card from the server is partial: the request names a property list, so the app cannot know what the server left out
        PB_CHECK_EQ(e.q("SELECT partial FROM contact WHERE id = " + std::to_string(p)), std::string("1"));
        PB_CHECK_EQ(e.q("SELECT partial FROM contact WHERE id = " + std::to_string(n)), std::string("1"));
    }
}

PB_TEST(every_card_from_the_server_is_partial_because_the_request_names_a_property_list) {
    // a server that honours the list sends no PHOTO line at all, so the app cannot tell that a photo exists: every edit of a contact from the server
    // therefore reads the full card first and merges into it (data-model.md, research R-05)
    ContactEnv e;
    e.server->put_card("contacts", "p.vcf", contact_vcf("u-p", "Pia", "Foto"));
    PB_CHECK(e.pull().ok());
    PB_CHECK_EQ(e.q("SELECT partial FROM contact WHERE uid = 'u-p'"), std::string("1"));
}

PB_TEST(an_unreadable_card_is_stored_as_such_and_the_pull_continues) {
    ContactEnv e;
    e.server->put_card("contacts", "bad.vcf", "this is not a card at all\r\n");
    e.server->put_card("contacts", "trunc.vcf", "BEGIN:VCARD\r\nVERSION:3.0\r\nUID:u-t\r\nFN:Cut Off\r\n");
    e.server->put_card("contacts", "ok.vcf", contact_vcf("u-ok", "Okay", "Fine"));
    pbo::ContactsCounts counts;
    PB_CHECK(e.pull(&counts).ok());
    PB_CHECK_EQ(counts.unreadable, 2);
    PB_CHECK_EQ(e.count("SELECT count(*) FROM contact"), 3);
    PB_CHECK_EQ(e.count("SELECT count(*) FROM contact WHERE unreadable = 1"), 2);
    PB_CHECK_EQ(e.q("SELECT uid FROM contact WHERE href LIKE '%bad.vcf'"), "href:" + e.server->card_href("contacts", "bad.vcf"));
    PB_CHECK_EQ(e.count("SELECT count(*) FROM contact WHERE uid = 'u-ok' AND unreadable = 0"), 1);
    // the list shows them (as unreadable), nothing is dropped
    PB_CHECK_EQ(e.store->contacts_count(""), 3);
}

PB_TEST(a_duplicate_uid_in_one_book_keeps_the_first_card_and_stores_the_second_as_unreadable) {
    ContactEnv e;
    e.server->put_card("contacts", "a.vcf", contact_vcf("same-uid", "Anna", "Erste"));
    PB_CHECK(e.pull().ok());
    e.server->put_card("contacts", "b.vcf", contact_vcf("same-uid", "Bert", "Zweite"));
    PB_CHECK(e.pull().ok());
    PB_CHECK_EQ(e.count("SELECT count(*) FROM contact"), 2);
    PB_CHECK_EQ(e.q("SELECT display_name FROM contact WHERE uid = 'same-uid'"), std::string("Anna Erste"));
    PB_CHECK_EQ(e.q("SELECT unreadable FROM contact WHERE href LIKE '%b.vcf'"), std::string("1"));
    PB_CHECK_EQ(e.q("SELECT uid FROM contact WHERE href LIKE '%b.vcf'"), "href:" + e.server->card_href("contacts", "b.vcf"));
    // and a third pull does not make it worse or lose it
    PB_CHECK(e.pull().ok());
    PB_CHECK_EQ(e.count("SELECT count(*) FROM contact"), 2);
}

PB_TEST(a_pulled_card_for_an_existing_path_updates_that_row_and_never_adds_a_second_one) {
    ContactEnv e;
    const int64_t id = e.server_contact("a.vcf", "u-a", "Anna", "Alt");
    e.server->put_card("contacts", "a.vcf", contact_vcf("u-a", "Anna", "Neu"));
    PB_CHECK(e.pull().ok());
    PB_CHECK_EQ(e.count("SELECT count(*) FROM contact"), 1);
    PB_CHECK_EQ(e.id_of_href("contacts", "a.vcf"), id);
    PB_CHECK_EQ(e.q("SELECT family FROM contact WHERE id = " + std::to_string(id)), std::string("Neu"));
    // the same card pulled again changes nothing
    PB_CHECK(e.pull().ok());
    PB_CHECK_EQ(e.count("SELECT count(*) FROM contact"), 1);
}

PB_TEST(the_cursor_moves_only_with_the_cards_it_covers) {
    ContactEnv e;
    e.server->put_card("contacts", "a.vcf", contact_vcf("u-a", "Anna", "Beispiel"));
    e.server->put_card("contacts", "b.vcf", contact_vcf("u-b", "Bert", "Beispiel"));
    const std::string token = e.q("SELECT sync_token FROM addressbook");   // from the first pull of the environment (the book was empty)
    FakeCardDavServer::Fault f;   // the list of changes is answered, the download of the cards is refused
    f.method = "REPORT";
    f.body_contains = "addressbook-multiget";
    f.status = 503;
    e.server->inject(f);
    const pbo::Error err = e.pull();
    PB_CHECK(!err.ok());
    PB_CHECK_EQ(err.key, std::string("server_error"));
    PB_CHECK_EQ(e.count("SELECT count(*) FROM contact"), 0);
    PB_CHECK_EQ(e.q("SELECT sync_token FROM addressbook"), token);   // the cursor did not move: nothing is skipped by the next run
    PB_CHECK(e.pull().ok());
    PB_CHECK_EQ(e.count("SELECT count(*) FROM contact"), 2);
    PB_CHECK(e.q("SELECT sync_token FROM addressbook") != token);
}

PB_TEST(an_interrupted_pull_run_again_ends_like_an_uninterrupted_one) {
    auto fill = [](ContactEnv& e) {
        for (int i = 0; i < 70; ++i) {
            const std::string n = std::to_string(i);
            e.server->put_card("contacts", "c" + n + ".vcf", contact_vcf("u" + n, "Given" + n, "Family" + n, "EMAIL:c" + n + "@example.org\r\n"));
        }
    };
    auto dump = [](ContactEnv& e) {
        return e.q("SELECT group_concat(href || '|' || uid || '|' || display_name || '|' || etag, ';') FROM (SELECT * FROM contact ORDER BY href)");
    };
    ContactEnv whole;
    fill(whole);
    PB_CHECK(whole.pull().ok());

    ContactEnv cut;
    fill(cut);
    FakeCardDavServer::Fault drop;   // the connection dies while the cards are downloaded
    drop.kind = FakeCardDavServer::Fault::Kind::Drop;
    drop.method = "REPORT";
    drop.body_contains = "addressbook-multiget";
    drop.times = 1;
    cut.server->inject(drop);
    PB_CHECK(!cut.pull().ok());
    PB_CHECK(cut.pull().ok());
    PB_CHECK_EQ(cut.count("SELECT count(*) FROM contact"), 70);
    PB_CHECK_EQ(dump(cut), dump(whole));
    PB_CHECK_EQ(cut.count("SELECT count(*) FROM contact_email"), 70);
    PB_CHECK_EQ(cut.q("SELECT sync_token FROM addressbook"), whole.q("SELECT sync_token FROM addressbook"));
    PB_CHECK(cut.pull().ok());   // and once more changes nothing
    PB_CHECK_EQ(dump(cut), dump(whole));
}

PB_TEST(a_book_that_disappeared_on_the_server_goes_with_its_synced_contacts) {
    ContactEnv e(true);
    e.server->put_card("company", "c.vcf", contact_vcf("u-c", "Carla", "Company"));
    e.server->put_card("contacts", "a.vcf", contact_vcf("u-a", "Anna", "Beispiel"));
    PB_CHECK(e.pull().ok());
    PB_CHECK_EQ(e.count("SELECT count(*) FROM contact"), 2);
    e.server->remove_book("company");
    PB_CHECK(e.pull().ok());
    PB_CHECK_EQ(e.store->addressbooks().size(), size_t(1));
    PB_CHECK_EQ(e.count("SELECT count(*) FROM contact"), 1);
    PB_CHECK_EQ(e.count("SELECT count(*) FROM contact WHERE uid = 'u-c'"), 0);
}

PB_TEST(a_server_that_names_no_address_book_is_not_a_failure_and_nothing_stored_is_touched) {
    ContactEnv e;
    e.server->put_card("contacts", "a.vcf", contact_vcf("u-a", "Anna", "Beispiel"));
    PB_CHECK(e.pull().ok());
    e.server->remove_book("contacts");   // the home holds no book now
    pbo::ContactsCounts counts;
    PB_CHECK(e.pull(&counts).ok());
    PB_CHECK(counts.no_addressbook);
    PB_CHECK_EQ(e.count("SELECT count(*) FROM contact"), 1);   // an empty answer must not wipe what the reader holds
    PB_CHECK_EQ(e.store->addressbooks().size(), size_t(1));
}

PB_TEST(a_hidden_book_is_not_downloaded) {
    ContactEnv e(true, false);
    e.server->put_card("company", "c.vcf", contact_vcf("u-c", "Carla", "Company"));
    e.server->put_card("contacts", "a.vcf", contact_vcf("u-a", "Anna", "Beispiel"));
    PB_CHECK(e.pull().ok());
    const int64_t company = e.book_id("Company");
    PB_CHECK(e.store->set_addressbook_shown(company, false).ok());
    e.server->put_card("company", "d.vcf", contact_vcf("u-d", "Dora", "Company"));
    const int reports = e.server->count("REPORT", "company/");
    pbo::ContactsCounts counts;
    PB_CHECK(e.pull(&counts).ok());
    PB_CHECK_EQ(counts.books, 1);
    PB_CHECK_EQ(e.server->count("REPORT", "company/"), reports);   // nothing was asked of the hidden book
    PB_CHECK_EQ(e.count("SELECT count(*) FROM contact WHERE uid = 'u-d'"), 0);
    PB_CHECK_EQ(e.count("SELECT count(*) FROM contact WHERE uid = 'u-c'"), 1);   // what was stored stays (hiding deletes nothing)
    PB_CHECK(e.store->set_addressbook_shown(company, true).ok());
    PB_CHECK(e.pull().ok());
    PB_CHECK_EQ(e.count("SELECT count(*) FROM contact WHERE uid = 'u-d'"), 1);
}

PB_TEST(a_wrong_password_and_a_failing_server_leave_the_local_contacts_intact) {
    ContactEnv e;
    e.server->put_card("contacts", "a.vcf", contact_vcf("u-a", "Anna", "Beispiel"));
    PB_CHECK(e.pull().ok());
    auto d = e.deps();
    const pbo::Error bad = pbo::pull_contacts(d, "wrong", nullptr);
    PB_CHECK_EQ(bad.key, std::string("auth_failed"));
    PB_CHECK_EQ(e.count("SELECT count(*) FROM contact"), 1);
    FakeCardDavServer::Fault f;
    f.status = 500;
    f.method = "PROPFIND";
    f.times = 5;
    e.server->inject(f);
    PB_CHECK(!e.pull().ok());
    PB_CHECK_EQ(e.count("SELECT count(*) FROM contact"), 1);
}

PB_TEST(without_a_calendar_server_account_the_pull_does_nothing) {
    pbtest::TmpDir data;
    pbtest::ContactClock clock;
    auto opened = pbo::Store::open(data.path(), clock);
    PB_CHECK(opened.ok());
    pbtest::ContactNoCancel cancel;
    pbtest::FakeTransport http;
    pbo::ContactsSyncDeps d{*opened.value, http, clock, cancel};
    pbo::ContactsCounts counts;
    PB_CHECK(pbo::pull_contacts(d, "pw", &counts).ok());
    PB_CHECK_EQ(http.requests().size(), size_t(0));
    PB_CHECK_EQ(counts.books, 0);
}

PB_TEST(the_log_of_a_pull_holds_no_contact_data_and_no_server_address) {
    ContactEnv e;
    FakeCardDavServer::BookOptions secret;
    secret.name = "Kontakte-Geheim";
    e.server->add_book("geheim", secret);
    e.server->put_card("geheim", "z.vcf", contact_vcf("u-z", "Zacharias", "Geheimnis", "TEL:+49 555 0199\r\nEMAIL:zacharias@example.org\r\nNOTE:Streng vertraulich\r\n"));
    e.server->put_card("contacts", "bad.vcf", "garbage Geheimtext\r\n");
    std::vector<std::string> lines;
    pbo::log::set_sink([&](const std::string& line) { lines.push_back(line); });
    PB_CHECK(e.pull().ok());
    pbo::log::reset_sink();
    PB_CHECK(!lines.empty());
    for (const std::string& l : lines)
        for (const std::string& needle : std::vector<std::string>{"Zacharias", "Geheimnis", "0199", "zacharias@example.org", "vertraulich", "Kontakte-Geheim", "Geheimtext", "localhost",
                                                                   "alice", "z.vcf", "bad.vcf", std::to_string(e.server->port())})
            if (l.find(needle) != std::string::npos) PB_FAIL_AT("the log line \"" + l + "\" holds " + needle);
}

PB_TEST_MAIN()
