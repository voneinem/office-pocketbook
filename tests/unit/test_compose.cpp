// T119: building an outgoing message (RFC 5322 / MIME) and the rules for reply, reply all and forward.
#include <string>

#include "pbo/compose.h"
#include "pbo/mime.h"
#include "test_support.h"

namespace {

struct FixedRandom : pbo::Random {
    uint8_t next = 0;
    void bytes(uint8_t* out, size_t n) override {
        for (size_t i = 0; i < n; ++i) out[i] = next++;
    }
};

pbo::ComposeDraft draft() {
    pbo::ComposeDraft d;
    d.from_name = "Alice Example";
    d.from_addr = "alice@example.org";
    d.to = "Bob <bob@example.org>";
    d.subject = "Hello";
    d.body = "Hi Bob,\nsee you soon.\n";
    d.message_id = "<abc123@example.org>";
    d.date_utc = 1791979200;   // Wed, 14 Oct 2026 12:00:00 +0000
    return d;
}

std::string header_block(const std::string& raw) { return raw.substr(0, raw.find("\r\n\r\n")); }
std::string body_part(const std::string& raw) { return raw.substr(raw.find("\r\n\r\n") + 4); }

bool no_bare_lf(const std::string& s) {
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\n' && (i == 0 || s[i - 1] != '\r')) return false;
        if (s[i] == '\r' && (i + 1 >= s.size() || s[i + 1] != '\n')) return false;
    }
    return true;
}

}  // namespace

PB_TEST(a_simple_message_has_the_required_headers_crlf_line_ends_and_round_trips) {
    const auto r = pbo::build_message(draft());
    PB_CHECK(r.ok());
    const std::string& raw = r.value.raw;
    PB_CHECK(no_bare_lf(raw));
    const std::string head = header_block(raw);
    PB_CHECK(head.find("Date: Wed, 14 Oct 2026 12:00:00 +0000\r\n") == 0);
    PB_CHECK(head.find("\r\nFrom: Alice Example <alice@example.org>") != std::string::npos);
    PB_CHECK(head.find("\r\nTo: Bob <bob@example.org>") != std::string::npos);
    PB_CHECK(head.find("\r\nSubject: Hello") != std::string::npos);
    PB_CHECK(head.find("\r\nMessage-ID: <abc123@example.org>") != std::string::npos);
    PB_CHECK(head.find("\r\nMIME-Version: 1.0") != std::string::npos);
    PB_CHECK(head.find("Content-Type: text/plain; charset=UTF-8") != std::string::npos);
    PB_CHECK(head.find("Content-Transfer-Encoding: quoted-printable") != std::string::npos);
    PB_CHECK(head.find("In-Reply-To") == std::string::npos);
    PB_CHECK_EQ(r.value.envelope_from, std::string("alice@example.org"));
    PB_CHECK((r.value.recipients == std::vector<std::string>{"bob@example.org"}));

    const pbo::ParsedMessage p = pbo::parse_message(raw);
    PB_CHECK_EQ(p.subject, std::string("Hello"));
    PB_CHECK_EQ(p.from_addr, std::string("alice@example.org"));
    PB_CHECK_EQ(p.from_name, std::string("Alice Example"));
    PB_CHECK_EQ(p.message_id, std::string("<abc123@example.org>"));
    PB_CHECK_EQ(p.date_utc, int64_t(1791979200));
    PB_CHECK(p.text.find("Hi Bob,\nsee you soon.") == 0);
}

PB_TEST(non_ascii_text_goes_into_quoted_printable_utf8_with_short_lines_and_round_trips) {
    auto d = draft();
    d.body = "Gr\xC3\xBC\xC3\x9F" "e aus K\xC3\xB6ln =)\n";
    d.body += std::string(300, 'x') + "\n";                                   // a very long line
    d.body += "ends with blanks   \n\n.\n" + std::string("\xF0\x9F\x93\x85 emoji and tab\there\n");
    d.subject = "Einladung zum Fr\xC3\xBChst\xC3\xBC" "ck \xE2\x80\x93 Sa 17 Uhr";
    d.from_name = "J\xC3\xBCrgen M\xC3\xBCller";
    d.to = "\"M\xC3\xBCller, Anna\" <anna@example.org>, carl@example.org";
    const auto r = pbo::build_message(d);
    PB_CHECK(r.ok());
    PB_CHECK(no_bare_lf(r.value.raw));
    // the headers are ASCII
    for (unsigned char c : header_block(r.value.raw)) PB_CHECK(c < 0x80);
    // every line of the body is at most 76 characters
    const std::string body = body_part(r.value.raw);
    size_t pos = 0;
    while (pos < body.size()) {
        const size_t e = body.find("\r\n", pos);
        PB_CHECK(e != std::string::npos);
        PB_CHECK(e - pos <= 76);
        pos = e + 2;
    }
    const pbo::ParsedMessage p = pbo::parse_message(r.value.raw);
    PB_CHECK_EQ(p.subject, d.subject);
    PB_CHECK_EQ(p.from_name, d.from_name);
    PB_CHECK(p.text.find("Gr\xC3\xBC\xC3\x9F" "e aus K\xC3\xB6ln =)") == 0);
    PB_CHECK(p.text.find(std::string(300, 'x')) != std::string::npos);
    PB_CHECK(p.text.find("ends with blanks") != std::string::npos);
    PB_CHECK(p.text.find("\n.\n") != std::string::npos);
    PB_CHECK(p.text.find("\xF0\x9F\x93\x85 emoji and tab\there") != std::string::npos);
    PB_CHECK_EQ(r.value.recipients.size(), size_t(2));
    PB_CHECK(r.value.raw.find("To: =?UTF-8?") != std::string::npos);        // the name with the umlaut is an encoded word
}

PB_TEST(a_long_subject_is_folded_and_every_header_line_stays_short) {
    auto d = draft();
    d.subject.clear();
    for (int i = 0; i < 30; ++i) d.subject += "word" + std::to_string(i) + " ";
    d.subject += "end";
    auto r = pbo::build_message(d);
    PB_CHECK(r.ok());
    size_t pos = 0;
    const std::string head = header_block(r.value.raw) + "\r\n";
    while (pos < head.size()) {
        const size_t e = head.find("\r\n", pos);
        PB_CHECK(e - pos <= 78);
        pos = e + 2;
    }
    PB_CHECK_EQ(pbo::parse_message(r.value.raw).subject, d.subject);
    d.subject.clear();
    for (int i = 0; i < 40; ++i) d.subject += "\xC3\xA4\xC3\xB6\xC3\xBC ";
    r = pbo::build_message(d);
    PB_CHECK(r.ok());
    pos = 0;
    const std::string head2 = header_block(r.value.raw) + "\r\n";
    while (pos < head2.size()) {
        const size_t e = head2.find("\r\n", pos);
        PB_CHECK(e - pos <= 78);
        pos = e + 2;
    }
    PB_CHECK(pbo::parse_message(r.value.raw).subject.find("\xC3\xA4\xC3\xB6\xC3\xBC \xC3\xA4\xC3\xB6\xC3\xBC") == 0);
}

PB_TEST(bcc_recipients_get_the_message_but_are_never_written_into_it) {
    auto d = draft();
    d.cc = "carol@example.org";
    d.bcc = "secret@example.org, hidden@example.org";
    const auto r = pbo::build_message(d);
    PB_CHECK(r.ok());
    PB_CHECK(r.value.raw.find("secret@example.org") == std::string::npos);
    PB_CHECK(r.value.raw.find("hidden@example.org") == std::string::npos);
    PB_CHECK(r.value.raw.find("Bcc") == std::string::npos);
    PB_CHECK(r.value.raw.find("Cc: carol@example.org") != std::string::npos);
    PB_CHECK((r.value.recipients == std::vector<std::string>{"bob@example.org", "carol@example.org", "secret@example.org", "hidden@example.org"}));
}

PB_TEST(recipients_that_appear_twice_are_sent_to_once) {
    auto d = draft();
    d.to = "bob@example.org";
    d.cc = "BOB@example.org, carol@example.org";
    d.bcc = "carol@example.org";
    const auto r = pbo::build_message(d);
    PB_CHECK(r.ok());
    PB_CHECK_EQ(r.value.recipients.size(), size_t(2));
}

PB_TEST(reply_headers_are_written_when_given) {
    auto d = draft();
    d.in_reply_to = "<parent@example.org>";
    d.references = "<root@example.org> <parent@example.org>";
    const auto r = pbo::build_message(d);
    PB_CHECK(r.ok());
    PB_CHECK(r.value.raw.find("\r\nIn-Reply-To: <parent@example.org>\r\n") != std::string::npos);
    PB_CHECK(r.value.raw.find("\r\nReferences: <root@example.org> <parent@example.org>\r\n") != std::string::npos);
    const auto p = pbo::parse_message(r.value.raw);
    PB_CHECK_EQ(p.in_reply_to, std::string("<parent@example.org>"));
    PB_CHECK_EQ(p.references, std::string("<root@example.org> <parent@example.org>"));
}

PB_TEST(bad_input_is_refused_with_the_field_named_and_nothing_is_built) {
    auto d = draft();
    d.to = "";
    auto r = pbo::build_message(d);
    PB_CHECK_EQ(r.error.key, std::string("no_recipient"));
    d = draft();
    d.bcc = "";
    d.to = "";
    d.cc = "";
    PB_CHECK_EQ(pbo::build_message(d).error.key, std::string("no_recipient"));
    for (const char* bad : {"not an address", "a@b", "bob@example.org, <broken", "bob@example.org\r\nBcc: evil@example.org", "bob@example.org\nX: y", "@example.org", "bob@"}) {
        d = draft();
        d.to = bad;
        r = pbo::build_message(d);
        if (r.ok()) PB_FAIL_AT(std::string("accepted: ") + bad);
        PB_CHECK_EQ(r.error.key, std::string("recipient_invalid"));
        PB_CHECK_EQ(r.error.detail, std::string("to"));
    }
    d = draft();
    d.cc = "nonsense";
    PB_CHECK_EQ(pbo::build_message(d).error.detail, std::string("cc"));
    d = draft();
    d.bcc = "nonsense";
    PB_CHECK_EQ(pbo::build_message(d).error.detail, std::string("bcc"));
    d = draft();
    d.from_addr = "not-an-address";
    PB_CHECK_EQ(pbo::build_message(d).error.key, std::string("from_invalid"));
    d = draft();
    d.message_id = "no brackets";
    PB_CHECK_EQ(pbo::build_message(d).error.key, std::string("invalid_input"));
    d = draft();
    d.message_id = "<a@b>\r\nBcc: evil@example.org";
    PB_CHECK_EQ(pbo::build_message(d).error.key, std::string("invalid_input"));
}

PB_TEST(line_breaks_in_the_subject_or_the_name_cannot_inject_headers) {
    auto d = draft();
    d.subject = "Hi\r\nBcc: evil@example.org\r\nX-Evil: 1";
    d.from_name = "Alice\r\nBcc: evil@example.org";
    d.body = "text";
    const auto r = pbo::build_message(d);
    PB_CHECK(r.ok());
    PB_CHECK(header_block(r.value.raw).find("\r\nBcc") == std::string::npos);
    PB_CHECK(header_block(r.value.raw).find("\r\nX-Evil") == std::string::npos);
    PB_CHECK_EQ(r.value.recipients.size(), size_t(1));
}

PB_TEST(a_body_with_all_kinds_of_line_ends_and_control_characters_becomes_clean_text) {
    auto d = draft();
    d.body = "one\r\ntwo\rthree\nfour\x01\x02 five\x7f\n";
    const auto r = pbo::build_message(d);
    PB_CHECK(r.ok());
    PB_CHECK(no_bare_lf(r.value.raw));
    const auto p = pbo::parse_message(r.value.raw);
    PB_CHECK(p.text.find("one\ntwo\nthree\nfour") == 0);
    PB_CHECK(p.text.find('\x01') == std::string::npos);
    PB_CHECK(p.text.find("five") != std::string::npos);
}

PB_TEST(an_empty_body_is_allowed_and_a_huge_one_is_refused) {
    auto d = draft();
    d.body = "";
    PB_CHECK(pbo::build_message(d).ok());
    d.body = std::string((1u << 20) + 1, 'x');
    const auto r = pbo::build_message(d);
    PB_CHECK_EQ(r.error.key, std::string("too_large"));
}

PB_TEST(message_ids_are_unpredictable_looking_unique_and_use_the_senders_domain) {
    FixedRandom rnd;
    const std::string a = pbo::make_message_id(rnd, "alice@example.org");
    const std::string b = pbo::make_message_id(rnd, "alice@example.org");
    PB_CHECK(a != b);
    PB_CHECK_EQ(a, std::string("<000102030405060708090a0b0c0d0e0f@example.org>"));
    PB_CHECK_EQ(pbo::make_message_id(rnd, "garbage").substr(33), std::string("@pboffice.invalid>"));
    PB_CHECK_EQ(pbo::make_message_id(rnd, "x@bad domain").substr(33), std::string("@pboffice.invalid>"));
    PB_CHECK(pbo::make_message_id(rnd, "alice@Example.ORG").find("@example.org>") != std::string::npos);
}

PB_TEST(reply_and_forward_subjects_are_not_stacked) {
    PB_CHECK_EQ(pbo::reply_subject("Plan"), std::string("Re: Plan"));
    PB_CHECK_EQ(pbo::reply_subject("Re: Plan"), std::string("Re: Plan"));
    PB_CHECK_EQ(pbo::reply_subject("RE: Plan"), std::string("RE: Plan"));
    PB_CHECK_EQ(pbo::reply_subject("AW: Plan"), std::string("AW: Plan"));
    PB_CHECK_EQ(pbo::reply_subject("Re[2]: Plan"), std::string("Re[2]: Plan"));
    PB_CHECK_EQ(pbo::reply_subject("  Plan"), std::string("Re: Plan"));
    PB_CHECK_EQ(pbo::reply_subject("Regarding the plan"), std::string("Re: Regarding the plan"));   // "Re" without a colon is part of the word
    PB_CHECK_EQ(pbo::reply_subject(""), std::string("Re: "));
    PB_CHECK_EQ(pbo::forward_subject("Plan"), std::string("Fwd: Plan"));
    PB_CHECK_EQ(pbo::forward_subject("Fwd: Plan"), std::string("Fwd: Plan"));
    PB_CHECK_EQ(pbo::forward_subject("FW: Plan"), std::string("FW: Plan"));
    PB_CHECK_EQ(pbo::forward_subject("WG: Plan"), std::string("WG: Plan"));
    PB_CHECK_EQ(pbo::forward_subject("Re: Plan"), std::string("Fwd: Re: Plan"));
    PB_CHECK_EQ(pbo::reply_subject("Fwd: Plan"), std::string("Re: Fwd: Plan"));
}

PB_TEST(reply_goes_to_the_sender_or_reply_to_and_reply_all_adds_the_others_without_me) {
    pbo::ReplySource s;
    s.from_name = "Anna Beispiel";
    s.from_addr = "anna@example.org";
    s.to = "Alice <alice@example.org>, Ben <ben@example.org>";
    s.cc = "Carl <carl@example.org>, ALICE@example.org";
    s.own_address = "alice@example.org";
    auto one = pbo::reply_recipients(s, false);
    PB_CHECK_EQ(one.to, std::string("Anna Beispiel <anna@example.org>"));
    PB_CHECK_EQ(one.cc, std::string(""));
    auto all = pbo::reply_recipients(s, true);
    PB_CHECK_EQ(all.to, std::string("Anna Beispiel <anna@example.org>, Ben <ben@example.org>"));
    PB_CHECK_EQ(all.cc, std::string("Carl <carl@example.org>"));
    s.reply_to = "lists@example.org";
    one = pbo::reply_recipients(s, false);
    PB_CHECK_EQ(one.to, std::string("lists@example.org"));
    all = pbo::reply_recipients(s, true);
    PB_CHECK(all.to.find("lists@example.org") == 0);
    // a duplicate in To and Cc is listed once, in To
    s.cc = "Ben <ben@example.org>, carl@example.org";
    all = pbo::reply_recipients(s, true);
    PB_CHECK_EQ(all.cc, std::string("carl@example.org"));
    // answering my own message: the reply goes to the original recipients
    pbo::ReplySource mine;
    mine.from_addr = "alice@example.org";
    mine.to = "Ben <ben@example.org>";
    mine.own_address = "alice@example.org";
    PB_CHECK_EQ(pbo::reply_recipients(mine, false).to, std::string("Ben <ben@example.org>"));
    PB_CHECK_EQ(pbo::reply_recipients(mine, true).to, std::string("Ben <ben@example.org>"));
}

PB_TEST(a_quoted_reply_prefixes_every_line_and_keeps_earlier_quotes_distinguishable) {
    PB_CHECK_EQ(pbo::quote_for_reply("Yes.\n\n> earlier\nThanks"), std::string("> Yes.\n>\n>> earlier\n> Thanks\n"));
    PB_CHECK_EQ(pbo::quote_for_reply(""), std::string(""));
    PB_CHECK_EQ(pbo::quote_for_reply("one\r\ntwo\r\n"), std::string("> one\n> two\n"));
    PB_CHECK_EQ(pbo::attribution_line("Anna Beispiel", "anna@example.org", "14 October 2026, 14:00"), std::string("On 14 October 2026, 14:00, Anna Beispiel wrote:"));
    PB_CHECK_EQ(pbo::attribution_line("", "anna@example.org", "14 October 2026"), std::string("On 14 October 2026, anna@example.org wrote:"));
    const std::string forwarded = pbo::forward_body("Anna <anna@example.org>", "14 October 2026, 14:00", "Plan", "Alice <alice@example.org>", "The text.\n");
    PB_CHECK(forwarded.find("---------- Forwarded message ----------\nFrom: Anna <anna@example.org>\nDate: 14 October 2026, 14:00\nSubject: Plan\nTo: Alice <alice@example.org>\n\nThe text.") != std::string::npos);
}

PB_TEST(references_grow_with_the_parent_and_stay_bounded) {
    PB_CHECK_EQ(pbo::references_for_reply("", "<a@x>"), std::string("<a@x>"));
    PB_CHECK_EQ(pbo::references_for_reply("<r@x>", "<a@x>"), std::string("<r@x> <a@x>"));
    PB_CHECK_EQ(pbo::references_for_reply("<r@x> <a@x>", "<a@x>"), std::string("<r@x> <a@x>"));
    PB_CHECK_EQ(pbo::references_for_reply("<r@x>", ""), std::string("<r@x>"));
    std::string many;
    for (int i = 0; i < 100; ++i) many += "<m" + std::to_string(i) + "@example.org> ";
    const std::string r = pbo::references_for_reply(many, "<last@example.org>");
    PB_CHECK(r.size() <= 900);
    PB_CHECK(r.find("<m0@example.org>") == 0);                // the root stays
    PB_CHECK(r.find("<last@example.org>") == r.size() - 18);  // the parent stays
}

PB_TEST_MAIN()
