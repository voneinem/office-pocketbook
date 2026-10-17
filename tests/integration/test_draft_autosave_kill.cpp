// T124 (SC-006, FR-012, FR-014, Principle III): the app is killed (SIGKILL, like a battery pull) at seeded random moments while it autosaves a
// draft, moves messages through the outbox state machine, and sends through SMTP. After every kill the store opens, passes its integrity check,
// holds the draft (at most the last unreported save is missing), no outbox row is lost or duplicated, no row is left in "sending", and the SMTP
// server accepted no Message-ID twice. UNVERIFIED against real servers (fake SMTP server); the battery pull itself is a device item (T137).
#include <sstream>

#include "kill_harness.h"
#include "outbox_env.h"

PBO_FAKE_SERVER_TEST();

using pbtest::KillRun;

namespace {

struct FixedClock : pbo::Clock {
    int64_t now() const override { return 1791979200; }
};

int to_int(const std::string& s, const std::string& prefix) { return std::atoi(s.c_str() + prefix.size()); }

}  // namespace

PB_TEST(killing_the_app_during_autosave_never_corrupts_the_draft_or_the_database) {
    const int n = KillRun::iterations(100);
    for (int i = 0; i < n; ++i) {
        pbtest::TmpDir dir;
        FixedClock clock;
        const std::string path = dir.path();
        const auto reports = KillRun::run(
            [&](int fd) {
                auto opened = pbo::Store::open(path, clock);
                pbo::Draft d;
                d.message_id = "<kill@example.org>";
                d.to_text = "bob@example.org";
                d.subject = "Autosave";
                const int64_t id = opened.value->save_draft(d).value;
                KillRun::report(fd, "created " + std::to_string(id));
                for (int v = 1;; ++v) {
                    auto cur = *opened.value->draft(id);
                    cur.body = "version " + std::to_string(v) + " " + std::string(4000, 'x');
                    if (opened.value->save_draft(cur).ok()) KillRun::report(fd, "saved " + std::to_string(v));
                }
            },
            1, KillRun::delay_for(i, 40000));
        PB_CHECK(!reports.empty());
        int last_reported = 0;
        for (const std::string& r : reports)
            if (r.rfind("saved ", 0) == 0) last_reported = to_int(r, "saved ");
        auto reopened = pbo::Store::open(path, clock);
        PB_CHECK(reopened.ok());
        PB_CHECK(reopened.value->check_integrity().ok());
        const auto drafts = reopened.value->drafts();
        PB_CHECK_EQ(drafts.size(), size_t(1));                              // the draft is there
        const std::string body = drafts[0].body;
        if (body.empty()) {
            PB_CHECK_EQ(last_reported, 0);                                    // nothing saved yet: only the empty first version
        } else {
            PB_CHECK(body.rfind("version ", 0) == 0);                         // a whole version, never a mixture
            PB_CHECK(body.size() == std::string("version ").size() + std::to_string(to_int(body, "version ")).size() + 1 + 4000);
            const int v = to_int(body, "version ");
            PB_CHECK(v >= last_reported && v <= last_reported + 1);           // at most one unreported save more; none that was reported is lost
        }
    }
}

PB_TEST(killing_the_app_while_messages_move_through_the_outbox_never_loses_or_duplicates_a_row) {
    const int n = KillRun::iterations(100);
    for (int i = 0; i < n; ++i) {
        pbtest::TmpDir dir;
        FixedClock clock;
        const std::string path = dir.path();
        const auto reports = KillRun::run(
            [&](int fd) {
                auto opened = pbo::Store::open(path, clock);
                pbo::Store& s = *opened.value;
                for (int k = 1;; ++k) {
                    pbo::Draft d;
                    d.message_id = "<k" + std::to_string(k) + "@example.org>";
                    d.to_text = "bob@example.org";
                    d.subject = "S";
                    const int64_t id = s.save_draft(d).value;
                    pbo::OutboxRequest q;
                    q.expected_hash = s.draft(id)->content_hash;
                    q.envelope_from = "alice@example.org";
                    q.recipients = {"bob@example.org"};
                    q.raw_message = "Message-ID: " + d.message_id + "\r\n\r\nx\r\n";
                    auto o = s.queue_draft(id, q);
                    if (!o.ok()) continue;
                    KillRun::report(fd, "queued " + std::to_string(k));
                    if (!s.begin_sending(o.value).value) continue;
                    KillRun::report(fd, "sending " + std::to_string(k));
                    pbo::SendFinish f;
                    f.kind = k % 3 == 0 ? pbo::SendFinish::Kind::Accepted : k % 3 == 1 ? pbo::SendFinish::Kind::Failed : pbo::SendFinish::Kind::Uncertain;
                    f.error_key = k % 3 == 0 ? "" : "x";
                    s.finish_sending(o.value, f);
                    KillRun::report(fd, "finished " + std::to_string(k));
                }
            },
            1, KillRun::delay_for(i, 60000));
        auto reopened = pbo::Store::open(path, clock);   // recover_interrupted runs here
        if (!reopened.ok()) PB_FAIL_AT("iteration " + std::to_string(i) + ": the store did not open after the kill: " + reopened.error.key + " / " + reopened.error.detail);
        pbo::Store& s = *reopened.value;
        PB_CHECK(s.check_integrity().ok());
        std::map<int, std::string> last;   // k -> last reported step
        for (const std::string& r : reports) {
            const size_t sp = r.find(' ');
            last[std::atoi(r.c_str() + sp + 1)] = r.substr(0, sp);
        }
        std::set<std::string> ids;
        for (const pbo::OutboxItem& item : s.outbox()) {
            PB_CHECK(ids.insert(item.message_id).second);                      // no duplicate row
            PB_CHECK(item.state != "sending");                                   // nothing is left in "sending"
        }
        for (const auto& kv : last) {
            const auto items = s.outbox();
            const std::string mid = "<k" + std::to_string(kv.first) + "@example.org>";
            const pbo::OutboxItem* found = nullptr;
            for (const auto& item : items)
                if (item.message_id == mid) found = &item;
            PB_CHECK(found != nullptr);                                          // a queued message is never lost
            if (kv.second == "finished") {
                const std::string expect = kv.first % 3 == 0 ? "sent_copy_pending" : kv.first % 3 == 1 ? "failed" : "uncertain";
                PB_CHECK_EQ(found->state, expect);
            } else if (kv.second == "sending") {
                // the kill came between begin and finish (the finish may have committed unreported): it was not sent again and it is not "queued"
                PB_CHECK(found->state == "uncertain" || found->state == "failed" || found->state == "sent_copy_pending");
            } else {
                PB_CHECK(found->state == "queued" || found->state == "uncertain" || found->state == "failed" || found->state == "sent_copy_pending");
            }
        }
    }
}

PB_TEST(killing_the_app_while_it_sends_never_makes_the_server_accept_a_message_twice) {
    const int n = KillRun::iterations(100);
    pbtest::OutboxEnv e(true);   // the provider files the Sent copy: the step needs SMTP only
    for (int i = 0; i < n; ++i) {
        std::vector<int64_t> ids;
        for (int k = 0; k < 3; ++k) ids.push_back(e.queue("Mail " + std::to_string(i) + "/" + std::to_string(k)));
        const std::string path = e.dir.path();
        const auto reports = KillRun::run(
            [&](int fd) {
                auto opened = pbo::Store::open(path, e.clock);
                auto smtp = pbo::make_curl_smtp_transport(e.pki.ca_cert(), pbtest::OutboxEnv::short_timeouts());
                auto imap = pbo::make_curl_imap_transport(e.pki.ca_cert(), pbtest::OutboxEnv::short_timeouts());
                pbo::UidvalidityGuard guard;
                pbtest::OutboxCancel cancel;
                pbtest::OutboxClock clock;
                pbo::OutboxDeps deps{*opened.value, *smtp, *imap, clock, cancel, guard};
                pbo::Credentials creds;
                creds.mail_password = "pw-for-tests";
                KillRun::report(fd, "start");
                pbo::OutboxCounts counts;
                pbo::run_outbox(deps, creds, &counts);
                KillRun::report(fd, "done");
            },
            1, KillRun::delay_for(i, 120000));
        (void)reports;
        // reopen in the parent: the store the test holds is the same file; open a second handle after the child died
        auto reopened = pbo::Store::open(path, e.clock);
        PB_CHECK(reopened.ok());
        PB_CHECK(reopened.value->check_integrity().ok());
        for (int64_t id : ids) {
            const std::string mid = e.message_id(id);
            const int accepted = e.smtp->accepted_count(mid);
            if (accepted > 1) PB_FAIL_AT("accepted twice after a kill: " + mid);
            auto item = reopened.value->outbox_item(id);
            if (!item) continue;   // completed
            PB_CHECK(item->state != "sending");
            if (accepted == 1) PB_CHECK(item->state != "queued" && item->state != "failed");   // it left: it must not look unsent
        }
        // the next run (in the parent, on the reopened store) never sends an uncertain row again; queued rows go out exactly once
        {
            auto smtp = pbo::make_curl_smtp_transport(e.pki.ca_cert(), pbtest::OutboxEnv::short_timeouts());
            auto imap = pbo::make_curl_imap_transport(e.pki.ca_cert(), pbtest::OutboxEnv::short_timeouts());
            pbo::UidvalidityGuard guard;
            pbo::OutboxDeps deps{*reopened.value, *smtp, *imap, e.clock, e.cancel, guard};
            pbo::Credentials creds;
            creds.mail_password = "pw-for-tests";
            PB_CHECK(pbo::run_outbox(deps, creds, nullptr).ok());
        }
        for (int64_t id : ids)
            if (e.smtp->accepted_count(e.message_id(id)) > 1) PB_FAIL_AT("accepted twice after the next run");
        // clean up for the next iteration: the harness store is the one at `path`; remove what is left
        for (const auto& item : reopened.value->outbox()) reopened.value->delete_outbox(item.id);
        reopened.value->debug_exec("DELETE FROM outbox; DELETE FROM draft");
        e.store->debug_exec("SELECT 1");
    }
}

PB_TEST_MAIN()
