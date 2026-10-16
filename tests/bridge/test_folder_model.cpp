// T082 (folder part): FolderModel lists the folders with role, unread count, "older mail on the server" and the sync switch.
#include <QCoreApplication>
#include <QSignalSpy>
#include <QVariantMap>

#include "folder_model.h"
#include "pbo/store.h"
#include "test_support.h"
#include "tmp_dir.h"

namespace {

int g_argc = 1;
char g_arg0[] = "test_folder_model";
char* g_argv[] = {g_arg0, nullptr};
QCoreApplication g_app(g_argc, g_argv);

struct TestClock : pbo::Clock {
    int64_t now() const override { return 1791979200; }
};

pbo::FolderListItem item(const std::string& name, std::set<std::string> attrs = {}, const std::string& delimiter = "/") {
    pbo::FolderListItem i;
    i.raw_name = i.name = name;
    i.delimiter = delimiter;
    i.attributes = std::move(attrs);
    return i;
}

struct Env {
    pbtest::TmpDir dir;
    TestClock clock;
    std::unique_ptr<pbo::Store> store;
    Env() {
        store = std::move(pbo::Store::open(dir.path(), clock).value);
        store->upsert_folders({item("Work/Reports"), item("Trash", {"\\Trash"}), item("INBOX"), item("[Gmail]/All Mail", {"\\All"}), item("Sent", {"\\Sent"}), item("Archive/2019", {})});
    }
    FolderModel::Config config() {
        FolderModel::Config c;
        c.store = store.get();
        return c;
    }
};

QVariantMap row_with_role(const FolderModel& m, const QString& role) {
    for (const QVariant& v : m.rows())
        if (v.toMap()["role"].toString() == role) return v.toMap();
    return {};
}

}  // namespace

PB_TEST(folders_are_listed_in_a_fixed_order_with_their_roles_and_short_names) {
    Env env;
    FolderModel model(env.config());
    model.refresh();
    PB_CHECK_EQ(model.rows().size(), 6);
    const QVariantList rows = model.rows();
    PB_CHECK_EQ(rows.at(0).toMap()["role"].toString().toStdString(), std::string("inbox"));
    PB_CHECK_EQ(rows.at(1).toMap()["role"].toString().toStdString(), std::string("sent"));    // inbox, sent, trash, ..., then the other folders by name
    PB_CHECK_EQ(rows.at(2).toMap()["role"].toString().toStdString(), std::string("trash"));
    PB_CHECK_EQ(rows.at(3).toMap()["role"].toString().toStdString(), std::string("all"));
    PB_CHECK_EQ(rows.at(4).toMap()["leafName"].toString().toStdString(), std::string("2019"));
    PB_CHECK_EQ(rows.at(5).toMap()["leafName"].toString().toStdString(), std::string("Reports"));
    PB_CHECK_EQ(rows.at(5).toMap()["name"].toString().toStdString(), std::string("Work/Reports"));
    PB_CHECK_EQ(rows.at(0).toMap()["id"].toLongLong() != 0, true);
}

PB_TEST(unread_counts_older_mail_and_the_sync_switch_are_shown) {
    Env env;
    pbo::PullBatch b;
    for (uint32_t uid = 1; uid <= 3; ++uid) {
        pbo::PulledMessage m;
        m.uid = uid;
        m.subject = "S";
        m.flags = uid == 1 ? 1u : 0u;   // one is read
        b.messages.push_back(m);
    }
    b.older_on_server = true;
    const int64_t inbox = env.store->folders().at(0).id;
    PB_CHECK(env.store->apply_pull(inbox, b).ok());
    FolderModel model(env.config());
    model.refresh();
    const QVariantMap in = row_with_role(model, "inbox");
    PB_CHECK_EQ(in["unread"].toInt(), 2);
    PB_CHECK(in["olderOnServer"].toBool());
    PB_CHECK(in["syncEnabled"].toBool());
    PB_CHECK(!row_with_role(model, "all")["syncEnabled"].toBool());   // Gmail's All Mail is off by default
    PB_CHECK(!row_with_role(model, "sent")["olderOnServer"].toBool());
}

PB_TEST(the_sync_switch_is_stored_and_the_model_changes) {
    Env env;
    FolderModel model(env.config());
    model.refresh();
    QSignalSpy spy(&model, &FolderModel::changed);
    const qlonglong all_id = row_with_role(model, "all")["id"].toLongLong();
    PB_CHECK(model.setSyncEnabled(all_id, true));
    PB_CHECK(spy.count() >= 1);
    PB_CHECK(row_with_role(model, "all")["syncEnabled"].toBool());
    PB_CHECK(model.setSyncEnabled(all_id, false));
    PB_CHECK(!row_with_role(model, "all")["syncEnabled"].toBool());
    PB_CHECK(!model.setSyncEnabled(424242, true));   // unknown folder: refused, nothing changes
}

PB_TEST(folders_without_the_inbox_do_not_break_the_model) {
    pbtest::TmpDir dir;
    TestClock clock;
    auto store = std::move(pbo::Store::open(dir.path(), clock).value);
    FolderModel::Config c;
    c.store = store.get();
    FolderModel model(c);
    model.refresh();
    PB_CHECK(model.rows().isEmpty());
    PB_CHECK(model.empty());
}

PB_TEST_MAIN()
