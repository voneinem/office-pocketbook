// T092 (SC-010, quickstart scenario 15): malformed and hostile XML never crashes or hangs the reader; hostile constructs are refused.
#include <dirent.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "mutate.h"
#include "pbo/xml_min.h"
#include "test_support.h"

namespace {

std::vector<std::string> files_in(const std::string& relative, const std::string& ext) {
    std::vector<std::string> out;
    const std::string dir = std::string(PBO_FIXTURE_DIR) + "/" + relative;
    if (DIR* d = opendir(dir.c_str())) {
        while (dirent* e = readdir(d)) {
            const std::string n = e->d_name;
            if (n.size() > ext.size() && n.compare(n.size() - ext.size(), ext.size(), ext) == 0) out.push_back(dir + "/" + n);
        }
        closedir(d);
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::string slurp(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

double seconds_since(std::chrono::steady_clock::time_point t) { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count(); }

}  // namespace

PB_TEST(there_are_at_least_20_malformed_xml_files) {
    PB_CHECK(files_in("malformed/xml", ".xml").size() >= 20);
}

PB_TEST(every_malformed_file_gives_a_tree_or_an_error_never_a_crash) {
    for (const std::string& f : files_in("malformed/xml", ".xml")) {
        const auto t = std::chrono::steady_clock::now();
        const auto r = pbo::parse_xml(slurp(f));
        if (!r.ok()) PB_CHECK_EQ(r.error.key, std::string("malformed_xml"));
        if (seconds_since(t) > 5.0) PB_FAIL_AT(f + ": too slow");
    }
}

PB_TEST(the_hostile_constructs_in_the_fixtures_are_refused) {
    for (const char* name : {"billion_laughs", "external_entity", "external_dtd", "processing_instruction", "pi_inside", "unknown_entity", "bad_numeric_references",
                             "mismatched_tags", "two_roots", "text_after_root", "unterminated_comment", "unterminated_cdata", "duplicate_attributes", "unbound_prefix",
                             "unterminated_everything", "empty_file"}) {
        const auto r = pbo::parse_xml(slurp(std::string(PBO_FIXTURE_DIR) + "/malformed/xml/" + name + ".xml"));
        if (r.ok()) PB_FAIL_AT(std::string(name) + " was accepted");
    }
}

PB_TEST(huge_generated_inputs_are_refused_quickly) {
    const auto t = std::chrono::steady_clock::now();
    PB_CHECK(!pbo::parse_xml(std::string(10000, '<') + "a>").ok());
    std::string deep, wide, big;
    for (int i = 0; i < 10000; ++i) deep += "<a>";
    for (int i = 0; i < 10000; ++i) deep += "</a>";
    PB_CHECK(!pbo::parse_xml(deep).ok());                                   // 64 levels at most
    wide = "<r>";
    for (int i = 0; i < 250000; ++i) wide += "<n/>";
    wide += "</r>";
    PB_CHECK(!pbo::parse_xml(wide).ok());                                   // 200,000 nodes at most
    big = "<a>" + std::string((9u << 20), 'x') + "</a>";
    PB_CHECK(!pbo::parse_xml(big).ok());                                    // 8 MiB at most
    PB_CHECK(pbo::parse_xml("<a x=\"" + std::string(1u << 20, 'v') + "\"/>").ok());   // a long value inside the limits is fine
    PB_CHECK(pbo::parse_xml("<" + std::string(100000, 'n') + "/>").ok());             // so is a long name
    if (seconds_since(t) > 20.0) PB_FAIL_AT("too slow");
}

PB_TEST(mutations_of_a_caldav_answer_never_crash) {
    const std::string doc =
        "<?xml version=\"1.0\"?><d:multistatus xmlns:d=\"DAV:\" xmlns:c=\"urn:ietf:params:xml:ns:caldav\"><d:response><d:href>/cal/a.ics</d:href>"
        "<d:propstat><d:prop><d:getetag>\"1\"</d:getetag><c:calendar-data><![CDATA[BEGIN:VCALENDAR\r\nEND:VCALENDAR\r\n]]></c:calendar-data></d:prop>"
        "<d:status>HTTP/1.1 200 OK</d:status></d:propstat></d:response></d:multistatus>";
    for (uint64_t seed = 1; seed <= 500; ++seed) {
        (void)pbo::parse_xml(pbtest::flipped(doc, seed, 4));
        (void)pbo::parse_xml(pbtest::with_insertions(doc, seed, 3));
    }
}

PB_TEST_MAIN()
