#pragma once
// Parsers for the untagged IMAP responses libcurl delivers for the app's custom requests
// (contracts/imap-smtp-subset.md section 3). All of them are total: truncated or hostile input returns what was
// understood plus ok=false; nothing reads out of bounds, nothing recurses deeper than 32, literals are capped.

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "pbo/imap_transport.h"
#include "pbo/mail_types.h"

namespace pbo {

template <class T>
struct Parsed {
    T value{};
    bool ok = true;   // false = the input was cut off, malformed or over a limit; `value` holds what was understood
};

struct FetchItem {
    uint32_t seq = 0;
    uint32_t uid = 0;
    std::optional<uint32_t> flags_bits;
    std::optional<uint64_t> modseq;
    std::optional<int64_t> internal_date;   // epoch seconds
    std::optional<uint64_t> size;
    std::string header_bytes;               // the BODY[HEADER...] literal
    std::string bodystructure_text;         // raw text of the BODYSTRUCTURE list, "" if absent or unreadable
    bool structure_unreadable = false;      // a BODYSTRUCTURE was there but garbled or nested too deep: skipped, the item is kept
    std::map<std::string, std::string> sections;   // "HEADER.FIELDS (...)", "1", "" (whole message) -> literal bytes
    bool truncated = false;                 // a literal was cut at the cap
};

struct FolderListItem {
    std::string raw_name;     // as sent (modified UTF-7)
    std::string name;         // decoded
    std::string delimiter;    // "" = NIL
    std::set<std::string> attributes;   // as sent, for example "\HasNoChildren"; compare case-insensitively
};

struct StatusInfo {
    std::string mailbox;
    uint32_t uidnext = 0, uidvalidity = 0, messages = 0, unseen = 0;
    uint64_t highestmodseq = 0;
    bool has_uidnext = false, has_uidvalidity = false, has_messages = false, has_unseen = false, has_highestmodseq = false;
};

Parsed<std::vector<FetchItem>> parse_fetch(const ImapRaw& raw);
Parsed<std::vector<FolderListItem>> parse_list(const ImapRaw& raw);
Parsed<StatusInfo> parse_status(const ImapRaw& raw);
Parsed<std::vector<uint32_t>> parse_search(const ImapRaw& raw);
std::set<std::string> parse_capability(const ImapRaw& raw);   // lowercase tokens

uint32_t flags_to_bits(const std::vector<std::string>& flags);   // unknown flags are ignored
std::string bits_to_flag_list(uint32_t bits);                     // "(\Seen \Flagged)"

// Modified UTF-7 (RFC 3501 5.1.3), used for mailbox names. decode never fails (invalid input is kept as is).
std::string decode_mutf7(const std::string& s);
std::string encode_mutf7(const std::string& utf8);

// A description of the STRUCTURE of a response for the log, at most `max_chars` long: IMAP keywords and standard flags
// are kept; numbers become N, quoted strings q, other words w, literal contents are dropped but their declared and
// available sizes are shown ({L<declared>/<available>}); line ends show as \r and \n. Contains no mail data.
std::string describe_shape(const ImapRaw& raw, size_t max_chars);

inline constexpr size_t kMaxImapInput = 64u << 20;   // per command
inline constexpr size_t kMaxImapLine = 1u << 20;
inline constexpr int kMaxImapNesting = 32;

}  // namespace pbo
