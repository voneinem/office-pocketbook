// T033: the IMAP response parsers used by the walking skeleton (contracts/imap-smtp-subset.md section 3). They are
// total: any prefix of a valid response, and hostile input, yields what was understood and ok=false, never an
// out-of-bounds read (run under PBO_SANITIZE to prove it).
#include <cstring>
#include <memory>
#include <string>

#include "fixtures.h"
#include "pbo/imap_parse.h"
#include "test_support.h"

using pbo::ImapRaw;
using pbtest::fixture;

namespace {
ImapRaw raw(const std::string& s) { return ImapRaw{s}; }
bool has(const std::set<std::string>& s, const std::string& v) { return s.count(v) == 1; }
}  // namespace

PB_TEST(parse_list_reads_attributes_delimiters_and_names) {
    auto r = pbo::parse_list(raw(fixture("imap/list_basic.unverified.txt")));
    PB_CHECK(r.ok);
    PB_CHECK_EQ(r.value.size(), static_cast<size_t>(8));
    PB_CHECK_EQ(r.value[0].raw_name, std::string("INBOX"));
    PB_CHECK_EQ(r.value[0].delimiter, std::string("/"));
    PB_CHECK(has(r.value[0].attributes, "\\HasNoChildren"));
    PB_CHECK_EQ(r.value[1].raw_name, std::string("Sent"));
    PB_CHECK(has(r.value[1].attributes, "\\Sent"));
    PB_CHECK_EQ(r.value[4].name, std::string("Projekte/Planung"));
}

PB_TEST(parse_list_decodes_a_literal_mailbox_name_from_modified_utf7) {
    auto r = pbo::parse_list(raw(fixture("imap/list_basic.unverified.txt")));
    PB_CHECK(r.ok);
    PB_CHECK_EQ(r.value[5].raw_name, std::string("Entw&APw-rfe"));
    PB_CHECK_EQ(r.value[5].name, std::string("Entw\xC3\xBCrfe"));   // Entwürfe
}

PB_TEST(parse_list_handles_other_delimiters_nil_and_unquoted_names) {
    auto r = pbo::parse_list(raw(fixture("imap/list_basic.unverified.txt")));
    PB_CHECK_EQ(r.value[6].raw_name, std::string("Shared"));
    PB_CHECK_EQ(r.value[6].delimiter, std::string("."));
    PB_CHECK(has(r.value[6].attributes, "\\Noselect"));
    PB_CHECK_EQ(r.value[7].raw_name, std::string("Weird"));
    PB_CHECK_EQ(r.value[7].delimiter, std::string(""));
    PB_CHECK(r.value[7].attributes.empty());
}

PB_TEST(parse_list_handles_escaped_quotes) {
    auto r = pbo::parse_list(raw("* LIST () \"/\" \"Say \\\"hi\\\" \\\\ there\"\r\n"));
    PB_CHECK(r.ok);
    PB_CHECK_EQ(r.value.size(), static_cast<size_t>(1));
    PB_CHECK_EQ(r.value[0].raw_name, std::string("Say \"hi\" \\ there"));
}

PB_TEST(parse_status_reads_the_cursor_items) {
    auto r = pbo::parse_status(raw(fixture("imap/status_basic.unverified.txt")));
    PB_CHECK(r.ok);
    PB_CHECK_EQ(r.value.mailbox, std::string("INBOX"));
    PB_CHECK(r.value.has_messages && r.value.has_uidnext && r.value.has_uidvalidity && r.value.has_highestmodseq);
    PB_CHECK(!r.value.has_unseen);
    PB_CHECK_EQ(r.value.messages, 12u);
    PB_CHECK_EQ(r.value.uidnext, 113u);
    PB_CHECK_EQ(r.value.uidvalidity, 1760000000u);
    PB_CHECK_EQ(r.value.highestmodseq, static_cast<uint64_t>(4711));
}

PB_TEST(parse_status_rejects_numbers_that_do_not_fit_and_missing_data) {
    auto big = pbo::parse_status(raw("* STATUS INBOX (UIDNEXT 99999999999 UIDVALIDITY 5)\r\n"));
    PB_CHECK(!big.ok);   // 32-bit UIDs
    auto none = pbo::parse_status(raw("* 5 EXISTS\r\n"));
    PB_CHECK(!none.ok);
    PB_CHECK(!none.value.has_uidvalidity);
}

PB_TEST(parse_search_lists_the_uids) {
    auto r = pbo::parse_search(raw(fixture("imap/search_basic.unverified.txt")));
    PB_CHECK(r.ok);
    PB_CHECK_EQ(r.value.size(), static_cast<size_t>(4));
    PB_CHECK_EQ(r.value[0], 101u);
    PB_CHECK_EQ(r.value[3], 110u);
    auto empty = pbo::parse_search(raw("* SEARCH\r\n"));
    PB_CHECK(empty.ok);
    PB_CHECK(empty.value.empty());
    auto junk = pbo::parse_search(raw("* SEARCH 5 x7 9\r\n"));
    PB_CHECK(!junk.ok);
    PB_CHECK_EQ(junk.value.size(), static_cast<size_t>(1));   // 5 understood, the rest dropped
}

PB_TEST(parse_capability_gives_lowercase_tokens) {
    auto caps = pbo::parse_capability(raw(fixture("imap/capability_condstore.unverified.txt")));
    PB_CHECK(has(caps, "imap4rev1"));
    PB_CHECK(has(caps, "condstore"));
    PB_CHECK(has(caps, "uidplus"));
    PB_CHECK(has(caps, "move"));
    PB_CHECK(has(caps, "auth=plain"));
    PB_CHECK(!has(caps, "IMAP4rev1"));
}

PB_TEST(parse_fetch_reads_flags_dates_sizes_and_the_header_literal) {
    auto r = pbo::parse_fetch(raw(fixture("imap/fetch_headers_basic.unverified.txt")));
    PB_CHECK(r.ok);
    PB_CHECK_EQ(r.value.size(), static_cast<size_t>(2));
    const auto& a = r.value[0];
    PB_CHECK_EQ(a.seq, 1u);
    PB_CHECK_EQ(a.uid, 101u);
    PB_CHECK(a.flags_bits.has_value());
    PB_CHECK_EQ(*a.flags_bits, static_cast<uint32_t>(pbo::kFlagSeen));
    PB_CHECK(a.internal_date.has_value());
    PB_CHECK_EQ(*a.internal_date, static_cast<int64_t>(1791194401));   // 05-Oct-2026 10:00:01 +0000
    PB_CHECK_EQ(*a.size, static_cast<uint64_t>(812));
    PB_CHECK(a.header_bytes.find("Subject: Planung Oktober\r\n") != std::string::npos);
    PB_CHECK(a.header_bytes.size() > 100 && a.header_bytes.compare(a.header_bytes.size() - 4, 4, "\r\n\r\n") == 0);
    const auto& b = r.value[1];
    PB_CHECK_EQ(b.uid, 102u);
    PB_CHECK_EQ(*b.flags_bits, 0u);
    PB_CHECK(b.header_bytes.find("Re: Planung Oktober") != std::string::npos);
}

PB_TEST(parse_fetch_tolerates_a_missing_closing_parenthesis_after_a_literal) {
    // libcurl does not deliver the ")" that closes the item after a literal (research R-01)
    auto r = pbo::parse_fetch(raw(fixture("imap/fetch_literal_missing_paren.unverified.txt")));
    PB_CHECK(r.ok);
    PB_CHECK_EQ(r.value.size(), static_cast<size_t>(1));
    PB_CHECK_EQ(r.value[0].uid, 101u);
    PB_CHECK_EQ(*r.value[0].flags_bits, static_cast<uint32_t>(pbo::kFlagSeen | pbo::kFlagFlagged));
    PB_CHECK(r.value[0].header_bytes.find("Message-ID: <a1@example.org>") != std::string::npos);
}

PB_TEST(parse_fetch_reads_modseq_and_ignores_unrelated_untagged_lines) {
    auto r = pbo::parse_fetch(raw("* 5 EXISTS\r\n* 2 FETCH (UID 7 MODSEQ (12345) FLAGS (\\Seen $Forwarded \\Weird))\r\n* OK done\r\n"));
    PB_CHECK(r.ok);
    PB_CHECK_EQ(r.value.size(), static_cast<size_t>(1));
    PB_CHECK_EQ(*r.value[0].modseq, static_cast<uint64_t>(12345));
    PB_CHECK_EQ(*r.value[0].flags_bits, static_cast<uint32_t>(pbo::kFlagSeen | pbo::kFlagForwarded));
}

PB_TEST(parse_fetch_keeps_bodystructure_text) {
    auto r = pbo::parse_fetch(raw("* 1 FETCH (UID 3 BODYSTRUCTURE (\"TEXT\" \"PLAIN\" (\"CHARSET\" \"utf-8\") NIL NIL \"7BIT\" 10 1))\r\n"));
    PB_CHECK(r.ok);
    PB_CHECK_EQ(r.value.size(), static_cast<size_t>(1));
    PB_CHECK(r.value[0].bodystructure_text.find("\"PLAIN\"") != std::string::npos);
}

PB_TEST(a_garbled_bodystructure_is_skipped_and_the_rest_of_the_item_is_kept) {
    auto r = pbo::parse_fetch(raw("* 1 FETCH (UID 3 BODY[HEADER] {2}\r\nab BODYSTRUCTURE (((((\r\n)\r\n* 2 FETCH (UID 4 FLAGS (\\Seen))\r\n"));
    PB_CHECK_EQ(r.value.size(), static_cast<size_t>(2));
    PB_CHECK_EQ(r.value[0].uid, 3u);
    PB_CHECK_EQ(r.value[0].header_bytes, std::string("ab"));
    PB_CHECK(r.value[0].structure_unreadable);
    PB_CHECK(r.value[0].bodystructure_text.empty());
    PB_CHECK_EQ(r.value[1].uid, 4u);                                  // the next item is read normally
    PB_CHECK(!r.value[1].structure_unreadable);
    // a structure that is cut off at the end of the input is an incomplete item, not a skipped one
    PB_CHECK(!pbo::parse_fetch(raw("* 1 FETCH (UID 3 BODYSTRUCTURE ((\"TEXT\" \"PLAIN\"")).ok);
}

PB_TEST(a_literal_larger_than_the_input_is_a_cut_off_not_a_crash) {
    auto r = pbo::parse_fetch(raw("* 1 FETCH (UID 3 BODY[] {99999}\r\nshort"));
    PB_CHECK(!r.ok);
    auto huge = pbo::parse_fetch(raw("* 1 FETCH (UID 3 BODY[] {4294967295}\r\nx"));
    PB_CHECK(!huge.ok);
}

// Truncation at every offset: every prefix of every fixture is handled without reading outside the buffer (the
// prefix is copied into an exact-size heap block so a sanitizer build catches an overrun). For FETCH data the rule
// is stronger: an item is returned only when its literal arrived completely, returned items equal the items of the
// full response, and a cut-off response is reported with ok=false.
PB_TEST(every_prefix_of_every_fixture_is_handled) {
    const char* names[] = {"imap/list_basic.unverified.txt", "imap/status_basic.unverified.txt",
                           "imap/search_basic.unverified.txt", "imap/capability_condstore.unverified.txt",
                           "imap/fetch_headers_basic.unverified.txt", "imap/fetch_literal_missing_paren.unverified.txt"};
    for (const char* name : names) {
        const std::string full = fixture(name);
        const auto whole = pbo::parse_fetch(raw(full));
        for (size_t n = 0; n <= full.size(); ++n) {
            std::unique_ptr<char[]> exact(new char[n ? n : 1]);
            std::memcpy(exact.get(), full.data(), n);
            ImapRaw prefix{std::string(exact.get(), n)};
            (void)pbo::parse_list(prefix);
            (void)pbo::parse_status(prefix);
            (void)pbo::parse_search(prefix);
            (void)pbo::parse_capability(prefix);
            const auto f = pbo::parse_fetch(prefix);
            PB_CHECK(f.value.size() <= whole.value.size());
            for (size_t i = 0; i < f.value.size(); ++i) {
                PB_CHECK_EQ(f.value[i].uid, whole.value[i].uid);
                PB_CHECK_EQ(f.value[i].header_bytes, whole.value[i].header_bytes);
                PB_CHECK(f.value[i].flags_bits == whole.value[i].flags_bits);
            }
            if (n == full.size()) PB_CHECK(f.ok == whole.ok);
            if (!whole.value.empty() && n > 0 && n < 100) PB_CHECK(!f.ok || f.value.empty());   // cut inside the first item
        }
    }
}

PB_TEST(hostile_input_is_bounded) {
    std::string nest = "* 1 FETCH (UID 1 BODYSTRUCTURE ";
    nest += std::string(100000, '(');
    auto n = pbo::parse_fetch(raw(nest));
    PB_CHECK(!n.ok);
    std::string noise;
    for (int i = 0; i < 4096; ++i) noise += static_cast<char>((i * 131 + 7) & 0xFF);
    (void)pbo::parse_fetch(raw(noise));
    (void)pbo::parse_list(raw(noise));
    std::string line = "* SEARCH " + std::string(2u << 20, '1');   // one line over 1 MiB
    auto s = pbo::parse_search(raw(line));
    PB_CHECK(!s.ok);
    auto cnt = pbo::parse_fetch(raw("* 1 FETCH (UID 1 BODY[] {18446744073709551615}\r\n"));
    PB_CHECK(!cnt.ok);
}

// The shape of a response, for the log when a server answer cannot be read: structure only. Known IMAP keywords stay,
// numbers, strings, addresses and literal contents are replaced, so no mail data can reach the log (FR-041).
PB_TEST(the_shape_of_a_response_keeps_the_structure_and_none_of_the_content) {
    const std::string shape = pbo::describe_shape(raw(fixture("imap/fetch_headers_basic.unverified.txt")), 400);
    PB_CHECK(shape.find("FETCH") != std::string::npos);
    PB_CHECK(shape.find("UID") != std::string::npos);
    PB_CHECK(shape.find("FLAGS") != std::string::npos);
    PB_CHECK(shape.find("\\Seen") != std::string::npos);   // standard flags are structure
    PB_CHECK(shape.find("{L") != std::string::npos);         // a literal, with its declared and available size
    PB_CHECK(shape.find("\\r\\n") != std::string::npos);    // line ends are visible
    for (const char* leaked : {"Planung", "Oktober", "example.org", "Anna", "anna", "Beispiel", "alice", "Message-ID", "a1@", "10:00:01", "2026"})
        if (shape.find(leaked) != std::string::npos) PB_FAIL_AT(std::string("the shape contains: ") + leaked);
}

PB_TEST(the_shape_of_a_structure_keeps_media_types_and_drops_names) {
    const std::string shape = pbo::describe_shape(raw("* 1 FETCH (UID 4 BODYSTRUCTURE ((\"TEXT\" \"PLAIN\" (\"CHARSET\" \"utf-8\") NIL NIL \"7BIT\" 12 1)"
                                                     "(\"APPLICATION\" \"PDF\" (\"NAME\" \"Steuer 2025 Anna Beispiel.pdf\") NIL NIL \"BASE64\" 900 NIL (\"ATTACHMENT\" (\"FILENAME\" \"Steuer 2025 Anna Beispiel.pdf\")) NIL) \"MIXED\"))\r\n"), 600);
    PB_CHECK(shape.find("\"APPLICATION\" \"PDF\"") != std::string::npos);
    PB_CHECK(shape.find("\"ATTACHMENT\"") != std::string::npos);
    PB_CHECK(shape.find("\"FILENAME\"") != std::string::npos);
    PB_CHECK(shape.find("\"MIXED\"") != std::string::npos);
    for (const char* leaked : {"Steuer", "Anna", "Beispiel", "2025", ".pdf"})
        if (shape.find(leaked) != std::string::npos) PB_FAIL_AT(std::string("the shape contains: ") + leaked);
}

PB_TEST(the_shape_shows_a_literal_that_is_shorter_than_declared) {
    const std::string shape = pbo::describe_shape(raw("* 1 FETCH (UID 3 BODY[] {99}\r\nshort"), 400);
    PB_CHECK(shape.find("{L99/5}") != std::string::npos);   // declared 99, 5 bytes available
}

PB_TEST(the_shape_is_bounded_and_survives_hostile_input) {
    PB_CHECK(pbo::describe_shape(raw(std::string(100000, '(')), 200).size() <= 200);
    std::string noise;
    for (int i = 0; i < 5000; ++i) noise += static_cast<char>((i * 31 + 7) & 0xFF);
    PB_CHECK(pbo::describe_shape(raw(noise), 300).size() <= 300);
    PB_CHECK(pbo::describe_shape(raw(""), 300).empty());
}

PB_TEST(flags_map_to_bits_and_back) {
    PB_CHECK_EQ(pbo::flags_to_bits({"\\Seen", "\\flagged", "$Forwarded", "\\Nonsense"}),
                static_cast<uint32_t>(pbo::kFlagSeen | pbo::kFlagFlagged | pbo::kFlagForwarded));
    PB_CHECK_EQ(pbo::bits_to_flag_list(pbo::kFlagSeen | pbo::kFlagDraft), std::string("(\\Seen \\Draft)"));
    PB_CHECK_EQ(pbo::bits_to_flag_list(0), std::string("()"));
}

PB_TEST(modified_utf7_round_trips_names) {
    PB_CHECK_EQ(pbo::decode_mutf7("INBOX"), std::string("INBOX"));
    PB_CHECK_EQ(pbo::decode_mutf7("Entw&APw-rfe"), std::string("Entw\xC3\xBCrfe"));
    PB_CHECK_EQ(pbo::decode_mutf7("A&-B"), std::string("A&B"));
    PB_CHECK_EQ(pbo::encode_mutf7("Entw\xC3\xBCrfe"), std::string("Entw&APw-rfe"));
    PB_CHECK_EQ(pbo::encode_mutf7("A&B"), std::string("A&-B"));
    // a character outside the BMP is a surrogate pair
    PB_CHECK_EQ(pbo::decode_mutf7(pbo::encode_mutf7("Mail \xF0\x9F\x93\xA7")), std::string("Mail \xF0\x9F\x93\xA7"));
    // invalid input is kept, not dropped
    PB_CHECK_EQ(pbo::decode_mutf7("Bad&***-name"), std::string("Bad&***-name"));
    PB_CHECK_EQ(pbo::decode_mutf7("Open&AOQ"), std::string("Open&AOQ"));
}

PB_TEST_MAIN()
