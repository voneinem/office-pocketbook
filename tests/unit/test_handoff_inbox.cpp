// T007 (spec 003): the folder of pending hand-overs (research R-04, data-model "Pending entry"). The launcher writes one folder per hand-over with one raw file
// per argument; the app reads it, saves the draft, and only then removes it. Entries expire after 10 minutes.
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "pbo/handoff_inbox.h"
#include "test_support.h"
#include "tmp_dir.h"

namespace {

constexpr int64_t kNow = 1791979200;

struct Clock : pbo::Clock {
    int64_t now() const override { return kNow; }
};

void set_mtime(const std::string& path, int64_t epoch) {
    struct timeval tv[2] = {{static_cast<time_t>(epoch), 0}, {static_cast<time_t>(epoch), 0}};
    ::utimes(path.c_str(), tv);
}

struct Env {
    pbtest::TmpDir tmp;
    Clock clock;
    std::string dir() const { return tmp.file("handoff"); }
    Env() { ::mkdir(dir().c_str(), 0700); }
    // an entry folder with the files "0", "1", ... (or the given names) holding the given bytes
    std::string entry(const std::string& id, const std::vector<std::string>& files, int64_t age_seconds = 5) {
        const std::string folder = dir() + "/" + id;
        ::mkdir(folder.c_str(), 0700);
        for (size_t i = 0; i < files.size(); ++i) tmp.write("handoff/" + id + "/" + std::to_string(i), files[i]);
        set_mtime(folder, kNow - age_seconds);
        return folder;
    }
    pbo::HandoffInbox inbox() { return pbo::HandoffInbox(dir(), clock); }
};

}  // namespace

PB_TEST(a_missing_directory_is_an_empty_result_not_an_error) {
    pbtest::TmpDir tmp;
    Clock clock;
    pbo::HandoffInbox inbox(tmp.file("does-not-exist"), clock);
    int expired = -1;
    PB_CHECK(inbox.take_ready(&expired).empty());
    PB_CHECK_EQ(expired, 0);
    PB_CHECK(!inbox.remove("1-1"));
}

PB_TEST(a_complete_entry_returns_the_raw_bytes_of_every_file_in_order) {
    Env env;
    const std::string text = std::string("text=line one\nline two \xc3\xa4\xe2\x82\xac 'q' \"d\" $(x) `y`;\n");   // bytes as the launcher wrote them
    env.entry("1761212401-977", {"pbo-mail-v1-not-included-but-any-bytes", text, "textlen=" + std::to_string(text.size() - 5), std::string("to=a@example.org")});
    auto inbox = env.inbox();
    const auto ready = inbox.take_ready();
    PB_CHECK_EQ(ready.size(), size_t(1));
    PB_CHECK_EQ(ready[0].id, std::string("1761212401-977"));
    PB_CHECK(!ready[0].refused);
    PB_CHECK_EQ(ready[0].args.size(), size_t(4));
    PB_CHECK_EQ(ready[0].args[1], text);   // no terminator added, no escaping, a trailing newline kept
    PB_CHECK_EQ(ready[0].args[3], std::string("to=a@example.org"));
}

PB_TEST(files_are_read_in_numeric_order_not_text_order) {
    Env env;
    const std::string folder = env.dir() + "/1-1";
    ::mkdir(folder.c_str(), 0700);
    for (const char* n : {"10", "2", "0", "1"}) env.tmp.write(std::string("handoff/1-1/") + n, n);
    set_mtime(folder, kNow - 1);
    auto inbox = env.inbox();
    const auto ready = inbox.take_ready();
    PB_CHECK_EQ(ready.size(), size_t(1));
    PB_CHECK_EQ(ready[0].args.size(), size_t(4));
    PB_CHECK_EQ(ready[0].args[0], std::string("0"));
    PB_CHECK_EQ(ready[0].args[1], std::string("1"));
    PB_CHECK_EQ(ready[0].args[2], std::string("2"));
    PB_CHECK_EQ(ready[0].args[3], std::string("10"));
}

PB_TEST(an_entry_without_files_is_returned_with_no_arguments) {
    Env env;
    env.entry("1-1", {});
    auto inbox = env.inbox();
    const auto ready = inbox.take_ready();
    PB_CHECK_EQ(ready.size(), size_t(1));
    PB_CHECK(ready[0].args.empty());
    PB_CHECK(!ready[0].refused);
}

PB_TEST(a_file_of_16384_bytes_is_accepted_and_16385_makes_the_entry_refused) {
    Env env;
    env.entry("1-1", {std::string(16384, 'x')});
    env.entry("2-2", {std::string(16385, 'x')});
    auto inbox = env.inbox();
    const auto ready = inbox.take_ready();
    PB_CHECK_EQ(ready.size(), size_t(2));
    PB_CHECK(!ready[0].refused);
    PB_CHECK_EQ(ready[0].args[0].size(), size_t(16384));
    PB_CHECK(ready[1].refused);
    PB_CHECK(ready[1].args.empty());   // a refused entry hands out nothing
    PB_CHECK_EQ(pbo::HandoffInbox::kMaxFileBytes, size_t(16384));
}

PB_TEST(eight_files_are_accepted_and_nine_make_the_entry_refused) {
    Env env;
    env.entry("1-1", std::vector<std::string>(8, "a"));
    env.entry("2-2", std::vector<std::string>(9, "a"));
    auto inbox = env.inbox();
    const auto ready = inbox.take_ready();
    PB_CHECK_EQ(ready.size(), size_t(2));
    PB_CHECK(!ready[0].refused);
    PB_CHECK_EQ(ready[0].args.size(), size_t(8));
    PB_CHECK(ready[1].refused);
    PB_CHECK_EQ(pbo::HandoffInbox::kMaxFiles, 8);
}

PB_TEST(an_entry_being_written_is_ignored_until_it_is_renamed) {
    Env env;
    const std::string wip = env.entry("1761212345-812.tmp", {"half"});
    env.entry("1761212401-977", {"done"});
    auto inbox = env.inbox();
    const auto ready = inbox.take_ready();
    PB_CHECK_EQ(ready.size(), size_t(1));
    PB_CHECK_EQ(ready[0].id, std::string("1761212401-977"));
    struct stat st;
    PB_CHECK(::stat(wip.c_str(), &st) == 0);   // fresh: left alone
}

PB_TEST(entries_come_oldest_first_by_name) {
    Env env;
    env.entry("1761212401-977", {"b"});
    env.entry("1761212345-812", {"a"});
    env.entry("1761212999-1", {"c"});
    auto inbox = env.inbox();
    const auto ready = inbox.take_ready();
    PB_CHECK_EQ(ready.size(), size_t(3));
    PB_CHECK_EQ(ready[0].args[0], std::string("a"));
    PB_CHECK_EQ(ready[1].args[0], std::string("b"));
    PB_CHECK_EQ(ready[2].args[0], std::string("c"));
}

PB_TEST(an_entry_older_than_600_seconds_is_removed_unread_and_counted_and_599_or_600_are_returned) {
    Env env;
    const std::string stale = env.entry("1-1", {"old"}, 601);
    env.entry("2-2", {"ok599"}, 599);
    env.entry("3-3", {"ok600"}, 600);
    auto inbox = env.inbox();
    int expired = 0;
    const auto ready = inbox.take_ready(&expired);
    PB_CHECK_EQ(expired, 1);
    PB_CHECK_EQ(ready.size(), size_t(2));
    PB_CHECK_EQ(ready[0].args[0], std::string("ok599"));
    PB_CHECK_EQ(ready[1].args[0], std::string("ok600"));
    struct stat st;
    PB_CHECK(::stat(stale.c_str(), &st) != 0);                      // gone
    PB_CHECK(!env.tmp.exists("handoff/1-1/0"));                     // its files too
    PB_CHECK_EQ(pbo::HandoffInbox::kExpirySeconds, 600);
}

PB_TEST(an_unfinished_entry_that_is_too_old_is_removed_too_because_it_holds_text) {
    Env env;
    const std::string wip = env.entry("1-1.tmp", {"secret"}, 4000);
    auto inbox = env.inbox();
    int expired = 0;
    PB_CHECK(inbox.take_ready(&expired).empty());
    PB_CHECK_EQ(expired, 1);
    struct stat st;
    PB_CHECK(::stat(wip.c_str(), &st) != 0);
}

PB_TEST(an_entry_with_a_time_in_the_future_counts_as_fresh) {
    Env env;
    env.entry("1-1", {"later"}, -3600);
    auto inbox = env.inbox();
    int expired = 0;
    const auto ready = inbox.take_ready(&expired);
    PB_CHECK_EQ(expired, 0);
    PB_CHECK_EQ(ready.size(), size_t(1));
}

PB_TEST(taking_does_not_remove_and_remove_removes_only_that_entry_once) {
    Env env;
    env.entry("1-1", {"a"});
    env.entry("2-2", {"b"});
    auto inbox = env.inbox();
    PB_CHECK_EQ(inbox.take_ready().size(), size_t(2));
    PB_CHECK_EQ(inbox.take_ready().size(), size_t(2));   // nothing was removed by taking: a crash repeats the draft, it does not lose the text
    PB_CHECK(inbox.remove("1-1"));
    PB_CHECK(!env.tmp.exists("handoff/1-1/0"));
    PB_CHECK(!inbox.remove("1-1"));                      // already gone
    const auto left = inbox.take_ready();
    PB_CHECK_EQ(left.size(), size_t(1));
    PB_CHECK_EQ(left[0].id, std::string("2-2"));
}

PB_TEST(remove_refuses_ids_that_could_leave_the_folder) {
    Env env;
    env.tmp.write("outside.txt", "keep");
    ::mkdir(env.tmp.file("sibling").c_str(), 0700);
    env.tmp.write("sibling/file", "keep");
    auto inbox = env.inbox();
    PB_CHECK(!inbox.remove(""));
    PB_CHECK(!inbox.remove("."));
    PB_CHECK(!inbox.remove(".."));
    PB_CHECK(!inbox.remove("../sibling"));
    PB_CHECK(!inbox.remove("a/b"));
    PB_CHECK(!inbox.remove("/etc"));
    PB_CHECK(env.tmp.exists("sibling/file"));
    PB_CHECK(env.tmp.exists("outside.txt"));
}

PB_TEST(an_unreadable_file_makes_that_entry_refused_not_a_crash) {
    if (::geteuid() == 0) PB_SKIP("root can read every file");
    Env env;
    env.entry("1-1", {"a", "b"});
    ::chmod(env.tmp.file("handoff/1-1/1").c_str(), 0000);
    env.entry("2-2", {"fine"});
    auto inbox = env.inbox();
    const auto ready = inbox.take_ready();
    ::chmod(env.tmp.file("handoff/1-1/1").c_str(), 0600);
    PB_CHECK_EQ(ready.size(), size_t(2));
    PB_CHECK(ready[0].refused);
    PB_CHECK(!ready[1].refused);
}

PB_TEST(a_symbolic_link_inside_an_entry_is_not_followed) {
    Env env;
    env.tmp.write("outside.txt", "OUTSIDE-SECRET");
    env.entry("1-1", {"a"});
    ::symlink(env.tmp.file("outside.txt").c_str(), env.tmp.file("handoff/1-1/1").c_str());
    auto inbox = env.inbox();
    const auto ready = inbox.take_ready();
    PB_CHECK_EQ(ready.size(), size_t(1));
    PB_CHECK(ready[0].refused);
    for (const auto& a : ready[0].args) PB_CHECK(a.find("OUTSIDE-SECRET") == std::string::npos);
}

PB_TEST(a_folder_inside_an_entry_or_a_stray_file_in_the_directory_is_handled) {
    Env env;
    env.entry("1-1", {"a"});
    ::mkdir(env.tmp.file("handoff/1-1/sub").c_str(), 0700);
    env.tmp.write("handoff/stray.txt", "x");
    env.entry("2-2", {"b"});
    auto inbox = env.inbox();
    const auto ready = inbox.take_ready();
    PB_CHECK_EQ(ready.size(), size_t(2));
    PB_CHECK(ready[0].refused);        // not what the launcher writes
    PB_CHECK(!ready[1].refused);
    PB_CHECK(env.tmp.exists("handoff/stray.txt"));   // a file that is not an entry is not touched
}

PB_TEST_MAIN()
