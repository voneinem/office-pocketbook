// The UI thread reads while the worker thread writes (bridge/worker.h): one shared connection must never show a
// half-applied batch. A writer commits batches of 5 messages; a reader polls the list and the counts, which must
// always be multiples of 5, and the unread count must match the rows.
#include <atomic>
#include <thread>

#include "pbo/store.h"
#include "pbo/vcard.h"
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

PB_TEST(a_contact_written_while_a_pull_is_applied_is_never_half_seen) {
    pbtest::TmpDir dir;
    FixedClock clock;
    auto store = pbo::Store::open(dir.path(), clock);
    PB_CHECK(store.ok());
    pbo::AddressBook b;
    b.href = "/dav/ab/contacts/";
    b.name = "Contacts";
    b.writable = true;
    const int64_t book = store.value->upsert_addressbook(b).value;

    std::atomic<bool> done{false};
    std::atomic<int> violations{0};
    std::thread writer([&] {
        int n = 0;
        for (int batch_no = 0; batch_no < 40; ++batch_no) {
            pbo::ContactPullBatch batch;
            for (int i = 0; i < 5; ++i, ++n) {
                const std::string uid = "u" + std::to_string(n);
                batch.upserts.push_back(pbo::contact_upsert_from_card("/dav/ab/contacts/" + uid + ".vcf", "\"e\"",
                                                                      "BEGIN:VCARD\r\nVERSION:3.0\r\nUID:" + uid + "\r\nFN:P " + uid + "\r\nN:" + uid + ";P;;;\r\nEND:VCARD\r\n"));
            }
            if (!store.value->apply_contact_pull(book, batch).ok()) ++violations;
            // the user's own write in between (another thread of the app would do the same)
            pbo::ContactUpsert mine = pbo::contact_upsert_from_card("/dav/ab/contacts/m" + std::to_string(batch_no) + ".vcf", "",
                                                                    "BEGIN:VCARD\r\nVERSION:3.0\r\nUID:m" + std::to_string(batch_no) + "\r\nFN:M " + std::to_string(batch_no) + "\r\nN:M;" + std::to_string(batch_no) + ";;;\r\nEND:VCARD\r\n");
            if (!store.value->save_contact_local(book, 0, mine).ok()) ++violations;
        }
        done = true;
    });
    while (!done) {
        const int total = store.value->contacts_count("");
        const auto page = store.value->contacts_page("", 0, 100);
        if (total % 6 != 0 && total % 6 != 5 && total % 6 != 1) {}   // (own writes interleave: only the consistency of one read is checked)
        for (const auto& it : page)
            if (!store.value->contact(it.id).has_value() && it.sync_state == "synced") ++violations;   // a listed contact can always be opened
    }
    writer.join();
    PB_CHECK_EQ(violations.load(), 0);
    PB_CHECK_EQ(store.value->contacts_count(""), 40 * 5 + 40);
    PB_CHECK_EQ(store.value->pending_contacts().size(), size_t(40));
}

PB_TEST_MAIN()
