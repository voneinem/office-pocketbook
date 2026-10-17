// T060: folder roles from attributes and names, folders that need the user's choice, Gmail's All/Junk folders (FR-005).
#include <string>
#include <vector>

#include "pbo/mail_roles.h"
#include "test_support.h"

namespace {

pbo::FolderListItem item(const std::string& name, std::set<std::string> attrs = {}) {
    pbo::FolderListItem i;
    i.raw_name = pbo::encode_mutf7(name);
    i.name = name;
    i.delimiter = "/";
    i.attributes = std::move(attrs);
    return i;
}

pbo::Folder folder_with(const std::string& role) {
    pbo::Folder f;
    f.role = role;
    return f;
}

}  // namespace

PB_TEST(the_inbox_is_found_in_any_spelling) {
    PB_CHECK_EQ(pbo::detect_role(item("INBOX")).role, std::string("inbox"));
    PB_CHECK_EQ(pbo::detect_role(item("Inbox")).role, std::string("inbox"));
}

PB_TEST(special_use_attributes_decide_the_role_whatever_the_name) {
    PB_CHECK_EQ(pbo::detect_role(item("Whatever", {"\\Sent"})).role, std::string("sent"));
    PB_CHECK_EQ(pbo::detect_role(item("Whatever", {"\\Drafts"})).role, std::string("drafts"));
    PB_CHECK_EQ(pbo::detect_role(item("Whatever", {"\\Trash"})).role, std::string("trash"));
    PB_CHECK_EQ(pbo::detect_role(item("Whatever", {"\\Junk"})).role, std::string("junk"));
    PB_CHECK_EQ(pbo::detect_role(item("Whatever", {"\\All"})).role, std::string("all"));
    PB_CHECK_EQ(pbo::detect_role(item("Whatever", {"\\Archive"})).role, std::string("archive"));
    PB_CHECK_EQ(pbo::detect_role(item("Sent", {"\\Trash"})).role, std::string("trash"));   // the attribute wins over the name
    PB_CHECK_EQ(pbo::detect_role(item("Whatever", {"\\sent", "\\HasNoChildren"})).role, std::string("sent"));   // any case
}

PB_TEST(names_are_matched_without_regard_to_case_and_after_removing_the_inbox_prefix) {
    for (const char* n : {"Sent", "sent items", "SENT MESSAGES", "Gesendet", "Gesendete Elemente", "Gesendete Objekte", "INBOX.Sent", "INBOX/Sent Items", "inbox.gesendet"})
        PB_CHECK_EQ(pbo::detect_role(item(n)).role, std::string("sent"));
    for (const char* n : {"Drafts", "DRAFTS", "Entw\xC3\xBCrfe", "ENTW\xC3\x9C" "RFE", "Entwurf", "INBOX.Drafts", "INBOX/Entw\xC3\xBCrfe"})
        PB_CHECK_EQ(pbo::detect_role(item(n)).role, std::string("drafts"));
    for (const char* n : {"Trash", "Deleted Items", "Deleted Messages", "Papierkorb", "Gel\xC3\xB6scht", "GEL\xC3\x96SCHT", "Gel\xC3\xB6schte Elemente", "INBOX.Trash"})
        PB_CHECK_EQ(pbo::detect_role(item(n)).role, std::string("trash"));
}

PB_TEST(ordinary_folders_stay_other_and_similar_names_are_not_taken) {
    for (const char* n : {"Projects", "Sent Archive 2019", "Trashy", "Drafts-old", "Work/Sent", "Sentinel"})
        PB_CHECK_EQ(pbo::detect_role(item(n)).role, std::string("other"));
    PB_CHECK_EQ(pbo::detect_role(item("Work/Papierkorb")).role, std::string("other"));   // only top level (after INBOX.) counts
}

PB_TEST(only_gmails_all_and_junk_folders_are_switched_off) {
    PB_CHECK(!pbo::detect_role(item("[Gmail]/All Mail", {"\\All"})).sync_enabled);
    PB_CHECK(!pbo::detect_role(item("[Gmail]/Spam", {"\\Junk"})).sync_enabled);
    PB_CHECK(pbo::detect_role(item("Label A")).sync_enabled);                  // other labels are ordinary folders
    PB_CHECK(pbo::detect_role(item("[Gmail]/Sent Mail", {"\\Sent"})).sync_enabled);
    PB_CHECK(pbo::detect_role(item("INBOX")).sync_enabled);
    PB_CHECK(pbo::detect_role(item("Archive", {"\\Archive"})).sync_enabled);
}

PB_TEST(roles_that_no_folder_has_need_the_users_choice) {
    std::vector<pbo::Folder> folders = {folder_with("inbox"), folder_with("sent"), folder_with("other")};
    const std::vector<std::string> missing = pbo::roles_needing_choice(folders);
    PB_CHECK_EQ(missing.size(), 2u);
    PB_CHECK_EQ(missing[0], std::string("drafts"));
    PB_CHECK_EQ(missing[1], std::string("trash"));
    folders.push_back(folder_with("drafts"));
    folders.push_back(folder_with("trash"));
    PB_CHECK(pbo::roles_needing_choice(folders).empty());
    PB_CHECK_EQ(pbo::roles_needing_choice({}).size(), 3u);
}

PB_TEST_MAIN()
