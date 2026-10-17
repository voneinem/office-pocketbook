// T061 (mail part, SC-010): every fixture, every malformed file and mutations of them go through parse_message without a
// crash or hang (run under PBO_SANITIZE for the memory checks) and always give valid UTF-8 and sane attachment data.
#include <dirent.h>

#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

#include "fixtures.h"
#include "mutate.h"
#include "pbo/charset.h"
#include "pbo/mime.h"
#include "test_support.h"

namespace {

std::vector<std::string> files_in(const std::string& relative) {
    std::vector<std::string> out;
    const std::string dir = std::string(PBO_FIXTURE_DIR) + "/" + relative;
    if (DIR* d = opendir(dir.c_str())) {
        while (dirent* e = readdir(d)) {
            const std::string n = e->d_name;
            if (n.size() > 4 && n.compare(n.size() - 4, 4, ".eml") == 0) out.push_back(relative + "/" + n);
        }
        closedir(d);
    }
    std::sort(out.begin(), out.end());
    return out;
}

// What must hold for the result of ANY input.
void check_result(const pbo::ParsedMessage& m, const std::string& what) {
    auto fail = [&](const std::string& why) { PB_FAIL_AT(what + ": " + why); };
    if (!pbo::is_valid_utf8(m.text)) fail("text is not valid UTF-8");
    if (!pbo::is_valid_utf8(m.subject) || !pbo::is_valid_utf8(m.from_name) || !pbo::is_valid_utf8(m.to)) fail("a header is not valid UTF-8");
    if (m.text.size() > (2u << 20) + 16) fail("text over the limit");
    if (m.text.find('\0') != std::string::npos) fail("text has a NUL byte");
    if (m.attachments.size() > 200) fail("too many attachments");
    for (const auto& a : m.attachments) {
        if (!pbo::is_valid_utf8(a.name)) fail("attachment name is not valid UTF-8");
        if (a.section.empty() || a.section.find_first_not_of("0123456789.") != std::string::npos) fail("bad section: " + a.section);
        if (a.mime_type.find_first_of("\r\n\0") != std::string::npos) fail("bad type");
    }
    for (const auto& l : m.links)
        if (l.compare(0, 7, "http://") != 0 && l.compare(0, 8, "https://") != 0 && l.compare(0, 7, "mailto:") != 0) fail("a link with another scheme: " + l.substr(0, 20));
    if (m.body_kind == pbo::ParsedMessage::Body::Unreadable && !m.text.empty()) fail("an unreadable message with text");
}

}  // namespace

PB_TEST(there_are_at_least_50_malformed_mail_files) {
    PB_CHECK(files_in("malformed/mail").size() >= 50);
    PB_CHECK(files_in("mail").size() >= 15);
}

PB_TEST(every_fixture_and_malformed_file_parses_to_a_sane_result) {
    std::vector<std::string> all = files_in("malformed/mail");
    for (const auto& f : files_in("mail")) all.push_back(f);
    for (const std::string& name : all) {
        const std::string raw = pbtest::fixture(name);
        const auto t0 = std::chrono::steady_clock::now();
        const pbo::ParsedMessage m = pbo::parse_message(raw);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        if (ms > 2000) PB_FAIL_AT(name + ": took " + std::to_string(ms) + " ms");
        check_result(m, name);
    }
}

PB_TEST(truncated_at_every_offset_flipped_and_with_other_line_ends) {
    for (const std::string& name : files_in("mail")) {
        const std::string raw = pbtest::fixture(name);
        for (size_t at : pbtest::truncation_points(raw.size())) check_result(pbo::parse_message(pbtest::truncated(raw, at)), name + " cut at " + std::to_string(at));
        for (uint64_t seed = 1; seed <= 25; ++seed) {
            check_result(pbo::parse_message(pbtest::flipped(raw, seed, 1 + static_cast<int>(seed % 8))), name + " flipped, seed " + std::to_string(seed));
            check_result(pbo::parse_message(pbtest::with_insertions(raw, seed * 7, 1 + static_cast<int>(seed % 5))), name + " with insertions, seed " + std::to_string(seed));
        }
        check_result(pbo::parse_message(pbtest::with_line_ends(raw, "\n")), name + " with LF");
        check_result(pbo::parse_message(pbtest::with_line_ends(raw, "\r")), name + " with CR");
    }
}

PB_TEST(size_and_nesting_bombs_made_in_memory) {
    // a line over 1 MiB, a text part of 40 MiB (over the input limit), 10,000 nested multipart levels, 50,000 headers
    check_result(pbo::parse_message("Content-Type: text/plain\r\n\r\n" + std::string((1 << 20) + 5, 'a') + "\r\n"), "line over 1 MiB");
    check_result(pbo::parse_message("Content-Type: text/plain\r\n\r\n" + std::string(40u << 20, 'b')), "40 MiB body");
    std::string nest = "Content-Type: multipart/mixed; boundary=\"L0\"\r\n\r\n";
    for (int i = 0; i < 10000; ++i) nest += "--L" + std::to_string(i) + "\r\nContent-Type: multipart/mixed; boundary=\"L" + std::to_string(i + 1) + "\"\r\n\r\n";
    check_result(pbo::parse_message(nest), "10,000 nested levels");
    std::string headers;
    for (int i = 0; i < 50000; ++i) headers += "X-H" + std::to_string(i) + ": v\r\n";
    check_result(pbo::parse_message(headers + "\r\nbody"), "50,000 headers");
    std::string words;
    for (int i = 0; i < 50000; ++i) words += "=?UTF-8?B?w6Q=?= ";
    check_result(pbo::parse_message("Subject: " + words + "\r\n\r\nbody"), "50,000 encoded words");
}

PB_TEST_MAIN()
