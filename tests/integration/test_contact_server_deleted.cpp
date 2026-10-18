// Spec 002 FR-022: a contact with unsynced changes that was removed on the server (or whose whole address book was removed) is kept as the user left
// it, marked "deleted on the server"; the user re-creates it or discards it. UNVERIFIED against real servers.
#include <string>

#include "contact_env.h"
#include "pbo/contact_edit.h"
#include "test_support.h"

using pbtest::ContactEnv;
using pbtest::FakeCardDavServer;
using pbtest::contact_vcf;

PBO_FAKE_SERVER_TEST();

namespace {

struct Gone : ContactEnv {
    using ContactEnv::ContactEnv;
    pbo::ContactEditDeps edit_deps() { return pbo::ContactEditDeps{*store, clock, random}; }
    pbo::Error push() {
        auto d = deps();
        return pbo::push_contacts(d, "pw-for-tests", nullptr);
    }
    pbo::Error change(int64_t id, const std::string& family) {
        auto v = store->contact(id);
        pbo::ContactEdit e = pbo::contact_edit_of(v->card);
        e.family = family;
        auto d = edit_deps();
        return pbo::change_contact(d, id, e).error;
    }
};

}  // namespace

PB_TEST(a_404_on_the_look_before_an_update_keeps_the_edited_version_and_marks_it) {
    Gone e;
    const int64_t id = e.server_contact("a.vcf", "u-a", "Anna", "Alt", "TEL:+1 555 0101\r\n");
    PB_CHECK(e.change(id, "Lokal").ok());
    e.server->remove_card("contacts", "a.vcf");
    pbo::ContactsPushCounts counts;
    auto d = e.deps();
    PB_CHECK(pbo::push_contacts(d, "pw-for-tests", &counts).ok());
    PB_CHECK_EQ(counts.server_deleted, 1);
    PB_CHECK_EQ(e.state_of(id), std::string("server_deleted"));
    PB_CHECK_EQ(e.q("SELECT family FROM contact WHERE id = " + std::to_string(id)), std::string("Lokal"));   // the edit is kept
    PB_CHECK_EQ(e.server->count("PUT"), 0);                                                                   // and nothing was written
    PB_CHECK_EQ(e.server->card_count("contacts"), size_t(0));
    const auto items = e.store->contacts_page("", 0, 10);
    PB_CHECK_EQ(items.size(), size_t(1));   // still listed, marked
    PB_CHECK_EQ(items[0].sync_state, std::string("server_deleted"));
    const auto problems = e.store->contact_problems();
    PB_CHECK_EQ(problems.size(), size_t(1));
    PB_CHECK_EQ(problems[0].state, std::string("server_deleted"));
    // a later pull or push does not make it disappear
    PB_CHECK(e.pull().ok());
    PB_CHECK(e.push().ok());
    PB_CHECK_EQ(e.state_of(id), std::string("server_deleted"));
}

PB_TEST(a_pull_that_reports_the_card_removed_marks_a_contact_with_unsynced_changes) {
    Gone e;
    const int64_t id = e.server_contact("a.vcf", "u-a", "Anna", "Alt");
    const int64_t other = e.server_contact("b.vcf", "u-b", "Bert", "Bleibt");
    PB_CHECK(e.change(id, "Lokal").ok());
    e.server->remove_card("contacts", "a.vcf");
    PB_CHECK(e.pull().ok());
    PB_CHECK_EQ(e.state_of(id), std::string("server_deleted"));
    PB_CHECK_EQ(e.q("SELECT family FROM contact WHERE id = " + std::to_string(id)), std::string("Lokal"));
    PB_CHECK_EQ(e.state_of(other), std::string("synced"));
    // a synced contact the server removed simply goes
    e.server->remove_card("contacts", "b.vcf");
    PB_CHECK(e.pull().ok());
    PB_CHECK_EQ(e.count("SELECT count(*) FROM contact WHERE id = " + std::to_string(other)), 0);
}

PB_TEST(a_full_listing_without_the_card_does_the_same) {
    ContactEnv e(false, false);
    FakeCardDavServer::BookOptions plain;
    plain.name = "Plain";
    plain.sync_collection = false;
    e.server->add_book("plain", plain);
    e.server->put_card("plain", "a.vcf", contact_vcf("p-a", "Pia", "Alt"));
    PB_CHECK(e.pull().ok());
    const int64_t id = e.id_of_uid("p-a");
    PB_CHECK(e.store->debug_exec("UPDATE contact SET sync_state = 'pending_update' WHERE id = " + std::to_string(id)).empty());
    e.server->remove_card("plain", "a.vcf");
    e.server->put_card("plain", "b.vcf", contact_vcf("p-b", "Paul", "Neu"));
    PB_CHECK(e.pull().ok());
    PB_CHECK_EQ(e.state_of(id), std::string("server_deleted"));
}

PB_TEST(a_book_removed_on_the_server_keeps_the_contacts_with_unsynced_changes_marked) {
    Gone e(true);
    const int64_t company = e.book_id("Company");
    e.server->put_card("company", "c.vcf", contact_vcf("u-c", "Carla", "Firma"));
    e.server->put_card("company", "d.vcf", contact_vcf("u-d", "Dora", "Firma"));
    PB_CHECK(e.pull().ok());
    const int64_t c = e.id_of_uid("u-c"), d = e.id_of_uid("u-d");
    // Carla has an unsynced change (made while the book was still writable on the server's side)
    PB_CHECK(e.store->debug_exec("UPDATE addressbook SET writable = 1 WHERE id = " + std::to_string(company)).empty());
    PB_CHECK(e.change(c, "Lokal").ok());
    e.server->remove_book("company");
    PB_CHECK(e.pull().ok());
    PB_CHECK_EQ(e.q("SELECT present_on_server FROM addressbook WHERE id = " + std::to_string(company)), std::string("0"));   // kept, because of Carla
    PB_CHECK_EQ(e.state_of(c), std::string("server_deleted"));
    PB_CHECK_EQ(e.q("SELECT family FROM contact WHERE id = " + std::to_string(c)), std::string("Lokal"));
    PB_CHECK_EQ(e.count("SELECT count(*) FROM contact WHERE id = " + std::to_string(d)), 0);   // Dora was only synced: gone with the book
    // a push does not try to write into the removed book
    auto dd = e.deps();
    pbo::ContactsPushCounts counts;
    PB_CHECK(pbo::push_contacts(dd, "pw-for-tests", &counts).ok());
    PB_CHECK_EQ(e.server->count("PUT"), 0);
}

PB_TEST(re_creating_sends_the_kept_contact_as_a_new_card_under_a_fresh_path) {
    Gone e;
    const int64_t id = e.server_contact("a.vcf", "u-a", "Anna", "Alt", "TEL:+1 555 0101\r\n");
    PB_CHECK(e.change(id, "Lokal").ok());
    e.server->remove_card("contacts", "a.vcf");
    PB_CHECK(e.push().ok());
    const std::string old_href = e.q("SELECT href FROM contact WHERE id = " + std::to_string(id));
    auto dd = e.edit_deps();
    PB_CHECK(pbo::recreate_contact(dd, id).ok());
    PB_CHECK_EQ(e.state_of(id), std::string("pending_create"));
    PB_CHECK(e.q("SELECT href FROM contact WHERE id = " + std::to_string(id)) != old_href);
    PB_CHECK_EQ(e.count("SELECT etag IS NULL FROM contact WHERE id = " + std::to_string(id)), 1);
    PB_CHECK(e.push().ok());
    PB_CHECK_EQ(e.state_of(id), std::string("synced"));
    PB_CHECK_EQ(e.server->card_count("contacts"), size_t(1));
    const std::string name = e.q("SELECT href FROM contact WHERE id = " + std::to_string(id));
    const std::string vcf = e.server->card("contacts", name.substr(name.rfind('/') + 1)).vcf;
    PB_CHECK(vcf.find("N:Lokal;Anna;;;") != std::string::npos);
    PB_CHECK(vcf.find("+1 555 0101") != std::string::npos);
    PB_CHECK_EQ(pbo::recreate_contact(dd, id).key, std::string("not_server_deleted"));   // only a contact the server removed can be re-created
}

PB_TEST(discarding_removes_the_kept_contact_for_good) {
    Gone e;
    const int64_t id = e.server_contact("a.vcf", "u-a", "Anna", "Alt");
    PB_CHECK(e.change(id, "Lokal").ok());
    e.server->remove_card("contacts", "a.vcf");
    PB_CHECK(e.push().ok());
    PB_CHECK(pbo::discard_server_deleted_contact(*e.store, id).ok());
    PB_CHECK_EQ(e.count("SELECT count(*) FROM contact"), 0);
    PB_CHECK_EQ(e.count("SELECT count(*) FROM contact_email"), 0);
    PB_CHECK_EQ(pbo::discard_server_deleted_contact(*e.store, id).key, std::string("not_found"));
    const int64_t b = e.server_contact("b.vcf", "u-b", "Bert", "Bleibt");
    PB_CHECK_EQ(pbo::discard_server_deleted_contact(*e.store, b).key, std::string("not_server_deleted"));   // a normal contact is not discarded this way
    PB_CHECK_EQ(e.count("SELECT count(*) FROM contact"), 1);
}

PB_TEST(the_edited_version_of_a_contact_that_the_server_removed_cannot_be_changed_further_without_a_decision) {
    Gone e;
    const int64_t id = e.server_contact("a.vcf", "u-a", "Anna", "Alt");
    PB_CHECK(e.change(id, "Lokal").ok());
    e.server->remove_card("contacts", "a.vcf");
    PB_CHECK(e.push().ok());
    // it can still be edited (the marked version is the user's), and stays marked
    PB_CHECK(e.change(id, "Noch lokaler").ok());
    PB_CHECK_EQ(e.state_of(id), std::string("server_deleted"));
    PB_CHECK_EQ(e.q("SELECT family FROM contact WHERE id = " + std::to_string(id)), std::string("Noch lokaler"));
}

PB_TEST_MAIN()
