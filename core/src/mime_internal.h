#pragma once
// Shared by parse_message (a whole message in bytes) and the BODYSTRUCTURE path (structure plus the fetched sections):
// both build a tree of PartNode and use the SAME selection of the readable text and the attachment list, so the two
// paths cannot drift apart (tests/integration/test_body_parity.cpp checks this on every fixture mail).

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "pbo/mime.h"

namespace pbo {
namespace mime_internal {

struct PartNode {
    std::string type;                           // full media type, lower case: "text/plain", "multipart/mixed"
    std::map<std::string, std::string> params;  // Content-Type parameters (resolved)
    std::string disposition;                    // "attachment", "inline" or ""
    std::string filename;                       // from the disposition, else from the type's "name"
    std::string content_id;
    std::string cte;                            // transfer encoding, lower case ("" = 7bit)
    std::string section;                        // IMAP section number
    uint64_t decoded_size = 0;                  // decoded size when known
    std::string_view body;                      // the bytes of the part in transfer encoding (empty if not available)
    bool has_body = false;                      // false: not fetched (structure only)
    std::vector<PartNode> children;

    bool multipart() const { return type.compare(0, 10, "multipart/") == 0; }
};

std::string decode_base64(std::string_view s, size_t limit);
std::string decode_qp(std::string_view s, size_t limit);
std::string decode_transfer(std::string_view body, const std::string& cte, size_t limit);
uint64_t decoded_size_of(std::string_view body, const std::string& cte);
uint64_t decoded_size_from_transfer(uint64_t transfer_size, const std::string& cte);   // estimate from the transfer size

// The leaves that make up the readable text: plain-text parts if any, else html parts.
std::vector<const PartNode*> choose_text_parts(const PartNode& root, const MimeLimits& limits);

// The html alternative that is worth fetching in addition to the plain-text one, to see whether it has pictures: the message has both, the html is at most
// 512 KiB, and it is likely to have pictures (the message has inline pictures, or the html is at least 8000 bytes). Empty otherwise.
std::vector<const PartNode*> choose_html_for_pictures(const PartNode& root);

// Readable text, links, note and attachments (the other ParsedMessage fields stay empty).
ParsedMessage assemble(const PartNode& root, const MimeLimits& limits);

}  // namespace mime_internal
}  // namespace pbo
