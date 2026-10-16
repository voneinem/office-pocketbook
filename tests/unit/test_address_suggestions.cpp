// T154 (part): address suggestions for the recipient fields come from cached mail and sent mail, are bounded and pruned.
#include <string>

#include "pbo/store.h"
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

PB_TEST_MAIN()
