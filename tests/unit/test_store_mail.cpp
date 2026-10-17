// T069: message bodies and attachment rows honor the constraints of data-model.md; the search index follows the message.
#include <string>

#include "pbo/charset.h"
#include "pbo/store.h"
#include "test_support.h"
#include "tmp_dir.h"

namespace {

struct TestClock : pbo::Clock {
    int64_t t = 1791979200;
    int64_t now() const override { return t; }
};

struct Env {
    pbtest::TmpDir dir;
    TestClock clock;
    std::unique_ptr<pbo::Store> store;
    int64_t folder = 0;
    Env() {
        store = std::move(pbo::Store::open(dir.path(), clock).value);
        pbo::FolderListItem inbox;
        inbox.raw_name = inbox.name = "INBOX";
        store->upsert_folders({inbox});
        folder = store->folders().at(0).id;
    }
    // inserts messages with the given UIDs (newest = highest) and returns their ids
    std::vector<int64_t> add(std::initializer_list<uint32_t> uids) {
        pbo::PullBatch b;
        for (uint32_t uid : uids) {
            pbo::PulledMessage m;
            m.uid = uid;
            m.subject = "Subject " + std::to_string(uid);
            m.from_name = "Anna";
            m.from_addr = "anna@example.org";
            m.date_utc = 1790000000 + uid;
            b.messages.push_back(m);
        }
        store->apply_pull(folder, b);
        std::vector<int64_t> ids;
        for (uint32_t uid : uids) ids.push_back(std::stoll(store->debug_query("SELECT id FROM message WHERE uid = " + std::to_string(uid))));
        return ids;
    }
};

pbo::StoredAttachment att(const char* section, const char* name, int64_t size = 100) {
    pbo::StoredAttachment a;
    a.section = section;
    a.name = name;
    a.mime_type = "application/pdf";
    a.encoding = "base64";
    a.size = size;
    return a;
}

}  // namespace

PB_TEST(a_body_is_written_and_read_back_with_links_and_attachments) {
    Env env;
    const auto ids = env.add({1});
    pbo::BodyUpdate u;
    u.text = "Hello [1]\n\nGr\xC3\xBC\xC3\x9F" "e aus K\xC3\xB6ln";
    u.note = "html_converted";
    u.links = {"https://www.example.org/a?x=1&y=\"2\"", "mailto:a@example.org"};
    u.attachments = {att("2", "report.pdf", 300), att("3", "gr\xC3\xBC\xC3\x9F" "e.png", 64)};
    PB_CHECK(env.store->apply_body(ids[0], u).ok());

    const auto v = env.store->message(ids[0]);
    PB_CHECK(v.has_value());
    PB_CHECK_EQ(v->header.subject, std::string("Subject 1"));
    PB_CHECK_EQ(v->header.body_state, std::string("text"));
    PB_CHECK_EQ(v->body_text, u.text);
    PB_CHECK_EQ(v->body_note, std::string("html_converted"));
    PB_CHECK(v->links == u.links);
    PB_CHECK(v->header.has_attachments);
    PB_CHECK_EQ(v->attachments.size(), static_cast<size_t>(2));
    PB_CHECK_EQ(v->attachments[0].section, std::string("2"));
    PB_CHECK_EQ(v->attachments[0].name, std::string("report.pdf"));
    PB_CHECK_EQ(v->attachments[0].size, static_cast<int64_t>(300));
    PB_CHECK_EQ(v->attachments[0].state, std::string("remote"));
    PB_CHECK_EQ(v->attachments[1].name, std::string("gr\xC3\xBC\xC3\x9F" "e.png"));
}

PB_TEST(an_invalid_state_or_an_unknown_message_changes_nothing) {
    Env env;
    const auto ids = env.add({1});
    pbo::BodyUpdate bad;
    bad.state = "banana";
    bad.text = "x";
    const auto e = env.store->apply_body(ids[0], bad);
    PB_CHECK_EQ(e.key, std::string("invalid_body_state"));
    PB_CHECK_EQ(env.store->message(ids[0])->header.body_state, std::string("headers"));
    pbo::BodyUpdate ok;
    ok.text = "x";
    PB_CHECK_EQ(env.store->apply_body(99999, ok).key, std::string("not_found"));
    PB_CHECK_EQ(env.store->message(99999).has_value(), false);
    // the database itself refuses a wrong state, too
    PB_CHECK(!env.store->debug_exec("UPDATE message SET body_state = 'banana'").empty());
}

PB_TEST(the_body_text_is_cut_at_2_mib_on_a_character_boundary) {
    Env env;
    const auto ids = env.add({1});
    pbo::BodyUpdate u;
    for (int i = 0; i < 800000; ++i) u.text += "\xC3\xA4\xC3\xB6";   // 3.2 MB of two-byte letters
    PB_CHECK(env.store->apply_body(ids[0], u).ok());
    const auto v = env.store->message(ids[0]);
    PB_CHECK(v->body_text.size() <= (2u << 20));
    PB_CHECK(v->body_text.size() > (2u << 20) - 4);
    PB_CHECK(pbo::is_valid_utf8(v->body_text));
    PB_CHECK_EQ(v->body_note, std::string("truncated"));
}

PB_TEST(an_attachment_section_is_unique_and_a_downloaded_one_keeps_its_state) {
    Env env;
    const auto ids = env.add({1});
    pbo::BodyUpdate u;
    u.text = "x";
    u.attachments = {att("2", "a.pdf"), att("3", "b.pdf")};
    PB_CHECK(env.store->apply_body(ids[0], u).ok());
    PB_CHECK(!env.store->debug_exec("INSERT INTO attachment(message_id, section, name) VALUES(" + std::to_string(ids[0]) + ", '2', 'dup')").empty());   // UNIQUE(message_id, section)
    PB_CHECK(env.store->debug_exec("UPDATE attachment SET state = 'downloaded' WHERE section = '2'").empty());

    u.attachments = {att("2", "a-renamed.pdf", 555)};                         // section 3 is gone, 2 has new data
    PB_CHECK(env.store->apply_body(ids[0], u).ok());
    const auto v = env.store->message(ids[0]);
    PB_CHECK_EQ(v->attachments.size(), static_cast<size_t>(1));
    PB_CHECK_EQ(v->attachments[0].name, std::string("a-renamed.pdf"));
    PB_CHECK_EQ(v->attachments[0].size, static_cast<int64_t>(555));
    PB_CHECK_EQ(v->attachments[0].state, std::string("downloaded"));        // the user's download is not forgotten

    u.attachments.clear();
    PB_CHECK(env.store->apply_body(ids[0], u).ok());
    PB_CHECK(!env.store->message(ids[0])->header.has_attachments);
}

PB_TEST(a_downloaded_attachment_remembers_where_it_was_saved) {
    Env env;
    const auto ids = env.add({1});
    pbo::BodyUpdate u;
    u.text = "x";
    u.attachments = {att("2", "a.pdf")};
    PB_CHECK(env.store->apply_body(ids[0], u).ok());
    const int64_t aid = env.store->message(ids[0])->attachments[0].id;
    PB_CHECK_EQ(env.store->message(ids[0])->attachments[0].saved_path, std::string(""));
    PB_CHECK(env.store->set_attachment_state(aid, "downloaded", "", "/mnt/ext1/Downloads/a.pdf").ok());
    PB_CHECK_EQ(env.store->attachment(aid)->attachment.saved_path, std::string("/mnt/ext1/Downloads/a.pdf"));
    PB_CHECK_EQ(env.store->message(ids[0])->attachments[0].saved_path, std::string("/mnt/ext1/Downloads/a.pdf"));
    // a new body keeps the saved path of a known section
    u.attachments = {att("2", "a-renamed.pdf")};
    PB_CHECK(env.store->apply_body(ids[0], u).ok());
    PB_CHECK_EQ(env.store->message(ids[0])->attachments[0].saved_path, std::string("/mnt/ext1/Downloads/a.pdf"));
    // going back to "remote" forgets the path
    PB_CHECK(env.store->set_attachment_state(aid, "remote", "refused").ok());
    PB_CHECK_EQ(env.store->attachment(aid)->attachment.saved_path, std::string(""));
}

PB_TEST(a_database_made_before_the_saved_path_existed_gets_the_column) {
    pbtest::TmpDir dir;
    TestClock clock;
    {
        auto s = pbo::Store::open(dir.path(), clock);
        PB_CHECK(s.ok());
        PB_CHECK(s.value->debug_exec("ALTER TABLE attachment DROP COLUMN saved_path").empty());   // as an earlier build left it
    }
    auto again = pbo::Store::open(dir.path(), clock);
    PB_CHECK(again.ok());
    PB_CHECK(again.value->debug_exec("SELECT saved_path FROM attachment").empty());
}

PB_TEST(messages_without_a_body_are_listed_newest_first_and_limited) {
    Env env;
    const auto ids = env.add({1, 2, 3, 4});
    pbo::BodyUpdate u;
    u.text = "x";
    PB_CHECK(env.store->apply_body(ids[1], u).ok());                           // uid 2 has its body
    const auto missing = env.store->messages_missing_body(env.folder, 10);
    PB_CHECK_EQ(missing.size(), static_cast<size_t>(3));
    PB_CHECK_EQ(missing[0].uid, 4u);
    PB_CHECK_EQ(missing[1].uid, 3u);
    PB_CHECK_EQ(missing[2].uid, 1u);
    PB_CHECK_EQ(env.store->messages_missing_body(env.folder, 2).size(), static_cast<size_t>(2));
    PB_CHECK(env.store->messages_missing_body(env.folder + 99, 10).empty());
}

PB_TEST(opening_a_message_marks_it_for_the_least_recently_used_rule) {
    Env env;
    const auto ids = env.add({1});
    PB_CHECK_EQ(env.store->debug_query("SELECT last_opened IS NULL FROM message"), std::string("1"));
    env.clock.t = 1791990000;
    PB_CHECK(env.store->message(ids[0]).has_value());
    PB_CHECK_EQ(env.store->debug_query("SELECT last_opened FROM message"), std::string("1791990000"));
}

PB_TEST(the_search_index_follows_the_body_and_the_message) {
    Env env;
    const auto ids = env.add({1, 2});
    pbo::BodyUpdate u;
    u.text = "Die Rechnung f\xC3\xBCr K\xC3\xB6ln ist da";
    PB_CHECK(env.store->apply_body(ids[0], u).ok());
    PB_CHECK_EQ(env.store->debug_query("SELECT count(*) FROM message_fts WHERE message_fts MATCH 'k\xC3\xB6ln'"), std::string("1"));      // diacritics folded
    PB_CHECK_EQ(env.store->debug_query("SELECT count(*) FROM message_fts WHERE message_fts MATCH 'koeln OR koln'"), std::string("1"));
    PB_CHECK_EQ(env.store->debug_query("SELECT count(*) FROM message_fts WHERE message_fts MATCH 'Subject'"), std::string("2"));
    // deleting the folder removes its messages, their attachments AND their index rows
    pbo::BodyUpdate with_att;
    with_att.text = "x";
    with_att.attachments = {att("2", "a.pdf")};
    PB_CHECK(env.store->apply_body(ids[1], with_att).ok());
    PB_CHECK(env.store->debug_exec("DELETE FROM folder WHERE id = " + std::to_string(env.folder)).empty());
    PB_CHECK_EQ(env.store->debug_query("SELECT count(*) FROM message"), std::string("0"));
    PB_CHECK_EQ(env.store->debug_query("SELECT count(*) FROM attachment"), std::string("0"));
    PB_CHECK_EQ(env.store->debug_query("SELECT count(*) FROM message_fts WHERE message_fts MATCH 'Subject'"), std::string("0"));
}

PB_TEST(pulled_headers_keep_local_flags_equal_to_the_server_flags_while_synced) {
    Env env;
    pbo::PullBatch b;
    pbo::PulledMessage m;
    m.uid = 7;
    m.subject = "s";
    m.date_utc = 1790000000;
    m.flags = pbo::kFlagSeen | pbo::kFlagFlagged;
    b.messages.push_back(m);
    PB_CHECK(env.store->apply_pull(env.folder, b).ok());
    PB_CHECK_EQ(env.store->debug_query("SELECT flags_base = flags_local FROM message WHERE uid = 7"), std::string("1"));
    PB_CHECK_EQ(env.store->debug_query("SELECT local_state FROM message WHERE uid = 7"), std::string("synced"));
}

PB_TEST(header_text_from_the_network_is_capped_on_a_character_boundary_and_dates_are_clamped) {
    Env env;
    pbo::PullBatch b;
    pbo::PulledMessage m;
    m.uid = 1;
    for (int i = 0; i < 1500; ++i) m.subject += "\xC3\xA4";                    // 3000 bytes
    m.to_text = std::string(20000, 'a');
    m.date_utc = 99999999999;                                                  // far in the future
    b.messages.push_back(m);
    pbo::PulledMessage neg;
    neg.uid = 2;
    neg.date_utc = -5;
    neg.internal_date = std::nullopt;
    b.messages.push_back(neg);
    PB_CHECK(env.store->apply_pull(env.folder, b).ok());
    const std::string subject = env.store->debug_query("SELECT subject FROM message WHERE uid = 1");
    PB_CHECK(subject.size() <= 998);
    PB_CHECK(subject.size() > 990);
    PB_CHECK(pbo::is_valid_utf8(subject));
    PB_CHECK(std::stoll(env.store->debug_query("SELECT length(to_text) FROM message WHERE uid = 1")) <= 8192);
    PB_CHECK_EQ(env.store->debug_query("SELECT date_utc FROM message WHERE uid = 1"), std::string("4102444800"));
    PB_CHECK_EQ(env.store->debug_query("SELECT date_utc FROM message WHERE uid = 2"), std::string("0"));
}

PB_TEST(a_folder_can_be_switched_off_and_the_unsynced_count_covers_local_changes_and_the_outbox) {
    Env env;
    pbo::FolderListItem sent;
    sent.raw_name = sent.name = "Sent";
    sent.attributes = {"\\Sent"};
    env.store->upsert_folders({sent});
    int64_t sent_id = 0;
    for (const pbo::Folder& f : env.store->folders())
        if (f.role == "sent") sent_id = f.id;
    PB_CHECK(sent_id != 0);
    PB_CHECK(env.store->set_folder_sync_enabled(sent_id, false).ok());
    for (const pbo::Folder& f : env.store->folders())
        if (f.id == sent_id) PB_CHECK(!f.sync_enabled);
    PB_CHECK(env.store->set_folder_sync_enabled(sent_id, true).ok());
    for (const pbo::Folder& f : env.store->folders())
        if (f.id == sent_id) PB_CHECK(f.sync_enabled);
    PB_CHECK(!env.store->set_folder_sync_enabled(987654, true).ok());   // no such folder

    // a re-listing keeps the choice of the user
    PB_CHECK(env.store->set_folder_sync_enabled(sent_id, false).ok());
    env.store->upsert_folders({sent});
    for (const pbo::Folder& f : env.store->folders())
        if (f.id == sent_id) PB_CHECK(!f.sync_enabled);

    PB_CHECK_EQ(env.store->unsynced_count(), 0);
    env.add({1, 2, 3});
    env.store->debug_exec("UPDATE message SET local_state = 'pending_flags' WHERE uid = 2");
    env.store->debug_exec("UPDATE message SET local_state = 'pending_delete' WHERE uid = 3");
    PB_CHECK_EQ(env.store->unsynced_count(), 2);
    env.store->debug_exec("INSERT INTO outbox(message_id, envelope_from, envelope_to, raw_message, state, queued_at, state_at) VALUES('<o@x>','a@x','b@x',x'41','queued',1,1)");
    env.store->debug_exec("INSERT INTO outbox(message_id, envelope_from, envelope_to, raw_message, state, queued_at, state_at) VALUES('<p@x>','a@x','b@x',x'41','failed',1,1)");
    PB_CHECK_EQ(env.store->unsynced_count(), 4);   // a failed send also waits for the user
}

PB_TEST_MAIN()
