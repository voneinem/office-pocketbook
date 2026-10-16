#pragma once
// Writing mail (FR-011 .. FR-014): the outgoing message as RFC 5322 / MIME bytes, and the rules for reply, reply all and forward. Pure functions;
// no Qt, no network. The message is built ONCE when the user taps Send and the same bytes are sent (and, on "Send again", sent again).

#include <cstdint>
#include <string>
#include <vector>

#include "pbo/types.h"

namespace pbo {

struct ComposeDraft {
    std::string from_name, from_addr;
    std::string to, cc, bcc;            // as typed: comma or semicolon separated, "Name <address>" allowed
    std::string subject, body;          // UTF-8
    std::string message_id;             // "<...@domain>", see make_message_id
    std::string in_reply_to, references;  // "<id>" lists, "" if none
    int64_t date_utc = 0;
};

struct BuiltMessage {
    std::string raw;                    // CRLF line ends, ASCII headers, the text as quoted-printable UTF-8
    std::string envelope_from;
    std::vector<std::string> recipients;  // To, Cc and Bcc, without duplicates; Bcc is NOT in the headers
};

// Errors (Unsupported class): "from_invalid", "recipient_invalid" (detail "to" | "cc" | "bcc"), "no_recipient", "invalid_input" (message id or date),
// "too_large" (text over 1 MiB). Nothing is built on an error. Line breaks in the subject or a name cannot add headers.
Result<BuiltMessage> build_message(const ComposeDraft& draft);

// The same message for a draft that is stored on the server (the Drafts folder): whatever is not valid yet (recipients, an empty text) is
// left out instead of refused, and the text is cut at the limit. Only the sender address must be valid. Never fails otherwise.
Result<BuiltMessage> build_draft_message(const ComposeDraft& draft);

// "<32 hex digits@domain>"; the domain is that of `from_addr` (lower case) or "pboffice.invalid" when it has none.
std::string make_message_id(Random& random, const std::string& from_addr);

// "Re: x" / "Fwd: x" unless the subject already starts with such a prefix (Re, Aw, Antw, Sv; Fwd, Fw, Wg, Tr; with a colon, Re[2]: too).
std::string reply_subject(const std::string& subject);
std::string forward_subject(const std::string& subject);

struct ReplySource {
    std::string from_name, from_addr;
    std::string reply_to;               // the Reply-To address list as shown ("" if none)
    std::string to, cc;                 // the address lists as shown ("Name <addr>, addr")
    std::string own_address;            // the user's own address: left out of a reply
};
struct ReplyTarget {
    std::string to, cc;                 // address lists ready for the recipient fields
};
ReplyTarget reply_recipients(const ReplySource& source, bool all);

// "> " in front of every line (an empty line gets ">"), so earlier quotes show as ">>". Line ends become "\n"; the result ends with "\n".
std::string quote_for_reply(const std::string& text);
// "On <when>, <name or address> wrote:"
std::string attribution_line(const std::string& from_name, const std::string& from_addr, const std::string& when_text);
// The body of a forwarded message: the header lines of the original, a blank line, the original text
std::string forward_body(const std::string& from, const std::string& when_text, const std::string& subject, const std::string& to, const std::string& text);
// The References header of a reply: the parent's references plus the parent's Message-ID, without duplicates, at most about 900 characters
// (the first id and the newest ones stay).
std::string references_for_reply(const std::string& parent_references, const std::string& parent_message_id);

}  // namespace pbo
