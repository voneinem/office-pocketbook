// Store::open creates schema 1 of data-model.md with its pragmas, constraints and permissions (T021, FR-032,
// constitution Principle III and the Security & Data Handling constraints).
#include <sys/stat.h>

#include <fstream>
#include <iterator>
#include <set>
#include <sstream>
#include <string>

#include <sqlite3.h>

#include "pbo/store.h"
#include "pbo/types.h"
#include "test_support.h"
#include "tmp_dir.h"

namespace {

pbo::SystemClock g_clock;

std::unique_ptr<pbo::Store> open_store(const pbtest::TmpDir& d, const std::string& sub = "data") {
    auto r = pbo::Store::open(d.file(sub), g_clock);
    if (!r.ok()) PB_FAIL_AT("Store::open failed: " + r.error.key);
    return std::move(r.value);
}

std::string read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

mode_t mode_of(const std::string& path) {
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) PB_FAIL_AT("cannot stat " + path);
    return st.st_mode & 0777;
}

}  // namespace

PB_TEST(open_creates_every_table_of_schema_1_and_2) {
    pbtest::TmpDir d;
    auto s = open_store(d);
    const char* tables[] = {"account_mail", "account_calendar", "settings", "folder", "message", "attachment", "message_fts",
                            "draft", "outbox", "calendar", "event", "event_conflict", "occurrence", "sync_state", "address_suggestion",
                            "addressbook", "contact", "contact_email", "contact_conflict"};
    for (const char* t : tables) {
        const std::string n = s->debug_query("SELECT count(*) FROM sqlite_master WHERE name = '" + std::string(t) + "'");
        PB_CHECK_EQ(n, std::string("1"));
    }
}

PB_TEST(open_sets_the_pragmas_and_the_user_version) {
    pbtest::TmpDir d;
    auto s = open_store(d);
    PB_CHECK_EQ(s->debug_query("PRAGMA journal_mode"), std::string("truncate"));
    PB_CHECK_EQ(s->debug_query("PRAGMA synchronous"), std::string("2"));  // FULL
    PB_CHECK_EQ(s->debug_query("PRAGMA foreign_keys"), std::string("1"));
    PB_CHECK_EQ(s->debug_query("PRAGMA secure_delete"), std::string("1"));
    PB_CHECK_EQ(s->debug_query("PRAGMA user_version"), std::string("2"));   // schema 2 since spec 002 (test_store_migration_2)
    PB_CHECK_EQ(s->schema_version(), 2);
    PB_CHECK(s->check_integrity().ok());
}

PB_TEST(the_data_directory_attachments_and_database_are_private_where_the_file_system_allows) {
    pbtest::TmpDir d;
    auto s = open_store(d);
    PB_CHECK_EQ(mode_of(d.file("data")) & 077, mode_t(0));
    PB_CHECK_EQ(mode_of(d.file("data/attachments")) & 077, mode_t(0));
    PB_CHECK_EQ(mode_of(d.file("data/pboffice.db")) & 077, mode_t(0));
}

PB_TEST(data_survives_a_reopen) {
    pbtest::TmpDir d;
    {
        auto s = open_store(d);
        PB_CHECK(s->debug_exec("INSERT INTO settings(key, value) VALUES('auto_sync', '1')").empty());
    }
    auto s = open_store(d);
    PB_CHECK_EQ(s->debug_query("SELECT value FROM settings WHERE key = 'auto_sync'"), std::string("1"));
}

PB_TEST(account_mail_constraints_come_straight_from_the_data_model) {
    pbtest::TmpDir d;
    auto s = open_store(d);
    const std::string base_cols = "imap_host, imap_port, imap_security, smtp_host, smtp_port, smtp_security, username, address, created";
    auto insert = [&](const std::string& extra_cols, const std::string& extra_vals, int id = 1, const std::string& imap_sec = "tls") {
        return s->debug_exec("INSERT INTO account_mail(id, " + base_cols + extra_cols + ") VALUES(" + std::to_string(id) +
                             ", 'imap.example.org', 993, '" + imap_sec + "', 'smtp.example.org', 465, 'tls', 'alice', 'alice@example.org', 1" + extra_vals + ")");
    };
    PB_CHECK(!insert(", sync_days", ", 0").empty());        // CHECK(sync_days BETWEEN 1 AND 365)
    PB_CHECK(!insert(", sync_days", ", 366").empty());
    PB_CHECK(!insert(", attachment_limit", ", 1048575").empty());   // BETWEEN 1048576 AND 104857600
    PB_CHECK(!insert(", attachment_limit", ", 104857601").empty());
    PB_CHECK(!insert(", storage_limit", ", 20971519").empty());     // >= 20971520
    PB_CHECK(!insert("", "", 1, "plain").empty());                  // CHECK(imap_security IN ('tls','starttls'))
    PB_CHECK(!insert("", "", 2).empty());                           // CHECK(id = 1)
    PB_CHECK(insert("", "").empty());                               // defaults are valid
    PB_CHECK_EQ(s->debug_query("SELECT sync_days || ',' || attachment_limit || ',' || storage_limit || ',' || sent_by_server FROM account_mail"),
                std::string("30,10485760,209715200,0"));
    PB_CHECK(!insert("", "").empty());                              // a second row for id 1 is refused
}

PB_TEST(message_outbox_and_event_states_are_constrained) {
    pbtest::TmpDir d;
    auto s = open_store(d);
    PB_CHECK(s->debug_exec("INSERT INTO folder(id, name, raw_name) VALUES(1, 'INBOX', 'INBOX')").empty());
    const std::string msg = "INSERT INTO message(folder_id, uid, date_utc, %s) VALUES(1, %d, 0, %s)";
    auto m = [&](int uid, const std::string& col, const std::string& val) {
        char buf[512];
        std::snprintf(buf, sizeof buf, msg.c_str(), col.c_str(), uid, val.c_str());
        return s->debug_exec(buf);
    };
    PB_CHECK(m(1, "local_state", "'synced'").empty());
    PB_CHECK(!m(1, "local_state", "'synced'").empty());                    // UNIQUE(folder_id, uid)
    PB_CHECK(!m(2, "local_state", "'pending_something'").empty());         // CHECK(local_state IN (...))
    PB_CHECK(m(3, "local_state", "'pending_flags'").empty());
    PB_CHECK(!m(4, "body_state", "'weird'").empty());                      // CHECK(body_state IN (...))
    PB_CHECK(!m(5, "local_error", "'weird'").empty());                     // CHECK(local_error IN (...))

    PB_CHECK(!s->debug_exec("INSERT INTO outbox(message_id, envelope_from, envelope_to, raw_message, state, queued_at, state_at) "
                            "VALUES('<a@x>', 'a@x', 'b@x', x'00', 'sent', 1, 1)").empty());   // 'sent' is not a state
    PB_CHECK(s->debug_exec("INSERT INTO outbox(message_id, envelope_from, envelope_to, raw_message, state, queued_at, state_at) "
                           "VALUES('<a@x>', 'a@x', 'b@x', x'00', 'queued', 1, 1)").empty());
    PB_CHECK(!s->debug_exec("INSERT INTO outbox(message_id, envelope_from, envelope_to, raw_message, state, queued_at, state_at) "
                            "VALUES('<a@x>', 'a@x', 'b@x', x'00', 'queued', 1, 1)").empty());  // message_id UNIQUE

    PB_CHECK(s->debug_exec("INSERT INTO calendar(id, kind, href, name) VALUES(1, 'caldav', '/c/1', 'Work')").empty());
    const std::string ev = "INSERT INTO event(calendar_id, uid, ics, start_local, sync_state) VALUES(1, '%s', 'x', '2026-10-13', '%s')";
    auto e = [&](const std::string& uid, const std::string& st) {
        char buf[300];
        std::snprintf(buf, sizeof buf, ev.c_str(), uid.c_str(), st.c_str());
        return s->debug_exec(buf);
    };
    for (const char* st : {"synced", "pending_create", "pending_update", "pending_delete", "conflict", "server_deleted"})
        PB_CHECK(e(std::string("u-") + st, st).empty());
    PB_CHECK(!e("u-bad", "merged").empty());
    PB_CHECK(!e("u-synced", "synced").empty());   // UNIQUE(calendar_id, uid)
}

PB_TEST(deleting_a_folder_cascades_to_messages_and_attachments) {
    pbtest::TmpDir d;
    auto s = open_store(d);
    PB_CHECK(s->debug_exec("INSERT INTO folder(id, name, raw_name) VALUES(1, 'INBOX', 'INBOX')").empty());
    PB_CHECK(s->debug_exec("INSERT INTO message(id, folder_id, uid, date_utc) VALUES(1, 1, 1, 0)").empty());
    PB_CHECK(s->debug_exec("INSERT INTO attachment(message_id, section, name) VALUES(1, '2', 'a.pdf')").empty());
    PB_CHECK(!s->debug_exec("INSERT INTO attachment(message_id, section, name) VALUES(1, '2', 'b.pdf')").empty());   // UNIQUE(message_id, section)
    PB_CHECK(s->debug_exec("DELETE FROM folder WHERE id = 1").empty());
    PB_CHECK_EQ(s->debug_query("SELECT count(*) FROM message"), std::string("0"));
    PB_CHECK_EQ(s->debug_query("SELECT count(*) FROM attachment"), std::string("0"));
}

PB_TEST(a_newer_database_is_refused_and_left_byte_identical) {
    pbtest::TmpDir d;
    ::mkdir(d.file("data").c_str(), 0700);
    const std::string path = d.file("data/pboffice.db");
    {
        sqlite3* db = nullptr;
        PB_CHECK(sqlite3_open(path.c_str(), &db) == SQLITE_OK);
        PB_CHECK(sqlite3_exec(db, "CREATE TABLE future(x); PRAGMA user_version = 3;", nullptr, nullptr, nullptr) == SQLITE_OK);
        sqlite3_close(db);
    }
    const std::string before = read_file(path);
    auto r = pbo::Store::open(d.file("data"), g_clock);
    PB_CHECK(!r.ok());
    PB_CHECK_EQ(r.error.key, std::string("schema_too_new"));
    PB_CHECK(r.value == nullptr);
    PB_CHECK(read_file(path) == before);
}

PB_TEST_MAIN()
