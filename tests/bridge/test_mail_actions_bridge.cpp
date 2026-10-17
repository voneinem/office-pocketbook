// T159: the bridges of US5 - mark read / unread, star, move, delete (ReaderBridge), sort and search (MessageListModel), the messages that "could not be
// applied" (ConflictsModel) and the address suggestions of the recipient fields (ComposeBridge). The real core and a throw-away store.
#include <QCoreApplication>
#include <QSignalSpy>
#include <QTest>
#include <QVariantMap>

#include "compose_bridge.h"
#include "conflicts_model.h"
#include "message_list_model.h"
#include "pbo/store.h"
#include "reader_bridge.h"
#include "test_support.h"
#include "tmp_dir.h"

namespace {

int g_argc = 1;
char g_arg0[] = "test_mail_actions_bridge";
char* g_argv[] = {g_arg0, nullptr};
QCoreApplication g_app(g_argc, g_argv);

struct Clock : pbo::Clock {
    int64_t now() const override { return 1791979200; }
};
struct Rng : pbo::Random {
    uint8_t n = 0;
    void bytes(uint8_t* out, size_t len) override {
        for (size_t i = 0; i < len; ++i) out[i] = n++;
    }
};

struct Env {
    pbtest::TmpDir dir;
    Clock clock;
    Rng rng;
    std::unique_ptr<pbo::Store> store;
    int64_t inbox = 0, trash = 0, archive = 0;
    Env(bool with_trash = true) {
        store = std::move(pbo::Store::open(dir.path(), clock).value);
        pbo::MailAccount a;
        a.imap_host = "mail.example.org";
        a.smtp_host = "smtp.example.org";
        a.username = "alice";
        a.address = "alice@example.org";
        store->save_mail_account(a);
        pbo::FolderListItem i, t, ar;
        i.raw_name = i.name = "INBOX";
        t.raw_name = t.name = "Trash";
        t.attributes = {"\\Trash"};
        ar.raw_name = ar.name = "Archive";
        ar.attributes = {"\\Archive"};
        std::vector<pbo::FolderListItem> list = {i, ar};
        if (with_trash) list.push_back(t);
        store->upsert_folders(list);
        for (const auto& f : store->folders()) {
            if (f.name == "INBOX") inbox = f.id;
            if (f.name == "Trash") trash = f.id;
            if (f.name == "Archive") archive = f.id;
        }
    }
    qlonglong add(int64_t folder, uint32_t uid, const std::string& subject, const std::string& from = "Anna", const std::string& addr = "anna@example.org", uint32_t flags = 0, int64_t date = 0) {
        pbo::PullBatch b;
        pbo::PulledMessage m;
        m.uid = uid;
        m.subject = subject;
        m.from_name = from;
        m.from_addr = addr;
        m.message_id = "<m" + std::to_string(uid) + "@x>";
        m.date_utc = date ? date : 1790000000 + uid;
        m.flags = flags;
        b.messages.push_back(m);
        PB_CHECK(store->apply_pull(folder, b).ok());
        return std::stoll(store->debug_query("SELECT id FROM message WHERE folder_id = " + std::to_string(folder) + " AND uid = " + std::to_string(uid)));
    }
    std::string q(const std::string& sql) { return store->debug_query(sql); }
};

}  // namespace

PB_TEST(the_reader_marks_a_message_read_and_unread_and_stars_it) {
    Env e;
    const qlonglong id = e.add(e.inbox, 1, "Hello");
    ReaderBridge r(ReaderBridge::Config{e.store.get()});
    QSignalSpy results(&r, &ReaderBridge::actionResult);
    r.open(id);
    PB_CHECK(r.unread());
    PB_CHECK(!r.flagged());
    r.markRead();
    PB_CHECK(!r.unread());
    PB_CHECK_EQ(e.q("SELECT local_state || '|' || flags_local FROM message WHERE id = " + std::to_string(id)), std::string("pending_flags|1"));
    PB_CHECK(r.pendingMark());
    r.setFlagged(true);
    PB_CHECK(r.flagged());
    r.markUnread();
    PB_CHECK(r.unread());
    PB_CHECK_EQ(e.q("SELECT flags_local FROM message WHERE id = " + std::to_string(id)), std::string("4"));
    PB_CHECK(results.count() >= 3);
    PB_CHECK(results.last().at(1).toBool());
}

PB_TEST(the_reader_offers_the_other_folders_as_move_targets_and_moves) {
    Env e;
    const qlonglong id = e.add(e.inbox, 1, "Hello");
    ReaderBridge r(ReaderBridge::Config{e.store.get()});
    r.open(id);
    const QVariantList targets = r.moveTargets();
    QStringList names;
    for (const QVariant& t : targets) names << t.toMap()["name"].toString();
    PB_CHECK(!names.contains(QStringLiteral("INBOX")));
    PB_CHECK(names.contains(QStringLiteral("Archive")));
    PB_CHECK(names.contains(QStringLiteral("Trash")));
    PB_CHECK(r.moveTo(e.archive));
    PB_CHECK_EQ(e.q("SELECT local_state || '|' || move_target_folder_id FROM message WHERE id = " + std::to_string(id)), "pending_move|" + std::to_string(e.archive));
}

PB_TEST(the_delete_wording_depends_on_where_the_message_is_and_a_permanent_delete_is_a_separate_step) {
    Env e;
    const qlonglong in_inbox = e.add(e.inbox, 1, "A");
    const qlonglong in_trash = e.add(e.trash, 2, "B");
    ReaderBridge r(ReaderBridge::Config{e.store.get()});
    r.open(in_inbox);
    PB_CHECK_EQ(r.deleteKind().toStdString(), std::string("to_trash"));
    r.open(in_trash);
    PB_CHECK_EQ(r.deleteKind().toStdString(), std::string("permanent_in_trash"));
    PB_CHECK(!r.deleteMessage(false));                   // not confirmed as permanent
    PB_CHECK_EQ(e.q("SELECT local_state FROM message WHERE id = " + std::to_string(in_trash)), std::string("synced"));
    PB_CHECK(r.deleteMessage(true));
    PB_CHECK_EQ(e.q("SELECT local_state FROM message WHERE id = " + std::to_string(in_trash)), std::string("pending_delete"));
    r.open(in_inbox);
    PB_CHECK(r.deleteMessage(false));
    PB_CHECK_EQ(e.q("SELECT local_state FROM message WHERE id = " + std::to_string(in_inbox)), std::string("pending_move"));
    Env n(false);
    const qlonglong lone = n.add(n.inbox, 1, "C");
    ReaderBridge r2(ReaderBridge::Config{n.store.get()});
    r2.open(lone);
    PB_CHECK_EQ(r2.deleteKind().toStdString(), std::string("permanent_no_trash"));
    PB_CHECK(!r2.deleteMessage(false));
    PB_CHECK(r2.deleteMessage(true));
}

PB_TEST(the_list_sorts_and_the_sort_is_kept_while_paging) {
    Env e;
    e.add(e.inbox, 1, "banana", "Zoe", "zoe@x", 0, 100);
    e.add(e.inbox, 2, "Apple", "anna", "anna@x", 0, 300);
    e.add(e.inbox, 3, "cherry", "Mia", "mia@x", 0, 200);
    MessageListModel m(MessageListModel::Config{e.store.get()});
    m.setPageSize(2);
    m.open(e.inbox);
    PB_CHECK_EQ(m.sort().toStdString(), std::string("date"));
    PB_CHECK_EQ(m.rows().at(0).toMap()["subject"].toString().toStdString(), std::string("Apple"));
    m.setSort(QStringLiteral("subject"));
    PB_CHECK_EQ(m.rows().at(0).toMap()["subject"].toString().toStdString(), std::string("Apple"));
    m.nextPage();
    PB_CHECK_EQ(m.rows().at(0).toMap()["subject"].toString().toStdString(), std::string("cherry"));
    m.setSort(QStringLiteral("sender"));
    PB_CHECK_EQ(m.page(), 0);                              // a new order starts at the first page
    PB_CHECK_EQ(m.rows().at(0).toMap()["sender"].toString().toStdString(), std::string("anna"));
    m.setSort(QStringLiteral("dateAsc"));
    PB_CHECK_EQ(m.rows().at(0).toMap()["subject"].toString().toStdString(), std::string("banana"));
    m.setSort(QStringLiteral("nonsense"));
    PB_CHECK_EQ(m.sort().toStdString(), std::string("dateAsc"));
}

PB_TEST(the_search_model_lists_results_of_every_folder_page_by_page) {
    Env e;
    for (uint32_t i = 1; i <= 5; ++i) e.add(e.inbox, i, "Report " + std::to_string(i));
    e.add(e.archive, 9, "Report old");
    e.add(e.inbox, 10, "Lunch");
    MessageListModel s(MessageListModel::Config{e.store.get()});
    s.setPageSize(4);
    s.search(QStringLiteral("report"));
    PB_CHECK(s.searching());
    PB_CHECK_EQ(s.total(), 6);
    PB_CHECK_EQ(s.pageCount(), 2);
    PB_CHECK_EQ(s.rows().size(), 4);
    s.nextPage();
    PB_CHECK_EQ(s.rows().size(), 2);
    s.search(QStringLiteral("zzz"));
    PB_CHECK(s.empty());
    PB_CHECK_EQ(s.queryText().toStdString(), std::string("zzz"));
    s.search(QString());
    PB_CHECK(!s.searching());
    PB_CHECK(s.empty());
}

PB_TEST(messages_that_could_not_be_changed_are_listed_and_can_be_acknowledged) {
    Env e;
    const qlonglong id = e.add(e.inbox, 1, "Gone message");
    PB_CHECK(e.store->set_flags_local(id, pbo::kFlagSeen, 0).ok());
    PB_CHECK(e.store->set_message_error(id, "gone_on_server").ok());
    ConflictsModel::Config cc;
    cc.store = e.store.get();
    ConflictsModel m(cc);
    m.refresh();
    PB_CHECK_EQ(m.total(), 1);
    const QVariantMap row = m.rows().at(0).toMap();
    PB_CHECK_EQ(row["kind"].toString().toStdString(), std::string("message"));
    PB_CHECK_EQ(row["title"].toString().toStdString(), std::string("Gone message"));
    PB_CHECK_EQ(row["errorKey"].toString().toStdString(), std::string("gone_on_server"));
    PB_CHECK(m.acknowledgeMessage(id));
    PB_CHECK_EQ(m.total(), 0);
    PB_CHECK_EQ(e.q("SELECT count(*) FROM message"), std::string("0"));
}

PB_TEST(the_recipient_fields_get_suggestions_from_cached_and_sent_mail) {
    Env e;
    e.add(e.inbox, 1, "Hi", "Anna Berg", "anna@example.org");
    e.add(e.inbox, 2, "Hi 2", "Andreas", "andreas@example.org");
    ComposeBridge::Config cc;
    cc.store = e.store.get();
    cc.clock = &e.clock;
    cc.random = &e.rng;
    ComposeBridge c(cc);
    QStringList s = c.suggest(QStringLiteral("an"));
    PB_CHECK_EQ(s.size(), 2);
    PB_CHECK(s.contains(QStringLiteral("Anna Berg <anna@example.org>")));
    // only the word being typed after the last comma counts, and addresses already in the field are not offered again
    s = c.suggest(QStringLiteral("anna@example.org, andr"));
    PB_CHECK_EQ(s.size(), 1);
    PB_CHECK_EQ(s.at(0).toStdString(), std::string("Andreas <andreas@example.org>"));
    PB_CHECK_EQ(c.suggest(QStringLiteral("anna@example.org, anna")).size(), 0);
    PB_CHECK_EQ(c.suggest(QStringLiteral("a")).size(), 0);                    // one character is too little to be useful
    PB_CHECK_EQ(c.applySuggestion(QStringLiteral("anna@example.org, andr"), QStringLiteral("Andreas <andreas@example.org>")).toStdString(),
                std::string("anna@example.org, Andreas <andreas@example.org>, "));
}

PB_TEST(sending_a_message_remembers_its_recipients_for_later_suggestions) {
    Env e;
    ComposeBridge::Config cc;
    cc.store = e.store.get();
    cc.clock = &e.clock;
    cc.random = &e.rng;
    ComposeBridge c(cc);
    c.start(QStringLiteral("new"), 0);
    c.setTo(QStringLiteral("carla@example.org"));
    c.setSubject(QStringLiteral("Hello"));
    c.setBody(QStringLiteral("Text"));
    c.send();
    PB_CHECK_EQ(c.suggest(QStringLiteral("carl")).size(), 1);
}

PB_TEST_MAIN()
