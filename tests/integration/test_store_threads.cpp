// The UI thread reads while the worker thread writes (bridge/worker.h): one shared connection must never show a
// half-applied batch. A writer commits batches of 5 messages; a reader polls the list and the counts, which must
// always be multiples of 5, and the unread count must match the rows.
#include <atomic>
#include <thread>

#include "pbo/store.h"
#include "test_support.h"
#include "tmp_dir.h"

namespace {
struct FixedClock : pbo::Clock {
    int64_t now() const override { return 1791979200; }
};
}  // namespace

PB_TEST(a_reader_never_sees_a_half_applied_batch) {
    pbtest::TmpDir dir;
    FixedClock clock;
    auto store = pbo::Store::open(dir.path(), clock);
    PB_CHECK(store.ok());
    pbo::FolderListItem inbox;
    inbox.raw_name = inbox.name = "INBOX";
    PB_CHECK(store.value->upsert_folders({inbox}).ok());
    const int64_t folder = store.value->folders().at(0).id;

    std::atomic<bool> done{false};
    std::atomic<int> violations{0};
    std::thread writer([&] {
        uint32_t uid = 1;
        for (int b = 0; b < 60; ++b) {
            pbo::PullBatch batch;
            for (int i = 0; i < 5; ++i) {
                pbo::PulledMessage m;
                m.uid = uid++;
                m.subject = "s" + std::to_string(m.uid);
                m.date_utc = 1790000000 + m.uid;
                batch.messages.push_back(m);
            }
            batch.uidnext = uid;
            if (!store.value->apply_pull(folder, batch).ok()) ++violations;
        }
        done = true;
    });
    int reads = 0;
    while (!done) {
        const auto page = store.value->list_messages(folder, pbo::SortKey::DateDesc, 0, 1000);
        if (page.total % 5 != 0 || static_cast<int>(page.items.size()) != page.total) ++violations;
        for (const auto& f : store.value->folders())
            if (f.uidnext && (*f.uidnext - 1) % 5 != 0) ++violations;
        ++reads;
    }
    writer.join();
    PB_CHECK(reads > 0);
    PB_CHECK_EQ(violations.load(), 0);
    PB_CHECK_EQ(store.value->list_messages(folder, pbo::SortKey::DateDesc, 0, 10).total, 300);
}

PB_TEST_MAIN()
