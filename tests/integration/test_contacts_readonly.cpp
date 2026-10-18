// Spec 002 FR-017: in a read-only address book creating, changing and deleting are refused with a reason, without touching the store or the network;
// the contact can still be read. UNVERIFIED against real servers.
#include <string>

#include "contact_env.h"
#include "pbo/contact_edit.h"
#include "test_support.h"

using pbtest::ContactEnv;

PBO_FAKE_SERVER_TEST();

namespace {

struct Ro : ContactEnv {
    using ContactEnv::ContactEnv;
    pbo::ContactEditDeps edit_deps() { return pbo::ContactEditDeps{*store, clock, random}; }
};

pbo::ContactEdit anna() {
    pbo::ContactEdit e;
    e.given = "Anna";
    e.family = "Beispiel";
    return e;
}

}  // namespace

PB_TEST(a_read_only_book_refuses_create_and_change_with_the_reason_and_stays_untouched) {
    Ro e(true);
    e.server->put_card("company", "c.vcf", pbtest::contact_vcf("u-c", "Carla", "Firma"));
    PB_CHECK(e.pull().ok());
    const int64_t company = e.book_id("Company");
    const int64_t id = e.id_of_uid("u-c");
    auto d = e.edit_deps();
    const std::string before = e.q("SELECT group_concat(id || vcf || sync_state, '|') FROM contact");

    auto created = pbo::create_contact(d, company, anna());
    PB_CHECK(!created.ok());
    PB_CHECK_EQ(created.error.key, std::string("read_only"));

    pbo::ContactEdit edit = pbo::contact_edit_of(e.store->contact(id)->card);
    edit.family = "Neu";
    auto changed = pbo::change_contact(d, id, edit);
    PB_CHECK(!changed.ok());
    PB_CHECK_EQ(changed.error.key, std::string("read_only"));

    PB_CHECK_EQ(e.q("SELECT group_concat(id || vcf || sync_state, '|') FROM contact"), before);   // nothing changed
    PB_CHECK_EQ(e.store->pending_contacts().size(), size_t(0));
    PB_CHECK_EQ(e.server->count("PUT"), 0);
    PB_CHECK_EQ(pbo::contact_edit_block_reason(*e.store->contact(id)), std::string("read_only"));
}

PB_TEST(the_contact_of_a_read_only_book_can_be_read_in_full) {
    Ro e(true);
    e.server->put_card("company", "c.vcf", pbtest::contact_vcf("u-c", "Carla", "Firma", "TEL;TYPE=WORK:+1 555 0100\r\nEMAIL:carla@example.org\r\n"));
    PB_CHECK(e.pull().ok());
    const auto v = e.store->contact(e.id_of_uid("u-c"));
    PB_CHECK(v.has_value());
    PB_CHECK(!v->addressbook_writable);
    PB_CHECK_EQ(v->card.phones.size(), size_t(1));
    PB_CHECK_EQ(v->card.emails[0].value, std::string("carla@example.org"));
    PB_CHECK_EQ(e.store->contacts_count(""), 1);
}

PB_TEST(a_book_that_turns_read_only_on_the_server_refuses_the_waiting_contact_with_the_reason) {
    Ro e;
    auto d = e.edit_deps();
    auto created = pbo::create_contact(d, e.main_book, anna());
    PB_CHECK(created.ok());
    PB_CHECK(e.store->debug_exec("UPDATE addressbook SET writable = 0 WHERE id = " + std::to_string(e.main_book)).empty());   // the next discovery says so
    auto dd = e.deps();
    PB_CHECK_EQ(pbo::push_contacts(dd, "pw-for-tests", nullptr).key, std::string("read_only"));
    PB_CHECK_EQ(e.q("SELECT error_key FROM contact WHERE id = " + std::to_string(created.value)), std::string("read_only"));
    PB_CHECK_EQ(e.state_of(created.value), std::string("pending_create"));   // kept
    PB_CHECK_EQ(e.server->count("PUT"), 0);
}

PB_TEST(a_read_only_book_refuses_a_deletion_with_the_reason_and_stays_untouched) {
    Ro e(true);
    e.server->put_card("company", "c.vcf", pbtest::contact_vcf("u-c", "Carla", "Firma"));
    PB_CHECK(e.pull().ok());
    const int64_t id = e.id_of_uid("u-c");
    const std::string before = e.q("SELECT group_concat(id || vcf || sync_state, '|') FROM contact");
    PB_CHECK_EQ(pbo::delete_contact(*e.store, id).key, std::string("read_only"));
    PB_CHECK_EQ(e.q("SELECT group_concat(id || vcf || sync_state, '|') FROM contact"), before);
    PB_CHECK_EQ(e.store->pending_contacts().size(), size_t(0));
    PB_CHECK_EQ(e.server->count("DELETE"), 0);
}

PB_TEST_MAIN()
