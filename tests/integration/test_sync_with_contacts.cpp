// The contacts steps of run_sync next to the mail and calendar steps (spec 002 research R-10, FR-003, FR-005): order, options, partial outcomes,
// the counts and the problem area. Fake servers only (UNVERIFIED against real servers).
#include <algorithm>
#include <string>

#include "contact_env.h"
#include "fake_caldav_server.h"
#include "fake_carddav_server.h"
#include "fake_imap_server.h"
#include "pbo/contact_edit.h"
#include "pbo/curl_transport.h"
#include "pbo/imap_transport.h"
#include "pbo/sync.h"
#include "test_support.h"
#include "tmp_dir.h"

using pbtest::contact_vcf;
using pbtest::FakeCalDavServer;
using pbtest::FakeCardDavServer;
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
    std::unique_ptr<FakeCalDavServer> cal;
    std::unique_ptr<FakeCardDavServer> dav;   // answers for the calendars too (same address)
    std::unique_ptr<pbo::Store> store;
    std::unique_ptr<pbo::TzDatabase> tz;
    std::unique_ptr<pbo::CurlTransport> http;
    explicit Env(bool with_mail = true) {
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
        FakeCalDavServer::Config cc;
        cc.cert = leaf.cert;
        cc.key = leaf.key;
        cal = std::make_unique<FakeCalDavServer>(cc);
        FakeCalDavServer::CalendarOptions work;
        work.name = "Work";
        cal->add_calendar("work", work);
        cal->put_event("work", "a.ics",
                       "BEGIN:VCALENDAR\r\nBEGIN:VEVENT\r\nUID:a@example.org\r\nSUMMARY:Standup\r\nDTSTART;TZID=Europe/Berlin:20261015T093000\r\nDTEND;TZID=Europe/Berlin:20261015T100000\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n",
                       kNow, kNow + 2 * 86400);
        FakeCardDavServer::Config dc;
        dc.cert = leaf.cert;
        dc.key = leaf.key;
        dav = std::make_unique<FakeCardDavServer>(dc);
        dav->set_calendar_peer(cal.get());
        FakeCardDavServer::BookOptions book;
        book.name = "Contacts";
        dav->add_book("contacts", book);
        dav->put_card("contacts", "a.vcf", contact_vcf("u-a", "Anna", "Beispiel"));
        dav->put_card("contacts", "b.vcf", contact_vcf("u-b", "Bert", "Beispiel"));
        store = std::move(pbo::Store::open(data.path(), clock).value);
        if (with_mail) {
            pbo::MailAccount m;
            m.imap_host = "localhost";
            m.imap_port = imap->port();
            m.smtp_host = "localhost";
            m.username = "alice";
            m.address = "alice@example.org";
            store->save_mail_account(m);
        }
        pbo::CalendarAccount a;
        a.caldav_url = dav->origin();
        a.username = "alice";
        store->save_calendar_account(a);
        store->set_setting("tz_override", "Europe/Berlin");
        tz = pbo::TzDatabase::open(std::string(PBO_SOURCE_DIR) + "/third_party/tzdata/zoneinfo");
        http = std::make_unique<pbo::CurlTransport>(pki.ca_cert());
    }
    pbo::SyncReport sync(const pbo::SyncOptions& options = {}, const std::string& calendar_password = "pw-for-tests") {
        auto transport = pbo::make_curl_imap_transport(pki.ca_cert());
        pbo::SyncDeps deps{*store, *transport, clock, cancel};
        deps.http = http.get();
        deps.tz = tz.get();
        pbo::Credentials creds;
        creds.mail_password = "pw-for-tests";
        creds.calendar_password = calendar_password;
        return pbo::run_sync(deps, creds, options);
    }
    int q(const std::string& sql) const { return std::atoi(store->debug_query(sql).c_str()); }
};

const pbo::StepResult* step(const pbo::SyncReport& r, const std::string& name) {
    for (const auto& s : r.steps)
        if (s.name == name) return &s;
    return nullptr;
}

}  // namespace

PB_TEST(the_steps_run_in_the_documented_order_with_contacts_after_the_calendar) {
    const std::vector<std::string> want = {"outbox", "mail_push", "mail_pull", "calendar_push", "calendar_pull", "contacts_push", "contacts_pull", "finish"};
    PB_CHECK((pbo::sync_step_names() == want));
}

PB_TEST(mail_calendar_and_contacts_are_pulled_in_one_sync) {
    Env e;
    const auto r = e.sync();
    PB_CHECK(r.outcome == pbo::SyncOutcome::Success);
    PB_CHECK_EQ(e.q("SELECT count(*) FROM message"), 1);
    PB_CHECK_EQ(e.q("SELECT count(*) FROM event"), 1);
    PB_CHECK_EQ(e.q("SELECT count(*) FROM contact"), 2);
    PB_CHECK_EQ(r.contacts_counts.changed, 2);
    PB_CHECK(step(r, "contacts_pull") && step(r, "contacts_pull")->ran && step(r, "contacts_pull")->error.ok());
    PB_CHECK(!step(r, "contacts_push")->ran);   // nothing waits
    const std::string counts = e.store->sync_state().counts_json;
    for (const char* key : {"\"contacts\":2", "\"contacts_removed\":0", "\"contacts_sent\":0", "\"contact_conflicts\":0"}) PB_CHECK(counts.find(key) != std::string::npos);
    PB_CHECK(e.store->sync_state().last_success.has_value());
}

PB_TEST(a_contacts_only_account_works_without_a_mail_account) {
    Env e(false);
    const auto r = e.sync();
    PB_CHECK(r.outcome == pbo::SyncOutcome::Success);
    PB_CHECK_EQ(e.q("SELECT count(*) FROM contact"), 2);
    PB_CHECK(!step(r, "mail_pull")->ran);
}

PB_TEST(with_contacts_switched_off_the_steps_do_not_run) {
    Env e;
    pbo::SyncOptions o;
    o.contacts = false;
    const auto r = e.sync(o);
    PB_CHECK(r.outcome == pbo::SyncOutcome::Success);
    PB_CHECK(!step(r, "contacts_pull")->ran);
    PB_CHECK_EQ(e.q("SELECT count(*) FROM contact"), 0);
    PB_CHECK_EQ(e.q("SELECT count(*) FROM event"), 1);   // the others ran
}

PB_TEST(a_calendar_feed_only_account_has_no_contacts_step) {
    Env e;
    pbo::CalendarAccount feed;
    feed.feed_url = "https://feeds.example.org/holidays.ics";
    PB_CHECK(e.store->save_calendar_account(feed).ok());
    const auto r = e.sync();
    PB_CHECK(!step(r, "contacts_pull")->ran);
    PB_CHECK_EQ(e.q("SELECT count(*) FROM contact"), 0);
}

PB_TEST(a_contacts_problem_leaves_mail_and_calendar_and_makes_the_outcome_partial) {
    Env e;
    FakeCardDavServer::Fault f;   // only the address book home is refused
    f.method = "PROPFIND";
    f.body_contains = "addressbook-home-set";
    f.status = 500;
    f.times = 1;
    e.dav->inject(f);
    const auto r = e.sync();
    PB_CHECK(r.outcome == pbo::SyncOutcome::Partial);
    PB_CHECK_EQ(r.problem_area, std::string("contacts"));
    PB_CHECK_EQ(r.first_problem.key, std::string("server_error"));
    PB_CHECK_EQ(e.q("SELECT count(*) FROM message"), 1);
    PB_CHECK_EQ(e.q("SELECT count(*) FROM event"), 1);
    PB_CHECK_EQ(e.q("SELECT count(*) FROM contact"), 0);
    PB_CHECK_EQ(e.store->sync_state().problem_area, std::string("contacts"));
    PB_CHECK_EQ(e.store->sync_state().outcome, std::string("partial"));
    // the next sync, with the server well again, repairs it
    const auto again = e.sync();
    PB_CHECK(again.outcome == pbo::SyncOutcome::Success);
    PB_CHECK_EQ(e.q("SELECT count(*) FROM contact"), 2);
    PB_CHECK(e.store->sync_state().last_success.has_value());
}

PB_TEST(a_server_without_address_books_is_a_success_for_the_contacts_step) {
    Env e;
    e.dav->remove_book("contacts");
    const auto r = e.sync();
    PB_CHECK(r.outcome == pbo::SyncOutcome::Success);
    PB_CHECK(r.contacts_counts.no_addressbook);
    PB_CHECK(r.problem_area.empty());
}

PB_TEST(a_wrong_password_is_reported_once_for_the_account) {
    Env e;
    const auto r = e.sync({}, "wrong-password");
    PB_CHECK(r.outcome == pbo::SyncOutcome::Partial || r.outcome == pbo::SyncOutcome::Failed);
    PB_CHECK_EQ(r.first_problem.key, std::string("auth_failed"));
    PB_CHECK_EQ(e.q("SELECT count(*) FROM message"), 1);   // the mail still came
    PB_CHECK_EQ(e.q("SELECT count(*) FROM contact"), 0);
}

PB_TEST(cancelling_stops_the_contacts_step) {
    Env e;
    e.cancel.flag = true;
    const auto r = e.sync();
    PB_CHECK(r.outcome == pbo::SyncOutcome::Cancelled);
    PB_CHECK_EQ(e.q("SELECT count(*) FROM contact"), 0);
}

namespace {

pbo::ContactEdit anna() {
    pbo::ContactEdit e;
    e.given = "Anna";
    e.family = "Neu";
    pbo::LabeledValue p;
    p.label = "mobile";
    p.value = "+1 555 0101";
    e.phones.push_back(p);
    return e;
}

int64_t create_in_first_book(Env& e) {
    struct Rnd : pbo::Random {
        uint8_t n = 40;
        void bytes(uint8_t* out, size_t len) override {
            for (size_t i = 0; i < len; ++i) out[i] = n++;
        }
    } random;
    pbo::ContactEditDeps deps{*e.store, e.clock, random};
    int64_t book = 0;
    for (const auto& b : e.store->addressbooks()) book = b.id;
    auto r = pbo::create_contact(deps, book, anna());
    PB_CHECK(r.ok());
    return r.value;
}

}  // namespace

PB_TEST(a_waiting_new_contact_goes_up_in_the_push_step_before_the_pull) {
    Env e;
    PB_CHECK(e.sync().outcome == pbo::SyncOutcome::Success);   // the address books are known now
    PB_CHECK_EQ(e.q("SELECT count(*) FROM contact"), 2);
    const int64_t id = create_in_first_book(e);
    pbo::SyncOptions o;
    std::vector<std::string> order;
    o.progress = [&](const pbo::SyncProgress& p) { order.push_back(p.step); };
    const auto r = e.sync(o);
    PB_CHECK(r.outcome == pbo::SyncOutcome::Success);
    PB_CHECK(step(r, "contacts_push")->ran && step(r, "contacts_push")->error.ok());
    PB_CHECK_EQ(r.contacts_push_counts.created, 1);
    PB_CHECK_EQ(e.store->debug_query("SELECT sync_state FROM contact WHERE id = " + std::to_string(id)), std::string("synced"));
    PB_CHECK_EQ(e.dav->card_count("contacts"), size_t(3));
    const auto push_at = std::find(order.begin(), order.end(), "contacts_push");
    const auto pull_at = std::find(order.begin(), order.end(), "contacts_pull");
    PB_CHECK(push_at != order.end() && pull_at != order.end() && push_at < pull_at);
    PB_CHECK(e.store->sync_state().counts_json.find("\"contacts_sent\":1") != std::string::npos);
    // nothing waits any more: the step does not run again
    const auto again = e.sync();
    PB_CHECK(!step(again, "contacts_push")->ran);
    PB_CHECK_EQ(e.dav->card_count("contacts"), size_t(3));
}

PB_TEST(a_refused_new_contact_makes_the_outcome_partial_and_names_the_contacts) {
    Env e;
    PB_CHECK(e.sync().outcome == pbo::SyncOutcome::Success);
    const int64_t id = create_in_first_book(e);
    FakeCardDavServer::Fault f;
    f.method = "PUT";
    f.status = 507;
    e.dav->inject(f);
    const auto r = e.sync();
    PB_CHECK(r.outcome == pbo::SyncOutcome::Partial);
    PB_CHECK_EQ(r.problem_area, std::string("contacts"));
    PB_CHECK_EQ(r.first_problem.key, std::string("storage_full_server"));
    PB_CHECK_EQ(e.store->sync_state().problem_area, std::string("contacts"));
    PB_CHECK_EQ(e.store->debug_query("SELECT error_key FROM contact WHERE id = " + std::to_string(id)), std::string("storage_full_server"));
    PB_CHECK_EQ(e.q("SELECT count(*) FROM message"), 1);   // mail and calendar came
    PB_CHECK_EQ(e.q("SELECT count(*) FROM event"), 1);
    // the next sync sends it again
    const auto again = e.sync();
    PB_CHECK(again.outcome == pbo::SyncOutcome::Success);
    PB_CHECK_EQ(e.store->debug_query("SELECT sync_state FROM contact WHERE id = " + std::to_string(id)), std::string("synced"));
}

PB_TEST(a_sync_that_only_pulls_leaves_the_waiting_contact_where_it_is) {
    Env e;
    PB_CHECK(e.sync().outcome == pbo::SyncOutcome::Success);
    const int64_t id = create_in_first_book(e);
    pbo::SyncOptions o;
    o.push = false;
    const auto r = e.sync(o);
    PB_CHECK(!step(r, "contacts_push")->ran);
    PB_CHECK_EQ(e.store->debug_query("SELECT sync_state FROM contact WHERE id = " + std::to_string(id)), std::string("pending_create"));
}

PB_TEST_MAIN()
