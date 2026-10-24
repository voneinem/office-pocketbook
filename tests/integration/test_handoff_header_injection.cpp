// T054 (spec 003 FR-013, FR-021, FR-022): a subject or a recipient handed over by another app can never add a header to the message that is sent: no hidden second recipient,
// no hidden blind copy. The path is the real one: parse the hand-over, save the draft in the store, build the outgoing message from the stored draft.
#include <fstream>
#include <iterator>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "pbo/compose.h"
#include "pbo/handoff.h"
#include "pbo/store.h"
#include "test_support.h"
#include "tmp_dir.h"

namespace {

struct Clock : pbo::Clock {
    int64_t now() const override { return 1791979200; }
};

std::string fixture(const std::string& name) {
    std::ifstream f(std::string(PBO_FIXTURE_DIR) + "/handoff/" + name, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// the header lines of a raw message (unfolded), up to the first empty line
std::vector<std::string> headers_of(const std::string& raw) {
    std::vector<std::string> out;
    std::istringstream in(raw);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) break;
        if ((line[0] == ' ' || line[0] == '\t') && !out.empty()) out.back() += line;
        else out.push_back(line);
    }
    return out;
}

std::vector<std::string> header_names(const std::vector<std::string>& headers) {
    std::vector<std::string> names;
    for (const std::string& h : headers) names.push_back(h.substr(0, h.find(':')));
    return names;
}

int count_header(const std::vector<std::string>& headers, const std::string& name) {
    int n = 0;
    for (const std::string& h : headers)
        if (h.compare(0, name.size() + 1, name + ":") == 0) ++n;
    return n;
}

// what the hand-over bridge does with a parsed hand-over (bridge/src/handoff_bridge.cpp), then what the compose bridge's Send does with the stored draft
pbo::Result<pbo::BuiltMessage> built_from_handoff(const std::vector<std::string>& args, const std::string& typed_recipient = "") {
    pbtest::TmpDir dir;
    Clock clock;
    auto store = std::move(pbo::Store::open(dir.path(), clock).value);
    const pbo::HandoffResult r = pbo::parse_handoff(args);
    pbo::Draft d;
    d.kind = "new";
    d.message_id = "<0123456789abcdef0123456789abcdef@example.org>";
    d.to_text = r.handoff.to.empty() ? typed_recipient : r.handoff.to;   // the user adds a recipient when the hand-over had none
    d.subject = r.handoff.subject;
    d.body = r.handoff.text;
    const auto id = store->save_draft(d);
    pbo::Result<pbo::BuiltMessage> none;
    if (!id.ok()) return none;
    const auto stored = store->draft(id.value);
    pbo::ComposeDraft c;
    c.from_name = "Alice Example";
    c.from_addr = "alice@example.org";
    c.to = stored->to_text;
    c.cc = stored->cc_text;
    c.bcc = stored->bcc_text;
    c.subject = stored->subject;
    c.body = stored->body;
    c.message_id = stored->message_id;
    c.date_utc = clock.now();
    return pbo::build_message(c);
}

std::vector<std::string> ordinary_header_names() {
    const auto built = built_from_handoff({"pbo-mail-v1", "text=hi", "textlen=2", "subject=Plan", "to=bob@example.org"});
    return header_names(headers_of(built.value.raw));
}

}  // namespace

PB_TEST(a_subject_with_a_second_header_gives_one_subject_line_one_recipient_and_no_bcc_or_cc) {
    const std::string subject = fixture("injection_subject.txt");   // "Hello\r\nBcc: attacker@example.org"
    const auto built = built_from_handoff({"pbo-mail-v1", "text=body", "textlen=4", "subject=" + subject, "to=bob@example.org"});
    PB_CHECK(built.ok());
    const auto headers = headers_of(built.value.raw);
    PB_CHECK_EQ(count_header(headers, "To"), 1);
    PB_CHECK_EQ(count_header(headers, "Bcc"), 0);
    PB_CHECK_EQ(count_header(headers, "Cc"), 0);
    PB_CHECK_EQ(count_header(headers, "Subject"), 1);
    PB_CHECK(built.value.raw.find("attacker@example.org") == std::string::npos || built.value.raw.find("Subject: Hello Bcc: attacker@example.org") != std::string::npos);   // at most as text of the subject line
    PB_CHECK_EQ(built.value.recipients.size(), size_t(1));          // the envelope has the one recipient only
    PB_CHECK_EQ(built.value.recipients[0], std::string("bob@example.org"));
    PB_CHECK(header_names(headers) == ordinary_header_names());     // no header beyond the ones of an ordinary message
}

PB_TEST(a_recipient_with_a_hidden_second_header_is_not_prefilled_so_the_message_has_only_the_address_the_user_types) {
    const std::string to = fixture("injection_recipient.txt");   // "bob@example.org\r\nBcc: attacker@example.org"
    const auto no_recipient = built_from_handoff({"pbo-mail-v1", "text=body", "textlen=4", "to=" + to});
    PB_CHECK(!no_recipient.ok());                                  // nothing was prefilled, so Send says that a recipient is missing
    PB_CHECK_EQ(no_recipient.error.key, std::string("no_recipient"));
    const auto typed = built_from_handoff({"pbo-mail-v1", "text=body", "textlen=4", "to=" + to}, "carol@example.org");
    PB_CHECK(typed.ok());
    const auto headers = headers_of(typed.value.raw);
    PB_CHECK_EQ(count_header(headers, "To"), 1);
    PB_CHECK_EQ(count_header(headers, "Bcc"), 0);
    PB_CHECK(typed.value.raw.find("attacker@example.org") == std::string::npos);
    PB_CHECK_EQ(typed.value.recipients.size(), size_t(1));
    PB_CHECK_EQ(typed.value.recipients[0], std::string("carol@example.org"));
}

PB_TEST(even_if_a_hostile_recipient_reached_a_draft_the_builder_refuses_it_or_adds_no_header) {
    pbo::ComposeDraft c;
    c.from_name = "Alice";
    c.from_addr = "alice@example.org";
    c.to = "bob@example.org\r\nBcc: attacker@example.org";
    c.subject = "x";
    c.body = "y";
    c.message_id = "<0123456789abcdef0123456789abcdef@example.org>";
    c.date_utc = 1791979200;
    const auto built = pbo::build_message(c);
    if (built.ok()) {
        const auto headers = headers_of(built.value.raw);
        PB_CHECK_EQ(count_header(headers, "Bcc"), 0);
        for (const std::string& r : built.value.recipients) PB_CHECK(r.find("attacker") == std::string::npos);
    } else {
        PB_CHECK_EQ(built.error.key, std::string("recipient_invalid"));
    }
}

PB_TEST(the_text_is_the_body_after_the_blank_line_and_unchanged) {
    const std::string text = fixture("link_with_password.txt");
    const auto built = built_from_handoff({"pbo-mail-v1", "text=" + text, "textlen=" + std::to_string(text.size()), "subject=Link", "to=bob@example.org"});
    PB_CHECK(built.ok());
    PB_CHECK(built.value.raw.find("\r\n\r\n") != std::string::npos);
    // the body is quoted-printable UTF-8: the ASCII link and password survive as they are, apart from soft line breaks
    const std::string body = built.value.raw.substr(built.value.raw.find("\r\n\r\n") + 4);
    PB_CHECK(body.find("https://cloud.example.org/s/AbCdEf123") != std::string::npos);
    PB_CHECK(body.find("Password: example-password") != std::string::npos);
}

PB_TEST_MAIN()
