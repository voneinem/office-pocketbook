// FTS5 must be compiled into the vendored SQLite (research R-17: offline search over sender, subject and
// body text, SC-004). German umlauts must match with the diacritics removed.
#include <sqlite3.h>

#include <string>
#include <vector>

#include "test_support.h"

namespace {

struct Db {
    sqlite3* db = nullptr;
    Db() { sqlite3_open(":memory:", &db); }
    ~Db() { sqlite3_close(db); }
    // Runs a statement; returns the sqlite error text ("" on success).
    std::string exec(const std::string& sql) {
        char* err = nullptr;
        const int rc = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err);
        std::string out = rc == SQLITE_OK ? "" : (err ? err : "error");
        sqlite3_free(err);
        return out;
    }
    int count_matches(const std::string& query) {
        sqlite3_stmt* st = nullptr;
        const std::string sql = "SELECT count(*) FROM t WHERE t MATCH ?1";
        if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) return -1;
        sqlite3_bind_text(st, 1, query.c_str(), -1, SQLITE_TRANSIENT);
        int n = -1;
        if (sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int(st, 0);
        sqlite3_finalize(st);
        return n;
    }
};

}  // namespace

PB_TEST(fts5_table_can_be_created) {
    Db d;
    PB_CHECK_EQ(d.exec("CREATE VIRTUAL TABLE t USING fts5(a, tokenize='unicode61 remove_diacritics 2')"), std::string());
}

PB_TEST(fts5_prefix_queries_find_umlauts_without_diacritics) {
    Db d;
    PB_CHECK_EQ(d.exec("CREATE VIRTUAL TABLE t USING fts5(a, tokenize='unicode61 remove_diacritics 2')"), std::string());
    PB_CHECK_EQ(d.exec("INSERT INTO t(a) VALUES ('Gr\xC3\xBC\xC3\x9F" "e aus K\xC3\xB6ln')"), std::string());
    PB_CHECK_EQ(d.count_matches("gru*"), 1);          // u without the diaeresis
    PB_CHECK_EQ(d.count_matches("gr\xC3\xBC*"), 1);   // with the diaeresis
    PB_CHECK_EQ(d.count_matches("koln*"), 1);
    PB_CHECK_EQ(d.count_matches("nichts*"), 0);
}

PB_TEST_MAIN()
