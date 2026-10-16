#pragma once
// IMAP BODYSTRUCTURE (RFC 3501 7.4.2) as a tree, and the readable body built from the parts the app fetched
// (research R-06): only the chosen text part is downloaded, attachments are listed from the structure and fetched on demand.

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pbo/mime.h"

namespace pbo {

struct BodyPart {
    std::string type, subtype;                           // lower case: "text", "plain"
    std::map<std::string, std::string> params;           // lower-case names; RFC 2231 and RFC 2047 already resolved
    std::string id;                                      // Content-ID
    std::string encoding;                                // lower case: "base64", "quoted-printable", "7bit", ...
    uint64_t size = 0;                                   // transfer size in bytes
    std::string disposition;                             // "attachment", "inline" or ""
    std::map<std::string, std::string> disposition_params;
    std::string section;                                 // IMAP section number: "1", "1.2" (a single part is "1")
    std::vector<BodyPart> children;                      // of a multipart
    bool multipart() const { return type == "multipart"; }
    std::string mime() const { return type + "/" + subtype; }
};

// Parses the parenthesized text. Total: nesting deeper than 32, truncated or garbled input gives nullopt (the caller then
// falls back to fetching the start of the whole message). Literals ({n}) inside the structure are understood.
std::optional<BodyPart> parse_bodystructure(std::string_view text);

// The sections to fetch for the readable text, in order: plain-text parts if there are any (alternative: the plain one),
// else the html parts. Mirrors the rule of parse_message: text before the first attachment forms the body.
std::vector<std::string> choose_text_sections(const BodyPart& root);

// The html sections to fetch IN ADDITION to the plain-text ones, to see whether the html has pictures (then it is the body): only when the message has
// both alternatives, the html is at most 512 KiB and likely to have pictures. Empty otherwise.
std::vector<std::string> choose_picture_html_sections(const BodyPart& root);

// The readable body and the attachment list from the structure and the bytes of the fetched sections (still in their
// transfer encoding, as BODY[section] delivers them). Sections that were not fetched are skipped. Header fields of the
// result are empty: they come from the stored message.
ParsedMessage assemble_from_parts(const BodyPart& root, const std::map<std::string, std::string>& section_bytes, const MimeLimits& limits = {});

}  // namespace pbo
