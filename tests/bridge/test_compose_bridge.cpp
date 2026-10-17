// T132: ComposeBridge (autosave 2 s after the last change, at leave and at suspend; reply/forward prefill; Send queues the message once) and
// OutboxModel (waiting / failed / may have been sent, the user's actions). The real core and a throw-away store.
#include <QCoreApplication>
#include <QSignalSpy>
#include <QTest>
#include <QVariantMap>

#include "compose_bridge.h"
#include "outbox_model.h"
#include "pbo/store.h"
#include "test_support.h"
#include "tmp_dir.h"

namespace {

int g_argc = 1;
char g_arg0[] = "test_compose_bridge";
char* g_argv[] = {g_arg0, nullptr};
QCoreApplication g_app(g_argc, g_argv);

struct Clock : pbo::Clock {
    int64_t now() const override { return 1791979200; }
};
struct Rng : pbo::Random {
    uint8_t n = 0;
    void bytes(uint8_t* out, size_t len) override {
        for (size_t i = 0; i < len; ++i) out[i] = n++;
    }
};

struct Env {
    pbtest::TmpDir dir;
    Clock clock;
    Rng rng;
    std::unique_ptr<pbo::Store> store;
    Env() {
        store = std::move(pbo::Store::open(dir.path(), clock).value);
        pbo::MailAccount a;
        a.imap_host = "mail.example.org";
        a.smtp_host = "smtp.example.org";
        a.username = "alice";
        a.address = "alice@example.org";
        a.display_name = "Alice Example";
        store->save_mail_account(a);
        pbo::FolderListItem inbox;
        inbox.raw_name = inbox.name = "INBOX";
        inbox.delimiter = "/";
        store->upsert_folders({inbox});
    }
    ComposeBridge::Config config(int autosave_ms = 2000) {
        ComposeBridge::Config c;
        c.store = store.get();
        c.clock = &clock;
        c.random = &rng;
        c.autosave_ms = autosave_ms;
        return c;
    }
    // a received message with a text; returns its id
    qlonglong received(const std::string& from_name, const std::string& from_addr, const std::string& subject, const std::string& text, const std::string& to = "Alice <alice@example.org>") {
        pbo::PullBatch batch;
        pbo::PulledMessage m;
        m.uid = ++uid_;
        m.subject = subject;
        m.from_name = from_name;
        m.from_addr = from_addr;
        m.to_text = to;
        m.message_id = "<orig" + std::to_string(uid_) + "@example.org>";
        m.date_utc = 1791979200 - 3600;
        batch.messages.push_back(m);
        store->apply_pull(store->folders().at(0).id, batch);
        const long long id = std::stoll(store->debug_query("SELECT id FROM message WHERE uid = " + std::to_string(uid_)));
        pbo::BodyUpdate u;
        u.text = text;
        store->apply_body(id, u);
        return id;
    }
    uint32_t uid_ = 0;
};

}  // namespace

PB_TEST(a_page_that_is_opened_and_left_without_a_change_leaves_no_draft) {
    Env e;
    ComposeBridge b(e.config());
    b.start("new");
    b.leave();
    PB_CHECK(e.store->drafts().empty());
    const qlonglong m = e.received("Anna", "anna@example.org", "Plan", "The text.\n");
    b.start("reply", m);
    PB_CHECK(!b.body().isEmpty());                              // the quote is prepared
    b.leave();
    PB_CHECK(e.store->drafts().empty());                        // but nothing was changed, so nothing is kept
}

PB_TEST(a_change_is_saved_two_seconds_later_and_again_when_the_page_is_left) {
    Env e;
    ComposeBridge b(e.config(150));   // the real delay is 2000 ms; the test shortens it
    QSignalSpy state(&b, &ComposeBridge::saveStateChanged);
    b.start("new");
    b.setTo("bob@example.org");
    PB_CHECK_EQ(b.saveState().toStdString(), std::string("pending"));
    PB_CHECK(e.store->drafts().empty());                         // not yet
    QTRY_COMPARE_WITH_TIMEOUT(b.saveState(), QStringLiteral("saved"), 3000);
    PB_CHECK_EQ(e.store->drafts().size(), size_t(1));
    PB_CHECK(b.draftId() > 0);
    b.setSubject("Hello");
    b.setBody("Text.");
    b.leave();                                                    // the page closes before the timer: saved at once
    PB_CHECK_EQ(e.store->drafts().size(), size_t(1));
    const auto d = *e.store->draft(b.draftId());
    PB_CHECK_EQ(d.subject, std::string("Hello"));
    PB_CHECK_EQ(d.body, std::string("Text."));
    PB_CHECK_EQ(d.to_text, std::string("bob@example.org"));
}

PB_TEST(every_change_restarts_the_delay_and_no_change_saves_nothing) {
    Env e;
    ComposeBridge b(e.config(300));
    b.start("new");
    b.setBody("a");
    QTest::qWait(200);
    b.setBody("ab");                                              // restarts the 300 ms
    QTest::qWait(200);
    PB_CHECK(e.store->drafts().empty());
    QTest::qWait(250);
    PB_CHECK_EQ(e.store->drafts().size(), size_t(1));
    PB_CHECK_EQ(e.store->drafts()[0].body, std::string("ab"));
    const int64_t updated = e.store->drafts()[0].updated;
    b.setBody("ab");                                              // the same text again: not a change
    b.autosave();
    PB_CHECK_EQ(e.store->drafts()[0].updated, updated);
}

PB_TEST(a_saved_draft_is_opened_again_with_all_its_fields) {
    Env e;
    {
        ComposeBridge b(e.config());
        b.start("new");
        b.setTo("bob@example.org");
        b.setCc("carol@example.org");
        b.setBcc("dave@example.org");
        b.setSubject("Plan");
        b.setBody("Hello\nBob.");
        b.leave();
    }
    ComposeBridge again(e.config());
    PB_CHECK(again.edit(e.store->drafts()[0].id));
    PB_CHECK_EQ(again.to().toStdString(), std::string("bob@example.org"));
    PB_CHECK_EQ(again.cc().toStdString(), std::string("carol@example.org"));
    PB_CHECK_EQ(again.bcc().toStdString(), std::string("dave@example.org"));
    PB_CHECK_EQ(again.subject().toStdString(), std::string("Plan"));
    PB_CHECK_EQ(again.body().toStdString(), std::string("Hello\nBob."));
    PB_CHECK_EQ(again.saveState().toStdString(), std::string("saved"));
    PB_CHECK(!again.edit(99999));
}

PB_TEST(a_reply_is_prefilled_with_recipient_subject_references_and_the_quoted_text) {
    Env e;
    const qlonglong m = e.received("Anna Beispiel", "anna@example.org", "Plan", "Can we meet?\nThursday?\n");
    ComposeBridge b(e.config());
    b.start("reply", m);
    PB_CHECK_EQ(b.to().toStdString(), std::string("Anna Beispiel <anna@example.org>"));
    PB_CHECK_EQ(b.subject().toStdString(), std::string("Re: Plan"));
    PB_CHECK(b.body().contains("wrote:"));
    PB_CHECK(b.body().contains("> Can we meet?"));
    PB_CHECK(b.body().contains("> Thursday?"));
    PB_CHECK_EQ(b.kind().toStdString(), std::string("reply"));
    b.setBody("Yes!\n\n" + b.body());
    b.leave();
    const auto d = *e.store->draft(b.draftId());
    PB_CHECK_EQ(d.kind, std::string("reply"));
    PB_CHECK_EQ(d.in_reply_to, std::string("<orig1@example.org>"));
    PB_CHECK_EQ(d.refs, std::string("<orig1@example.org>"));
    PB_CHECK_EQ(d.reply_to_message, int64_t(m));
}

PB_TEST(reply_all_keeps_the_others_without_me_and_forward_carries_the_text) {
    Env e;
    const qlonglong m = e.received("Anna", "anna@example.org", "Plan", "Text.\n", "Alice <alice@example.org>, Ben <ben@example.org>");
    ComposeBridge b(e.config());
    b.start("reply_all", m);
    PB_CHECK_EQ(b.to().toStdString(), std::string("Anna <anna@example.org>, Ben <ben@example.org>"));
    b.start("forward", m);
    PB_CHECK_EQ(b.subject().toStdString(), std::string("Fwd: Plan"));
    PB_CHECK(b.to().isEmpty());
    PB_CHECK(b.body().contains("---------- Forwarded message ----------"));
    PB_CHECK(b.body().contains("Subject: Plan"));
    PB_CHECK(b.forwardNote());
    b.start("reply", 424242);                                      // an unknown message: an empty form, not a crash
    PB_CHECK(b.to().isEmpty() && b.body().isEmpty());
}

PB_TEST(send_moves_the_draft_into_the_outbox_once_and_names_what_is_wrong_otherwise) {
    Env e;
    ComposeBridge b(e.config());
    QSignalSpy result(&b, &ComposeBridge::sendResult);
    QSignalSpy queued(&b, &ComposeBridge::queued);
    b.start("new");
    b.send();                                                      // nothing typed: no recipient
    PB_CHECK_EQ(result.count(), 1);
    PB_CHECK(!result.at(0).at(0).toBool());
    PB_CHECK_EQ(result.at(0).at(1).toString().toStdString(), std::string("no_recipient"));
    PB_CHECK_EQ(e.store->outbox().size(), size_t(0));

    b.setTo("not an address");
    b.send();
    PB_CHECK_EQ(result.at(1).at(1).toString().toStdString(), std::string("recipient_invalid"));
    PB_CHECK_EQ(result.at(1).at(2).toString().toStdString(), std::string("to"));
    b.setTo("bob@example.org");
    b.setCc("broken");
    b.send();
    PB_CHECK_EQ(result.at(2).at(2).toString().toStdString(), std::string("cc"));
    PB_CHECK(!e.store->drafts().empty());                          // the text was saved even though sending was refused

    b.setCc("");
    b.setSubject("Hello");
    b.setBody("Hi Bob.\n");
    b.send();
    PB_CHECK(result.at(3).at(0).toBool());
    PB_CHECK_EQ(queued.count(), 1);
    const auto outbox = e.store->outbox();
    PB_CHECK_EQ(outbox.size(), size_t(1));
    PB_CHECK_EQ(outbox[0].state, std::string("queued"));
    PB_CHECK_EQ(outbox[0].envelope_from, std::string("alice@example.org"));
    PB_CHECK(outbox[0].raw_message.find("Subject: Hello") != std::string::npos);
    PB_CHECK(outbox[0].raw_message.find("From: Alice Example <alice@example.org>") != std::string::npos);
    PB_CHECK(e.store->drafts().empty());                           // it is in the outbox now
    b.send();                                                      // a second tap: nothing more happens
    PB_CHECK_EQ(e.store->outbox().size(), size_t(1));
    PB_CHECK_EQ(queued.count(), 1);
}

PB_TEST(send_without_an_account_says_so) {
    pbtest::TmpDir dir;
    Clock clock;
    Rng rng;
    auto store = std::move(pbo::Store::open(dir.path(), clock).value);
    ComposeBridge::Config c;
    c.store = store.get();
    c.clock = &clock;
    c.random = &rng;
    ComposeBridge b(c);
    QSignalSpy result(&b, &ComposeBridge::sendResult);
    b.setTo("bob@example.org");
    b.send();
    PB_CHECK_EQ(result.at(0).at(1).toString().toStdString(), std::string("no_account"));
}

PB_TEST(discard_deletes_the_draft_and_empties_the_form) {
    Env e;
    ComposeBridge b(e.config());
    b.start("new");
    b.setBody("text");
    b.leave();
    PB_CHECK_EQ(e.store->drafts().size(), size_t(1));
    b.discard();
    PB_CHECK(e.store->drafts().empty());
    PB_CHECK(b.body().isEmpty());
    PB_CHECK_EQ(b.draftId(), qlonglong(0));
}

PB_TEST(the_outbox_model_describes_every_state_and_offers_only_the_right_actions) {
    Env e;
    ComposeBridge b(e.config());
    int64_t ids[3];
    for (int i = 0; i < 3; ++i) {
        b.start("new");
        b.setTo("bob@example.org");
        b.setSubject("Mail " + QString::number(i));
        b.send();
        ids[i] = e.store->outbox().back().id;
    }
    PB_CHECK(e.store->begin_sending(ids[1]).value);
    PB_CHECK(e.store->finish_sending(ids[1], {pbo::SendFinish::Kind::Failed, "refused_recipient", "x"}).ok());
    PB_CHECK(e.store->begin_sending(ids[2]).value);
    PB_CHECK(e.store->finish_sending(ids[2], {pbo::SendFinish::Kind::Uncertain, "connection_lost", "x"}).ok());
    OutboxModel m({e.store.get()});
    m.refresh();
    PB_CHECK_EQ(m.total(), 3);
    PB_CHECK_EQ(m.waitingCount(), 1);
    PB_CHECK_EQ(m.attentionCount(), 2);
    auto row = [&](int i) { return m.rows().at(i).toMap(); };
    PB_CHECK_EQ(row(0)["stateKey"].toString().toStdString(), std::string("waiting"));
    PB_CHECK(row(0)["waiting"].toBool() && !row(0)["canRetry"].toBool() && !row(0)["canDelete"].toBool());
    PB_CHECK_EQ(row(1)["stateKey"].toString().toStdString(), std::string("failed"));
    PB_CHECK_EQ(row(1)["reasonKey"].toString().toStdString(), std::string("refused_recipient"));
    PB_CHECK(row(1)["canRetry"].toBool() && row(1)["canEdit"].toBool() && row(1)["canDelete"].toBool() && !row(1)["canSendAgain"].toBool());
    PB_CHECK_EQ(row(2)["stateKey"].toString().toStdString(), std::string("uncertain"));
    PB_CHECK(row(2)["canSendAgain"].toBool() && row(2)["canMarkSent"].toBool() && row(2)["canDelete"].toBool() && !row(2)["canRetry"].toBool() && !row(2)["canEdit"].toBool());
    PB_CHECK_EQ(row(0)["subject"].toString().toStdString(), std::string("Mail 0"));
    PB_CHECK_EQ(row(0)["recipients"].toString().toStdString(), std::string("bob@example.org"));

    QSignalSpy action(&m, &OutboxModel::actionResult);
    PB_CHECK(!m.retry(ids[2]));                                     // not for an uncertain message
    PB_CHECK(!action.at(0).at(1).toBool());
    PB_CHECK(m.retry(ids[1]));
    PB_CHECK_EQ(m.waitingCount(), 2);
    PB_CHECK(e.store->begin_sending(ids[1]).value);
    PB_CHECK(e.store->finish_sending(ids[1], {pbo::SendFinish::Kind::Failed, "auth_failed", ""}).ok());
    m.refresh();
    const qlonglong draft = m.edit(ids[1]);
    PB_CHECK(draft > 0);
    PB_CHECK_EQ(e.store->drafts().size(), size_t(1));
    PB_CHECK(m.sendAgain(ids[2]));
    PB_CHECK_EQ(m.attentionCount(), 0);
    PB_CHECK(e.store->begin_sending(ids[2]).value);
    PB_CHECK(e.store->finish_sending(ids[2], {pbo::SendFinish::Kind::Uncertain, "timeout", ""}).ok());
    PB_CHECK(m.markSent(ids[2]));
    PB_CHECK_EQ(m.rows().at(1).toMap()["stateKey"].toString().toStdString(), std::string("copy"));
}

PB_TEST_MAIN()
