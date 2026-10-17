// T154 (part): the sort orders of the message list - date, sender, subject - are stable and total.
#include <string>

#include "pbo/store.h"
#include "test_support.h"
#include "tmp_dir.h"

namespace {

struct Clock : pbo::Clock {
    int64_t now() const override { return 1791979200; }
};

std::string order(pbo::Store& s, int64_t folder, pbo::SortKey key) {
    std::string out;
    for (const auto& m : s.list_messages(folder, key, 0, 50).items) out += (out.empty() ? "" : ",") + std::to_string(m.uid);
    return out;
}

}  // namespace

PB_TEST(the_list_sorts_by_date_sender_and_subject_with_a_stable_tie_break) {
    pbtest::TmpDir dir;
    Clock clock;
    auto store = std::move(pbo::Store::open(dir.path(), clock).value);
    pbo::FolderListItem i;
    i.raw_name = i.name = "INBOX";
    store->upsert_folders({i});
    const int64_t f = store->folders()[0].id;
    struct M {
        uint32_t uid;
        const char *name, *addr, *subject;
        int64_t date;
    } msgs[] = {{1, "Zoe", "zoe@x", "banana", 100}, {2, "anna", "anna@x", "Apple", 300}, {3, "", "mia@x", "cherry", 200},
                {4, "Anna", "anna2@x", "apple", 200}, {5, "Émile", "emile@x", "Eclair", 300}};
    pbo::PullBatch b;
    for (const M& m : msgs) {
        pbo::PulledMessage p;
        p.uid = m.uid;
        p.from_name = m.name;
        p.from_addr = m.addr;
        p.subject = m.subject;
        p.date_utc = m.date;
        b.messages.push_back(p);
    }
    PB_CHECK(store->apply_pull(f, b).ok());
    PB_CHECK_EQ(order(*store, f, pbo::SortKey::DateDesc), std::string("5,2,4,3,1"));          // ties: the later row first
    PB_CHECK_EQ(order(*store, f, pbo::SortKey::DateAsc), std::string("1,3,4,2,5"));
    // sender: the name, else the address, ignoring case: anna, Anna, Émile, mia@x, Zoe
    PB_CHECK_EQ(order(*store, f, pbo::SortKey::SenderAsc), std::string("2,4,5,3,1"));
    // subject, ignoring case: Apple, apple, banana, cherry, Eclair
    PB_CHECK_EQ(order(*store, f, pbo::SortKey::SubjectAsc), std::string("2,4,1,3,5"));
    // the same input always gives the same order (stable), page by page
    const auto p0 = store->list_messages(f, pbo::SortKey::SenderAsc, 0, 2).items;
    const auto p1 = store->list_messages(f, pbo::SortKey::SenderAsc, 1, 2).items;
    PB_CHECK_EQ(p0[0].uid, uint32_t(2));
    PB_CHECK_EQ(p1[0].uid, uint32_t(5));
}

PB_TEST_MAIN()
