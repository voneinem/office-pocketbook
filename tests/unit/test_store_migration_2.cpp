// T009 (spec 002, research R-07, data-model.md "Schema 2"): the first built-in migration, schema 1 -> 2 (address books and contacts). The schema-1
// fixture database is made with the schema text of schema 1 itself (core/src/store_schema.h), so the test proves that a database of the
// released build 0.2.0 survives the upgrade with every row.
#include <sys/stat.h>

#include <fstream>
#include <iterator>
#include <string>

#include <sqlite3.h>

#include "../../core/src/store_schema.h"
#include "pbo/store.h"
#include "pbo/types.h"
#include "test_support.h"
#include "tmp_dir.h"

namespace {

pbo::SystemClock g_clock;

bool exists(const std::string& path) {
    struct stat st;
    return ::stat(path.c_str(), &st) == 0;
}

std::string read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

// A database exactly as build 0.2.0 leaves it: schema 1, user_version 1, with data in the tables the upgrade must not disturb.
void make_schema_1_database(const std::string& dir) {
    ::mkdir(dir.c_str(), 0700);
    sqlite3* db = nullptr;
    PB_CHECK(sqlite3_open((dir + "/pboffice.db").c_str(), &db) == SQLITE_OK);
    const char* data =
        "INSERT INTO account_mail(id, imap_host, imap_port, imap_security, smtp_host, smtp_port, smtp_security, username, address, created)"
        " VALUES(1, 'imap.example.org', 993, 'tls', 'smtp.example.org', 465, 'tls', 'alice', 'alice@example.org', 100);"
        "INSERT INTO folder(id, name, raw_name, role) VALUES(1, 'INBOX', 'INBOX', 'inbox');"
        "INSERT INTO message(id, folder_id, uid, date_utc, subject) VALUES(1, 1, 7, 1000, 'first'), (2, 1, 8, 2000, 'second');"
        "INSERT INTO draft(id, kind, message_id, created, updated, body) VALUES(1, 'new', '<d1@example.org>', 5, 6, 'draft text');"
        "INSERT INTO outbox(id, message_id, envelope_from, envelope_to, raw_message, state, queued_at, state_at)"
        " VALUES(1, '<o1@example.org>', 'alice@example.org', 'bob@example.org', x'414243', 'queued', 7, 8);"
        "INSERT INTO account_calendar(id, caldav_url, username, created) VALUES(1, 'https://dav.example.org/dav/', 'alice', 100);"
        "INSERT INTO calendar(id, kind, href, name, writable) VALUES(1, 'caldav', '/dav/calendars/alice/work/', 'Work', 1);"
        "INSERT INTO event(id, calendar_id, uid, ics, summary, start_local, sync_state)"
        " VALUES(1, 1, 'e1@example.org', 'BEGIN:VCALENDAR', 'Lunch', '2026-10-20T12:00:00', 'pending_update');"
        "INSERT INTO settings(key, value) VALUES('auto_sync', '1');"
        "INSERT INTO sync_state(id, outcome, problem_key, problem_area) VALUES(1, 'partial', 'server_unreachable', 'calendar');";
    PB_CHECK(sqlite3_exec(db, pbo::kSchemaSql, nullptr, nullptr, nullptr) == SQLITE_OK);
    PB_CHECK(sqlite3_exec(db, data, nullptr, nullptr, nullptr) == SQLITE_OK);
    PB_CHECK(sqlite3_exec(db, "PRAGMA user_version = 1", nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(db);
}

std::unique_ptr<pbo::Store> open_ok(const std::string& dir, const pbo::StoreOptions& options = {}) {
    auto r = pbo::Store::open(dir, g_clock, options);
    if (!r.ok()) PB_FAIL_AT("Store::open failed: " + r.error.key);
    return std::move(r.value);
}

std::string count(pbo::Store& s, const std::string& table) { return s.debug_query("SELECT count(*) FROM " + table); }

}  // namespace

PB_TEST(a_fresh_database_ends_at_schema_2_with_the_contact_tables) {
    pbtest::TmpDir d;
    auto s = open_ok(d.file("data"));
    PB_CHECK_EQ(s->schema_version(), 2);
    PB_CHECK_EQ(s->debug_query("PRAGMA user_version"), std::string("2"));
    PB_CHECK_EQ(pbo::Store::kSchemaVersion, 2);
    for (const char* t : {"addressbook", "contact", "contact_email", "contact_conflict", "sync_state"})
        PB_CHECK_EQ(s->debug_query("SELECT count(*) FROM sqlite_master WHERE type = 'table' AND name = '" + std::string(t) + "'"), std::string("1"));
    for (const char* i : {"contact_sort", "contact_state", "contact_email_key"})
        PB_CHECK_EQ(s->debug_query("SELECT count(*) FROM sqlite_master WHERE type = 'index' AND name = '" + std::string(i) + "'"), std::string("1"));
    PB_CHECK(!exists(d.file("data/pboffice.db.bak")));   // nothing to back up
    PB_CHECK_EQ(s->debug_query("SELECT count(*) FROM settings WHERE key = 'schema_backup_pending'"), std::string("0"));
    PB_CHECK(s->check_integrity().ok());
}

PB_TEST(a_schema_1_database_is_upgraded_and_every_row_survives) {
    pbtest::TmpDir d;
    make_schema_1_database(d.file("data"));
    auto s = open_ok(d.file("data"));
    PB_CHECK_EQ(s->schema_version(), 2);
    PB_CHECK_EQ(count(*s, "account_mail"), std::string("1"));
    PB_CHECK_EQ(s->debug_query("SELECT username FROM account_mail"), std::string("alice"));
    PB_CHECK_EQ(count(*s, "folder"), std::string("1"));
    PB_CHECK_EQ(count(*s, "message"), std::string("2"));
    PB_CHECK_EQ(s->debug_query("SELECT subject FROM message WHERE uid = 8"), std::string("second"));
    PB_CHECK_EQ(s->debug_query("SELECT body FROM draft"), std::string("draft text"));
    PB_CHECK_EQ(s->debug_query("SELECT length(raw_message) FROM outbox"), std::string("3"));
    PB_CHECK_EQ(s->debug_query("SELECT sync_state FROM event WHERE uid = 'e1@example.org'"), std::string("pending_update"));
    PB_CHECK_EQ(s->debug_query("SELECT value FROM settings WHERE key = 'auto_sync'"), std::string("1"));
    // the rebuilt sync_state keeps its row
    PB_CHECK_EQ(count(*s, "sync_state"), std::string("1"));
    PB_CHECK_EQ(s->debug_query("SELECT outcome || '/' || problem_key || '/' || problem_area FROM sync_state WHERE id = 1"), std::string("partial/server_unreachable/calendar"));
    // the new tables are empty
    for (const char* t : {"addressbook", "contact", "contact_email", "contact_conflict"}) PB_CHECK_EQ(count(*s, t), std::string("0"));
    PB_CHECK(s->check_integrity().ok());
}

PB_TEST(sync_state_accepts_the_problem_area_contacts_after_the_upgrade) {
    pbtest::TmpDir d;
    make_schema_1_database(d.file("data"));
    auto s = open_ok(d.file("data"));
    PB_CHECK(s->debug_exec("UPDATE sync_state SET problem_area = 'contacts' WHERE id = 1").empty());
    PB_CHECK(!s->debug_exec("UPDATE sync_state SET problem_area = 'other' WHERE id = 1").empty());
    PB_CHECK(!s->debug_exec("UPDATE sync_state SET outcome = 'broken' WHERE id = 1").empty());   // the rest of the old constraints stay
}

PB_TEST(the_backup_is_kept_after_the_upgrade_until_the_next_successful_start) {
    pbtest::TmpDir d;
    make_schema_1_database(d.file("data"));
    const std::string before = read_file(d.file("data/pboffice.db"));
    {
        auto s = open_ok(d.file("data"));
        PB_CHECK(exists(d.file("data/pboffice.db.bak")));                                  // research R-07: kept
        PB_CHECK(read_file(d.file("data/pboffice.db.bak")) == before);                      // the file of the previous build, byte for byte
        PB_CHECK_EQ(s->debug_query("SELECT value FROM settings WHERE key = 'schema_backup_pending'"), std::string("1"));
    }
    {
        auto s = open_ok(d.file("data"));   // the next start opened the upgraded database successfully
        PB_CHECK(!exists(d.file("data/pboffice.db.bak")));
        PB_CHECK_EQ(s->debug_query("SELECT count(*) FROM settings WHERE key = 'schema_backup_pending'"), std::string("0"));
        PB_CHECK_EQ(count(*s, "message"), std::string("2"));
    }
}

PB_TEST(a_failure_after_the_first_step_rolls_the_whole_upgrade_back) {
    pbtest::TmpDir d;
    make_schema_1_database(d.file("data"));
    pbo::StoreOptions o;
    o.target_version = 3;
    o.extra_migrations.push_back({2, 3, "ALTER TABLE no_such_table ADD COLUMN x TEXT"});   // the built-in step 1 -> 2 ran, step 2 -> 3 fails
    auto bad = pbo::Store::open(d.file("data"), g_clock, o);
    PB_CHECK(!bad.ok());
    PB_CHECK_EQ(bad.error.key, std::string("migration_failed"));
    PB_CHECK(exists(d.file("data/pboffice.db.bak")));

    sqlite3* db = nullptr;
    PB_CHECK(sqlite3_open(d.file("data/pboffice.db").c_str(), &db) == SQLITE_OK);
    sqlite3_stmt* st = nullptr;
    PB_CHECK(sqlite3_prepare_v2(db, "PRAGMA user_version", -1, &st, nullptr) == SQLITE_OK);
    PB_CHECK(sqlite3_step(st) == SQLITE_ROW);
    PB_CHECK_EQ(sqlite3_column_int(st, 0), 1);   // still schema 1: no half upgrade
    sqlite3_finalize(st);
    PB_CHECK(sqlite3_prepare_v2(db, "SELECT count(*) FROM contact", -1, &st, nullptr) != SQLITE_OK);   // the new tables were rolled back
    sqlite3_finalize(st);
    PB_CHECK(sqlite3_prepare_v2(db, "SELECT count(*) FROM message", -1, &st, nullptr) == SQLITE_OK);
    PB_CHECK(sqlite3_step(st) == SQLITE_ROW);
    PB_CHECK_EQ(sqlite3_column_int(st, 0), 2);
    sqlite3_finalize(st);
    sqlite3_close(db);

    auto again = open_ok(d.file("data"));   // and the normal open upgrades it properly afterwards
    PB_CHECK_EQ(again->schema_version(), 2);
    PB_CHECK_EQ(count(*again, "message"), std::string("2"));
}

PB_TEST(an_older_build_refuses_a_schema_2_database_and_leaves_it_untouched) {
    pbtest::TmpDir d;
    { auto s = open_ok(d.file("data")); }
    const std::string before = read_file(d.file("data/pboffice.db"));
    pbo::StoreOptions older;
    older.target_version = 1;   // what build 0.2.0 knows
    auto r = pbo::Store::open(d.file("data"), g_clock, older);
    PB_CHECK(!r.ok());
    PB_CHECK_EQ(r.error.key, std::string("schema_too_new"));
    PB_CHECK(read_file(d.file("data/pboffice.db")) == before);

    // a database of an even newer version is refused by this build in the same way
    {
        sqlite3* db = nullptr;
        PB_CHECK(sqlite3_open(d.file("data/pboffice.db").c_str(), &db) == SQLITE_OK);
        PB_CHECK(sqlite3_exec(db, "PRAGMA user_version = 3", nullptr, nullptr, nullptr) == SQLITE_OK);
        sqlite3_close(db);
    }
    const std::string v3 = read_file(d.file("data/pboffice.db"));
    auto now = pbo::Store::open(d.file("data"), g_clock);
    PB_CHECK(!now.ok());
    PB_CHECK_EQ(now.error.key, std::string("schema_too_new"));
    PB_CHECK(read_file(d.file("data/pboffice.db")) == v3);
}

PB_TEST(the_check_constraints_of_the_contact_tables_reject_bad_values) {
    pbtest::TmpDir d;
    auto s = open_ok(d.file("data"));
    PB_CHECK(s->debug_exec("INSERT INTO addressbook(id, href, name) VALUES(1, '/dav/addressbooks/alice/contacts/', 'Contacts')").empty());
    const std::string base = "INSERT INTO contact(addressbook_id, href, uid, vcf";
    PB_CHECK(s->debug_exec(base + ") VALUES(1, '/dav/a/1.vcf', 'u1', 'BEGIN:VCARD')").empty());
    // sync_state IN ('synced','pending_create','pending_update','pending_delete','conflict','server_deleted')
    PB_CHECK(!s->debug_exec(base + ", sync_state) VALUES(1, '/dav/a/2.vcf', 'u2', 'x', 'failed')").empty());
    for (const char* ok : {"synced", "pending_create", "pending_update", "pending_delete", "conflict", "server_deleted"}) {
        const std::string n = std::string("ok_") + ok;
        PB_CHECK(s->debug_exec(base + ", sync_state) VALUES(1, '/dav/a/" + n + ".vcf', '" + n + "', 'x', '" + ok + "')").empty());
    }
    // undo_state IN ('','synced','pending_update')
    PB_CHECK(!s->debug_exec(base + ", undo_state) VALUES(1, '/dav/a/3.vcf', 'u3', 'x', 'conflict')").empty());
    PB_CHECK(s->debug_exec(base + ", undo_state) VALUES(1, '/dav/a/3b.vcf', 'u3b', 'x', 'pending_update')").empty());
    // error_key IN ('','read_only','too_large','refused','storage_full_server')
    PB_CHECK(!s->debug_exec(base + ", error_key) VALUES(1, '/dav/a/4.vcf', 'u4', 'x', 'nope')").empty());
    PB_CHECK(s->debug_exec(base + ", error_key) VALUES(1, '/dav/a/4b.vcf', 'u4b', 'x', 'storage_full_server')").empty());
    // partial / unreadable / refetch IN (0,1)
    PB_CHECK(!s->debug_exec(base + ", partial) VALUES(1, '/dav/a/5.vcf', 'u5', 'x', 2)").empty());
    PB_CHECK(!s->debug_exec(base + ", unreadable) VALUES(1, '/dav/a/6.vcf', 'u6', 'x', 2)").empty());
    PB_CHECK(!s->debug_exec(base + ", refetch) VALUES(1, '/dav/a/7.vcf', 'u7', 'x', 2)").empty());
    // href and uid are required
    PB_CHECK(!s->debug_exec(base + ") VALUES(1, NULL, 'u8', 'x')").empty());
    PB_CHECK(!s->debug_exec(base + ") VALUES(1, '/dav/a/9.vcf', NULL, 'x')").empty());
}

PB_TEST(a_resource_path_and_a_uid_are_unique_inside_one_address_book) {
    pbtest::TmpDir d;
    auto s = open_ok(d.file("data"));
    PB_CHECK(s->debug_exec("INSERT INTO addressbook(id, href, name) VALUES(1, '/b1/', 'One'), (2, '/b2/', 'Two')").empty());
    const std::string base = "INSERT INTO contact(addressbook_id, href, uid, vcf) VALUES";
    PB_CHECK(s->debug_exec(base + "(1, '/b1/a.vcf', 'uid-a', 'x')").empty());
    PB_CHECK(!s->debug_exec(base + "(1, '/b1/b.vcf', 'uid-a', 'x')").empty());     // UNIQUE(addressbook_id, uid)
    PB_CHECK(!s->debug_exec(base + "(1, '/b1/a.vcf', 'uid-b', 'x')").empty());     // UNIQUE(addressbook_id, href)
    PB_CHECK(s->debug_exec(base + "(2, '/b2/a.vcf', 'uid-a', 'x')").empty());      // the same names in another book are fine
    // a duplicate or missing UID is stored under the synthetic uid href:<href> (data-model.md, validation rules)
    PB_CHECK(s->debug_exec(base + "(1, '/b1/b.vcf', 'href:/b1/b.vcf', 'x')").empty());
    PB_CHECK(s->debug_exec(base + "(1, '/b1/c.vcf', 'href:/b1/c.vcf', 'x')").empty());
    PB_CHECK(!s->debug_exec("INSERT INTO addressbook(href, name) VALUES('/b1/', 'Again')").empty());   // addressbook.href is unique
}

PB_TEST(deleting_an_address_book_cascades_to_its_contacts_emails_and_conflicts) {
    pbtest::TmpDir d;
    auto s = open_ok(d.file("data"));
    PB_CHECK(s->debug_exec("INSERT INTO addressbook(id, href, name) VALUES(1, '/b1/', 'One'), (2, '/b2/', 'Two')").empty());
    PB_CHECK(s->debug_exec("INSERT INTO contact(id, addressbook_id, href, uid, vcf) VALUES(1, 1, '/b1/a.vcf', 'a', 'x'), (2, 2, '/b2/a.vcf', 'a', 'x')").empty());
    PB_CHECK(s->debug_exec("INSERT INTO contact_email(contact_id, idx, addr, addr_key) VALUES(1, 0, 'A@example.org', 'a@example.org'), (2, 0, 'B@example.org', 'b@example.org')").empty());
    PB_CHECK(s->debug_exec("INSERT INTO contact_conflict(contact_id, server_vcf, fetched_at) VALUES(1, 'y', 5)").empty());
    PB_CHECK(!s->debug_exec("INSERT INTO contact_email(contact_id, idx, addr, addr_key) VALUES(1, 0, 'dup@example.org', 'dup@example.org')").empty());   // PRIMARY KEY(contact_id, idx)
    PB_CHECK(s->debug_exec("DELETE FROM addressbook WHERE id = 1").empty());
    PB_CHECK_EQ(count(*s, "contact"), std::string("1"));
    PB_CHECK_EQ(count(*s, "contact_email"), std::string("1"));
    PB_CHECK_EQ(count(*s, "contact_conflict"), std::string("0"));
    PB_CHECK_EQ(s->debug_query("SELECT addr FROM contact_email"), std::string("B@example.org"));
}

PB_TEST_MAIN()
