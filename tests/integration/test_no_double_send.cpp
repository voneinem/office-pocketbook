// T121 (SC-007): across every injection point, with repeated Send taps, repeated syncs and retries, the SMTP server accepts a given Message-ID at most
// once - unless the user chose "Send again" from "uncertain". Every message whose outcome is unknown ends in "uncertain" and is never resent by
// itself. UNVERIFIED against real servers (fake servers).
#include "outbox_env.h"

PBO_FAKE_SERVER_TEST();

using pbtest::OutboxEnv;

namespace {
std::string mid(OutboxEnv& e, int64_t outbox_id) { return e.message_id(outbox_id); }
}  // namespace

PB_TEST(a_normal_send_is_accepted_once_however_often_the_step_runs) {
    OutboxEnv e;
    const int64_t o = e.queue();
    const std::string id = mid(e, o);
    const auto first = e.run();
    PB_CHECK_EQ(first.sent, 1);
    PB_CHECK_EQ(e.smtp->accepted_count(id), 1);
    for (int i = 0; i < 4; ++i) e.run();
    PB_CHECK_EQ(e.smtp->accepted_count(id), 1);
}

PB_TEST(repeated_send_taps_make_one_outbox_row_and_one_message) {
    OutboxEnv e;
    pbo::Draft d;
    d.message_id = "<tap@example.org>";
    d.to_text = "bob@example.org";
    d.subject = "Tapped";
    const int64_t id = e.store->save_draft(d).value;
    pbo::OutboxRequest q;
    q.expected_hash = e.store->draft(id)->content_hash;
    q.envelope_from = "alice@example.org";
    q.recipients = {"bob@example.org"};
    q.raw_message = "From: alice@example.org\r\nTo: bob@example.org\r\nSubject: Tapped\r\nMessage-ID: <tap@example.org>\r\n\r\nx\r\n";
    int queued = 0;
    for (int i = 0; i < 5; ++i)
        if (e.store->queue_draft(id, q).ok()) ++queued;
    PB_CHECK_EQ(queued, 1);
    PB_CHECK_EQ(e.store->outbox().size(), size_t(1));
    e.run();
    e.run();
    PB_CHECK_EQ(e.smtp->accepted_count("<tap@example.org>"), 1);
}

PB_TEST(an_unknown_outcome_becomes_uncertain_and_is_never_resent_by_a_sync) {
    OutboxEnv e;
    const int64_t o = e.queue();
    e.smtp->drop_before_final_reply = true;               // the server has the message and never says so
    const auto c = e.run();
    PB_CHECK_EQ(c.uncertain, 1);
    PB_CHECK_EQ(e.state(o), std::string("uncertain"));
    PB_CHECK(!e.store->outbox_item(o)->error_key.empty());
    PB_CHECK_EQ(e.smtp->accepted_count(mid(e, o)), 1);
    e.smtp->drop_before_final_reply = false;
    for (int i = 0; i < 5; ++i) e.run();                  // sync after sync: the message is not sent again
    PB_CHECK_EQ(e.smtp->accepted_count(mid(e, o)), 1);
    PB_CHECK_EQ(e.state(o), std::string("uncertain"));
    // only the user's "Send again" sends it again, with the identical bytes
    const std::string raw = e.store->outbox_item(o)->raw_message;
    PB_CHECK(e.store->send_again(o).ok());
    e.run();
    PB_CHECK_EQ(e.smtp->accepted_count(mid(e, o)), 2);
    PB_CHECK_EQ(e.smtp->accepted().back().data, raw);
}

PB_TEST(it_was_sent_completes_the_row_without_another_send) {
    OutboxEnv e;
    const int64_t o = e.queue();
    const std::string id = mid(e, o);
    e.smtp->drop_before_final_reply = true;
    e.run();
    e.smtp->drop_before_final_reply = false;
    PB_CHECK(e.store->mark_was_sent(o).ok());
    const auto c = e.run();
    PB_CHECK_EQ(c.sent, 0);
    PB_CHECK_EQ(c.copies, 1);
    PB_CHECK_EQ(e.state(o), std::string("gone"));
    PB_CHECK_EQ(e.smtp->accepted_count(id), 1);
}

PB_TEST(a_connection_lost_before_the_end_of_the_text_fails_and_retry_sends_once) {
    OutboxEnv e;
    const int64_t o = e.queue();
    auto item = *e.store->outbox_item(o);
    e.smtp->drop_after_354 = true;
    e.run();
    // small message: the whole text may already have left, so the honest answer can be uncertain; either way it is not sent twice
    const std::string st = e.state(o);
    PB_CHECK(st == "failed" || st == "uncertain");
    PB_CHECK_EQ(e.smtp->accepted_count(item.message_id), 0);
    e.smtp->drop_after_354 = false;
    for (int i = 0; i < 3; ++i) e.run();
    PB_CHECK_EQ(e.smtp->accepted_count(item.message_id), 0);          // neither failed nor uncertain rows are retried by themselves
    if (st == "failed") PB_CHECK(e.store->retry_outbox(o).ok());
    else PB_CHECK(e.store->send_again(o).ok());
    e.run();
    PB_CHECK_EQ(e.smtp->accepted_count(item.message_id), 1);
}

PB_TEST(a_refused_recipient_or_a_wrong_password_is_failed_and_waits_for_the_user) {
    OutboxEnv e;
    const int64_t a = e.queue("Refused", "bob@example.org");
    e.smtp->refuse_recipient("bob@example.org");
    PB_CHECK_EQ(e.run().failed, 1);
    PB_CHECK_EQ(e.state(a), std::string("failed"));
    PB_CHECK_EQ(e.store->outbox_item(a)->error_key, std::string("refused_recipient"));
    e.run();
    PB_CHECK_EQ(e.state(a), std::string("failed"));                    // not retried by itself
    const int64_t b = e.queue("Password", "carol@example.org");
    e.run("wrong");
    PB_CHECK_EQ(e.state(b), std::string("failed"));
    PB_CHECK_EQ(e.store->outbox_item(b)->error_key, std::string("auth_failed"));
    PB_CHECK_EQ(e.smtp->accepted().size(), size_t(0));
    // the user fixes the cause and taps Retry
    const std::string bid = mid(e, b);
    PB_CHECK(e.store->retry_outbox(b).ok());
    PB_CHECK_EQ(e.run().sent, 1);
    PB_CHECK_EQ(e.smtp->accepted_count(bid), 1);
    e.smtp->refuse_recipient("none@example.org");
}

PB_TEST(without_a_connection_the_message_waits_in_the_queue_and_goes_out_later) {
    OutboxEnv e;
    const int64_t o = e.queue();
    const std::string id = mid(e, o);
    auto a = *e.store->mail_account();
    const int good_port = a.smtp_port;
    a.smtp_port = 1;                                         // nothing listens there
    e.store->save_mail_account(a);
    const auto c = e.run();
    PB_CHECK_EQ(c.requeued, 1);
    PB_CHECK_EQ(e.state(o), std::string("queued"));
    PB_CHECK_EQ(e.store->outbox_item(o)->error_key, std::string("server_unreachable"));
    a.smtp_port = good_port;
    e.store->save_mail_account(a);
    PB_CHECK_EQ(e.run().sent, 1);
    PB_CHECK_EQ(e.smtp->accepted_count(id), 1);
    PB_CHECK_EQ(e.state(o), std::string("gone"));
}

PB_TEST(a_message_found_in_sending_after_a_crash_is_uncertain_and_not_resent) {
    OutboxEnv e;
    const int64_t o = e.queue();
    PB_CHECK(e.store->begin_sending(o).value);                          // the app dies after this commit
    int n = 0;
    PB_CHECK(e.store->recover_interrupted(&n).ok());                    // what Store::open does at the next start
    PB_CHECK_EQ(n, 1);
    for (int i = 0; i < 3; ++i) e.run();
    PB_CHECK_EQ(e.smtp->accepted().size(), size_t(0));
    PB_CHECK_EQ(e.state(o), std::string("uncertain"));
    PB_CHECK_EQ(e.store->outbox_item(o)->error_key, std::string("interrupted"));
}

PB_TEST(the_matrix_of_injection_points_never_produces_a_duplicate) {
    struct Case {
        const char* name;
        void (*arm)(pbtest::FakeSmtpServer&);
        void (*disarm)(pbtest::FakeSmtpServer&);
    };
    const Case cases[] = {
        {"drop after 354", [](pbtest::FakeSmtpServer& s) { s.drop_after_354 = true; }, [](pbtest::FakeSmtpServer& s) { s.drop_after_354 = false; }},
        {"drop in the body", [](pbtest::FakeSmtpServer& s) { s.drop_in_body_after = 10; }, [](pbtest::FakeSmtpServer& s) { s.drop_in_body_after = -1; }},
        {"drop before the final reply", [](pbtest::FakeSmtpServer& s) { s.drop_before_final_reply = true; }, [](pbtest::FakeSmtpServer& s) { s.drop_before_final_reply = false; }},
        {"drop after the final reply", [](pbtest::FakeSmtpServer& s) { s.drop_after_reply = true; }, [](pbtest::FakeSmtpServer& s) { s.drop_after_reply = false; }},
        {"reject the data (554)", [](pbtest::FakeSmtpServer& s) { s.reject_data_code = 554; }, [](pbtest::FakeSmtpServer& s) { s.reject_data_code = 0; }},
        {"temporary error (451)", [](pbtest::FakeSmtpServer& s) { s.reject_data_code = 451; }, [](pbtest::FakeSmtpServer& s) { s.reject_data_code = 0; }},
        {"refused recipient", [](pbtest::FakeSmtpServer& s) { s.refuse_recipient("bob@example.org"); }, [](pbtest::FakeSmtpServer&) {}},
    };
    for (const Case& c : cases) {
        OutboxEnv e;
        const int64_t o = e.queue();
        const std::string id = mid(e, o);
        c.arm(*e.smtp);
        for (int i = 0; i < 3; ++i) e.run();                              // repeated syncs while the fault is on
        if (e.smtp->accepted_count(id) > 1) PB_FAIL_AT(std::string(c.name) + ": duplicate while the fault was on");
        c.disarm(*e.smtp);
        for (int i = 0; i < 3; ++i) e.run();                              // and after it is gone: failed and uncertain rows still wait for the user
        const std::string st = e.state(o);
        if (st == "queued" || st == "sending") PB_FAIL_AT(std::string(c.name) + ": left in " + st);
        if (e.smtp->accepted_count(id) > 1) PB_FAIL_AT(std::string(c.name) + ": duplicate after the fault");
        if (st == "uncertain" || st == "failed") {
            // the user decides; the unknown outcome is shown as "may have been sent"
            if (st == "uncertain") PB_CHECK(e.store->send_again(o).ok());
            else PB_CHECK(e.store->retry_outbox(o).ok());
            e.smtp->refuse_recipient("nobody@example.org");
            e.run();
            const int n = e.smtp->accepted_count(id);
            if (std::string(c.name) == "refused recipient") PB_CHECK(n <= 1);
            else PB_CHECK(n >= 1 && n <= 2);                              // 2 only if the first attempt was accepted before the connection broke
        }
    }
}

PB_TEST_MAIN()
