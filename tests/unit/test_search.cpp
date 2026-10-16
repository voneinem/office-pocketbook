// T154 (part): offline full-text search over sender, recipient, subject and stored body text. The input is taken literally.
#include <string>

#include "pbo/store.h"
#include "test_support.h"
#include "tmp_dir.h"

namespace {

struct Clock : pbo::Clock {
    int64_t now() const override { return 1791979200; }
};

struct Env {
    pbtest::TmpDir dir;
    Clock clock;
    std::unique_ptr<pbo::Store> store;
    int64_t inbox = 0;
    Env() {
        store = std::move(pbo::Store::open(dir.path(), clock).value);
        pbo::FolderListItem i;
        i.raw_name = i.name = "INBOX";
        store->upsert_folders({i});
        inbox = store->folders()[0].id;
    }
    int64_t add(uint32_t uid, const std::string& subject, const std::string& from_name, const std::string& from_addr, const std::string& to = "alice@example.org") {
        pbo::PullBatch b;
        pbo::PulledMessage m;
        m.uid = uid;
        m.subject = subject;
        m.from_name = from_name;
        m.from_addr = from_addr;
        m.to_text = to;
        m.date_utc = 1790000000 + uid;
        b.messages.push_back(m);
        PB_CHECK(store->apply_pull(inbox, b).ok());
        return std::stoll(store->debug_query("SELECT id FROM message WHERE uid = " + std::to_string(uid)));
    }
};

std::string subjects(const std::vector<pbo::MessageHeader>& v) {
    std::string out;
    for (const auto& m : v) out += (out.empty() ? "" : " | ") + m.subject;
    return out;
}

}  // namespace

PB_TEST(search_finds_sender_recipient_subject_and_body_with_prefixes) {
    Env e;
    e.add(1, "Quarterly report", "Berta Meyer", "berta@firma.example");
    e.add(2, "Lunch?", "Carl", "carl@example.org", "dora@example.org");
    const int64_t body = e.add(3, "Notes", "Anna", "anna@example.org");
    pbo::BodyUpdate u;
    u.text = "The invoice number is 4711 and the delivery is on Friday.";
    PB_CHECK(e.store->apply_body(body, u).ok());
    PB_CHECK_EQ(subjects(e.store->search("quart", 10)), std::string("Quarterly report"));
    PB_CHECK_EQ(subjects(e.store->search("meyer", 10)), std::string("Quarterly report"));
    PB_CHECK_EQ(subjects(e.store->search("dora", 10)), std::string("Lunch?"));
    PB_CHECK_EQ(subjects(e.store->search("invoice 4711", 10)), std::string("Notes"));   // all words must match
    PB_CHECK_EQ(subjects(e.store->search("invoice 9999", 10)), std::string(""));
    PB_CHECK_EQ(subjects(e.store->search("QUARTERLY", 10)), std::string("Quarterly report"));                       // case does not matter
}

PB_TEST(accents_do_not_matter_and_results_are_newest_first) {
    Env e;
    e.add(1, "Grüße aus Köln", "Jörg", "joerg@example.org");
    e.add(2, "Gruesse und Grüße", "Jorg", "jorg@example.org");
    PB_CHECK_EQ(subjects(e.store->search("koln", 10)), std::string("Grüße aus Köln"));
    PB_CHECK_EQ(subjects(e.store->search("jorg", 10)), std::string("Gruesse und Grüße | Grüße aus Köln"));
}

PB_TEST(quotes_wildcards_and_fts_syntax_in_the_input_are_literal) {
    Env e;
    e.add(1, "100% sure", "A", "a@example.org");
    e.add(2, "snake_case name", "B", "b@example.org");
    e.add(3, "He said \"hello\"", "C", "c@example.org");
    e.add(4, "plain", "D", "d@example.org");
    // none of these may be an error, an injection of FTS operators, or match everything
    for (const char* hostile : {"\"", "\"\"", "*", "NEAR(a b)", "a OR b", "-plain", "plain AND", "subject:plain", "(", ")", "^plain", "%", "_", "';--"})
        PB_CHECK(e.store->search(hostile, 10).size() <= 1);
    PB_CHECK_EQ(subjects(e.store->search("100%", 10)), std::string("100% sure"));
    PB_CHECK_EQ(subjects(e.store->search("snake_", 10)), std::string("snake_case name"));
    PB_CHECK_EQ(subjects(e.store->search("\"hello\"", 10)), std::string("He said \"hello\""));
    PB_CHECK_EQ(e.store->search("", 10).size(), size_t(0));
    PB_CHECK_EQ(e.store->search("   ", 10).size(), size_t(0));
}

PB_TEST(a_search_does_not_list_deleted_messages_and_respects_the_limit) {
    Env e;
    for (uint32_t i = 1; i <= 5; ++i) e.add(i, "Report " + std::to_string(i), "A", "a@example.org");
    PB_CHECK_EQ(e.store->search("report", 3).size(), size_t(3));
    PB_CHECK_EQ(e.store->search("report", 3)[0].subject, std::string("Report 5"));
    const int64_t id = std::stoll(e.store->debug_query("SELECT id FROM message WHERE uid = 5"));
    PB_CHECK(e.store->request_delete(id, true).ok());
    PB_CHECK_EQ(e.store->search("report", 10).size(), size_t(4));
}

PB_TEST(when_the_body_is_cleared_by_eviction_the_headers_stay_searchable) {
    Env e;
    const int64_t id = e.add(1, "Contract", "A", "a@example.org");
    pbo::BodyUpdate u;
    u.text = "secretword inside the text";
    PB_CHECK(e.store->apply_body(id, u).ok());
    PB_CHECK_EQ(e.store->search("secretword", 10).size(), size_t(1));
    // the body text goes (as the cache eviction does), the header row and its index entry stay
    PB_CHECK(e.store->debug_exec("UPDATE message SET body_text = NULL, body_state = 'headers' WHERE id = " + std::to_string(id)).empty());
    PB_CHECK(e.store->debug_exec("DELETE FROM message_fts WHERE rowid = " + std::to_string(id)).empty());
    PB_CHECK(e.store->debug_exec("INSERT INTO message_fts(rowid, subject, from_text, to_text, body_text) SELECT id, subject, from_name || ' ' || from_addr, to_text, '' FROM message WHERE id = " + std::to_string(id)).empty());
    PB_CHECK_EQ(e.store->search("secretword", 10).size(), size_t(0));
    PB_CHECK_EQ(e.store->search("contract", 10).size(), size_t(1));
}

PB_TEST_MAIN()
