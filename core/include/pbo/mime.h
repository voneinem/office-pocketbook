#pragma once
// MIME header handling (contracts/core-interfaces.md section 3), grown task by task. All functions are total:
// malformed input gives the best readable result, never an exception or an out-of-bounds read. Text out is UTF-8.

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace pbo {

// Limits that keep hostile or broken mail from using up time or memory (contracts/core-interfaces.md section 3).
struct MimeLimits {
    int max_depth = 8;                     // nesting of multipart parts
    int max_parts = 200;                   // parts looked at
    size_t max_header_block = 64 * 1024;   // one header block
    size_t max_line = 1 << 20;             // one line
    size_t max_text_part = 2 << 20;        // readable text kept from one part (more is cut, note "truncated")
    size_t max_input = 32 << 20;           // the whole message
};

// RFC 2047 encoded words ("=?UTF-8?B?...?=", "=?ISO-8859-1?Q?...?=") in a header value, decoded. Whitespace between
// two encoded words is dropped, adjacent words of one charset are decoded together (a multi-byte character may be split
// between them), invalid words stay as written, folded lines are joined, control characters become blanks.
std::string decode_header_words(std::string_view value);

// "text/plain; charset=utf-8; name*0*=..." -> the main value and the parameters (names lower-cased). RFC 2231
// continuations and extended values (charset'language'%XX) and RFC 2047 words in values are decoded. At most 64 parameters.
struct HeaderParams {
    std::string value;
    std::map<std::string, std::string> params;
};
HeaderParams parse_header_params(std::string_view header_value);

// One address of an address header, as shown to the reader.
struct MailAddress {
    std::string name;      // display name, decoded; "" if none
    std::string address;   // local@domain
};

// Tolerant reading of From/To/Cc: display names (quoted or not), encoded words, comments, groups ("Team: a@x, b@x;").
// Elements that are not addresses are skipped. At most 200 addresses.
std::vector<MailAddress> parse_mailboxes(std::string_view header_value);

// The addresses of a header for display: "Name <addr>, addr2" with decoded names (a name containing a comma or angle
// bracket is quoted). A value that is not a list of addresses is shown decoded as written.
std::string format_address_header(std::string_view raw_header);

// Strict reading of what the user types as recipients (comma or semicolon separated, "Name <addr>" allowed): the
// validated bare addresses, duplicates (ignoring case) removed. ANY syntax error, a line break or more than 100
// entries gives an empty result for the whole list.
std::vector<std::string> parse_address_list(std::string_view text);

}  // namespace pbo

namespace pbo {

struct AttachmentInfo {
    std::string section;     // IMAP-style body section number: "2", "1.2" (a single-part message is "1")
    std::string name;        // file name as the sender gave it, decoded; "" if none (see safe_file_name)
    std::string mime_type;   // lower case, "application/pdf"
    std::string encoding;    // transfer encoding, lower case: "base64", "7bit", ...
    uint64_t size = 0;       // decoded size when known, else the encoded size
};

// A picture the HTML body of a message asks for. `kind` is "cid" (a part of the message, named by its Content-ID; `section` is empty when no part has
// that Content-ID) or "remote" (an https address: loaded only when the user asks).
struct MessageImage {
    std::string kind;
    std::string src;          // "cid:<id>" or the https address
    std::string alt;
    std::string section;      // cid: the IMAP section of the part
    std::string mime_type;    // cid: the part's media type
    std::string encoding;     // cid: its transfer encoding
    uint64_t size = 0;        // cid: decoded size when known
    int width = 0, height = 0;   // as the HTML gave them, 0 = not given
};

struct ParsedMessage {
    std::string from_name, from_addr, to, cc, subject, message_id, in_reply_to, references;
    int64_t date_utc = 0;   // 0 = no usable Date header
    enum class Body { Text, NoReadableText, Unreadable } body_kind = Body::NoReadableText;
    std::string text;                 // readable text, UTF-8, HTML already converted, "\n" line ends
    std::vector<std::string> links;   // [1..n]: the targets of the "[n]" markers in `text`
    std::vector<AttachmentInfo> attachments;
    std::vector<MessageImage> images;   // the pictures of the HTML body; their places in `text` are lines of U+FFFC, n, U+FFFC (n = 1-based index here)
    std::string note;                 // "", "truncated", "charset_unsupported", "html_converted" (the first that applies)
};

// Parses a whole RFC 5322 message. Never throws and never reads beyond the limits; broken input gives the best readable
// result or Body::Unreadable (FR-040). Deterministic. Nothing in the message is ever fetched or executed.
ParsedMessage parse_message(std::string_view raw, const MimeLimits& limits = {});

// A file name that is safe to show and to use on a file system: no path parts, no control characters, no leading dots,
// at most 120 bytes (extension kept), "attachment" if nothing is left.
std::string safe_file_name(std::string_view name);

}  // namespace pbo
