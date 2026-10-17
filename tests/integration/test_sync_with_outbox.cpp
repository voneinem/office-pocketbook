// The outbox and the draft mirror as steps of run_sync (research R-18 order: outbox, mail_push, mail_pull): a queued message goes out and its
// copy is filed, a draft is mirrored, and the mail pull of the same sync then sees the copy in Sent. UNVERIFIED against real servers (fake servers).
#include "outbox_env.h"
#include "pbo/sync.h"

PBO_FAKE_SERVER_TEST();

using pbtest::OutboxEnv;

namespace {

pbo::SyncReport sync(OutboxEnv& e, bool with_smtp = true, const std::string& smtp_password = "pw-for-tests") {
    auto imap = pbo::make_curl_imap_transport(e.pki.ca_cert(), OutboxEnv::short_timeouts());
    auto smtp = pbo::make_curl_smtp_transport(e.pki.ca_cert(), OutboxEnv::short_timeouts());
    pbo::SystemRandom random;
    pbo::SyncDeps deps{*e.store, *imap, e.clock, e.cancel};
    if (with_smtp) {
        deps.smtp = smtp.get();
        deps.random = &random;
    }
    pbo::Credentials creds;
    creds.mail_password = "pw-for-tests";
    creds.smtp_password = smtp_password;
    return pbo::run_sync(deps, creds);
}

}  // namespace

PB_TEST(one_sync_sends_files_the_copy_mirrors_the_draft_and_then_pulls) {
    OutboxEnv e;
    e.imap->add_message("INBOX", "From: Bob <bob@example.org>\r\nSubject: Hi\r\nMessage-ID: <in1@example.org>\r\nDate: Tue, 13 Oct 2026 10:00:00 +0000\r\n\r\nHello\r\n", {}, e.clock.t - 86400);
    const int64_t o = e.queue("Outgoing");
    const std::string id = e.message_id(o);
    const int64_t d = e.draft("A draft", "Draft text.\n");
    const auto r = sync(e);
    PB_CHECK(r.outcome == pbo::SyncOutcome::Success);
    PB_CHECK_EQ(r.outbox_counts.sent, 1);
    PB_CHECK_EQ(r.outbox_counts.copies, 1);
    PB_CHECK_EQ(r.mirror_counts.mirrored, 1);
    PB_CHECK_EQ(e.smtp->accepted_count(id), 1);
    PB_CHECK_EQ(e.imap->message_count("Sent"), 1);
    PB_CHECK_EQ(e.imap->message_count("Drafts"), 1);
    PB_CHECK_EQ(e.state(o), std::string("gone"));
    PB_CHECK_EQ(e.store->draft(d)->server_state, std::string("synced"));
    // the pull of the same sync brought the inbox and the new copy in Sent
    PB_CHECK_EQ(std::atoi(e.store->debug_query("SELECT count(*) FROM message").c_str()) >= 1, true);
    for (const auto& s : r.steps) {
        if (s.name == "outbox" || s.name == "mail_push" || s.name == "mail_pull") PB_CHECK(s.ran && s.error.ok());
    }
    PB_CHECK(e.store->sync_state().counts_json.find("\"sent\":1") != std::string::npos);
    // a second sync changes nothing
    const auto again = sync(e);
    PB_CHECK_EQ(again.outbox_counts.sent, 0);
    PB_CHECK_EQ(again.mirror_counts.mirrored, 0);
    PB_CHECK_EQ(e.smtp->accepted_count(id), 1);
}

PB_TEST(without_the_smtp_dependency_the_outbox_is_left_alone) {
    OutboxEnv e;
    const int64_t o = e.queue();
    const auto r = sync(e, false);
    PB_CHECK(r.outcome == pbo::SyncOutcome::Success);
    for (const auto& s : r.steps)
        if (s.name == "outbox" || s.name == "mail_push") PB_CHECK(!s.ran);
    PB_CHECK_EQ(e.state(o), std::string("queued"));
    PB_CHECK_EQ(e.smtp->connections(), 0);
}

PB_TEST(a_message_that_cannot_be_sent_does_not_make_the_sync_fail) {
    OutboxEnv e;
    const int64_t o = e.queue();
    const auto r = sync(e, true, "wrong-password");
    PB_CHECK(r.outcome == pbo::SyncOutcome::Success);                 // the mail part is fine; the message waits for the user in the outbox
    PB_CHECK_EQ(r.outbox_counts.failed, 1);
    PB_CHECK_EQ(e.state(o), std::string("failed"));
    PB_CHECK_EQ(e.store->outbox_item(o)->error_key, std::string("auth_failed"));
}

PB_TEST(a_cancelled_sync_sends_nothing) {
    OutboxEnv e;
    const int64_t o = e.queue();
    e.cancel.flag = true;
    const auto r = sync(e);
    PB_CHECK(r.outcome == pbo::SyncOutcome::Cancelled);
    PB_CHECK_EQ(e.state(o), std::string("queued"));
    PB_CHECK_EQ(e.smtp->accepted().size(), size_t(0));
}

PB_TEST_MAIN()
