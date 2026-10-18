// Spec 002: the push of contacts the user created (US2), then edited (US3) and deleted (US4), against the fake CardDAV server: one conditional
// request each, the contact marked until the server confirmed, an answer that was lost, the pull's own-card rule, and logs without contact data
// (FR-004, FR-018, FR-020, FR-027, SC-005). UNVERIFIED against real servers.
#include <dirent.h>

#include <algorithm>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>

#include "contact_env.h"
#include "pbo/contact_edit.h"
#include "pbo/log.h"
#include "test_support.h"

using pbtest::ContactEnv;
using pbtest::FakeCardDavServer;
using pbtest::contact_vcf;

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
    e.org = "Example Works";
    e.phones = {lv("mobile", "+1 555 0101")};
    e.emails = {lv("work", "anna@example.org")};
    return e;
}

struct Pushed : ContactEnv {
    using ContactEnv::ContactEnv;
    pbo::ContactEditDeps edit_deps() { return pbo::ContactEditDeps{*store, clock, random}; }
    pbo::Error push(pbo::ContactsPushCounts* counts = nullptr) {
        auto d = deps();
        return pbo::push_contacts(d, "pw-for-tests", counts);
    }
    pbo::ContactEdit edit_of(int64_t id) {
        auto v = store->contact(id);
        PB_CHECK(v.has_value());
        return pbo::contact_edit_of(v->card);
    }
    pbo::Error change(int64_t id, const pbo::ContactEdit& edit) {
        auto d = edit_deps();
        return pbo::change_contact(d, id, edit).error;
    }
    int64_t create(const pbo::ContactEdit& edit, int64_t book = 0) {
        auto d = edit_deps();
        auto r = pbo::create_contact(d, book ? book : main_book, edit);
        PB_CHECK(r.ok());
        return r.value;
    }
};

}  // namespace

PB_TEST(a_created_contact_is_stored_first_shown_at_once_and_marked_until_the_server_confirmed) {
    Pushed e;
    const int64_t id = e.create(anna());
    PB_CHECK_EQ(e.state_of(id), std::string("pending_create"));
    PB_CHECK_EQ(e.count("SELECT count(*) FROM contact"), 1);
    const std::string href = e.q("SELECT href FROM contact WHERE id = " + std::to_string(id));
    PB_CHECK(href.compare(0, e.server->book_href("contacts").size(), e.server->book_href("contacts")) == 0);   // inside its book
    PB_CHECK_EQ(href.size(), e.server->book_href("contacts").size() + 32 + 4);                                  // <32 hex>.vcf, chosen before the first request
    PB_CHECK_EQ(e.count("SELECT etag IS NULL FROM contact WHERE id = " + std::to_string(id)), 1);               // not confirmed
    const std::string uid = e.q("SELECT uid FROM contact WHERE id = " + std::to_string(id));
    PB_CHECK(uid.size() > std::string("@pboffice.invalid").size() && uid.compare(uid.size() - 17, 17, "@pboffice.invalid") == 0);
    const auto items = e.store->contacts_page("", 0, 10);
    PB_CHECK_EQ(items.size(), size_t(1));
    PB_CHECK_EQ(items[0].display_name, std::string("Anna Beispiel"));
    PB_CHECK_EQ(items[0].sync_state, std::string("pending_create"));
    PB_CHECK_EQ(e.server->card_count("contacts"), size_t(0));   // nothing left the reader yet
    PB_CHECK_EQ(e.q("SELECT partial FROM contact WHERE id = " + std::to_string(id)), std::string("0"));   // the app wrote the whole card itself
    PB_CHECK_EQ(e.count("SELECT count(*) FROM contact_email WHERE contact_id = " + std::to_string(id)), 1);
    PB_CHECK_EQ(e.store->pending_contacts().size(), size_t(1));
}

PB_TEST(the_push_sends_exactly_one_conditional_put_and_the_mark_goes) {
    Pushed e;
    const int64_t id = e.create(anna());
    pbo::ContactsPushCounts counts;
    PB_CHECK(e.push(&counts).ok());
    PB_CHECK_EQ(counts.created, 1);
    PB_CHECK_EQ(e.server->count("PUT"), 1);
    for (const auto& r : e.server->log())
        if (r.method == "PUT") {
            PB_CHECK_EQ(r.headers.at("if-none-match"), std::string("*"));
            PB_CHECK(r.headers.count("if-match") == 0);
            PB_CHECK_EQ(r.headers.at("content-type"), std::string("text/vcard; charset=utf-8"));
        }
    PB_CHECK_EQ(e.state_of(id), std::string("synced"));
    PB_CHECK_EQ(e.count("SELECT etag IS NULL FROM contact WHERE id = " + std::to_string(id)), 0);
    const std::string href = e.q("SELECT href FROM contact WHERE id = " + std::to_string(id));
    const std::string name = href.substr(href.rfind('/') + 1);
    PB_CHECK(e.server->has_card("contacts", name));
    PB_CHECK_EQ(e.q("SELECT etag FROM contact WHERE id = " + std::to_string(id)), e.server->card("contacts", name).etag);
    const std::string sent = e.server->card("contacts", name).vcf;
    PB_CHECK(sent.find("FN:Anna Beispiel\r\n") != std::string::npos);
    PB_CHECK(sent.find("EMAIL;TYPE=WORK:anna@example.org\r\n") != std::string::npos);
    PB_CHECK_EQ(e.store->pending_contacts().size(), size_t(0));
    // the next push has nothing to do, and the next pull does not add a second copy
    PB_CHECK(e.push().ok());
    PB_CHECK_EQ(e.server->count("PUT"), 1);
    PB_CHECK(e.pull().ok());
    PB_CHECK_EQ(e.count("SELECT count(*) FROM contact"), 1);
    PB_CHECK_EQ(e.state_of(id), std::string("synced"));
}

PB_TEST(two_creates_give_two_cards_in_the_order_they_were_made) {
    Pushed e;
    pbo::ContactEdit second = anna();
    second.given = "Bert";
    const int64_t a = e.create(anna());
    const int64_t b = e.create(second);
    PB_CHECK(e.q("SELECT href FROM contact WHERE id = " + std::to_string(a)) != e.q("SELECT href FROM contact WHERE id = " + std::to_string(b)));
    const auto pending = e.store->pending_contacts();
    PB_CHECK_EQ(pending.size(), size_t(2));
    PB_CHECK_EQ(pending[0].id, a);
    PB_CHECK_EQ(pending[1].id, b);
    PB_CHECK(e.push().ok());
    PB_CHECK_EQ(e.server->card_count("contacts"), size_t(2));
    PB_CHECK_EQ(e.server->count("PUT"), 2);
}

PB_TEST(an_answer_lost_after_the_server_took_the_card_never_makes_a_second_card) {
    Pushed e;
    const int64_t id = e.create(anna());
    FakeCardDavServer::Fault lost;
    lost.kind = FakeCardDavServer::Fault::Kind::DropAfterProcessing;
    lost.method = "PUT";
    e.server->inject(lost);
    const pbo::Error first = e.push();
    PB_CHECK(!first.ok());
    PB_CHECK_EQ(e.state_of(id), std::string("pending_create"));   // the reader does not know that the card arrived
    PB_CHECK_EQ(e.server->card_count("contacts"), size_t(1));
    pbo::ContactsPushCounts counts;
    PB_CHECK(e.push(&counts).ok());
    PB_CHECK_EQ(e.server->card_count("contacts"), size_t(1));      // still one card
    PB_CHECK_EQ(e.server->count("PUT"), 2);                         // the retry went to the same path and was refused as "exists"
    PB_CHECK_EQ(e.server->count("GET"), 1);                         // and one look at that path showed the app's own card
    PB_CHECK_EQ(e.state_of(id), std::string("synced"));
    const std::string href = e.q("SELECT href FROM contact WHERE id = " + std::to_string(id));
    PB_CHECK_EQ(e.q("SELECT etag FROM contact WHERE id = " + std::to_string(id)), e.server->card("contacts", href.substr(href.rfind('/') + 1)).etag);
    PB_CHECK_EQ(counts.created, 1);
    PB_CHECK(e.pull().ok());
    PB_CHECK_EQ(e.count("SELECT count(*) FROM contact"), 1);
}

PB_TEST(a_pulled_card_at_the_path_of_a_pending_create_is_the_apps_own_card_and_is_adopted) {
    Pushed e;
    const int64_t id = e.create(anna());
    const std::string href = e.q("SELECT href FROM contact WHERE id = " + std::to_string(id));
    const std::string name = href.substr(href.rfind('/') + 1);
    const std::string local = e.q("SELECT vcf FROM contact WHERE id = " + std::to_string(id));
    // the card is on the server with the content the reader wrote (the answer to its PUT was lost, then another sync ran)
    e.server->put_card("contacts", name, local);
    PB_CHECK(e.pull().ok());
    PB_CHECK_EQ(e.count("SELECT count(*) FROM contact"), 1);
    PB_CHECK_EQ(e.state_of(id), std::string("synced"));
    PB_CHECK_EQ(e.q("SELECT etag FROM contact WHERE id = " + std::to_string(id)), e.server->card("contacts", name).etag);
    PB_CHECK_EQ(e.q("SELECT conflict_rows FROM (SELECT count(*) AS conflict_rows FROM contact_conflict)"), std::string("0"));
}

PB_TEST(the_push_goes_on_when_the_book_is_not_shown_and_stops_at_a_login_problem) {
    Pushed e;
    const int64_t id = e.create(anna());
    PB_CHECK(e.store->set_addressbook_shown(e.main_book, false).ok());
    PB_CHECK(e.push().ok());   // hiding a book never keeps back what the user made in it
    PB_CHECK_EQ(e.state_of(id), std::string("synced"));
    pbo::ContactEdit second = anna();
    second.given = "Bert";
    PB_CHECK(e.store->set_addressbook_shown(e.main_book, true).ok());
    const int64_t b = e.create(second);
    auto d = e.deps();
    const pbo::Error bad = pbo::push_contacts(d, "wrong-password", nullptr);
    PB_CHECK_EQ(bad.key, std::string("auth_failed"));
    PB_CHECK_EQ(e.state_of(b), std::string("pending_create"));   // nothing is lost by a refused login
}

PB_TEST(the_log_of_a_push_holds_no_contact_data_and_no_server_address) {
    Pushed e;
    pbo::ContactEdit secret;
    secret.given = "Zacharias";
    secret.family = "Geheimnis";
    secret.phones = {lv("mobile", "+49 555 0199")};
    secret.emails = {lv("work", "zacharias@example.org")};
    secret.note = "Streng vertraulich";
    e.create(secret);
    std::vector<std::string> lines;
    pbo::log::set_sink([&](const std::string& line) { lines.push_back(line); });
    PB_CHECK(e.push().ok());
    pbo::log::reset_sink();
    PB_CHECK(!lines.empty());
    for (const std::string& l : lines)
        for (const std::string& needle : std::vector<std::string>{"Zacharias", "Geheimnis", "0199", "zacharias@example.org", "vertraulich", "localhost", "alice", ".vcf",
                                                                   std::to_string(e.server->port())})
            if (l.find(needle) != std::string::npos) PB_FAIL_AT("the log line \"" + l + "\" holds " + needle);
}

// ---- changing a contact (US3) ----------------------------------------------------------------------------------------------------------------

namespace {

const std::string kPhotoLines = "PHOTO;ENCODING=b;TYPE=JPEG:" + std::string(3000, 'A') + "\r\nLOGO;ENCODING=b:" + std::string(500, 'B') + "\r\n";

std::string server_name(Pushed& e, int64_t id) {
    const std::string href = e.q("SELECT href FROM contact WHERE id = " + std::to_string(id));
    return href.substr(href.rfind('/') + 1);
}

std::vector<std::string> lines_of(const std::string& text) {
    std::vector<std::string> out;
    for (const auto& l : pbo::parse_vcard(text).lines) {
        std::string raw = l.raw;
        while (!raw.empty() && (raw.back() == '\r' || raw.back() == '\n')) raw.pop_back();
        if (!raw.empty()) out.push_back(raw);
    }
    return out;
}

}  // namespace

PB_TEST(a_changed_contact_shows_at_once_marked_and_two_edits_send_one_change) {
    Pushed e;
    const int64_t id = e.server_contact("a.vcf", "u-a", "Anna", "Alt", "TEL;TYPE=CELL:+1 555 0101\r\n");
    pbo::ContactEdit edit = e.edit_of(id);
    edit.family = "Neu";
    PB_CHECK(e.change(id, edit).ok());
    PB_CHECK_EQ(e.state_of(id), std::string("pending_update"));
    PB_CHECK_EQ(e.store->contacts_page("neu", 0, 10).size(), size_t(1));
    PB_CHECK_EQ(e.store->contacts_page("neu", 0, 10)[0].sync_state, std::string("pending_update"));
    edit = e.edit_of(id);
    edit.family = "Neuer";
    edit.phones[0].value = "+1 555 0102";
    PB_CHECK(e.change(id, edit).ok());
    PB_CHECK_EQ(e.state_of(id), std::string("pending_update"));   // still one waiting change
    PB_CHECK_EQ(e.store->pending_contacts().size(), size_t(1));
    pbo::ContactsPushCounts counts;
    PB_CHECK(e.push(&counts).ok());
    PB_CHECK_EQ(counts.updated, 1);
    PB_CHECK_EQ(e.server->count("PUT"), 1);
    PB_CHECK_EQ(e.state_of(id), std::string("synced"));
    const std::string vcf = e.server->card("contacts", "a.vcf").vcf;
    PB_CHECK(vcf.find("N:Neuer;Anna;;;") != std::string::npos);
    PB_CHECK(vcf.find("+1 555 0102") != std::string::npos);
    PB_CHECK_EQ(e.q("SELECT etag FROM contact WHERE id = " + std::to_string(id)), e.server->card("contacts", "a.vcf").etag);
    PB_CHECK(e.push().ok());
    PB_CHECK_EQ(e.server->count("PUT"), 1);   // nothing more to send
}

PB_TEST(editing_a_contact_from_the_server_reads_the_full_card_first_and_keeps_what_the_app_does_not_show) {
    Pushed e;
    const std::string extra = kPhotoLines + "BDAY:1985-04-12\r\nURL:https://www.example.org/anna\r\nX-CUSTOM-FIELD:keep me\r\nCATEGORIES:friends\r\n";
    e.server->put_card("contacts", "a.vcf", contact_vcf("u-a", "Anna", "Alt", extra));
    PB_CHECK(e.pull().ok());
    const int64_t id = e.id_of_uid("u-a");
    PB_CHECK(e.q("SELECT vcf FROM contact WHERE id = " + std::to_string(id)).find("PHOTO") == std::string::npos);   // the reader never kept it
    pbo::ContactEdit edit = e.edit_of(id);
    edit.family = "Neu";
    edit.emails.push_back(lv("work", "anna@example.org"));
    PB_CHECK(e.change(id, edit).ok());
    const std::string etag_before = e.server->card("contacts", "a.vcf").etag;
    const int gets = e.server->count("GET");
    PB_CHECK(e.push().ok());
    PB_CHECK_EQ(e.server->count("GET"), gets + 1);   // one look at the whole card
    int puts = 0;
    for (const auto& r : e.server->log())
        if (r.method == "PUT") {
            ++puts;
            PB_CHECK_EQ(r.headers.at("if-match"), etag_before);   // against the version the GET showed
            PB_CHECK(r.headers.count("if-none-match") == 0);
        }
    PB_CHECK_EQ(puts, 1);
    const std::string vcf = e.server->card("contacts", "a.vcf").vcf;
    PB_CHECK(vcf.find(kPhotoLines.substr(0, 60)) != std::string::npos);   // the photo is still on the server
    PB_CHECK(vcf.find("LOGO;ENCODING=b:") != std::string::npos);
    for (const char* kept : {"BDAY:1985-04-12", "URL:https://www.example.org/anna", "X-CUSTOM-FIELD:keep me", "CATEGORIES:friends"}) PB_CHECK(vcf.find(kept) != std::string::npos);
    PB_CHECK(vcf.find("N:Neu;Anna;;;") != std::string::npos);
    PB_CHECK(vcf.find("EMAIL;TYPE=WORK:anna@example.org") != std::string::npos);
    PB_CHECK_EQ(e.state_of(id), std::string("synced"));
    PB_CHECK(e.q("SELECT vcf FROM contact WHERE id = " + std::to_string(id)).find("PHOTO") == std::string::npos);
}

PB_TEST(editing_the_cards_of_the_corpus_loses_none_of_their_other_properties_on_the_server) {
    Pushed e;
    const std::string dir = std::string(PBO_FIXTURE_DIR) + "/vcard/corpus";
    std::vector<std::string> names;
    if (DIR* d = opendir(dir.c_str())) {
        while (dirent* ent = readdir(d)) {
            const std::string n = ent->d_name;
            if (n.size() > 4 && n.compare(n.size() - 4, 4, ".vcf") == 0) names.push_back(n);
        }
        closedir(d);
    }
    std::sort(names.begin(), names.end());
    PB_CHECK(names.size() >= 24);
    std::map<std::string, std::string> original;   // server name -> text
    for (const std::string& n : names) {
        std::ifstream f(dir + "/" + n, std::ios::binary);
        std::ostringstream ss;
        ss << f.rdbuf();
        original[n] = ss.str();
        e.server->put_card("contacts", n, ss.str());
    }
    PB_CHECK(e.pull().ok());
    int edited = 0;
    for (const std::string& n : names) {
        const int64_t id = e.id_of_href("contacts", n);
        PB_CHECK(id != 0);
        auto view = e.store->contact(id);
        if (!view || !view->card.readable) continue;
        pbo::ContactEdit edit = pbo::contact_edit_of(view->card);
        edit.family = "Edited";
        if (!edit.phones.empty() && !edit.phones[0].locked) edit.phones[0].value = "+1 555 0000";
        PB_CHECK(e.change(id, edit).ok());
        ++edited;
    }
    PB_CHECK(edited >= 24);
    PB_CHECK(e.push().ok());
    for (const std::string& n : names) {
        const auto before = lines_of(original[n]);
        const auto after = lines_of(e.server->card("contacts", n).vcf);
        std::multiset<std::string> kept(after.begin(), after.end());
        for (const std::string& l : before) {
            // the lines the edit may rewrite: names, the first phone, the revision
            std::string head = l.substr(0, l.find_first_of(":;"));   // the property name, with its group
            if (const size_t dot = head.rfind('.'); dot != std::string::npos) head.erase(0, dot + 1);
            const bool rewritten = head == "N" || head == "FN" || head == "REV" || head == "TEL";
            if (rewritten) continue;
            auto it = kept.find(l);
            if (it == kept.end()) PB_FAIL_AT(n + ": the server lost the line " + l.substr(0, 60));
            kept.erase(it);
        }
        PB_CHECK(e.server->card("contacts", n).vcf.find("Edited") != std::string::npos);
    }
}

PB_TEST(a_contact_the_app_wrote_itself_is_changed_against_its_own_etag_without_a_look_at_the_server) {
    Pushed e;
    const int64_t id = e.create(anna());
    PB_CHECK(e.push().ok());
    PB_CHECK_EQ(e.q("SELECT partial FROM contact WHERE id = " + std::to_string(id)), std::string("0"));
    pbo::ContactEdit edit = e.edit_of(id);
    edit.org = "Other Works";
    PB_CHECK(e.change(id, edit).ok());
    const int gets = e.server->count("GET");
    PB_CHECK(e.push().ok());
    PB_CHECK_EQ(e.server->count("GET"), gets);
    PB_CHECK_EQ(e.state_of(id), std::string("synced"));
    PB_CHECK(e.server->card("contacts", server_name(e, id)).vcf.find("ORG:Other Works") != std::string::npos);
}

PB_TEST(an_edit_made_while_the_push_runs_stays_waiting_against_the_new_etag) {
    Pushed e;
    const int64_t id = e.server_contact("a.vcf", "u-a", "Anna", "Alt", "TEL;TYPE=CELL:+1 555 0101\r\n");
    pbo::ContactEdit edit = e.edit_of(id);
    edit.family = "Erste";
    PB_CHECK(e.change(id, edit).ok());
    e.server->before_write([&](FakeCardDavServer&) {   // the user edits again while the request is on its way
        pbo::ContactEdit second = e.edit_of(id);
        second.family = "Zweite";
        e.change(id, second);
    });
    PB_CHECK(e.push().ok());
    e.server->before_write(nullptr);
    PB_CHECK_EQ(e.state_of(id), std::string("pending_update"));   // the second edit has not reached the server
    PB_CHECK_EQ(e.q("SELECT family FROM contact WHERE id = " + std::to_string(id)), std::string("Zweite"));
    PB_CHECK_EQ(e.q("SELECT etag FROM contact WHERE id = " + std::to_string(id)), e.server->card("contacts", "a.vcf").etag);
    PB_CHECK(e.server->card("contacts", "a.vcf").vcf.find("N:Erste;Anna;;;") != std::string::npos);
    PB_CHECK(e.push().ok());
    PB_CHECK_EQ(e.state_of(id), std::string("synced"));
    PB_CHECK(e.server->card("contacts", "a.vcf").vcf.find("N:Zweite;Anna;;;") != std::string::npos);
}

PB_TEST(the_card_of_an_update_whose_answer_was_lost_is_recognised_at_the_next_pull) {
    Pushed e;
    const int64_t id = e.server_contact("a.vcf", "u-a", "Anna", "Alt");
    pbo::ContactEdit edit = e.edit_of(id);
    edit.family = "Neu";
    PB_CHECK(e.change(id, edit).ok());
    FakeCardDavServer::Fault lost;
    lost.kind = FakeCardDavServer::Fault::Kind::DropAfterProcessing;
    lost.method = "PUT";
    e.server->inject(lost);
    PB_CHECK(!e.push().ok());   // the server took it, the reader does not know
    PB_CHECK_EQ(e.state_of(id), std::string("pending_update"));
    PB_CHECK(e.pull().ok());    // the card with the reader's own content comes back with a new ETag
    PB_CHECK_EQ(e.state_of(id), std::string("synced"));
    PB_CHECK_EQ(e.count("SELECT count(*) FROM contact_conflict"), 0);
    PB_CHECK_EQ(e.q("SELECT etag FROM contact WHERE id = " + std::to_string(id)), e.server->card("contacts", "a.vcf").etag);
    PB_CHECK_EQ(e.server->count("PUT"), 1);
}

PB_TEST(the_log_of_an_update_holds_no_contact_data) {
    Pushed e;
    const int64_t id = e.server_contact("a.vcf", "u-a", "Zacharias", "Geheimnis", "TEL:+49 555 0199\r\nEMAIL:zacharias@example.org\r\nNOTE:Streng vertraulich\r\n");
    pbo::ContactEdit edit = e.edit_of(id);
    edit.family = "Verschlusssache";
    PB_CHECK(e.change(id, edit).ok());
    std::vector<std::string> lines;
    pbo::log::set_sink([&](const std::string& line) { lines.push_back(line); });
    PB_CHECK(e.push().ok());
    pbo::log::reset_sink();
    for (const std::string& l : lines)
        for (const std::string& needle : std::vector<std::string>{"Zacharias", "Geheimnis", "Verschlusssache", "0199", "zacharias@example.org", "vertraulich", "localhost", "a.vcf"})
            if (l.find(needle) != std::string::npos) PB_FAIL_AT("the log line \"" + l + "\" holds " + needle);
}

PB_TEST(a_lost_answer_followed_by_an_edit_of_the_new_contact_keeps_the_edit) {
    Pushed e;
    const int64_t id = e.create(anna());
    FakeCardDavServer::Fault lost;
    lost.kind = FakeCardDavServer::Fault::Kind::DropAfterProcessing;
    lost.method = "PUT";
    e.server->inject(lost);
    PB_CHECK(!e.push().ok());
    // the user changes the family name before the next push: the server holds the first version, the reader the second
    auto d = e.edit_deps();
    pbo::ContactEdit changed = anna();
    changed.family = "Neu";
    changed.phones[0].orig_index = 0;
    changed.emails[0].orig_index = 0;
    auto r = pbo::change_contact(d, id, changed);
    PB_CHECK(r.ok());
    PB_CHECK(e.push().ok());
    PB_CHECK_EQ(e.server->card_count("contacts"), size_t(1));
    PB_CHECK_EQ(e.q("SELECT family FROM contact WHERE id = " + std::to_string(id)), std::string("Neu"));   // the edit is not lost
    PB_CHECK_EQ(e.state_of(id), std::string("pending_update"));   // and is sent against the adopted ETag
    PB_CHECK(e.push().ok());
    PB_CHECK_EQ(e.state_of(id), std::string("synced"));
    const std::string href = e.q("SELECT href FROM contact WHERE id = " + std::to_string(id));
    PB_CHECK(e.server->card("contacts", href.substr(href.rfind('/') + 1)).vcf.find("Neu") != std::string::npos);
}

PB_TEST(a_pulled_card_at_the_path_of_a_pending_create_with_other_content_leaves_an_update_not_a_conflict) {
    Pushed e;
    const int64_t id = e.create(anna());
    const std::string href = e.q("SELECT href FROM contact WHERE id = " + std::to_string(id));
    const std::string name = href.substr(href.rfind('/') + 1);
    std::string first_version = e.q("SELECT vcf FROM contact WHERE id = " + std::to_string(id));
    e.server->put_card("contacts", name, first_version);   // what the first PUT carried
    // the user edits the new contact before the pull runs
    auto d = e.edit_deps();
    pbo::ContactEdit changed = anna();
    changed.family = "Neu";
    changed.phones[0].orig_index = 0;
    changed.emails[0].orig_index = 0;
    PB_CHECK(pbo::change_contact(d, id, changed).ok());
    PB_CHECK(e.pull().ok());
    PB_CHECK_EQ(e.count("SELECT count(*) FROM contact"), 1);
    PB_CHECK_EQ(e.state_of(id), std::string("pending_update"));
    PB_CHECK_EQ(e.q("SELECT family FROM contact WHERE id = " + std::to_string(id)), std::string("Neu"));
    PB_CHECK_EQ(e.q("SELECT etag FROM contact WHERE id = " + std::to_string(id)), e.server->card("contacts", name).etag);
    PB_CHECK_EQ(e.count("SELECT count(*) FROM contact_conflict"), 0);
}


// ---- deleting a contact (US4) --------------------------------------------------------------------------------------------------------------------

PB_TEST(a_deleted_contact_leaves_the_list_at_once_and_waits_marked_for_the_push) {
    Pushed e;
    const int64_t id = e.server_contact("a.vcf", "u-a", "Anna", "Alt", "TEL:+1 555 0101\r\n");
    e.server_contact("b.vcf", "u-b", "Bert", "Bleibt");
    PB_CHECK(pbo::delete_contact(*e.store, id).ok());
    PB_CHECK_EQ(e.state_of(id), std::string("pending_delete"));
    PB_CHECK_EQ(e.q("SELECT undo_state FROM contact WHERE id = " + std::to_string(id)), std::string("synced"));
    PB_CHECK_EQ(e.store->contacts_count(""), 1);
    PB_CHECK_EQ(e.store->contacts_page("alt", 0, 10).size(), size_t(0));
    PB_CHECK_EQ(e.store->pending_contacts().size(), size_t(1));
    PB_CHECK_EQ(e.store->pending_contacts()[0].sync_state, std::string("pending_delete"));
    PB_CHECK_EQ(e.server->card_count("contacts"), size_t(2));   // nothing has reached the server yet
}

PB_TEST(the_push_sends_one_conditional_delete_and_the_row_goes_when_the_server_confirmed) {
    Pushed e;
    const int64_t id = e.server_contact("a.vcf", "u-a", "Anna", "Alt");
    const std::string etag = e.server->card("contacts", "a.vcf").etag;
    PB_CHECK(pbo::delete_contact(*e.store, id).ok());
    pbo::ContactsPushCounts counts;
    PB_CHECK(e.push(&counts).ok());
    PB_CHECK_EQ(counts.deleted, 1);
    int deletes = 0;
    for (const auto& r : e.server->log())
        if (r.method == "DELETE") {
            ++deletes;
            PB_CHECK_EQ(r.headers.at("if-match"), etag);   // never without the version the reader knows
        }
    PB_CHECK_EQ(deletes, 1);
    PB_CHECK_EQ(e.server->card_count("contacts"), size_t(0));
    PB_CHECK_EQ(e.count("SELECT count(*) FROM contact"), 0);
    PB_CHECK_EQ(e.count("SELECT count(*) FROM contact_email"), 0);
    PB_CHECK_EQ(pbo::undo_delete_contact(*e.store, id).key, std::string("not_found"));   // after the server confirmed there is nothing to undo
    PB_CHECK(e.push().ok());
    PB_CHECK_EQ(e.server->count("DELETE"), 1);
}

PB_TEST(undo_brings_the_contact_back_in_the_state_it_had) {
    Pushed e;
    const int64_t synced = e.server_contact("a.vcf", "u-a", "Anna", "Alt");
    const int64_t changed = e.server_contact("b.vcf", "u-b", "Bert", "Alt");
    pbo::ContactEdit edit = e.edit_of(changed);
    edit.family = "Neu";
    PB_CHECK(e.change(changed, edit).ok());
    PB_CHECK(pbo::delete_contact(*e.store, synced).ok());
    PB_CHECK(pbo::delete_contact(*e.store, changed).ok());
    PB_CHECK_EQ(e.q("SELECT undo_state FROM contact WHERE id = " + std::to_string(changed)), std::string("pending_update"));
    PB_CHECK(pbo::undo_delete_contact(*e.store, synced).ok());
    PB_CHECK(pbo::undo_delete_contact(*e.store, changed).ok());
    PB_CHECK_EQ(e.state_of(synced), std::string("synced"));
    PB_CHECK_EQ(e.state_of(changed), std::string("pending_update"));
    PB_CHECK_EQ(e.q("SELECT family FROM contact WHERE id = " + std::to_string(changed)), std::string("Neu"));   // the edit was kept
    PB_CHECK_EQ(e.q("SELECT undo_state FROM contact WHERE id = " + std::to_string(changed)), std::string());
    PB_CHECK_EQ(pbo::undo_delete_contact(*e.store, synced).key, std::string("nothing_to_undo"));
    PB_CHECK_EQ(pbo::undo_delete_contact(*e.store, 4242).key, std::string("not_found"));
    PB_CHECK_EQ(e.store->contacts_count(""), 2);
    PB_CHECK(e.push().ok());
    PB_CHECK_EQ(e.server->count("DELETE"), 0);
    PB_CHECK(e.server->card("contacts", "b.vcf").vcf.find("N:Neu;Bert;;;") != std::string::npos);
}

PB_TEST(a_contact_that_was_never_sent_is_removed_without_any_request) {
    Pushed e;
    const int64_t id = e.create(anna());
    PB_CHECK(pbo::delete_contact(*e.store, id).ok());
    PB_CHECK_EQ(e.count("SELECT count(*) FROM contact"), 0);
    PB_CHECK(e.push().ok());
    PB_CHECK_EQ(e.server->count("PUT"), 0);
    PB_CHECK_EQ(e.server->count("DELETE"), 0);
    PB_CHECK_EQ(pbo::delete_contact(*e.store, id).key, std::string("not_found"));
}

PB_TEST(a_card_that_is_already_gone_on_the_server_counts_as_deleted) {
    Pushed e;
    const int64_t id = e.server_contact("a.vcf", "u-a", "Anna", "Alt");
    PB_CHECK(pbo::delete_contact(*e.store, id).ok());
    e.server->remove_card("contacts", "a.vcf");
    PB_CHECK(e.push().ok());
    PB_CHECK_EQ(e.count("SELECT count(*) FROM contact"), 0);
}

PB_TEST(a_pull_that_reports_the_card_removed_finishes_a_waiting_deletion) {
    Pushed e;
    const int64_t id = e.server_contact("a.vcf", "u-a", "Anna", "Alt");
    PB_CHECK(pbo::delete_contact(*e.store, id).ok());
    e.server->remove_card("contacts", "a.vcf");
    PB_CHECK(e.pull().ok());
    PB_CHECK_EQ(e.count("SELECT count(*) FROM contact"), 0);
}

PB_TEST(an_answer_lost_after_the_server_deleted_the_card_ends_with_the_next_push) {
    Pushed e;
    const int64_t id = e.server_contact("a.vcf", "u-a", "Anna", "Alt");
    PB_CHECK(pbo::delete_contact(*e.store, id).ok());
    FakeCardDavServer::Fault lost;
    lost.kind = FakeCardDavServer::Fault::Kind::DropAfterProcessing;
    lost.method = "DELETE";
    e.server->inject(lost);
    PB_CHECK(!e.push().ok());
    PB_CHECK_EQ(e.state_of(id), std::string("pending_delete"));
    PB_CHECK_EQ(e.server->card_count("contacts"), size_t(0));   // the server did it
    PB_CHECK(e.push().ok());                                    // 404 now: the goal is reached
    PB_CHECK_EQ(e.count("SELECT count(*) FROM contact"), 0);
}

PB_TEST(a_refused_delete_stays_waiting_with_the_reason_and_can_be_undone) {
    Pushed e;
    const int64_t id = e.server_contact("a.vcf", "u-a", "Anna", "Alt");
    PB_CHECK(pbo::delete_contact(*e.store, id).ok());
    FakeCardDavServer::Fault refuse;
    refuse.kind = FakeCardDavServer::Fault::Kind::Status;
    refuse.status = 403;
    refuse.method = "DELETE";
    e.server->inject(refuse);
    PB_CHECK_EQ(e.push().key, std::string("refused"));
    PB_CHECK_EQ(e.state_of(id), std::string("pending_delete"));
    PB_CHECK_EQ(e.q("SELECT error_key FROM contact WHERE id = " + std::to_string(id)), std::string("refused"));
    PB_CHECK(pbo::undo_delete_contact(*e.store, id).ok());
    PB_CHECK_EQ(e.state_of(id), std::string("synced"));
    PB_CHECK_EQ(e.q("SELECT error_key FROM contact WHERE id = " + std::to_string(id)), std::string());
}

PB_TEST(the_log_of_a_delete_holds_no_contact_data) {
    Pushed e;
    const int64_t id = e.server_contact("a.vcf", "u-a", "Zacharias", "Geheimnis", "EMAIL:zacharias@example.org\r\n");
    PB_CHECK(pbo::delete_contact(*e.store, id).ok());
    std::vector<std::string> lines;
    pbo::log::set_sink([&](const std::string& line) { lines.push_back(line); });
    PB_CHECK(e.push().ok());
    pbo::log::reset_sink();
    for (const std::string& l : lines)
        for (const std::string& needle : std::vector<std::string>{"Zacharias", "Geheimnis", "zacharias@example.org", "localhost", "a.vcf"})
            if (l.find(needle) != std::string::npos) PB_FAIL_AT("the log line \"" + l + "\" holds " + needle);
}


// ---- hidden address books (US5) -------------------------------------------------------------------------------------------------------------

PB_TEST(a_hidden_book_still_sends_what_waits_and_shows_nothing_in_list_or_search) {
    Pushed e;
    const int64_t id = e.server_contact("a.vcf", "u-a", "Anna", "Alt");
    pbo::ContactEdit edit = e.edit_of(id);
    edit.family = "Neu";
    PB_CHECK(e.change(id, edit).ok());
    PB_CHECK(e.store->set_addressbook_shown(e.main_book, false).ok());
    PB_CHECK_EQ(e.store->contacts_count(""), 0);
    PB_CHECK_EQ(e.store->contacts_page("neu", 0, 10).size(), size_t(0));
    PB_CHECK_EQ(e.count("SELECT count(*) FROM contact"), 1);   // hiding deletes nothing: the row and its waiting change stay
    PB_CHECK(e.push().ok());                                   // the waiting change still goes up
    PB_CHECK_EQ(e.state_of(id), std::string("synced"));
    PB_CHECK(e.server->card("contacts", "a.vcf").vcf.find("N:Neu;Anna;;;") != std::string::npos);   // and the server copy is intact
    e.server->put_card("contacts", "b.vcf", contact_vcf("u-b", "Bert", "Neu"));
    PB_CHECK(e.pull().ok());
    PB_CHECK_EQ(e.count("SELECT count(*) FROM contact WHERE uid = 'u-b'"), 0);   // a hidden book is not pulled
    PB_CHECK(e.store->set_addressbook_shown(e.main_book, true).ok());
    PB_CHECK(e.pull().ok());
    PB_CHECK_EQ(e.count("SELECT count(*) FROM contact WHERE uid = 'u-b'"), 1);   // shown again: the next sync brings it
    PB_CHECK_EQ(e.store->contacts_count(""), 2);
}

PB_TEST_MAIN()
