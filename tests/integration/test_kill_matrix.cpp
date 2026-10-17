// T180 (SC-006, Principle III): the global kill matrix. The app is killed (SIGKILL, like a battery pull) at seeded random moments while ONE WHOLE SYNC runs:
// send, file the Sent copy, mirror drafts, push flag changes, pull. The parts have their own kill tests (draft autosave, outbox, event edit); this one
// kills the sync that strings them together. After every kill: the store opens and passes its integrity check, no draft and no queued message is lost,
// no message was accepted twice, a message of unknown outcome is not sent again by the next sync, and the next sync finishes what was left - once.
// UNVERIFIED against real servers (fake servers).
#include <algorithm>
#include <cstdio>
#include <set>

#include "kill_harness.h"
#include "outbox_env.h"
#include "pbo/sync.h"

PBO_FAKE_SERVER_TEST();

using pbtest::KillRun;
using pbtest::OutboxEnv;

namespace {

pbo::SyncReport run(pbo::Store& store, OutboxEnv& e) {
    auto imap = pbo::make_curl_imap_transport(e.pki.ca_cert(), OutboxEnv::short_timeouts());
    auto smtp = pbo::make_curl_smtp_transport(e.pki.ca_cert(), OutboxEnv::short_timeouts());
    pbo::SystemRandom random;
    pbtest::OutboxClock clock;
    pbtest::OutboxCancel cancel;
    pbo::SyncDeps deps{store, *imap, clock, cancel};
    deps.smtp = smtp.get();
    deps.random = &random;
    pbo::Credentials creds;
    creds.mail_password = "pw-for-tests";
    creds.smtp_password = "pw-for-tests";
    pbo::SyncOptions options;
    options.calendar = false;
    return pbo::run_sync(deps, creds, options);
}

int count_with_id(const std::vector<std::pair<uint32_t, std::string>>& messages, const std::string& message_id) {
    int n = 0;
    for (const auto& m : messages)
        if (m.second.find("Message-ID: " + message_id) != std::string::npos) ++n;
    return n;
}

}  // namespace

PB_TEST(killing_a_whole_sync_never_loses_or_doubles_a_draft_a_message_or_a_change) {
    const int n = KillRun::iterations(100);
    OutboxEnv e;
    for (int k = 1; k <= 6; ++k)
        e.imap->add_message("INBOX", "From: Bob <bob@example.org>\r\nSubject: Mail " + std::to_string(k) + "\r\nMessage-ID: <in" + std::to_string(k) +
                                         "@example.org>\r\nDate: Tue, 13 Oct 2026 10:00:00 +0000\r\n\r\nHello\r\n", {}, e.clock.t - 86400);
    PB_CHECK(run(*e.store, e).outcome == pbo::SyncOutcome::Success);   // the six messages are on the reader
    const std::string path = e.dir.path();

    int interrupted = 0;
    std::set<std::string> uncertain_before;   // a message of unknown outcome is never sent by a later sync
    for (int i = 0; i < n; ++i) {
        // local work of this round: two messages to send, one draft to mirror, one flag change
        const std::string tag = "r" + std::to_string(i);
        std::vector<int64_t> outbox_ids = {e.queue("Send " + tag + "a"), e.queue("Send " + tag + "b")};
        const int64_t draft_id = e.draft("Draft " + tag, "Text of " + tag + ".\n");
        const std::string draft_mid = e.store->draft(draft_id)->message_id;
        const uint32_t uid = static_cast<uint32_t>(1 + i % 6);
        const bool server_seen = e.imap->flags_of("INBOX", uid).count("\\Seen") > 0;
        const int64_t msg = std::atoll(e.store->debug_query("SELECT id FROM message WHERE uid = " + std::to_string(uid) + " AND stale = 0").c_str());
        PB_CHECK(msg > 0);
        PB_CHECK(server_seen ? e.store->set_flags_local(msg, 0, pbo::kFlagSeen).ok() : e.store->set_flags_local(msg, pbo::kFlagSeen, 0).ok());

        const auto reports = KillRun::run([&](int fd) {
            auto opened = pbo::Store::open(path, e.clock);
            if (!opened.ok()) return;
            KillRun::report(fd, "start");
            run(*opened.value, e);
            KillRun::report(fd, "done");
        }, 1, KillRun::delay_for(i, 3000000));
        if (std::find(reports.begin(), reports.end(), "done") == reports.end()) ++interrupted;   // the kill came before the sync had finished

        auto reopened = pbo::Store::open(path, e.clock);
        if (!reopened.ok()) PB_FAIL_AT("iteration " + std::to_string(i) + ": the store did not open after the kill: " + reopened.error.key);
        PB_CHECK(reopened.value->check_integrity().ok());
        PB_CHECK(reopened.value->draft(draft_id).has_value());                 // the draft is never lost by a kill
        for (int64_t id : outbox_ids) {
            const std::string mid = e.message_id(id);
            if (e.smtp->accepted_count(mid) > 1) PB_FAIL_AT("iteration " + std::to_string(i) + ": accepted twice after the kill: " + mid);
            const auto item = reopened.value->outbox_item(id);
            if (!item) {   // gone: it was completed, so the server must have accepted it
                PB_CHECK_EQ(e.smtp->accepted_count(mid), 1);
                continue;
            }
            PB_CHECK(item->state != "sending");                                 // a state the user can see and decide on
            if (e.smtp->accepted_count(mid) == 1) PB_CHECK(item->state != "queued" && item->state != "failed");
            if (item->state == "uncertain") uncertain_before.insert(mid);
        }

        // the next sync, on the reopened store, finishes what was left
        const auto next = run(*reopened.value, e);
        PB_CHECK(next.outcome == pbo::SyncOutcome::Success || next.outcome == pbo::SyncOutcome::Partial);
        for (int64_t id : outbox_ids) {
            const std::string mid = e.message_id(id);
            const int accepted = e.smtp->accepted_count(mid);
            if (accepted > 1) PB_FAIL_AT("iteration " + std::to_string(i) + ": accepted twice after the next sync: " + mid);
            if (uncertain_before.count(mid)) {
                PB_CHECK(accepted <= 1);   // an unknown outcome is not resent: still at most the one the server may already have had
            } else {
                PB_CHECK_EQ(accepted, 1);  // everything else went out, once
            }
            PB_CHECK(count_with_id(e.imap->messages("Sent"), mid) <= 1);        // at most one filed copy
        }
        PB_CHECK_EQ(count_with_id(e.imap->messages("Drafts"), draft_mid), 1);   // the mirrored draft: one copy, not none, not two
        const bool want_seen = !server_seen;
        PB_CHECK_EQ(e.imap->flags_of("INBOX", uid).count("\\Seen") > 0, want_seen);   // the change reached the server, once
        // clean up the round (the uncertain rows stay for the user in real life; the test removes them so that the rounds do not pile up)
        reopened.value->debug_exec("DELETE FROM outbox; DELETE FROM draft");
    }
    std::printf("kill matrix: %d of %d syncs were killed before they finished\n", interrupted, n);
    if (n >= 50) PB_CHECK(interrupted >= n / 5);   // the test is only worth something if the kills really land inside the sync
}

PB_TEST_MAIN()
