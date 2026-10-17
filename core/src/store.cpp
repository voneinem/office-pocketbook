#include "pbo/store.h"
#include "pbo/utf8.h"

#include <sqlite3.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <fstream>

#include "pbo/log.h"
#include "store_schema.h"
#include "store_sql.h"

namespace pbo {

using sql::exec;
using sql::query_text;

namespace {

bool file_size(const std::string& path, long long* size) {
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) return false;
    *size = static_cast<long long>(st.st_size);
    return true;
}

bool copy_file(const std::string& from, const std::string& to) {
    std::ifstream in(from, std::ios::binary);
    std::ofstream out(to, std::ios::binary | std::ios::trunc);
    if (!in || !out) return false;
    out << in.rdbuf();
    out.flush();
    if (!out) return false;
    ::chmod(to.c_str(), 0600);   // the backup holds the same data as the database
    return true;
}

// Creates the directory with mode 0700 (and narrows an existing one).
void make_private_dir(const std::string& path) {
    ::mkdir(path.c_str(), 0700);
    ::chmod(path.c_str(), 0700);
}

Result<std::unique_ptr<Store>> fail(const char* key, const char* detail) {
    Result<std::unique_ptr<Store>> r;
    r.error = make_error(ErrorClass::Storage, key, detail);
    return r;
}

// user_version of an existing database file. The file is opened for reading AND writing so that SQLite can roll back the journal of a
// transaction that was cut short (the app was killed, the battery pulled); a read-only handle cannot do that and would report a perfectly
// good database as unreadable. Nothing is written otherwise.
bool read_user_version(const std::string& path, int* version) {
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK) {
        sqlite3_close(db);
        db = nullptr;
        if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
            sqlite3_close(db);
            return false;
        }
    }
    std::string v, err;
    const bool ok = query_text(db, "PRAGMA user_version", &v, &err);
    sqlite3_close(db);
    if (ok) *version = std::atoi(v.c_str());
    return ok;
}

}  // namespace

Store::~Store() {
    if (db_) sqlite3_close(db_);
}

Result<std::unique_ptr<Store>> Store::open(const std::string& dir, Clock& clock, const StoreOptions& options) {
    const int target = options.target_version > 0 ? options.target_version : kSchemaVersion;

    make_private_dir(dir);
    make_private_dir(dir + "/attachments");
    const std::string path = dir + "/pboffice.db";
    const std::string backup = path + ".bak";

    long long size = 0;
    const bool existed_nonempty = file_size(path, &size) && size > 0;
    if (existed_nonempty) {
        int stored = 0;
        if (!read_user_version(path, &stored)) return fail("open_failed", "the database cannot be read");
        if (stored > target) return fail("schema_too_new", "the database was written by a newer version of the app");
    }

    sqlite3* db = nullptr;
    if (sqlite3_open(path.c_str(), &db) != SQLITE_OK) {
        sqlite3_close(db);
        return fail("open_failed", "the database cannot be opened");
    }
    ::chmod(path.c_str(), 0600);
    // the ordering of names and subjects (utf8.h: sort_key)
    sqlite3_create_function(db, "pbo_fold", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, nullptr,
                            [](sqlite3_context* ctx, int, sqlite3_value** argv) {
                                const unsigned char* t = sqlite3_value_text(argv[0]);
                                const std::string key = pbo::sort_key(t ? reinterpret_cast<const char*>(t) : "");
                                sqlite3_result_text(ctx, key.c_str(), static_cast<int>(key.size()), SQLITE_TRANSIENT);
                            },
                            nullptr, nullptr);

    std::string value, err;
    if (!query_text(db, "PRAGMA journal_mode=TRUNCATE", &value, &err) ||
        !exec(db, "PRAGMA synchronous=FULL; PRAGMA foreign_keys=ON; PRAGMA secure_delete=ON;", &err) ||
        !query_text(db, "PRAGMA user_version", &value, &err)) {
        sqlite3_close(db);
        return fail("open_failed", "the database is unusable");
    }
    int version = std::atoi(value.c_str());
    if (version == 0) {   // a fresh database: create schema 1 in one transaction
        if (!exec(db, "BEGIN IMMEDIATE", &err) || !exec(db, kSchemaSql, &err) || !exec(db, "PRAGMA user_version=1", &err)) {
            exec(db, "ROLLBACK", nullptr);
            sqlite3_close(db);
            log::event("store.create_failed", {});
            return fail("open_failed", "the schema could not be created");
        }
        if (!exec(db, "COMMIT", &err)) {
            sqlite3_close(db);
            return fail("open_failed", "the schema could not be created");
        }
        version = 1;
    }

    if (version < target) {   // one-way upgrade steps
        if (existed_nonempty && !copy_file(path, backup)) {
            sqlite3_close(db);
            return fail("migration_failed", "the backup before the upgrade could not be written");
        }
        bool ok = exec(db, "BEGIN IMMEDIATE", &err);
        while (ok && version < target) {
            auto it = std::find_if(options.extra_migrations.begin(), options.extra_migrations.end(),
                                   [&](const Migration& m) { return m.from == version; });
            if (it == options.extra_migrations.end()) { ok = false; break; }
            ok = exec(db, it->sql.c_str(), &err) && exec(db, ("PRAGMA user_version=" + std::to_string(it->to)).c_str(), &err);
            version = it->to;
        }
        if (ok) ok = exec(db, "COMMIT", &err);
        if (!ok) {
            exec(db, "ROLLBACK", nullptr);
            sqlite3_close(db);
            log::event("store.migration_failed", {});
            return fail("migration_failed", "the upgrade failed; your data was not changed");
        }
        if (!query_text(db, "PRAGMA integrity_check", &value, &err) || value != "ok") {
            sqlite3_close(db);
            return fail("integrity_failed", "the database failed its check after the upgrade");
        }
        if (existed_nonempty) std::remove(backup.c_str());   // no lingering copy of the data
    }

    // objects of schema 1 that are created when missing (a database made by an earlier build has no trigger yet):
    // the search index follows its message when the message is deleted, also by a cascade
    if (!exec(db,
              "CREATE TRIGGER IF NOT EXISTS message_fts_delete AFTER DELETE ON message BEGIN"
              " DELETE FROM message_fts WHERE rowid = old.id; END;",
              &err)) {
        sqlite3_close(db);
        return fail("open_failed", "the database is unusable");
    }

    // the pictures of HTML bodies were added after the first builds: the table is created when missing (a cascade follows the message)
    if (!exec(db,
              "CREATE TABLE IF NOT EXISTS message_image("
              " message_id INTEGER NOT NULL REFERENCES message(id) ON DELETE CASCADE,"
              " idx INTEGER NOT NULL,"
              " kind TEXT NOT NULL CHECK(kind IN ('cid','remote')),"
              " src TEXT NOT NULL, alt TEXT NOT NULL DEFAULT '',"
              " section TEXT NOT NULL DEFAULT '', mime_type TEXT NOT NULL DEFAULT '', encoding TEXT NOT NULL DEFAULT '',"
              " size INTEGER NOT NULL DEFAULT 0, width INTEGER NOT NULL DEFAULT 0, height INTEGER NOT NULL DEFAULT 0,"
              " state TEXT NOT NULL DEFAULT 'pending' CHECK(state IN ('pending','loaded','failed','too_large','unavailable')),"
              " data BLOB,"
              " PRIMARY KEY(message_id, idx));",
              &err)) {
        sqlite3_close(db);
        return fail("open_failed", "the database is unusable");
    }

    // the attachment's saved path was added after the first builds: databases made before it get the column
    {
        sqlite3_stmt* st = nullptr;
        bool has_saved_path = false;
        if (sqlite3_prepare_v2(db, "PRAGMA table_info(attachment)", -1, &st, nullptr) == SQLITE_OK) {
            while (sqlite3_step(st) == SQLITE_ROW) {
                const unsigned char* n = sqlite3_column_text(st, 1);
                if (n && std::string(reinterpret_cast<const char*>(n)) == "saved_path") has_saved_path = true;
            }
        }
        sqlite3_finalize(st);
        if (!has_saved_path && !exec(db, "ALTER TABLE attachment ADD COLUMN saved_path TEXT NOT NULL DEFAULT ''", &err)) {
            sqlite3_close(db);
            return fail("open_failed", "the database is unusable");
        }
    }

    Result<std::unique_ptr<Store>> r;
    r.value.reset(new Store(db, dir, clock));
    // a message found in "sending" was being sent when the app stopped: whether it left is unknown (never resent by itself)
    int recovered = 0;
    r.value->recover_interrupted(&recovered);
    if (recovered > 0) log::event("store.recovered", {log::count("sending", recovered)});
    return r;
}

int Store::schema_version() const { return std::atoi(debug_query("PRAGMA user_version").c_str()); }

Error Store::check_integrity() const {
    const std::string v = debug_query("PRAGMA integrity_check");
    return v == "ok" ? Error() : make_error(ErrorClass::Storage, "integrity_failed", "the database failed its check");
}

std::string Store::debug_exec(const std::string& sql) {
    std::lock_guard<std::mutex> lock(mu_);
    std::string err;
    return exec(db_, sql.c_str(), &err) ? std::string() : (err.empty() ? std::string("error") : err);
}

std::string Store::debug_query(const std::string& sql) const {
    std::lock_guard<std::mutex> lock(mu_);
    std::string out, err;
    query_text(db_, sql, &out, &err);
    return out;
}

}  // namespace pbo
