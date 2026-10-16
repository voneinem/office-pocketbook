// T154 (part): search over 5,000 headers with stored bodies finishes in well under 3 seconds on the host. SC-004 for the reader stays UNVERIFIED
// until it is measured on the device (T162): the host figure is a proxy, not the device value.
#include <chrono>
#include <cstdio>
#include <string>

#include "pbo/store.h"
#include "test_support.h"
#include "tmp_dir.h"

namespace {

struct Clock : pbo::Clock {
    int64_t now() const override { return 1791979200; }
};

}  // namespace

PB_TEST(searching_5000_messages_with_bodies_takes_less_than_3_seconds_on_the_host) {
    pbtest::TmpDir dir;
    Clock clock;
    auto store = std::move(pbo::Store::open(dir.path(), clock).value);
    pbo::FolderListItem i;
    i.raw_name = i.name = "INBOX";
    store->upsert_folders({i});
    const int64_t f = store->folders()[0].id;
    const char* words[] = {"invoice", "meeting", "holiday", "project", "budget", "contract", "delivery", "report", "reminder", "schedule"};
    for (uint32_t base = 0; base < 5000; base += 50) {
        pbo::PullBatch b;
        for (uint32_t k = 0; k < 50; ++k) {
            const uint32_t uid = base + k + 1;
            pbo::PulledMessage m;
            m.uid = uid;
            m.subject = std::string(words[uid % 10]) + " number " + std::to_string(uid);
            m.from_name = "Sender " + std::to_string(uid % 97);
            m.from_addr = "s" + std::to_string(uid % 97) + "@example.org";
            m.to_text = "alice@example.org";
            m.date_utc = 1700000000 + uid * 60;
            b.messages.push_back(m);
        }
        PB_CHECK(store->apply_pull(f, b).ok());
    }
    // bodies of 2 KB each (a different word mixture per message)
    for (uint32_t uid = 1; uid <= 5000; ++uid) {
        const int64_t id = std::atoll(store->debug_query("SELECT id FROM message WHERE uid = " + std::to_string(uid)).c_str());
        std::string text;
        for (int w = 0; w < 300; ++w) text += std::string(words[(uid + w) % 10]) + " word" + std::to_string((uid * 31 + w) % 997) + " ";
        pbo::BodyUpdate u;
        u.text = text;
        PB_CHECK(store->apply_body(id, u).ok());
    }
    const auto t0 = std::chrono::steady_clock::now();
    size_t found = 0;
    for (const char* q : {"invoice", "meeting number", "word500", "s42@example", "contra deliv", "zzzz"}) found += store->search(q, 200).size();
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("search over 5000 messages, 6 queries: %.0f ms, %zu results (host proxy; the device figure is unverified)\n", ms, found);
    PB_CHECK(found > 0);
    PB_CHECK(ms < 3000.0);
}

PB_TEST_MAIN()
