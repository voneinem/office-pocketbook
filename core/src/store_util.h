#pragma once
// Private to the store implementation: helpers shared by store_mail.cpp and store_calendar.cpp.

#include <sqlite3.h>

#include <algorithm>
#include <optional>
#include <string>

#include <vector>

#include "pbo/calendar_types.h"
#include "pbo/mail_types.h"
#include "pbo/types.h"
#include "store_sql.h"

namespace pbo {
namespace store_util {

// At most `max` bytes, never cutting inside a UTF-8 character.
inline std::string cap(const std::string& s, size_t max) {
    if (s.size() <= max) return s;
    size_t cut = max;
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;
    return s.substr(0, cut);
}

Error storage_error(sqlite3* db, const char* fallback_key);   // store_util.cpp

// The pictures of a message in one transaction of the caller (store_images.cpp): rows of pictures that are not in `images` any more (by number and source) go,
// the others are inserted or updated; a picture with the same number and source keeps its state and data. False when SQLite refuses.
bool replace_images_sql(sqlite3* db, int64_t message_id, const std::vector<StoredImage>& images);
// The text without the picture markers (U+FFFC, number, U+FFFC): what the search index holds.
std::string without_picture_markers(const std::string& text);

struct Transaction {
    sqlite3* db;
    bool open = false;
    explicit Transaction(sqlite3* d) : db(d) { open = sql::exec(db, "BEGIN IMMEDIATE", nullptr); }
    bool commit() {
        if (!open) return false;
        open = false;
        if (sql::exec(db, "COMMIT", nullptr)) return true;
        sql::exec(db, "ROLLBACK", nullptr);
        return false;
    }
    ~Transaction() {
        if (open) sql::exec(db, "ROLLBACK", nullptr);
    }
};

inline void bind_text_or_null(sql::Stmt& st, int i, const std::string& v) {
    if (v.empty()) st.bind_null(i);
    else st.bind_text(i, v);
}

// The occurrences of an event (the caller deletes the old ones first); false on a failing statement.
bool insert_occurrences(sqlite3* db, int64_t event_id, const std::vector<OccurrenceRow>& rows);

// Address suggestions (the table address_suggestion): a plain address only (false otherwise, nothing written), lower-cased; the pruning to
// 2,000 rows (the least used and oldest first) is a separate call so that a batch prunes once.
bool note_address_sql(sqlite3* db, int64_t now, const std::string& addr, const std::string& name, int weight);
bool prune_addresses_sql(sqlite3* db);

inline void bind_opt(sql::Stmt& st, int i, const std::optional<int64_t>& v) {
    if (v) st.bind_int64(i, *v);
    else st.bind_null(i);
}

inline std::optional<int64_t> opt_col(const sql::Stmt& st, int col) {
    if (st.is_null(col)) return std::nullopt;
    return st.int64(col);
}

}  // namespace store_util
}  // namespace pbo
