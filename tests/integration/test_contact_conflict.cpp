// Spec 002 FR-020, FR-021, SC-006: a contact changed on the reader and on the server keeps BOTH versions, and the user decides; a contact changed on
// the server is never deleted or overwritten by an older local version. UNVERIFIED against real servers.
#include <string>

#include "contact_env.h"
#include "pbo/contact_edit.h"
#include "test_support.h"

using pbtest::ContactEnv;
using pbtest::FakeCardDavServer;
using pbtest::contact_vcf;

PBO_FAKE_SERVER_TEST();

namespace {

struct Conflict : ContactEnv {
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
    pbo::Error resolve(int64_t id, pbo::ContactChoice choice) {
        auto d = edit_deps();
        return pbo::resolve_contact_conflict(d, id, choice);
    }
    std::string server_card() { return server->card("contacts", "a.vcf").vcf; }
};

// How the two versions come about: 0 = the server changed before the push (the look shows a new ETag), 1 = the server changed between the look and the
// write (412), 2 = a pull found the changed card first
enum Variant { BeforePush = 0, BetweenLookAndWrite = 1, ByPull = 2 };

// PUT requests the app itself made while the conflict came about: only the refused one of the second variant
int variant_puts(Variant how) { return how == BetweenLookAndWrite ? 1 : 0; }

int64_t make_conflict(Conflict& e, Variant how) {
    const int64_t id = e.server_contact("a.vcf", "u-a", "Anna", "Alt", "TEL;TYPE=CELL:+1 555 0101\r\nBDAY:1985-04-12\r\n");
    PB_CHECK(e.change(id, "Lokal").ok());
    const std::string theirs = contact_vcf("u-a", "Anna", "Server", "TEL;TYPE=CELL:+1 555 0101\r\nBDAY:1985-04-12\r\n");
    switch (how) {
        case BeforePush:
            e.server->put_card("contacts", "a.vcf", theirs);
            PB_CHECK(e.push().ok());
            break;
        case BetweenLookAndWrite:
            e.server->before_write([](FakeCardDavServer& s) { s.put_card("contacts", "a.vcf", contact_vcf("u-a", "Anna", "Server", "TEL;TYPE=CELL:+1 555 0101\r\nBDAY:1985-04-12\r\n")); });
            PB_CHECK(e.push().ok());
            e.server->before_write(nullptr);
            break;
        case ByPull:
            e.server->put_card("contacts", "a.vcf", theirs);
            PB_CHECK(e.pull().ok());
            break;
    }
    return id;
}

}  // namespace

PB_TEST(a_change_on_both_sides_keeps_both_versions_however_it_comes_about) {
    for (Variant how : {BeforePush, BetweenLookAndWrite, ByPull}) {
        Conflict e;
        const int64_t id = make_conflict(e, how);
        PB_CHECK_EQ(e.state_of(id), std::string("conflict"));
        PB_CHECK_EQ(e.count("SELECT count(*) FROM contact_conflict WHERE contact_id = " + std::to_string(id)), 1);
        // the reader's version is untouched, the server's is kept beside it
        PB_CHECK_EQ(e.q("SELECT family FROM contact WHERE id = " + std::to_string(id)), std::string("Lokal"));
        const std::string kept = e.q("SELECT server_vcf FROM contact_conflict WHERE contact_id = " + std::to_string(id));
        PB_CHECK(kept.find("N:Server;Anna;;;") != std::string::npos);
        PB_CHECK(kept.find("PHOTO") == std::string::npos);
        PB_CHECK_EQ(e.q("SELECT server_etag FROM contact_conflict WHERE contact_id = " + std::to_string(id)), e.server->card("contacts", "a.vcf").etag);
        // and nothing of ours overwrote the server's card
        PB_CHECK(e.server_card().find("N:Server;Anna;;;") != std::string::npos);
        PB_CHECK(e.server_card().find("Lokal") == std::string::npos);
        // a further push or pull changes nothing and does not lose either side
        PB_CHECK(e.push().ok());
        PB_CHECK(e.pull().ok());
        PB_CHECK_EQ(e.state_of(id), std::string("conflict"));
        PB_CHECK_EQ(e.q("SELECT family FROM contact WHERE id = " + std::to_string(id)), std::string("Lokal"));
        const auto problems = e.store->contact_problems();
        PB_CHECK_EQ(problems.size(), size_t(1));
        PB_CHECK_EQ(problems[0].state, std::string("conflict"));
        PB_CHECK_EQ(problems[0].contact_id, id);
        PB_CHECK(!problems[0].deleting);
        PB_CHECK(problems[0].server_vcf.find("N:Server;Anna;;;") != std::string::npos);
    }
}

PB_TEST(the_first_look_at_a_changed_card_stops_the_write) {
    Conflict e;
    make_conflict(e, BeforePush);
    PB_CHECK_EQ(e.server->count("PUT"), 0);   // the ETag of the look differed: no write was tried at all
}

PB_TEST(a_conflict_is_reported_in_the_counts_of_the_push) {
    Conflict e;
    const int64_t id = e.server_contact("a.vcf", "u-a", "Anna", "Alt");
    PB_CHECK(e.change(id, "Lokal").ok());
    e.server->put_card("contacts", "a.vcf", contact_vcf("u-a", "Anna", "Server"));
    pbo::ContactsPushCounts counts;
    auto d = e.deps();
    PB_CHECK(pbo::push_contacts(d, "pw-for-tests", &counts).ok());
    PB_CHECK_EQ(counts.conflicts, 1);
    PB_CHECK_EQ(counts.updated, 0);
}

PB_TEST(keep_mine_sends_the_readers_version_against_the_servers_new_etag) {
    for (Variant how : {BeforePush, BetweenLookAndWrite, ByPull}) {
        Conflict e;
        const int64_t id = make_conflict(e, how);
        PB_CHECK(e.resolve(id, pbo::ContactChoice::KeepMine).ok());
        PB_CHECK_EQ(e.state_of(id), std::string("pending_update"));
        PB_CHECK_EQ(e.count("SELECT count(*) FROM contact_conflict"), 0);
        PB_CHECK_EQ(e.q("SELECT etag FROM contact WHERE id = " + std::to_string(id)), e.server->card("contacts", "a.vcf").etag);
        PB_CHECK(e.push().ok());
        PB_CHECK_EQ(e.state_of(id), std::string("synced"));
        PB_CHECK(e.server_card().find("N:Lokal;Anna;;;") != std::string::npos);
        PB_CHECK(e.server_card().find("BDAY:1985-04-12") != std::string::npos);   // what the app does not show is still there
    }
}

PB_TEST(keep_the_servers_replaces_the_readers_version_and_changes_nothing_on_the_server) {
    for (Variant how : {BeforePush, BetweenLookAndWrite, ByPull}) {
        Conflict e;
        const int64_t id = make_conflict(e, how);
        const std::string etag = e.server->card("contacts", "a.vcf").etag;
        PB_CHECK(e.resolve(id, pbo::ContactChoice::KeepServer).ok());
        PB_CHECK_EQ(e.state_of(id), std::string("synced"));
        PB_CHECK_EQ(e.q("SELECT family FROM contact WHERE id = " + std::to_string(id)), std::string("Server"));
        PB_CHECK_EQ(e.q("SELECT etag FROM contact WHERE id = " + std::to_string(id)), etag);
        PB_CHECK_EQ(e.count("SELECT count(*) FROM contact_conflict"), 0);
        PB_CHECK_EQ(e.store->pending_contacts().size(), size_t(0));
        PB_CHECK_EQ(e.server->card("contacts", "a.vcf").etag, etag);
        PB_CHECK(e.push().ok());
        PB_CHECK_EQ(e.server->count("PUT"), variant_puts(how));
    }
}

PB_TEST(keep_both_keeps_the_servers_card_and_makes_the_readers_version_a_new_contact) {
    for (Variant how : {BeforePush, BetweenLookAndWrite, ByPull}) {
        Conflict e;
        const int64_t id = make_conflict(e, how);
        PB_CHECK(e.resolve(id, pbo::ContactChoice::KeepBoth).ok());
        PB_CHECK_EQ(e.state_of(id), std::string("synced"));
        PB_CHECK_EQ(e.q("SELECT family FROM contact WHERE id = " + std::to_string(id)), std::string("Server"));
        PB_CHECK_EQ(e.count("SELECT count(*) FROM contact"), 2);
        PB_CHECK_EQ(e.count("SELECT count(*) FROM contact WHERE sync_state = 'pending_create'"), 1);
        const int64_t copy = std::atoll(e.q("SELECT id FROM contact WHERE sync_state = 'pending_create'").c_str()) ;
        PB_CHECK(e.q("SELECT uid FROM contact WHERE id = " + std::to_string(copy)) != "u-a");                                        // a new identity
        PB_CHECK(e.q("SELECT href FROM contact WHERE id = " + std::to_string(copy)) != e.q("SELECT href FROM contact WHERE id = " + std::to_string(id)));   // and a new path
        PB_CHECK_EQ(e.q("SELECT family FROM contact WHERE id = " + std::to_string(copy)), std::string("Lokal"));
        PB_CHECK(e.push().ok());
        PB_CHECK_EQ(e.server->card_count("contacts"), size_t(2));
        PB_CHECK_EQ(e.state_of(copy), std::string("synced"));
        // both cards are on the server, the old one unchanged
        PB_CHECK(e.server_card().find("N:Server;Anna;;;") != std::string::npos);
        bool mine = false;
        for (const std::string& n : e.server->card_names("contacts"))
            if (e.server->card("contacts", n).vcf.find("N:Lokal;Anna;;;") != std::string::npos) mine = true;
        PB_CHECK(mine);
    }
}

PB_TEST(a_resolve_of_a_contact_without_a_conflict_is_refused) {
    Conflict e;
    const int64_t id = e.server_contact("a.vcf", "u-a", "Anna", "Alt");
    PB_CHECK_EQ(e.resolve(id, pbo::ContactChoice::KeepMine).key, std::string("not_in_conflict"));
    PB_CHECK_EQ(e.resolve(4242, pbo::ContactChoice::KeepMine).key, std::string("not_found"));
}

PB_TEST(a_conflict_blocks_further_edits_until_it_is_resolved) {
    Conflict e;
    const int64_t id = make_conflict(e, BeforePush);
    PB_CHECK_EQ(e.change(id, "Noch einmal").key, std::string("in_conflict"));
    PB_CHECK_EQ(e.q("SELECT family FROM contact WHERE id = " + std::to_string(id)), std::string("Lokal"));
}

// ---- a deletion against a card that changed on the server (US4) -----------------------------------------------------------------------------------

namespace {

// 0 = the server changed before the push (the DELETE answers 412), 1 = it changed while the request was on its way, 2 = a pull found it first
int64_t make_deletion_conflict(Conflict& e, Variant how) {
    const int64_t id = e.server_contact("a.vcf", "u-a", "Anna", "Alt", "TEL;TYPE=CELL:+1 555 0101\r\nBDAY:1985-04-12\r\n");
    PB_CHECK(pbo::delete_contact(*e.store, id).ok());
    const std::string theirs = contact_vcf("u-a", "Anna", "Server", "TEL;TYPE=CELL:+1 555 0101\r\nBDAY:1985-04-12\r\n");
    switch (how) {
        case BeforePush:
            e.server->put_card("contacts", "a.vcf", theirs);
            PB_CHECK(e.push().ok());
            break;
        case BetweenLookAndWrite:
            e.server->before_write([](FakeCardDavServer& s) { s.put_card("contacts", "a.vcf", contact_vcf("u-a", "Anna", "Server", "TEL;TYPE=CELL:+1 555 0101\r\nBDAY:1985-04-12\r\n")); });
            PB_CHECK(e.push().ok());
            e.server->before_write(nullptr);
            break;
        case ByPull:
            e.server->put_card("contacts", "a.vcf", theirs);
            PB_CHECK(e.pull().ok());
            break;
    }
    return id;
}

}  // namespace

PB_TEST(a_contact_changed_on_the_server_is_never_deleted_by_the_old_version) {
    for (Variant how : {BeforePush, BetweenLookAndWrite, ByPull}) {
        Conflict e;
        const int64_t id = make_deletion_conflict(e, how);
        PB_CHECK_EQ(e.state_of(id), std::string("conflict"));
        PB_CHECK_EQ(e.server->card_count("contacts"), size_t(1));   // the card is still there
        PB_CHECK(e.server_card().find("N:Server;Anna;;;") != std::string::npos);
        const auto problems = e.store->contact_problems();
        PB_CHECK_EQ(problems.size(), size_t(1));
        PB_CHECK(problems[0].deleting);
        PB_CHECK(problems[0].server_vcf.find("N:Server;Anna;;;") != std::string::npos);
        // a further push or pull does not delete it either
        PB_CHECK(e.push().ok());
        PB_CHECK(e.pull().ok());
        PB_CHECK_EQ(e.server->card_count("contacts"), size_t(1));
        PB_CHECK_EQ(e.state_of(id), std::string("conflict"));
        // every DELETE the server saw carried a stale ETag and was refused
        for (const auto& r : e.server->log())
            if (r.method == "DELETE") PB_CHECK(r.headers.count("if-match") == 1);
    }
}

PB_TEST(a_deletion_conflict_resolved_for_the_server_keeps_the_card_and_drops_the_deletion) {
    for (Variant how : {BeforePush, BetweenLookAndWrite, ByPull}) {
        Conflict e;
        const int64_t id = make_deletion_conflict(e, how);
        const int deletes = e.server->count("DELETE");
        PB_CHECK(e.resolve(id, pbo::ContactChoice::KeepServer).ok());
        PB_CHECK_EQ(e.state_of(id), std::string("synced"));
        PB_CHECK_EQ(e.q("SELECT family FROM contact WHERE id = " + std::to_string(id)), std::string("Server"));
        PB_CHECK_EQ(e.store->pending_contacts().size(), size_t(0));
        PB_CHECK(e.push().ok());
        PB_CHECK_EQ(e.server->count("DELETE"), deletes);
        PB_CHECK_EQ(e.server->card_count("contacts"), size_t(1));
    }
}

PB_TEST(a_deletion_conflict_resolved_for_mine_deletes_against_the_new_etag) {
    for (Variant how : {BeforePush, BetweenLookAndWrite, ByPull}) {
        Conflict e;
        const int64_t id = make_deletion_conflict(e, how);
        PB_CHECK(e.resolve(id, pbo::ContactChoice::KeepMine).ok());
        PB_CHECK_EQ(e.state_of(id), std::string("pending_delete"));
        PB_CHECK_EQ(e.q("SELECT etag FROM contact WHERE id = " + std::to_string(id)), e.server->card("contacts", "a.vcf").etag);
        PB_CHECK(e.push().ok());
        PB_CHECK_EQ(e.server->card_count("contacts"), size_t(0));
        PB_CHECK_EQ(e.count("SELECT count(*) FROM contact"), 0);
    }
}

PB_TEST(keeping_both_after_a_deletion_conflict_makes_no_copy) {
    Conflict e;
    const int64_t id = make_deletion_conflict(e, BeforePush);
    PB_CHECK(e.resolve(id, pbo::ContactChoice::KeepBoth).ok());
    PB_CHECK_EQ(e.state_of(id), std::string("synced"));
    PB_CHECK_EQ(e.count("SELECT count(*) FROM contact"), 1);   // there is no version of the reader's to keep: it was a deletion
}

PB_TEST_MAIN()
