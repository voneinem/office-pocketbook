// Store: the pictures of a message's HTML body (data-model.md: message_image). A row per picture with its state; the bytes (a BLOB) arrive when the picture
// is loaded and go again when the space is short. A message that is deleted takes its pictures (cascade).
#include <sqlite3.h>

#include <algorithm>

#include "pbo/store.h"
#include "store_sql.h"
#include "store_util.h"

namespace pbo {

using sql::Stmt;
using store_util::cap;
using store_util::storage_error;
using store_util::Transaction;

namespace {

constexpr size_t kMaxPictureBytes = 8u << 20;   // one picture; the fetch step has its own, smaller limits

bool picture_type(const std::string& t) {
    return t == "image/png" || t == "image/jpeg" || t == "image/jpg" || t == "image/pjpeg" || t == "image/gif" || t == "image/bmp" || t == "image/webp";
}

StoredImage read_row(Stmt& st) {
    StoredImage im;
    im.idx = static_cast<int>(st.int64(0));
    im.kind = st.text(1);
    im.src = st.text(2);
    im.alt = st.text(3);
    im.section = st.text(4);
    im.mime_type = st.text(5);
    im.encoding = st.text(6);
    im.size = st.int64(7);
    im.width = static_cast<int>(st.int64(8));
    im.height = static_cast<int>(st.int64(9));
    im.state = st.text(10);
    return im;
}

}  // namespace

namespace store_util {

std::string without_picture_markers(const std::string& text) {
    static const std::string kMark = "\xEF\xBF\xBC";
    std::string out;
    out.reserve(text.size());
    size_t i = 0;
    while (i < text.size()) {
        if (text.compare(i, 3, kMark) == 0) {
            size_t j = i + 3;
            while (j < text.size() && text[j] >= '0' && text[j] <= '9') ++j;
            if (j > i + 3 && text.compare(j, 3, kMark) == 0) {   // a whole marker: a line of its own, gone with its line end
                i = j + 3;
                if (i < text.size() && text[i] == '\n') ++i;
                continue;
            }
        }
        out += text[i++];
    }
    return out;
}

bool replace_images_sql(sqlite3* db, int64_t message_id, const std::vector<StoredImage>& images) {
    // the pictures that are not in the new list (by number and source) go
    {
        Stmt st(db, "SELECT idx, src FROM message_image WHERE message_id = ?");
        if (!st.ok()) return false;
        st.bind_int64(1, message_id);
        std::vector<int> drop;
        while (st.step() == SQLITE_ROW) {
            const int idx = static_cast<int>(st.int64(0));
            const std::string src = st.text(1);
            const bool kept = std::any_of(images.begin(), images.end(), [&](const StoredImage& im) { return im.idx == idx && im.src == src; });
            if (!kept) drop.push_back(idx);
        }
        for (int idx : drop) {
            Stmt d(db, "DELETE FROM message_image WHERE message_id = ? AND idx = ?");
            if (!d.ok()) return false;
            d.bind_int64(1, message_id);
            d.bind_int64(2, idx);
            if (d.step() != SQLITE_DONE) return false;
        }
    }
    for (const StoredImage& im : images) {
        if (im.idx < 1 || im.idx > 1000 || (im.kind != "cid" && im.kind != "remote")) continue;
        const std::string state = im.state == "unavailable" ? "unavailable" : "pending";
        Stmt ins(db,
                 "INSERT INTO message_image(message_id, idx, kind, src, alt, section, mime_type, encoding, size, width, height, state) VALUES(?,?,?,?,?,?,?,?,?,?,?,?)"
                 " ON CONFLICT(message_id, idx) DO UPDATE SET kind=excluded.kind, alt=excluded.alt, section=excluded.section, mime_type=CASE WHEN message_image.state='loaded' THEN message_image.mime_type ELSE excluded.mime_type END,"
                 " encoding=excluded.encoding, width=excluded.width, height=excluded.height,"
                 " size=CASE WHEN message_image.state='loaded' THEN message_image.size ELSE excluded.size END");
        if (!ins.ok()) return false;
        ins.bind_int64(1, message_id);
        ins.bind_int64(2, im.idx);
        ins.bind_text(3, im.kind);
        ins.bind_text(4, cap(im.src, 2100));
        ins.bind_text(5, cap(im.alt, 200));
        ins.bind_text(6, cap(im.section, 64));
        ins.bind_text(7, cap(im.mime_type, 100));
        ins.bind_text(8, cap(im.encoding, 64));
        ins.bind_int64(9, im.size < 0 ? 0 : im.size);
        ins.bind_int64(10, std::max(0, im.width));
        ins.bind_int64(11, std::max(0, im.height));
        ins.bind_text(12, state);
        if (ins.step() != SQLITE_DONE) return false;
    }
    return true;
}

}  // namespace store_util

std::vector<StoredImage> Store::message_images(int64_t message_id) const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<StoredImage> out;
    Stmt st(db_, "SELECT idx, kind, src, alt, section, mime_type, encoding, size, width, height, state FROM message_image WHERE message_id = ? ORDER BY idx");
    if (!st.ok()) return out;
    st.bind_int64(1, message_id);
    while (st.step() == SQLITE_ROW) out.push_back(read_row(st));
    return out;
}

std::optional<MessageLocation> Store::message_location(int64_t message_id) const {
    std::lock_guard<std::mutex> lock(mu_);
    Stmt st(db_, "SELECT m.uid, m.folder_id, f.raw_name FROM message m JOIN folder f ON f.id = m.folder_id WHERE m.id = ?");
    if (!st.ok()) return std::nullopt;
    st.bind_int64(1, message_id);
    if (st.step() != SQLITE_ROW) return std::nullopt;
    MessageLocation l;
    l.uid = static_cast<uint32_t>(st.int64(0));
    l.folder_id = st.int64(1);
    l.folder_raw_name = st.text(2);
    return l;
}

std::optional<std::string> Store::image_data(int64_t message_id, int idx) const {
    std::lock_guard<std::mutex> lock(mu_);
    Stmt st(db_, "SELECT data FROM message_image WHERE message_id = ? AND idx = ? AND state = 'loaded'");
    if (!st.ok()) return std::nullopt;
    st.bind_int64(1, message_id);
    st.bind_int64(2, idx);
    if (st.step() != SQLITE_ROW || st.is_null(0)) return std::nullopt;
    return st.blob(0);
}

Error Store::set_image_loaded(int64_t message_id, int idx, const std::string& mime_type, const std::string& bytes) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!picture_type(mime_type)) return make_error(ErrorClass::Storage, "not_a_picture", "not a picture type");
    if (bytes.empty() || bytes.size() > kMaxPictureBytes) return make_error(ErrorClass::Storage, "too_large", "the picture is empty or too large");
    Stmt up(db_, "UPDATE message_image SET state = 'loaded', data = ?, mime_type = ?, size = ? WHERE message_id = ? AND idx = ?");
    if (!up.ok()) return storage_error(db_, "write_failed");
    up.bind_blob(1, bytes);
    up.bind_text(2, mime_type);
    up.bind_int64(3, static_cast<long long>(bytes.size()));
    up.bind_int64(4, message_id);
    up.bind_int64(5, idx);
    if (up.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
    if (sqlite3_changes(db_) == 0) return make_error(ErrorClass::Storage, "not_found", "no such picture");
    return Error();
}

Error Store::set_image_state(int64_t message_id, int idx, const std::string& state) {
    std::lock_guard<std::mutex> lock(mu_);
    if (state != "pending" && state != "failed" && state != "too_large" && state != "unavailable") return make_error(ErrorClass::Storage, "invalid_state", "not a picture state");
    Stmt up(db_, "UPDATE message_image SET state = ?, data = NULL WHERE message_id = ? AND idx = ?");
    if (!up.ok()) return storage_error(db_, "write_failed");
    up.bind_text(1, state);
    up.bind_int64(2, message_id);
    up.bind_int64(3, idx);
    if (up.step() != SQLITE_DONE) return storage_error(db_, "write_failed");
    if (sqlite3_changes(db_) == 0) return make_error(ErrorClass::Storage, "not_found", "no such picture");
    return Error();
}

}  // namespace pbo
