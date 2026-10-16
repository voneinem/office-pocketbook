// The calendar step of run_sync next to the mail step: both run, a problem in one does not stop the other, the outcome says "partial" when one
// of them worked, and the problem area names the part that failed. Fake servers only (UNVERIFIED against real servers).
#include <string>

#include "fake_caldav_server.h"
#include "fake_imap_server.h"
#include "pbo/curl_transport.h"
#include "pbo/event_edit.h"
#include "pbo/imap_transport.h"
#include "pbo/sync.h"
#include "test_support.h"
#include "tmp_dir.h"

using pbtest::FakeCalDavServer;
using pbtest::FakeImapServer;

PBO_FAKE_SERVER_TEST();

namespace {

constexpr int64_t kNow = 1791979200;

struct Clock : pbo::Clock {
    int64_t now() const override { return kNow; }
};
struct Cancel : pbo::Cancel {
    bool flag = false;
    bool requested() const override { return flag; }
};

struct Env {
    pbtest::TmpDir data, pki_dir;
    pbtest::TestPki pki{pki_dir};
    pbtest::TestPki::Leaf leaf;
    Clock clock;
    Cancel cancel;
    std::unique_ptr<FakeImapServer> imap;
    std::unique_ptr<FakeCalDavServer> dav;
    std::unique_ptr<pbo::Store> store;
    std::unique_ptr<pbo::TzDatabase> tz;
    std::unique_ptr<pbo::CurlTransport> http;
    Env() {
        if (!pki.available()) throw pbtest::Skip{"the openssl command is not available"};
        leaf = pki.issue("srv", "DNS:localhost,IP:127.0.0.1");
        if (!leaf.ok) throw pbtest::Skip{"cannot create a test certificate"};
        FakeImapServer::Config c;
        c.cert = leaf.cert;
        c.key = leaf.key;
        imap = std::make_unique<FakeImapServer>(c);
        imap->set_capabilities("IMAP4rev1 UIDPLUS LITERAL+");
        imap->add_folder("INBOX");
        imap->add_message("INBOX", "From: A <a@example.org>\r\nSubject: Hello\r\nDate: Mon, 12 Oct 2026 10:00:00 +0000\r\nMessage-ID: <m1@example.org>\r\n\r\nBody\r\n", {}, kNow - 86400);
        FakeCalDavServer::Config dc;
        dc.cert = leaf.cert;
        dc.key = leaf.key;
        dav = std::make_unique<FakeCalDavServer>(dc);
        FakeCalDavServer::CalendarOptions work;
        work.name = "Work";
        dav->add_calendar("work", work);
        dav->put_event("work", "a.ics",
                       "BEGIN:VCALENDAR\r\nBEGIN:VEVENT\r\nUID:a@example.org\r\nSUMMARY:Standup\r\nDTSTART;TZID=Europe/Berlin:20261015T093000\r\nDTEND;TZID=Europe/Berlin:20261015T100000\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n",
                       kNow, kNow + 2 * 86400);
        store = std::move(pbo::Store::open(data.path(), clock).value);
        pbo::MailAccount m;
        m.imap_host = "localhost";
        m.imap_port = imap->port();
        m.smtp_host = "localhost";
        m.username = "alice";
        m.address = "alice@example.org";
        store->save_mail_account(m);
        pbo::CalendarAccount a;
        a.caldav_url = dav->origin();
        a.username = "alice";
        store->save_calendar_account(a);
        store->set_setting("tz_override", "Europe/Berlin");
        tz = pbo::TzDatabase::open(std::string(PBO_SOURCE_DIR) + "/third_party/tzdata/zoneinfo");
        http = std::make_unique<pbo::CurlTransport>(pki.ca_cert());
    }
    pbo::SyncReport sync(const std::string& calendar_password = "pw-for-tests", bool with_calendar = true) {
        auto transport = pbo::make_curl_imap_transport(pki.ca_cert());
        pbo::SyncDeps deps{*store, *transport, clock, cancel};
        if (with_calendar) {
            deps.http = http.get();
            deps.tz = tz.get();
        }
        pbo::Credentials creds;
        creds.mail_password = "pw-for-tests";
        creds.calendar_password = calendar_password;
        return pbo::run_sync(deps, creds);
    }
    int q(const std::string& sql) const { return std::atoi(store->debug_query(sql).c_str()); }
};

}  // namespace

PB_TEST(mail_and_calendar_are_pulled_in_one_sync) {
    Env e;
    const auto r = e.sync();
    PB_CHECK(r.outcome == pbo::SyncOutcome::Success);
    PB_CHECK_EQ(e.q("SELECT count(*) FROM message"), 1);
    PB_CHECK_EQ(e.q("SELECT count(*) FROM event"), 1);
    PB_CHECK_EQ(r.calendar_counts.changed, 1);
    for (const auto& s : r.steps) {
        if (s.name == "mail_pull" || s.name == "calendar_pull") PB_CHECK(s.ran && s.error.ok());
    }
    PB_CHECK(e.store->sync_state().counts_json.find("\"events\":1") != std::string::npos);
    PB_CHECK(e.store->sync_state().last_success.has_value());
}

PB_TEST(a_calendar_problem_leaves_the_mail_and_makes_the_outcome_partial) {
    Env e;
    const auto r = e.sync("wrong-password");
    PB_CHECK(r.outcome == pbo::SyncOutcome::Partial);
    PB_CHECK_EQ(r.problem_area, std::string("calendar"));
    PB_CHECK_EQ(r.first_problem.key, std::string("auth_failed"));
    PB_CHECK_EQ(e.q("SELECT count(*) FROM message"), 1);
    PB_CHECK_EQ(e.store->sync_state().problem_area, std::string("calendar"));
    PB_CHECK_EQ(e.store->sync_state().outcome, std::string("partial"));
    PB_CHECK(!e.store->sync_state().last_success.has_value());
    // the next sync with the right password repairs it
    PB_CHECK(e.sync().outcome == pbo::SyncOutcome::Success);
}

PB_TEST(a_mail_problem_does_not_stop_the_calendar) {
    Env e;
    auto m = *e.store->mail_account();
    m.imap_port = 1;                                // nothing listens there
    e.store->save_mail_account(m);
    const auto r = e.sync();
    PB_CHECK(r.outcome == pbo::SyncOutcome::Partial);
    PB_CHECK_EQ(r.problem_area, std::string("mail"));
    PB_CHECK_EQ(e.q("SELECT count(*) FROM event"), 1);
}

PB_TEST(without_the_calendar_dependencies_the_step_is_skipped) {
    Env e;
    const auto r = e.sync("pw-for-tests", false);
    PB_CHECK(r.outcome == pbo::SyncOutcome::Success);
    for (const auto& s : r.steps)
        if (s.name == "calendar_pull") PB_CHECK(!s.ran);
    PB_CHECK_EQ(e.q("SELECT count(*) FROM event"), 0);
}

PB_TEST(a_sync_with_only_a_calendar_account_works) {
    Env e;
    e.store->debug_exec("DELETE FROM account_mail");
    const auto r = e.sync();
    PB_CHECK(r.outcome == pbo::SyncOutcome::Success);
    PB_CHECK_EQ(e.q("SELECT count(*) FROM event"), 1);
    for (const auto& s : r.steps)
        if (s.name == "mail_pull") PB_CHECK(!s.ran);
}

PB_TEST(a_cancelled_sync_is_cancelled_and_keeps_what_was_stored) {
    Env e;
    PB_CHECK(e.sync().outcome == pbo::SyncOutcome::Success);
    e.cancel.flag = true;
    const auto r = e.sync();
    PB_CHECK(r.outcome == pbo::SyncOutcome::Cancelled);
    PB_CHECK_EQ(e.q("SELECT count(*) FROM event"), 1);
    PB_CHECK_EQ(e.q("SELECT count(*) FROM message"), 1);
}

PB_TEST(a_waiting_event_goes_up_in_the_calendar_push_step_before_the_pull) {
    Env e;
    PB_CHECK(e.sync().outcome == pbo::SyncOutcome::Success);
    struct Rnd : pbo::Random {
        uint8_t n = 7;
        void bytes(uint8_t* out, size_t len) override { for (size_t i = 0; i < len; ++i) out[i] = n++; }
    } rnd;
    pbo::EventEditDeps deps{*e.store, *e.tz, e.clock, rnd};
    pbo::EventEdit edit;
    edit.title = "From the reader";
    edit.tzid = "Europe/Berlin";
    edit.start = {2026, 10, 16, 9, 0, 0};
    edit.end = {2026, 10, 16, 10, 0, 0};
    int64_t work = 0;
    for (const auto& c : e.store->calendars()) work = c.id;
    PB_CHECK(pbo::create_event(deps, work, edit).ok());
    const auto r = e.sync();
    PB_CHECK(r.outcome == pbo::SyncOutcome::Success);
    PB_CHECK_EQ(r.push_counts.created, 1);
    bool ran = false;
    for (const auto& s : r.steps)
        if (s.name == "calendar_push") ran = s.ran && s.error.ok();
    PB_CHECK(ran);
    PB_CHECK_EQ(e.dav->event_count("work"), size_t(2));
    PB_CHECK_EQ(e.q("SELECT count(*) FROM event WHERE sync_state <> 'synced'"), 0);
    PB_CHECK(e.store->sync_state().counts_json.find("\"events_sent\":1") != std::string::npos);
    // nothing waiting: the step does not run
    const auto again = e.sync();
    for (const auto& s : again.steps)
        if (s.name == "calendar_push") PB_CHECK(!s.ran);
}

PB_TEST(a_refused_event_push_makes_the_outcome_partial_and_names_the_calendar) {
    Env e;
    PB_CHECK(e.sync().outcome == pbo::SyncOutcome::Success);
    struct Rnd : pbo::Random {
        uint8_t n = 9;
        void bytes(uint8_t* out, size_t len) override { for (size_t i = 0; i < len; ++i) out[i] = n++; }
    } rnd;
    pbo::EventEditDeps deps{*e.store, *e.tz, e.clock, rnd};
    pbo::EventEdit edit;
    edit.title = "Refused";
    edit.tzid = "Europe/Berlin";
    edit.start = {2026, 10, 16, 9, 0, 0};
    edit.end = {2026, 10, 16, 10, 0, 0};
    int64_t work = 0;
    for (const auto& c : e.store->calendars()) work = c.id;
    PB_CHECK(pbo::create_event(deps, work, edit).ok());
    FakeCalDavServer::Fault f;
    f.method = "PUT";
    f.status = 403;
    e.dav->inject(f);
    const auto r = e.sync();
    PB_CHECK(r.outcome == pbo::SyncOutcome::Partial);
    PB_CHECK_EQ(r.problem_area, std::string("calendar"));
    PB_CHECK_EQ(r.first_problem.key, std::string("read_only"));
    PB_CHECK_EQ(e.q("SELECT count(*) FROM event WHERE sync_state = 'pending_create'"), 1);   // kept for the next try
}

PB_TEST_MAIN()
