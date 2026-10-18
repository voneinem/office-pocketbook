// T017 (spec 002 FR-026, SC-008): every malformed card, mutations of the good ones and a few huge generated inputs go through the vCard reader
// (parse, strip, derive, edit starting point) without a crash or hang (run under PBO_SANITIZE for the memory checks). Results are bounded and
// valid UTF-8.
#include <dirent.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "mutate.h"
#include "pbo/charset.h"
#include "pbo/vcard.h"
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

void check_text(const std::string& s, const std::string& what) {
    if (!pbo::is_valid_utf8(s)) PB_FAIL_AT(what + ": text is not valid UTF-8");
}

void check(const std::string& data, const std::string& what) {
    const auto start = std::chrono::steady_clock::now();
    const pbo::ParsedContact c = pbo::parse_vcard(data);
    if (!c.readable && c.problem.empty()) PB_FAIL_AT(what + ": unreadable without a reason");
    if (c.readable && !c.problem.empty()) PB_FAIL_AT(what + ": readable with a problem key");
    if (c.phones.size() > pbo::kMaxCardEntries || c.emails.size() > pbo::kMaxCardEntries || c.addresses.size() > pbo::kMaxCardEntries) {
        if (c.readable) PB_FAIL_AT(what + ": too many entries");
    }
    for (const std::string* s : {&c.uid, &c.fn, &c.family, &c.given, &c.additional, &c.prefix, &c.suffix, &c.org, &c.note, &c.version}) check_text(*s, what);
    for (const auto& p : c.phones) { check_text(p.value, what); check_text(p.label, what); }
    for (const auto& p : c.emails) { check_text(p.value, what); check_text(p.label, what); }
    for (const auto& a : c.addresses) for (const std::string* s : {&a.street, &a.city, &a.region, &a.postcode, &a.country, &a.label}) check_text(*s, what);
    // the lines of the text are all kept: the writer can reproduce the card
    std::string joined;
    for (const auto& l : c.lines) joined += l.raw;
    if (c.problem != "too_large" && joined != data) PB_FAIL_AT(what + ": the lines do not add up to the text");
    for (int idx : c.phone_lines) if (idx < 0 || static_cast<size_t>(idx) >= c.lines.size()) PB_FAIL_AT(what + ": phone line index out of range");
    for (int idx : c.email_lines) if (idx < 0 || static_cast<size_t>(idx) >= c.lines.size()) PB_FAIL_AT(what + ": email line index out of range");
    for (int idx : c.address_lines) if (idx < 0 || static_cast<size_t>(idx) >= c.lines.size()) PB_FAIL_AT(what + ": address line index out of range");

    const pbo::ContactFields f = pbo::contact_derive(c);
    for (const std::string* s : {&f.display_name, &f.family, &f.given, &f.org, &f.subtitle, &f.sort_key, &f.search_text}) {
        if (s->size() > (256u << 10)) PB_FAIL_AT(what + ": derived text over the cap");
    }
    check_text(f.display_name, what);
    check_text(f.search_text, what);
    if (f.display_name.empty()) PB_FAIL_AT(what + ": no display name");
    for (const auto& e : f.emails) { check_text(e.addr, what); check_text(e.label, what); }

    const pbo::ContactEdit e = pbo::contact_edit_of(c);
    (void)e;
    bool removed = false;
    const std::string stripped = pbo::strip_heavy_properties(data, &removed);
    if (stripped.size() > data.size()) PB_FAIL_AT(what + ": strip made the text longer");
    if (!removed && stripped != data) PB_FAIL_AT(what + ": strip changed the text but reported nothing");
    const pbo::ParsedContact again = pbo::parse_vcard(stripped);
    (void)again;
    const auto secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    if (secs > 20.0) PB_FAIL_AT(what + ": took " + std::to_string(secs) + " s");
}

}  // namespace

PB_TEST(there_are_at_least_20_malformed_card_files) {
    PB_CHECK(files_in("malformed/vcard", ".vcf").size() >= 20);
}

PB_TEST(every_malformed_file_and_good_fixture_is_handled) {
    for (const std::string& f : files_in("malformed/vcard", ".vcf")) check(slurp(f), f);
    for (const std::string& f : files_in("vcard", ".vcf")) check(slurp(f), f);
    for (const std::string& f : files_in("vcard/corpus", ".vcf")) check(slurp(f), f);
}

PB_TEST(truncation_and_mutation_of_good_cards_never_crash) {
    const std::string good =
        "BEGIN:VCARD\r\nVERSION:3.0\r\nUID:g1@example.org\r\nFN:Anna Beispiel\r\nN:Beispiel;Anna;;;\r\nORG:Example Works GmbH;Sales\r\n"
        "TEL;TYPE=WORK,VOICE:+1 555 0100\r\nitem1.EMAIL;type=INTERNET:anna@example.org\r\nitem1.X-ABLabel:_$!<Work>!$_\r\n"
        "ADR;TYPE=WORK:;;1 Example Street;Exampletown;EX;12345;Exampleland\r\nNOTE;ENCODING=QUOTED-PRINTABLE:Zeile=0D=0A eins=\r\n zwei\r\n"
        "PHOTO;ENCODING=b;TYPE=JPEG:AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\r\n AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\r\nEND:VCARD\r\n";
    for (size_t at = 0; at <= good.size(); at += 2) check(pbtest::truncated(good, at), "truncated at " + std::to_string(at));
    for (uint64_t seed = 1; seed <= 400; ++seed) {
        check(pbtest::flipped(good, seed, 5), "flipped " + std::to_string(seed));
        check(pbtest::with_insertions(good, seed, 4), "inserted " + std::to_string(seed));
    }
    for (const char* eol : {"\n", "\r", "\r\n"}) check(pbtest::with_line_ends(good, eol), "line ends changed");
}

PB_TEST(huge_generated_inputs_are_bounded) {
    const std::string head = "BEGIN:VCARD\r\nVERSION:3.0\r\nUID:a\r\n";
    // one line of 1 MiB + 1
    check(head + "NOTE:" + std::string((1u << 20) + 1, 'A') + "\r\nEND:VCARD\r\n", "1 MiB line");
    // a card of 300 KiB made of ordinary lines
    std::string big = head;
    while (big.size() < (300u << 10)) big += "X-FILL:0123456789012345678901234567890123456789012345678901234567890123456789\r\n";
    check(big + "END:VCARD\r\n", "300 KiB card");
    // many folded continuation lines forming one huge logical line
    std::string folded = head + "NOTE:x\r\n";
    for (int i = 0; i < 100000; ++i) folded += " yyyyyyyyyy\r\n";
    check(folded + "END:VCARD\r\n", "folded 1.1 MB");
    // 200,001 lines
    std::string lines = head;
    for (int i = 0; i < 200001; ++i) lines += "X-A:b\r\n";
    check(lines + "END:VCARD\r\n", "200,001 lines");
    // an input of several MiB without any structure
    check(std::string((8u << 20) + 1, 'x'), "8 MiB + 1");
    // semicolons without end in structured values
    check(head + "N:" + std::string(100000, ';') + "\r\nADR:" + std::string(100000, ';') + "\r\nORG:" + std::string(100000, ';') + "\r\nEND:VCARD\r\n", "100,000 separators");
    // a quoted-printable value that never ends
    check(head + "NOTE;ENCODING=QUOTED-PRINTABLE:" + std::string(50000, '=') + "\r\nEND:VCARD\r\n", "equal signs");
    // deeply repeated groups and parameters
    std::string groups = head;
    for (int i = 0; i < 5000; ++i) groups += "g" + std::to_string(i) + ".TEL;TYPE=WORK:+1 555 0000\r\n";
    check(groups + "END:VCARD\r\n", "5,000 grouped phones");
}

PB_TEST_MAIN()
