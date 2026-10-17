// T099 (part): CalDAV discovery, change detection, fetching and writing against the fake server (contracts/caldav-ical-subset.md section 1
// and the status table). Evidence is from the fake server only: UNVERIFIED against Nextcloud, OpenCloud or any other real server.
#include <set>
#include <string>

#include "fake_caldav_server.h"
#include "pbo/caldav.h"
#include "pbo/curl_transport.h"
#include "test_support.h"

using pbo::CalDavClient;
using pbo::CalendarInfo;
using pbtest::FakeCalDavServer;

PBO_FAKE_SERVER_TEST();

namespace {

const char* kEvent = "BEGIN:VCALENDAR\r\nBEGIN:VEVENT\r\nUID:a@example.org\r\nDTSTART:20261014T093000Z\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n";

struct Env {
    pbtest::TmpDir dir;
    pbtest::TestPki pki{dir};
    pbtest::TestPki::Leaf leaf;
    std::unique_ptr<FakeCalDavServer> server;
    std::unique_ptr<pbo::CurlTransport> http;
    Env() {
        if (!pki.available()) PB_SKIP("openssl CLI not available");
        leaf = pki.issue("srv", "DNS:localhost,IP:127.0.0.1");
        PB_CHECK(leaf.ok);
        FakeCalDavServer::Config c;
        c.cert = leaf.cert;
        c.key = leaf.key;
        server = std::make_unique<FakeCalDavServer>(c);
        http = std::make_unique<pbo::CurlTransport>(pki.ca_cert());
        FakeCalDavServer::CalendarOptions work;
        work.name = "Work";
        server->add_calendar("work", work);
        FakeCalDavServer::CalendarOptions ro;
        ro.name = "Holidays";
        ro.writable = false;
        server->add_calendar("holidays", ro);
    }
    CalDavClient client(const std::string& url = "", const std::string& pw = "pw-for-tests") {
        return CalDavClient(*http, url.empty() ? server->origin() : url, "alice", pw);
    }
    CalendarInfo calendar(const std::string& slug) {
        auto d = client().discover();
        PB_CHECK(d.ok());
        for (const auto& c : d.value)
            if (c.href == server->calendar_href(slug)) return c;
        PB_FAIL_AT("calendar not found: " + slug);
        return CalendarInfo();
    }
};

}  // namespace

PB_TEST(discovery_from_the_server_address_follows_well_known_principal_and_home) {
    Env e;
    auto d = e.client().discover();
    PB_CHECK(d.ok());
    PB_CHECK_EQ(d.value.size(), size_t(2));
    std::set<std::string> names;
    for (const auto& c : d.value) names.insert(c.name);
    PB_CHECK(names.count("Work") && names.count("Holidays"));
}

PB_TEST(discovery_from_a_dav_address_works_too) {
    Env e;
    auto d = e.client(e.server->base_url()).discover();
    PB_CHECK(d.ok());
    PB_CHECK_EQ(d.value.size(), size_t(2));
}

PB_TEST(writable_only_with_write_content_or_all_and_the_flags_are_read) {
    Env e;
    FakeCalDavServer::CalendarOptions all;
    all.name = "Shared";
    all.privilege_all = true;
    e.server->add_calendar("shared", all);
    FakeCalDavServer::CalendarOptions nosync;
    nosync.name = "Plain";
    nosync.sync_collection = false;
    e.server->add_calendar("plain", nosync);
    auto d = e.client().discover();
    PB_CHECK(d.ok());
    for (const auto& c : d.value) {
        PB_CHECK(c.supports_vevent);
        PB_CHECK(!c.href.empty() && c.href[0] == '/');
        PB_CHECK(!c.ctag.empty());
        if (c.name == "Work") { PB_CHECK(c.writable); PB_CHECK(c.supports_sync_report); PB_CHECK(!c.sync_token.empty()); }
        if (c.name == "Holidays") PB_CHECK(!c.writable);
        if (c.name == "Shared") PB_CHECK(c.writable);
        if (c.name == "Plain") { PB_CHECK(!c.supports_sync_report); }
    }
}

PB_TEST(collections_without_vevent_and_plain_collections_are_skipped) {
    Env e;
    FakeCalDavServer::CalendarOptions tasks;
    tasks.name = "Tasks";
    tasks.vevent = false;
    e.server->add_calendar("tasks", tasks);
    FakeCalDavServer::CalendarOptions folder;
    folder.name = "Inbox";
    folder.is_calendar = false;
    e.server->add_calendar("inbox", folder);
    auto d = e.client().discover();
    PB_CHECK(d.ok());
    PB_CHECK_EQ(d.value.size(), size_t(2));
}

PB_TEST(a_wrong_password_is_auth_failed_and_nothing_else_is_asked) {
    Env e;
    auto d = e.client("", "wrong").discover();
    PB_CHECK(!d.ok());
    PB_CHECK_EQ(d.error.key, std::string("auth_failed"));
    PB_CHECK(d.error.cls == pbo::ErrorClass::Auth);
    PB_CHECK(e.server->count("PROPFIND") <= 2);              // it stops at the first refusal
}

PB_TEST(credentials_are_never_part_of_a_url) {
    Env e;
    PB_CHECK(e.client().discover().ok());
    PB_CHECK(!e.server->saw_credentials_in_url());
}

PB_TEST(a_redirect_to_another_host_receives_no_credentials) {
    Env e;
    pbtest::TlsTestServer other(e.leaf.cert, e.leaf.key, [](const std::string&) { return pbtest::TlsTestServer::http_response(207, "<d:multistatus xmlns:d=\"DAV:\"/>"); });
    FakeCalDavServer::Fault f;
    f.kind = FakeCalDavServer::Fault::Kind::RedirectOtherHost;
    f.location = other.url("/dav/");
    f.times = 5;
    e.server->inject(f);
    auto d = e.client().discover();
    PB_CHECK(!other.saw_authorization());
    PB_CHECK(!d.ok());                                       // the other host's answer has no principal
}

PB_TEST(sync_collection_gives_changes_and_removals_since_the_stored_token) {
    Env e;
    e.server->put_event("work", "a.ics", kEvent);
    e.server->put_event("work", "b.ics", kEvent);
    auto cal = e.calendar("work");
    auto first = e.client().changes(cal, "", 0, 4000000000LL);
    PB_CHECK(first.ok());
    PB_CHECK(first.value.full);                              // nothing stored: the complete list
    PB_CHECK_EQ(first.value.changed.size(), size_t(2));
    PB_CHECK(!first.value.new_sync_token.empty());
    for (const auto& r : first.value.changed) PB_CHECK(!r.etag.empty() && r.href.rfind("/dav/calendars/alice/work/", 0) == 0);

    e.server->remove_event("work", "a.ics");
    e.server->put_event("work", "c.ics", kEvent);
    auto next = e.client().changes(cal, first.value.new_sync_token, 0, 4000000000LL);
    PB_CHECK(next.ok());
    PB_CHECK(!next.value.full);
    PB_CHECK_EQ(next.value.changed.size(), size_t(1));
    PB_CHECK(next.value.changed[0].href.find("c.ics") != std::string::npos);
    PB_CHECK_EQ(next.value.removed.size(), size_t(1));
    PB_CHECK(next.value.removed[0].find("a.ics") != std::string::npos);
    PB_CHECK(!next.value.new_sync_token.empty() && next.value.new_sync_token != first.value.new_sync_token);

    auto none = e.client().changes(cal, next.value.new_sync_token, 0, 4000000000LL);
    PB_CHECK(none.ok());
    PB_CHECK(none.value.changed.empty() && none.value.removed.empty() && !none.value.full);
}

PB_TEST(an_invalid_sync_token_means_a_full_resync_with_the_window_query) {
    Env e;
    e.server->put_event("work", "in.ics", kEvent, 1791979200, 1791982800);
    e.server->put_event("work", "out.ics", kEvent, 1000000000, 1000003600);
    auto cal = e.calendar("work");
    auto first = e.client().changes(cal, "", 1791900000, 1792000000);
    PB_CHECK(first.ok() && first.value.full);
    PB_CHECK_EQ(first.value.changed.size(), size_t(1));      // only what has an instance in the window
    PB_CHECK(first.value.changed[0].href.find("in.ics") != std::string::npos);
    e.server->reset_sync_tokens("work");
    auto again = e.client().changes(cal, first.value.new_sync_token, 1791900000, 1792000000);
    PB_CHECK(again.ok());
    PB_CHECK(again.value.full);
    PB_CHECK_EQ(again.value.changed.size(), size_t(1));
    PB_CHECK(!again.value.new_sync_token.empty());
}

PB_TEST(a_calendar_without_sync_collection_uses_ctag_and_calendar_query) {
    Env e;
    FakeCalDavServer::CalendarOptions nosync;
    nosync.name = "Plain";
    nosync.sync_collection = false;
    e.server->add_calendar("plain", nosync);
    e.server->put_event("plain", "a.ics", kEvent);
    auto cal = e.calendar("plain");
    PB_CHECK(!cal.supports_sync_report);
    auto first = e.client().changes(cal, "", 0, 4000000000LL);
    PB_CHECK(first.ok());
    PB_CHECK(first.value.full);
    PB_CHECK_EQ(first.value.changed.size(), size_t(1));
    PB_CHECK(!first.value.new_ctag.empty());
    const int queries = e.server->count("REPORT");
    auto same = e.client().changes(cal, first.value.new_ctag, 0, 4000000000LL);
    PB_CHECK(same.ok());
    PB_CHECK(same.value.unchanged);
    PB_CHECK_EQ(e.server->count("REPORT"), queries);          // equal ctag: no query at all
    e.server->put_event("plain", "b.ics", kEvent);
    auto more = e.client().changes(cal, first.value.new_ctag, 0, 4000000000LL);
    PB_CHECK(more.ok());
    PB_CHECK(more.value.full && !more.value.unchanged);
    PB_CHECK_EQ(more.value.changed.size(), size_t(2));
}

PB_TEST(multiget_fetches_in_batches_of_30) {
    Env e;
    std::vector<std::string> hrefs;
    for (int i = 0; i < 65; ++i) {
        const std::string name = "e" + std::to_string(i) + ".ics";
        e.server->put_event("work", name, kEvent);
        hrefs.push_back(e.server->event_href("work", name));
    }
    auto cal = e.calendar("work");
    auto r = e.client().fetch(cal, hrefs);
    PB_CHECK(r.ok());
    PB_CHECK_EQ(r.value.events.size(), size_t(65));
    PB_CHECK(r.value.gone.empty() && r.value.failed.empty());
    PB_CHECK(r.value.events[0].ics.find("BEGIN:VCALENDAR") != std::string::npos);
    PB_CHECK(!r.value.events[0].etag.empty());
    const auto sizes = e.server->multiget_batch_sizes();
    PB_CHECK((sizes == std::vector<size_t>{30, 30, 5}));
}

PB_TEST(a_resource_that_disappeared_is_reported_as_gone) {
    Env e;
    e.server->put_event("work", "a.ics", kEvent);
    auto cal = e.calendar("work");
    auto r = e.client().fetch(cal, {e.server->event_href("work", "a.ics"), e.server->event_href("work", "missing.ics")});
    PB_CHECK(r.ok());
    PB_CHECK_EQ(r.value.events.size(), size_t(1));
    PB_CHECK_EQ(r.value.gone.size(), size_t(1));
}

PB_TEST(a_hostile_href_outside_the_calendar_host_is_not_requested) {
    Env e;
    auto cal = e.calendar("work");
    auto r = e.client().fetch(cal, {"https://evil.example.org/dav/x.ics", "//evil.example.org/x.ics"});
    PB_CHECK(r.ok());
    PB_CHECK(r.value.events.empty());
    PB_CHECK_EQ(e.server->multiget_batch_sizes().size(), size_t(0));     // nothing was asked
}

PB_TEST(status_codes_are_mapped_to_the_core_results) {
    Env e;
    struct Row { int status; const char* key; pbo::ErrorClass cls; };
    for (const Row& row : {Row{401, "auth_failed", pbo::ErrorClass::Auth}, Row{500, "server_error", pbo::ErrorClass::Server}, Row{503, "server_error", pbo::ErrorClass::Server},
                           Row{403, "forbidden", pbo::ErrorClass::Server}}) {
        FakeCalDavServer::Fault f;
        f.kind = FakeCalDavServer::Fault::Kind::Status;
        f.status = row.status;
        f.method = "PROPFIND";
        e.server->inject(f);
        auto d = e.client().discover();
        PB_CHECK(!d.ok());
        PB_CHECK_EQ(d.error.key, std::string(row.key));
        PB_CHECK(d.error.cls == row.cls);
    }
}

PB_TEST(a_dropped_connection_and_malformed_xml_are_errors_not_crashes) {
    Env e;
    FakeCalDavServer::Fault drop;
    drop.kind = FakeCalDavServer::Fault::Kind::Drop;
    e.server->inject(drop);
    auto d = e.client().discover();
    PB_CHECK(!d.ok());
    FakeCalDavServer::Fault bad;
    bad.kind = FakeCalDavServer::Fault::Kind::MalformedXml;
    bad.times = 1;
    e.server->inject(bad);
    d = e.client().discover();
    PB_CHECK(!d.ok());
    PB_CHECK_EQ(d.error.key, std::string("malformed_xml"));
    PB_CHECK(e.client().discover().ok());                     // the server is fine again
}

PB_TEST(create_update_and_delete_are_conditional_and_report_the_new_etag) {
    Env e;
    CalDavClient c = e.client();
    const std::string href = e.server->event_href("work", "new.ics");
    auto created = c.put(href, kEvent, nullptr);
    PB_CHECK(created.kind == pbo::WriteResult::Kind::Created);
    PB_CHECK(!created.etag.empty());
    PB_CHECK_EQ(e.server->event("work", "new.ics").etag, created.etag);
    PB_CHECK(c.put(href, kEvent, nullptr).kind == pbo::WriteResult::Kind::PreconditionFailed);     // If-None-Match: * on an existing resource
    const std::string stale = "\"stale\"";
    PB_CHECK(c.put(href, kEvent, &stale).kind == pbo::WriteResult::Kind::PreconditionFailed);
    auto updated = c.put(href, kEvent, &created.etag);
    PB_CHECK(updated.kind == pbo::WriteResult::Kind::Updated);
    PB_CHECK(updated.etag != created.etag);
    PB_CHECK(c.remove(href, "\"stale\"").kind == pbo::WriteResult::Kind::PreconditionFailed);
    PB_CHECK(c.remove(href, updated.etag).kind == pbo::WriteResult::Kind::Updated);
    PB_CHECK(c.remove(href, updated.etag).kind == pbo::WriteResult::Kind::NotFound);
    const auto missing = e.server->event_href("work", "never.ics");
    const std::string tag = "\"x\"";
    PB_CHECK(c.put(missing, kEvent, &tag).kind == pbo::WriteResult::Kind::NotFound);
    // the requests carried the conditional headers
    int conditional = 0;
    for (const auto& r : e.server->log())
        if ((r.method == "PUT" || r.method == "DELETE") && (r.headers.count("if-match") || r.headers.count("if-none-match"))) ++conditional;
    PB_CHECK_EQ(conditional, 8);
    for (const auto& r : e.server->log())
        if (r.method == "PUT") PB_CHECK(r.headers.at("content-type").find("text/calendar") == 0);
}

PB_TEST(a_write_to_a_read_only_calendar_is_refused_with_the_reason) {
    Env e;
    auto r = e.client().put(e.server->event_href("holidays", "x.ics"), kEvent, nullptr);
    PB_CHECK(r.kind == pbo::WriteResult::Kind::Refused);
    PB_CHECK_EQ(r.error.key, std::string("read_only"));
}

PB_TEST(a_server_edit_between_read_and_write_is_a_precondition_failure_not_an_overwrite) {
    Env e;
    const std::string etag = e.server->put_event("work", "a.ics", kEvent);
    e.server->before_write([](FakeCalDavServer& s) { s.put_event("work", "a.ics", "BEGIN:VCALENDAR\r\nEND:VCALENDAR\r\n"); });
    auto r = e.client().put(e.server->event_href("work", "a.ics"), kEvent, &etag);
    PB_CHECK(r.kind == pbo::WriteResult::Kind::PreconditionFailed);
    PB_CHECK_EQ(e.server->event("work", "a.ics").ics, std::string("BEGIN:VCALENDAR\r\nEND:VCALENDAR\r\n"));   // the other version survived
}

PB_TEST(transient_failures_while_writing_are_failed_with_an_error_and_change_nothing) {
    Env e;
    FakeCalDavServer::Fault f;
    f.kind = FakeCalDavServer::Fault::Kind::Status;
    f.status = 503;
    f.method = "PUT";
    e.server->inject(f);
    auto r = e.client().put(e.server->event_href("work", "t.ics"), kEvent, nullptr);
    PB_CHECK(r.kind == pbo::WriteResult::Kind::Failed);
    PB_CHECK_EQ(r.error.key, std::string("server_error"));
    PB_CHECK(!e.server->has_event("work", "t.ics"));
}

PB_TEST_MAIN()
