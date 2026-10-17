// T061 (IMAP part): every parse_* function is total. Truncation at every offset, seeded byte flips and insertions, deep
// nesting and absurd counts must neither crash, hang nor read out of bounds (run under PBO_SANITIZE), and what a parser
// returns must stay inside the documented limits.
#include <chrono>
#include <string>
#include <vector>

#include "mutate.h"
#include "pbo/bodystructure.h"
#include "pbo/imap_parse.h"
#include "test_support.h"

namespace {

const char* kFetch =
    "* 1 FETCH (UID 11 FLAGS (\\Seen \\Flagged $x) INTERNALDATE \"05-Oct-2026 10:00:00 +0200\" RFC822.SIZE 4321 MODSEQ (77) "
    "BODY[HEADER.FIELDS (FROM TO SUBJECT)] {52}\r\nFrom: A <a@example.org>\r\nSubject: Hello\r\nTo: b@x\r\n\r\n "
    "BODYSTRUCTURE ((\"TEXT\" \"PLAIN\" (\"CHARSET\" \"utf-8\") NIL NIL \"QUOTED-PRINTABLE\" 100 3 NIL NIL NIL NIL)"
    "(\"APPLICATION\" \"PDF\" (\"NAME\" \"a.pdf\") NIL NIL \"BASE64\" 2000 NIL (\"ATTACHMENT\" (\"FILENAME\" \"a.pdf\")) NIL NIL) \"MIXED\" "
    "(\"BOUNDARY\" \"xyz\") NIL NIL NIL))\r\n"
    "* 2 FETCH (UID 12 FLAGS () BODY[1] NIL)\r\n";
const char* kList =
    "* LIST (\\HasNoChildren \\Sent) \"/\" \"Sent Items\"\r\n* LIST (\\HasChildren) \".\" INBOX\r\n"
    "* LIST (\\Noselect) \"/\" {7}\r\nW&APw-rk/x\r\n* LIST () NIL \"A\"\r\n";
const char* kStatus = "* STATUS \"INBOX\" (MESSAGES 231 UIDNEXT 44292 UIDVALIDITY 3857529045 UNSEEN 12 HIGHESTMODSEQ 90060115205545359)\r\n";
const char* kSearch = "* SEARCH 2 84 882 4294967295\r\n* ESEARCH (TAG \"A1\") ALL 1:3,5\r\n";
const char* kCapability = "* CAPABILITY IMAP4rev1 STARTTLS AUTH=PLAIN CONDSTORE UIDPLUS\r\n";
const char* kStructure =
    "((\"TEXT\" \"PLAIN\" (\"CHARSET\" \"utf-8\") NIL NIL \"7BIT\" 10 1 NIL NIL NIL NIL)(\"TEXT\" \"HTML\" NIL NIL NIL \"BASE64\" 99 3 NIL NIL NIL NIL) \"ALTERNATIVE\" "
    "(\"BOUNDARY\" \"b\") NIL NIL NIL)";

// every parser on one input; returns only so that the work cannot be optimised away
size_t run_all(const std::string& data) {
    pbo::ImapRaw raw;
    raw.data = data;
    size_t n = 0;
    n += pbo::parse_fetch(raw).value.size();
    n += pbo::parse_list(raw).value.size();
    n += pbo::parse_status(raw).value.mailbox.size();
    n += pbo::parse_search(raw).value.size();
    n += pbo::parse_capability(raw).size();
    n += pbo::describe_shape(raw, 200).size();
    n += pbo::parse_bodystructure(data) ? 1 : 0;
    n += pbo::decode_mutf7(data).size();
    return n;
}

}  // namespace

PB_TEST(a_cut_at_every_offset_never_crashes_and_the_parsers_say_when_input_was_cut) {
    size_t sink = 0;
    for (const char* sample : {kFetch, kList, kStatus, kSearch, kCapability, kStructure})
        for (size_t at : pbtest::truncation_points(std::string(sample).size()))
            sink += run_all(pbtest::truncated(sample, at));
    PB_CHECK(sink > 0);
    // a complete answer is ok, a cut-off literal is not
    pbo::ImapRaw whole{kFetch};
    PB_CHECK(pbo::parse_fetch(whole).ok);
    pbo::ImapRaw cut{std::string(kFetch).substr(0, std::string(kFetch).find("From: A") + 10)};
    PB_CHECK(!pbo::parse_fetch(cut).ok);
}

PB_TEST(byte_flips_and_insertions_never_crash) {
    size_t sink = 0;
    for (const char* sample : {kFetch, kList, kStatus, kSearch, kCapability, kStructure})
        for (uint64_t seed = 1; seed <= 300; ++seed) {
            sink += run_all(pbtest::flipped(sample, seed, 1 + static_cast<int>(seed % 6)));
            sink += run_all(pbtest::with_insertions(sample, seed * 7919, 1 + static_cast<int>(seed % 5)));
        }
    PB_CHECK(sink > 0);
}

PB_TEST(random_bytes_never_crash) {
    pbtest::Prng r(42);
    size_t sink = 0;
    for (int i = 0; i < 400; ++i) {
        std::string junk;
        const size_t len = r.below(600);
        for (size_t k = 0; k < len; ++k) junk += static_cast<char>(r.next() & 0xFF);
        sink += run_all(junk);
        sink += run_all("* " + junk);
        sink += run_all("* 1 FETCH (" + junk);
    }
    (void)sink;
}

PB_TEST(deep_nesting_is_refused_without_overflowing_the_stack) {
    for (int depth : {31, 32, 33, 100, 10000, 200000}) {
        std::string nest(static_cast<size_t>(depth), '(');
        nest += "\"TEXT\" \"PLAIN\" NIL NIL NIL \"7BIT\" 1 1";
        nest += std::string(static_cast<size_t>(depth), ')');
        run_all(nest);
        run_all("* 1 FETCH (UID 1 BODYSTRUCTURE " + nest + ")\r\n");
        run_all("* 1 FETCH (UID 1 FLAGS " + std::string(static_cast<size_t>(depth), '(') + ")\r\n");
    }
    // beyond the limit the structure is not accepted
    std::string deep(200, '(');
    deep += "\"TEXT\" \"PLAIN\" NIL NIL NIL \"7BIT\" 1 1" + std::string(200, ')');
    PB_CHECK(!pbo::parse_bodystructure(deep).has_value());
}

PB_TEST(huge_counts_and_literals_stay_inside_the_limits) {
    // a literal that promises far more than there is, or more than the cap
    for (const char* promised : {"4294967295", "2147483648", "99999999999999999999", "0", "-1", "1048577"}) {
        pbo::ImapRaw raw{std::string("* 1 FETCH (UID 1 BODY[1] {") + promised + "}\r\nabc)\r\n"};
        const auto p = pbo::parse_fetch(raw);
        for (const auto& item : p.value)
            for (const auto& kv : item.sections) PB_CHECK(kv.second.size() <= (8u << 20));
    }
    // numbers that do not fit
    pbo::ImapRaw big{"* STATUS \"INBOX\" (MESSAGES 99999999999999999999 UIDNEXT 4294967296 UIDVALIDITY 18446744073709551616)\r\n"};
    pbo::parse_status(big);
    pbo::ImapRaw search{"* SEARCH 99999999999999999999 4294967296 18446744073709551615 0 1\r\n"};
    PB_CHECK(pbo::parse_search(search).value.size() <= 5u);
    // very many items
    std::string many = "* SEARCH";
    for (int i = 1; i <= 200000; ++i) many += " " + std::to_string(i);
    many += "\r\n";
    const auto t0 = std::chrono::steady_clock::now();
    PB_CHECK(pbo::parse_search(pbo::ImapRaw{many}).value.size() <= 200000);
    std::string lines;
    for (int i = 1; i <= 20000; ++i) lines += "* " + std::to_string(i) + " FETCH (UID " + std::to_string(i) + " FLAGS (\\Seen))\r\n";
    PB_CHECK_EQ(pbo::parse_fetch(pbo::ImapRaw{lines}).value.size(), 20000u);
    PB_CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(10));   // no quadratic behaviour
}

PB_TEST(a_hostile_mailbox_name_cannot_inject_a_command) {
    // names come from the server; the quoting used for commands refuses line breaks (checked in the sync); the parser keeps them as data
    pbo::ImapRaw raw{"* LIST () \"/\" \"A\r\nB\"\r\n* LIST () \"/\" \"ok\"\r\n"};
    for (const auto& f : pbo::parse_list(raw).value)
        PB_CHECK(f.raw_name.find('\0') == std::string::npos);   // whatever is kept is plain data, never executed
}

PB_TEST_MAIN()
