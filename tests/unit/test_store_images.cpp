// T190: the pictures of a message in the store. A row per picture with its state; the data (a BLOB) arrives when the picture is loaded; a re-fetched body
// keeps what was loaded; deleting a message takes its pictures; eviction drops the picture data before whole messages.
#include <string>

#include "pbo/store.h"
#include "test_support.h"
#include "tmp_dir.h"

namespace {

struct Clock : pbo::Clock {
    int64_t now() const override { return 1791979200; }
};

constexpr int64_t kDay = 86400;
constexpr int64_t kNow = 1791979200;

struct Env {
    pbtest::TmpDir dir;
    Clock clock;
    std::unique_ptr<pbo::Store> store;
    int64_t inbox = 0;
    Env() {
        store = std::move(pbo::Store::open(dir.path(), clock).value);
        pbo::FolderListItem i;
        i.raw_name = i.name = "INBOX";
        store->upsert_folders({i});
        inbox = store->folders()[0].id;
    }
    int64_t add(uint32_t uid, int age_days = 1) {
        pbo::PullBatch b;
        pbo::PulledMessage m;
        m.uid = uid;
        m.subject = "Message " + std::to_string(uid);
        m.from_addr = "a@example.org";
        m.date_utc = kNow - age_days * kDay;
        b.messages.push_back(m);
        PB_CHECK(store->apply_pull(inbox, b).ok());
        return std::atoll(store->debug_query("SELECT id FROM message WHERE uid = " + std::to_string(uid)).c_str());
    }
    static pbo::StoredImage image(int idx, const std::string& kind, const std::string& src) {
        pbo::StoredImage im;
        im.idx = idx;
        im.kind = kind;
        im.src = src;
        im.alt = "alt " + std::to_string(idx);
        if (kind == "cid") {
            im.section = "1." + std::to_string(idx + 1);
            im.mime_type = "image/png";
            im.encoding = "base64";
            im.size = 1000;
        }
        return im;
    }
    pbo::BodyUpdate body(std::vector<pbo::StoredImage> images, const std::string& text = "Hello\n\xEF\xBF\xBC" "1\xEF\xBF\xBC") {
        pbo::BodyUpdate u;
        u.text = text;
        u.images = std::move(images);
        return u;
    }
};

}  // namespace

PB_TEST(a_body_with_pictures_stores_a_row_per_picture_with_the_state_pending) {
    Env e;
    const int64_t id = e.add(1);
    PB_CHECK(e.store->apply_body(id, e.body({Env::image(1, "cid", "cid:logo"), Env::image(2, "remote", "https://img.example.org/a.jpg")})).ok());
    const auto images = e.store->message_images(id);
    PB_CHECK_EQ(images.size(), size_t(2));
    PB_CHECK_EQ(images[0].idx, 1);
    PB_CHECK_EQ(images[0].kind, std::string("cid"));
    PB_CHECK_EQ(images[0].section, std::string("1.2"));
    PB_CHECK_EQ(images[0].state, std::string("pending"));
    PB_CHECK_EQ(images[1].kind, std::string("remote"));
    PB_CHECK_EQ(images[1].src, std::string("https://img.example.org/a.jpg"));
    PB_CHECK(!e.store->image_data(id, 1).has_value());
}

PB_TEST(loading_a_picture_stores_its_bytes_and_a_failure_is_remembered) {
    Env e;
    const int64_t id = e.add(1);
    PB_CHECK(e.store->apply_body(id, e.body({Env::image(1, "cid", "cid:a"), Env::image(2, "remote", "https://i.example.org/b.png")})).ok());
    PB_CHECK(e.store->set_image_loaded(id, 1, "image/png", std::string("\x89PNG-bytes", 10)).ok());
    PB_CHECK(e.store->set_image_state(id, 2, "too_large").ok());
    const auto images = e.store->message_images(id);
    PB_CHECK_EQ(images[0].state, std::string("loaded"));
    PB_CHECK_EQ(images[0].size, int64_t(10));
    PB_CHECK_EQ(images[1].state, std::string("too_large"));
    PB_CHECK_EQ(e.store->image_data(id, 1).value_or(""), std::string("\x89PNG-bytes", 10));
    PB_CHECK(!e.store->set_image_state(id, 2, "bogus").ok());                    // only the known states
    PB_CHECK(!e.store->set_image_loaded(id, 9, "image/png", "x").ok());          // no such picture
    PB_CHECK(!e.store->set_image_loaded(id, 2, "text/html", "<p>").ok());        // only picture types are kept
}

PB_TEST(a_refetched_body_keeps_the_loaded_pictures_and_drops_the_ones_that_are_gone) {
    Env e;
    const int64_t id = e.add(1);
    PB_CHECK(e.store->apply_body(id, e.body({Env::image(1, "cid", "cid:a"), Env::image(2, "remote", "https://i.example.org/b.png")})).ok());
    PB_CHECK(e.store->set_image_loaded(id, 1, "image/png", "AAAA").ok());
    PB_CHECK(e.store->set_image_loaded(id, 2, "image/png", "BBBB").ok());
    PB_CHECK(e.store->apply_body(id, e.body({Env::image(1, "cid", "cid:a")})).ok());   // the second picture is not in the new text
    const auto images = e.store->message_images(id);
    PB_CHECK_EQ(images.size(), size_t(1));
    PB_CHECK_EQ(images[0].state, std::string("loaded"));
    PB_CHECK_EQ(e.store->image_data(id, 1).value_or(""), std::string("AAAA"));
    PB_CHECK(!e.store->image_data(id, 2).has_value());
    // a different picture under the same number is a new one
    PB_CHECK(e.store->apply_body(id, e.body({Env::image(1, "cid", "cid:other")})).ok());
    PB_CHECK_EQ(e.store->message_images(id)[0].state, std::string("pending"));
}

PB_TEST(deleting_the_message_takes_its_pictures_and_the_search_index_has_no_markers) {
    Env e;
    const int64_t id = e.add(1);
    PB_CHECK(e.store->apply_body(id, e.body({Env::image(1, "cid", "cid:a")}, "Offer for you\n\xEF\xBF\xBC" "1\xEF\xBF\xBC\nmore text")).ok());
    PB_CHECK(e.store->set_image_loaded(id, 1, "image/png", "AAAA").ok());
    const auto hits = e.store->search("offer", 10);
    PB_CHECK_EQ(hits.size(), size_t(1));
    PB_CHECK_EQ(e.store->debug_query("SELECT count(*) FROM message_fts WHERE message_fts MATCH 'body_text:\"1\"'"), std::string("0"));   // the number of a picture is not a word of the mail
    e.store->debug_exec("DELETE FROM message WHERE id = " + std::to_string(id));
    PB_CHECK_EQ(e.store->debug_query("SELECT count(*) FROM message_image"), std::string("0"));
}

PB_TEST(the_picture_data_goes_before_old_messages_when_the_space_is_short) {
    Env e;
    std::vector<int64_t> ids;
    for (uint32_t i = 1; i <= 10; ++i) {
        const int64_t id = e.add(i, 100 + static_cast<int>(i));   // old: outside a 30 day window
        PB_CHECK(e.store->apply_body(id, e.body({Env::image(1, "cid", "cid:a")})).ok());
        PB_CHECK(e.store->set_image_loaded(id, 1, "image/png", std::string(150000, 'x')).ok());
        ids.push_back(id);
    }
    const auto used = e.store->usage().database_bytes;
    PB_CHECK(used > 1400000);
    const auto r = e.store->evict_to_limit(used - 400000, kNow - 30 * kDay);
    PB_CHECK(r.deleted == 0);                                                      // the pictures were enough: no message went
    PB_CHECK_EQ(e.store->debug_query("SELECT count(*) FROM message"), std::string("10"));
    PB_CHECK(std::atoi(e.store->debug_query("SELECT count(*) FROM message_image WHERE state = 'pending'").c_str()) >= 3);   // dropped data: loadable again
    PB_CHECK(!r.over_limit);
}

PB_TEST_MAIN()
