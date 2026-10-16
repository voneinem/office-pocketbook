// T155 (K4, Principle V): the memory and time budgets of research R-19 measured on the HOST as proxies. Every figure is printed; none of them is a
// device value - the reader stays UNVERIFIED until T177/T186. Peak RSS is the process high-water mark (ru_maxrss), so the checks run in the order
// of the budgets and each limit holds for everything done before it.
#include <sys/resource.h>

#include <chrono>
#include <cstdio>
#include <string>

#include "fake_imap_server.h"
#include "pbo/imap_transport.h"
#include "pbo/store.h"
#include "pbo/sync.h"
#include "test_support.h"
#include "tls_test_server.h"
#include "tmp_dir.h"

PBO_FAKE_SERVER_TEST();

namespace {

constexpr long kBrowseBudgetKb = 150 * 1024;   // research R-19: 150 MB while browsing
constexpr long kSyncBudgetKb = 250 * 1024;     // 250 MB during a sync
constexpr double kOpenBudgetS = 2.0;           // a stored 1 MB message opens in under 2 s (SC-005)

struct Clock : pbo::Clock {
    int64_t t = 1791979200;
    int64_t now() const override { return t; }
};
struct NoCancel : pbo::Cancel {
    bool requested() const override { return false; }
};

// Under AddressSanitizer the memory use is several times higher: the budgets are for the real build
#if defined(__SANITIZE_ADDRESS__)
constexpr bool kSanitized = true;
#else
constexpr bool kSanitized = false;
#endif

long peak_kb() {
    rusage r{};
    getrusage(RUSAGE_SELF, &r);
    return r.ru_maxrss;
}

}  // namespace

PB_TEST(browsing_a_folder_of_5000_headers_stays_within_the_memory_budget) {
    pbtest::TmpDir dir;
    Clock clock;
    auto store = std::move(pbo::Store::open(dir.path(), clock).value);
    pbo::FolderListItem i;
    i.raw_name = i.name = "INBOX";
    store->upsert_folders({i});
    const int64_t f = store->folders()[0].id;
    for (uint32_t base = 0; base < 5000; base += 50) {
        pbo::PullBatch b;
        for (uint32_t k = 0; k < 50; ++k) {
            pbo::PulledMessage m;
            m.uid = base + k + 1;
            m.subject = "Subject " + std::to_string(m.uid);
            m.from_name = "Sender";
            m.from_addr = "s@example.org";
            m.date_utc = 1700000000 + m.uid;
            b.messages.push_back(m);
        }
        PB_CHECK(store->apply_pull(f, b).ok());
    }
    const auto t0 = std::chrono::steady_clock::now();
    for (int page = 0; page < 100; ++page) PB_CHECK(!store->list_messages(f, pbo::SortKey::SenderAsc, page, 12).items.empty());
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("browse 100 pages of 5000 headers: %.0f ms, peak RSS %ld KB (budget %ld KB; host proxy, device unverified)\n", ms, peak_kb(), kBrowseBudgetKb);
    if (!kSanitized) PB_CHECK(peak_kb() <= kBrowseBudgetKb);
}

PB_TEST(opening_a_stored_message_of_1_mb_takes_under_2_seconds) {
    pbtest::TmpDir dir;
    Clock clock;
    auto store = std::move(pbo::Store::open(dir.path(), clock).value);
    pbo::FolderListItem i;
    i.raw_name = i.name = "INBOX";
    store->upsert_folders({i});
    const int64_t f = store->folders()[0].id;
    pbo::PullBatch b;
    pbo::PulledMessage m;
    m.uid = 1;
    m.subject = "Large";
    m.date_utc = 1700000000;
    b.messages.push_back(m);
    PB_CHECK(store->apply_pull(f, b).ok());
    const int64_t id = std::atoll(store->debug_query("SELECT id FROM message").c_str());
    pbo::BodyUpdate u;
    for (int line = 0; line < 14000; ++line) u.text += "A line of about seventy characters that is part of a long message, number " + std::to_string(line) + "\n";
    PB_CHECK(u.text.size() > 1000000);
    PB_CHECK(store->apply_body(id, u).ok());
    const auto t0 = std::chrono::steady_clock::now();
    const auto v = store->message(id);
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    PB_CHECK(v.has_value());
    PB_CHECK(v->body_text.size() > 1000000);
    std::printf("open a stored 1 MB message: %.3f s (budget %.1f s; host proxy, device unverified)\n", s, kOpenBudgetS);
    PB_CHECK(s < kOpenBudgetS);
}

PB_TEST(the_first_sync_of_500_messages_stays_within_the_memory_budget_and_reports_its_time) {
    pbtest::TmpDir dir, pki_dir;
    pbtest::TestPki pki(pki_dir);
    if (!pki.available()) PB_SKIP("openssl CLI not available");
    auto leaf = pki.issue("srv", "DNS:localhost,IP:127.0.0.1");
    PB_CHECK(leaf.ok);
    pbtest::FakeImapServer::Config c;
    c.cert = leaf.cert;
    c.key = leaf.key;
    pbtest::FakeImapServer server(c);
    server.set_capabilities("IMAP4rev1 UIDPLUS LITERAL+ CONDSTORE");
    server.add_folder("INBOX");
    Clock clock;
    for (int n = 1; n <= 500; ++n)
        server.add_message("INBOX",
                           "From: Sender <s" + std::to_string(n) + "@example.org>\r\nSubject: Subject " + std::to_string(n) + "\r\nDate: Mon, 05 Oct 2026 10:00:00 +0000\r\nMessage-ID: <m" + std::to_string(n) +
                               "@example.org>\r\n\r\n" + std::string(2000, 'x') + "\r\n",
                           {}, clock.t - 86400);
    auto store = std::move(pbo::Store::open(dir.path(), clock).value);
    pbo::MailAccount a;
    a.imap_host = "localhost";
    a.imap_port = server.port();
    a.smtp_host = "localhost";
    a.username = "alice";
    a.address = "alice@example.org";
    store->save_mail_account(a);
    NoCancel cancel;
    auto transport = pbo::make_curl_imap_transport(pki.ca_cert());
    pbo::SyncDeps deps{*store, *transport, clock, cancel};
    pbo::Credentials creds;
    creds.mail_password = "pw-for-tests";
    pbo::SyncOptions options;
    options.calendar = false;
    const auto t0 = std::chrono::steady_clock::now();
    const auto report = pbo::run_sync(deps, creds, options);
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    PB_CHECK(report.outcome == pbo::SyncOutcome::Success);
    PB_CHECK_EQ(report.counts.new_messages, 500);
    std::printf("first sync of 500 messages against the fake server: %.2f s, peak RSS %ld KB (budget %ld KB; host proxy, device unverified)\n", s, peak_kb(), kSyncBudgetKb);
    if (!kSanitized) PB_CHECK(peak_kb() <= kSyncBudgetKb);
    PB_CHECK(s < 120.0);   // a generous host bound; the figure printed above is what counts
}

PB_TEST_MAIN()
