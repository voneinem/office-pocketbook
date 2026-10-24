#pragma once
// A text handed to PB Office by another app (spec 003, research R-03 and R-07, contracts/handoff-contract.md). Pure and Qt-free: this file only
// decides what the arguments mean. It never logs, never reads a file and treats everything as untrusted (bounded work, no interpretation).

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace pbo {

constexpr size_t kHandoffMaxTextBytes = 8192;       // lowered to the measured limit by experiment E4, never below 6000
constexpr size_t kHandoffMaxSubjectChars = 300;
constexpr size_t kHandoffMaxAddressBytes = 254;
extern const char* const kHandoffMarker;            // "pbo-mail-v1"

// The hand-over still produced a message, but something was left out (the user is told, without the value).
enum class HandoffNotice { SubjectTooLong, RecipientIgnored };
// Nothing is created; the user is told why.
enum class HandoffRefusal { None, TooLong, Incomplete, Malformed };

struct Handoff {
    std::string text, subject, to;                  // cleaned
    std::vector<HandoffNotice> notices;
    bool empty() const;                             // text, subject and to all empty or only white space
};

struct HandoffResult {
    bool is_handoff = false;                        // false: no marker, an ordinary start
    HandoffRefusal refusal = HandoffRefusal::None;  // not None: no Handoff
    Handoff handoff;                                // valid when is_handoff and refusal == None
};

// The arguments exactly as received (the system may put the app path first; everything before the marker is skipped).
HandoffResult parse_handoff(const std::vector<std::string>& args);

// The cleaning rules alone: invalid UTF-8 bytes (one replacement per byte) and control characters except line feed and tab (C0, DEL, C1)
// become U+FFFD; "\r\n" and "\r" become "\n". The subject variant also turns every line feed and tab into one space.
std::string clean_handoff_text(std::string_view text);
std::string clean_handoff_subject(std::string_view text);

}  // namespace pbo
