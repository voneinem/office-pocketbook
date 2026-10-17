// T019: the logger accepts only event codes, HTTP status, counts and file paths; sensitive
// field names are redacted (constitution Principle III).
#include <dirent.h>

#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "../../core/src/log_codes.h"
#include "pbo/log.h"
#include "test_support.h"

namespace log = pbo::log;

namespace {
struct Capture {
    std::vector<std::string> lines;
    Capture() {
        log::set_sink([this](const std::string& line) { lines.push_back(line); });
    }
    ~Capture() { log::reset_sink(); }
    std::string all() const {
        std::string s;
        for (const auto& l : lines) s += l + "\n";
        return s;
    }
};
bool contains(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }
}  // namespace

PB_TEST(logs_event_status_counts_and_path) {
    Capture c;
    log::event("sync.finished", {log::status(200), log::count("pushed", 3), log::path("/mnt/ext1/system/pboffice/pboffice.db")});
    PB_CHECK_EQ(c.lines.size(), static_cast<size_t>(1));
    PB_CHECK(contains(c.all(), "sync.finished"));
    PB_CHECK(contains(c.all(), "status=200"));
    PB_CHECK(contains(c.all(), "pushed=3"));
    PB_CHECK(contains(c.all(), "path=/mnt/ext1/system/pboffice/pboffice.db"));
}

PB_TEST(sensitive_field_names_are_redacted) {
    Capture c;
    log::event("account.saved", {log::text("password", "hunter2"), log::text("USERNAME", "alice"),
                                 log::text("content", "my secret diary"), log::text("title", "My diary"),
                                 log::text("Authorization", "Basic YWxpY2U6aHVudGVyMg=="),
                                 log::text("token", "abc123"), log::text("url", "https://alice:pw@host/")});
    const std::string out = c.all();
    for (const char* leaked : {"hunter2", "alice", "diary", "YWxpY2U6", "abc123", "pw@host"})
        PB_CHECK(!contains(out, leaked));
    PB_CHECK(contains(out, "password=[redacted]"));
}

PB_TEST(harmless_text_fields_are_kept) {
    Capture c;
    log::event("sync.finished", {log::text("outcome", "success"), log::text("error_class", "none")});
    PB_CHECK(contains(c.all(), "outcome=success"));
}

PB_TEST(newlines_in_values_cannot_forge_log_lines) {
    Capture c;
    log::event("x", {log::text("outcome", "ok\nFAKE LINE")});
    PB_CHECK_EQ(c.lines.size(), static_cast<size_t>(1));
    PB_CHECK(!contains(c.lines[0], "\n"));
}

PB_TEST(mail_and_calendar_words_are_redacted_as_whole_words) {
    Capture c;
    log::event("x", {log::text("host", "mail.example.org"), log::text("folder", "Projects/Secret"),
                     log::text("subject", "Layoffs"), log::text("from", "boss@example.org"),
                     log::text("to", "me@example.org"), log::text("sent_to", "you@example.org"),
                     log::text("recipient", "carol"), log::text("summary", "Dentist"),
                     log::text("attendee", "dave"), log::text("location", "Room 7"),
                     log::text("event.title", "Budget")});
    const std::string out = c.all();
    for (const char* leaked : {"example.org", "Secret", "Layoffs", "carol", "Dentist", "dave", "Room 7", "Budget"})
        PB_CHECK(!contains(out, leaked));
    PB_CHECK(contains(out, "host=[redacted]"));
    PB_CHECK(contains(out, "to=[redacted]"));
    PB_CHECK(contains(out, "sent_to=[redacted]"));
}

PB_TEST(words_inside_longer_names_and_typed_fields_are_unaffected) {
    Capture c;
    log::event("x", {log::text("total", "5"), log::text("tofu", "ok"), log::text("outcome", "success"),
                     log::text("fromage", "brie"), log::count("to_fetch", 7), log::count("host_count", 2),
                     log::count("folder", 3), log::status(404)});
    const std::string out = c.all();
    for (const char* kept : {"total=5", "tofu=ok", "outcome=success", "fromage=brie", "to_fetch=7", "host_count=2",
                             "folder=3", "status=404"})
        PB_CHECK(contains(out, kept));
}

// Every literal event code in the sources is listed in core/src/log_codes.h, and every listed code is used
// (a stale list would hide a new code that was never reviewed).
PB_TEST(event_codes_in_the_sources_are_exactly_the_listed_ones) {
    const std::regex call(R"re((?:log::event|log_failure)\(\s*"([A-Za-z0-9_.]+)")re");
    std::set<std::string> used;
    std::vector<std::string> dirs = {std::string(PBO_SOURCE_DIR) + "/core/src", std::string(PBO_SOURCE_DIR) + "/bridge/src",
                                     std::string(PBO_SOURCE_DIR) + "/app"};
    for (const std::string& dir : dirs) {
        DIR* d = opendir(dir.c_str());
        PB_CHECK(d != nullptr);
        if (!d) continue;
        while (dirent* e = readdir(d)) {
            const std::string name = e->d_name;
            if (name.size() < 5 || name.compare(name.size() - 4, 4, ".cpp") != 0) continue;
            std::ifstream in(dir + "/" + name);
            std::stringstream ss;
            ss << in.rdbuf();
            const std::string text = ss.str();
            for (std::sregex_iterator it(text.begin(), text.end(), call), end; it != end; ++it) used.insert((*it)[1]);
        }
        closedir(d);
    }
    std::set<std::string> listed(std::begin(pbo::log::kAllowedCodes), std::end(pbo::log::kAllowedCodes));
    for (const std::string& code : used)
        if (listed.count(code) != 1) PB_FAIL_AT("code not listed in log_codes.h: " + code);
    for (const std::string& code : listed)
        if (used.count(code) != 1) PB_FAIL_AT("listed code is unused: " + code);
    PB_CHECK(pbo::log::is_allowed_code("about.opened"));
    PB_CHECK(!pbo::log::is_allowed_code("password.leaked"));
}

PB_TEST_MAIN()
