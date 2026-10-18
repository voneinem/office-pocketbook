// Spec 002 FR-024, FR-017: a contact the server does not take stays on the reader, marked failed with the reason, and can be retried, edited or
// discarded; a read-only address book refuses at once. UNVERIFIED against real servers.
#include <string>

#include "contact_env.h"
#include "pbo/contact_edit.h"
#include "test_support.h"

using pbtest::ContactEnv;
using pbtest::FakeCardDavServer;

PBO_FAKE_SERVER_TEST();

namespace {

pbo::LabeledValue lv(const std::string& label, const std::string& value) {
    pbo::LabeledValue v;
    v.label = label;
    v.value = value;
    return v;
}

pbo::ContactEdit anna() {
    pbo::ContactEdit e;
    e.given = "Anna";
    e.family = "Beispiel";
    e.phones = {lv("mobile", "+1 555 0101")};
    return e;
}

struct Failed : ContactEnv {
    using ContactEnv::ContactEnv;
    pbo::ContactEditDeps edit_deps() { return pbo::ContactEditDeps{*store, clock, random}; }
    pbo::Error push() {
        auto d = deps();
        return pbo::push_contacts(d, "pw-for-tests", nullptr);
    }
    int64_t create(int64_t book = 0) {
        auto d = edit_deps();
        auto r = pbo::create_contact(d, book ? book : main_book, anna());
        PB_CHECK(r.ok());
        return r.value;
    }
    void refuse_next_put(int status) {
        FakeCardDavServer::Fault f;
        f.method = "PUT";
        f.status = status;
        server->inject(f);
    }
};

}  // namespace

PB_TEST(a_refused_create_stays_with_the_reason_and_nothing_is_lost) {
    struct Row { int status; const char* key; };
    for (const Row& row : {Row{403, "refused"}, Row{413, "too_large"}, Row{507, "storage_full_server"}}) {
        Failed e;
        const int64_t id = e.create();
        e.refuse_next_put(row.status);
        const pbo::Error err = e.push();
        PB_CHECK_EQ(err.key, std::string(row.key));   // the first problem is reported, so the status line says why
        PB_CHECK_EQ(e.state_of(id), std::string("pending_create"));
        PB_CHECK_EQ(e.q("SELECT error_key FROM contact WHERE id = " + std::to_string(id)), std::string(row.key));
        PB_CHECK_EQ(e.server->card_count("contacts"), size_t(0));
        const auto items = e.store->contacts_page("", 0, 10);
        PB_CHECK_EQ(items.size(), size_t(1));                          // still listed
        PB_CHECK_EQ(items[0].error_key, std::string(row.key));
        PB_CHECK_EQ(items[0].display_name, std::string("Anna Beispiel"));
        PB_CHECK(e.q("SELECT vcf FROM contact WHERE id = " + std::to_string(id)).find("FN:Anna Beispiel") != std::string::npos);
    }
}

PB_TEST(retry_clears_the_mark_and_the_next_push_sends_the_contact_again) {
    Failed e;
    const int64_t id = e.create();
    e.refuse_next_put(507);
    PB_CHECK(!e.push().ok());
    PB_CHECK(pbo::retry_contact(*e.store, id).ok());
    PB_CHECK_EQ(e.q("SELECT error_key FROM contact WHERE id = " + std::to_string(id)), std::string());
    PB_CHECK_EQ(e.state_of(id), std::string("pending_create"));
    PB_CHECK(e.push().ok());
    PB_CHECK_EQ(e.state_of(id), std::string("synced"));
    PB_CHECK_EQ(e.server->card_count("contacts"), size_t(1));
}

PB_TEST(a_failed_contact_is_sent_again_by_the_next_sync_even_without_a_retry) {
    // the refusal may have been temporary (the server had no space): the mark stays visible, and the next push tries again
    Failed e;
    const int64_t id = e.create();
    e.refuse_next_put(507);
    PB_CHECK(!e.push().ok());
    PB_CHECK(e.push().ok());
    PB_CHECK_EQ(e.state_of(id), std::string("synced"));
    PB_CHECK_EQ(e.q("SELECT error_key FROM contact WHERE id = " + std::to_string(id)), std::string());
}

PB_TEST(discarding_a_failed_new_contact_removes_it) {
    Failed e;
    const int64_t id = e.create();
    e.refuse_next_put(403);
    PB_CHECK(!e.push().ok());
    PB_CHECK(pbo::discard_contact_change(*e.store, id).ok());
    PB_CHECK_EQ(e.count("SELECT count(*) FROM contact"), 0);
    PB_CHECK_EQ(e.count("SELECT count(*) FROM contact_email"), 0);
    PB_CHECK_EQ(e.store->pending_contacts().size(), size_t(0));
    PB_CHECK(e.push().ok());
    PB_CHECK_EQ(e.server->count("PUT"), 1);   // nothing more was sent
    PB_CHECK_EQ(pbo::discard_contact_change(*e.store, 4242).key, std::string("not_found"));
}

PB_TEST(a_read_only_book_refuses_a_new_contact_at_once_and_stores_nothing) {
    Failed e(true);
    auto d = e.edit_deps();
    const int64_t company = e.book_id("Company");
    PB_CHECK(company != 0);
    auto r = pbo::create_contact(d, company, anna());
    PB_CHECK(!r.ok());
    PB_CHECK_EQ(r.error.key, std::string("read_only"));
    PB_CHECK_EQ(e.count("SELECT count(*) FROM contact"), 0);
    PB_CHECK_EQ(e.server->count("PUT"), 0);
    // an unknown book
    PB_CHECK_EQ(pbo::create_contact(d, 4242, anna()).error.key, std::string("not_found"));
}

PB_TEST(an_invalid_contact_is_refused_before_anything_is_stored) {
    Failed e;
    auto d = e.edit_deps();
    auto empty = pbo::create_contact(d, e.main_book, pbo::ContactEdit());
    PB_CHECK(!empty.ok());
    PB_CHECK_EQ(empty.error.key, std::string("empty"));
    pbo::ContactEdit bad = anna();
    bad.emails = {lv("work", "no-at-sign")};
    auto r = pbo::create_contact(d, e.main_book, bad);
    PB_CHECK(!r.ok());
    PB_CHECK_EQ(r.error.key, std::string("email_invalid"));
    PB_CHECK_EQ(r.error.detail, std::string("emails[0]"));   // the field to correct
    PB_CHECK_EQ(e.count("SELECT count(*) FROM contact"), 0);
}

PB_TEST(a_contact_whose_book_is_gone_cannot_be_created_in_it) {
    Failed e;
    PB_CHECK(e.store->mark_addressbooks_present({}).ok());   // the book held nothing with local work: it is removed
    auto d = e.edit_deps();
    PB_CHECK_EQ(pbo::create_contact(d, e.main_book, anna()).error.key, std::string("not_found"));
}

PB_TEST(the_block_reason_names_why_a_contact_cannot_be_changed) {
    Failed e(true);
    e.server->put_card("contacts", "a.vcf", pbtest::contact_vcf("u-a", "Anna", "Alt"));
    e.server->put_card("company", "c.vcf", pbtest::contact_vcf("u-c", "Carla", "Firma"));
    PB_CHECK(e.pull().ok());
    auto reason = [&](int64_t id) { return pbo::contact_edit_block_reason(*e.store->contact(id)); };
    const int64_t a = e.id_of_uid("u-a"), c = e.id_of_uid("u-c");
    PB_CHECK_EQ(reason(a), std::string());
    PB_CHECK_EQ(reason(c), std::string("read_only"));
    PB_CHECK(e.store->debug_exec("UPDATE contact SET unreadable = 1 WHERE id = " + std::to_string(a)).empty());
    PB_CHECK_EQ(reason(a), std::string("unreadable"));
    PB_CHECK(e.store->debug_exec("UPDATE contact SET unreadable = 0, sync_state = 'conflict' WHERE id = " + std::to_string(a)).empty());
    PB_CHECK_EQ(reason(a), std::string("in_conflict"));
    PB_CHECK(e.store->debug_exec("UPDATE contact SET sync_state = 'pending_delete' WHERE id = " + std::to_string(a)).empty());
    PB_CHECK_EQ(reason(a), std::string("delete_pending"));
    PB_CHECK(e.store->debug_exec("UPDATE contact SET sync_state = 'synced' WHERE id = " + std::to_string(a)).empty());
    PB_CHECK(e.store->debug_exec("UPDATE addressbook SET present_on_server = 0 WHERE name = 'Contacts'").empty());
    PB_CHECK_EQ(reason(a), std::string("book_gone"));
}

// ---- a failed change of a contact that exists (US3) ------------------------------------------------------------------------------------------

namespace {

pbo::Error change_family(Failed& e, int64_t id, const std::string& family) {
    auto v = e.store->contact(id);
    pbo::ContactEdit edit = pbo::contact_edit_of(v->card);
    edit.family = family;
    auto d = e.edit_deps();
    return pbo::change_contact(d, id, edit).error;
}

}  // namespace

PB_TEST(a_refused_update_stays_with_the_reason_and_can_be_retried) {
    Failed e;
    const int64_t id = e.server_contact("a.vcf", "u-a", "Anna", "Alt");
    PB_CHECK(change_family(e, id, "Neu").ok());
    e.refuse_next_put(403);
    PB_CHECK_EQ(e.push().key, std::string("refused"));
    PB_CHECK_EQ(e.state_of(id), std::string("pending_update"));
    PB_CHECK_EQ(e.q("SELECT error_key FROM contact WHERE id = " + std::to_string(id)), std::string("refused"));
    PB_CHECK_EQ(e.q("SELECT family FROM contact WHERE id = " + std::to_string(id)), std::string("Neu"));
    PB_CHECK(pbo::retry_contact(*e.store, id).ok());
    PB_CHECK(e.push().ok());
    PB_CHECK_EQ(e.state_of(id), std::string("synced"));
    PB_CHECK(e.server->card("contacts", "a.vcf").vcf.find("N:Neu;Anna;;;") != std::string::npos);
}

PB_TEST(editing_a_failed_contact_clears_the_mark_so_the_new_version_is_sent) {
    Failed e;
    const int64_t id = e.server_contact("a.vcf", "u-a", "Anna", "Alt");
    PB_CHECK(change_family(e, id, "Neu").ok());
    e.refuse_next_put(413);
    PB_CHECK(!e.push().ok());
    PB_CHECK_EQ(e.q("SELECT error_key FROM contact WHERE id = " + std::to_string(id)), std::string("too_large"));
    PB_CHECK(change_family(e, id, "Kleiner").ok());
    PB_CHECK_EQ(e.q("SELECT error_key FROM contact WHERE id = " + std::to_string(id)), std::string());
    PB_CHECK(e.push().ok());
    PB_CHECK_EQ(e.state_of(id), std::string("synced"));
}

PB_TEST(discarding_a_failed_update_brings_back_the_servers_version_at_the_next_pull) {
    Failed e;
    const int64_t id = e.server_contact("a.vcf", "u-a", "Anna", "Alt", "BDAY:1985-04-12\r\n");
    PB_CHECK(change_family(e, id, "Neu").ok());
    e.refuse_next_put(507);
    PB_CHECK(!e.push().ok());
    PB_CHECK(pbo::discard_contact_change(*e.store, id).ok());
    PB_CHECK_EQ(e.state_of(id), std::string("synced"));
    PB_CHECK_EQ(e.count("SELECT refetch FROM contact WHERE id = " + std::to_string(id)), 1);
    PB_CHECK_EQ(e.q("SELECT error_key FROM contact WHERE id = " + std::to_string(id)), std::string());
    PB_CHECK_EQ(e.store->pending_contacts().size(), size_t(0));
    PB_CHECK(e.pull().ok());   // the card is downloaded again whatever the cursor says
    PB_CHECK_EQ(e.q("SELECT family FROM contact WHERE id = " + std::to_string(id)), std::string("Alt"));
    PB_CHECK_EQ(e.count("SELECT refetch FROM contact WHERE id = " + std::to_string(id)), 0);
    PB_CHECK_EQ(e.server->count("PUT"), 1);   // the refused one; nothing was sent after the discard
}

PB_TEST(a_read_only_book_refuses_a_change_at_once) {
    Failed e(true);
    e.server->put_card("company", "c.vcf", pbtest::contact_vcf("u-c", "Carla", "Firma"));
    PB_CHECK(e.pull().ok());
    const int64_t id = e.id_of_uid("u-c");
    PB_CHECK_EQ(change_family(e, id, "Neu").key, std::string("read_only"));
    PB_CHECK_EQ(e.state_of(id), std::string("synced"));
    PB_CHECK_EQ(e.q("SELECT family FROM contact WHERE id = " + std::to_string(id)), std::string("Firma"));
    PB_CHECK_EQ(e.server->count("PUT"), 0);
}

PB_TEST(an_edit_that_changes_nothing_writes_nothing) {
    Failed e;
    const int64_t id = e.server_contact("a.vcf", "u-a", "Anna", "Alt");
    auto v = e.store->contact(id);
    const pbo::ContactEdit same = pbo::contact_edit_of(v->card);
    auto d = e.edit_deps();
    PB_CHECK(pbo::change_contact(d, id, same).ok());
    PB_CHECK_EQ(e.state_of(id), std::string("synced"));
    PB_CHECK_EQ(e.store->pending_contacts().size(), size_t(0));
}

PB_TEST(an_invalid_change_is_refused_and_the_contact_stays_as_it_was) {
    Failed e;
    const int64_t id = e.server_contact("a.vcf", "u-a", "Anna", "Alt");
    auto v = e.store->contact(id);
    pbo::ContactEdit bad = pbo::contact_edit_of(v->card);
    bad.emails.push_back(lv("work", "kaputt"));
    auto d = e.edit_deps();
    auto r = pbo::change_contact(d, id, bad);
    PB_CHECK(!r.ok());
    PB_CHECK_EQ(r.error.key, std::string("email_invalid"));
    PB_CHECK_EQ(r.error.detail, std::string("emails[0]"));
    PB_CHECK_EQ(e.state_of(id), std::string("synced"));
    pbo::ContactEdit nothing;
    r = pbo::change_contact(d, id, nothing);
    PB_CHECK_EQ(r.error.key, std::string("empty"));
    PB_CHECK_EQ(pbo::change_contact(d, 4242, bad).error.key, std::string("not_found"));
}

PB_TEST_MAIN()
