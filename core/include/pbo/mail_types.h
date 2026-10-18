#pragma once
// Plain data types of the mail side of the store (data-model.md: Folder, Message).

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace pbo {

// IMAP flags as a bit set (flags_base / flags_local in the message table).
enum MailFlag : uint32_t {
    kFlagSeen = 1u << 0,
    kFlagAnswered = 1u << 1,
    kFlagFlagged = 1u << 2,
    kFlagDeleted = 1u << 3,
    kFlagDraft = 1u << 4,
    kFlagForwarded = 1u << 5,   // $Forwarded
};

struct Folder {
    int64_t id = 0;
    std::string name;        // full mailbox name, modified UTF-7 decoded
    std::string raw_name;    // as sent by the server (used in commands)
    std::string delimiter = "/";
    std::string role = "other";   // inbox, sent, drafts, trash, junk, all, archive, other
    bool sync_enabled = true;
    std::optional<int64_t> uidvalidity, uidnext, highestmodseq, window_low_uid, last_sync_at;
    bool older_on_server = false;
    int unread_count = 0;
};

struct MessageHeader {
    int64_t id = 0;
    int64_t folder_id = 0;
    uint32_t uid = 0;
    std::string message_id;
    std::string from_name, from_addr, to_text, subject;
    int64_t date_utc = 0;
    int64_t size = 0;
    uint32_t flags = 0;               // flags_local: what the user sees
    std::string local_state = "synced";
    std::string local_error;          // "", gone_on_server, moved_on_server, refused, no_trash
    bool stale = false;               // from before a UIDVALIDITY reset, kept because it has pending work
    std::string body_state = "headers";
    bool has_attachments = false;
    bool unread() const { return (flags & kFlagSeen) == 0; }
};

// An attachment as stored (data-model.md: attachment). `state` is "remote" until the user downloads it.
struct StoredAttachment {
    int64_t id = 0;
    std::string section, name, mime_type, encoding;
    int64_t size = 0;
    std::string state = "remote";   // remote | downloaded
    std::string error_key;          // "", too_large, offline, refused
    std::string saved_path;         // the file in the Downloads folder once downloaded
};

// A picture of a message's HTML body (data-model.md: message_image). `idx` is the number in the text marker. The data are loaded later
// (Store::set_image_loaded); `state` is pending | loaded | failed | too_large | unavailable (a cid that names no part).
struct StoredImage {
    int idx = 0;
    std::string kind;       // cid | remote
    std::string src, alt, section, mime_type, encoding;
    int64_t size = 0;       // cid: the decoded size when known; loaded: the bytes held
    int width = 0, height = 0;
    std::string state = "pending";
};

// One message with its body, for reading.
struct MessageView {
    MessageHeader header;
    std::string cc_text;
    std::string body_text, body_note;
    std::vector<std::string> links;   // the targets of the [n] markers in body_text
    std::vector<StoredAttachment> attachments;
};

// An attachment with what is needed to fetch it from the server.
struct AttachmentRef {
    StoredAttachment attachment;
    int64_t message_id = 0;
    uint32_t uid = 0;
    int64_t folder_id = 0;
    std::string folder_raw_name;
};

// Where a message lives on the server.
struct MessageLocation {
    uint32_t uid = 0;
    int64_t folder_id = 0;
    std::string folder_raw_name;
};

// What a body fetch gives the store for one message, written in one transaction.
struct BodyUpdate {
    std::string state = "text";   // text | unreadable | too_large
    std::string text, note;       // text is cut at 2 MiB
    std::vector<std::string> links;
    std::vector<StoredAttachment> attachments;   // section, name, mime_type, size, encoding; state/error are kept for known ones
    std::vector<StoredImage> images;             // the pictures of the HTML body; a picture with the same number and source keeps what was loaded
};

struct MissingBody {
    int64_t id = 0;
    uint32_t uid = 0;
};

// One page of a list (paged lists, never kinetic scrolling: constitution VI).
template <class T>
struct Page {
    std::vector<T> items;
    int page = 0;         // 0-based
    int page_count = 0;   // at least 1
    int total = 0;
};

enum class SortKey { DateDesc, DateAsc, SenderAsc, SubjectAsc };   // ties always fall back to the date and the id: the order is stable

// A message with a change the server has not seen (US5).
struct PendingMessage {
    int64_t id = 0, folder_id = 0;
    uint32_t uid = 0;
    uint32_t flags_base = 0, flags_local = 0;
    std::string local_state;                 // 'pending_flags' | 'pending_move' | 'pending_delete'
    std::optional<int64_t> move_target_folder_id;
    std::string message_id;
};

// What a header pull gives the store for one folder in ONE transaction: the rows and the cursor.
struct PulledMessage {
    uint32_t uid = 0;
    std::string message_id, in_reply_to, references;
    std::string from_name, from_addr, to_text, cc_text, subject;
    int64_t date_utc = 0;
    std::optional<int64_t> internal_date;
    int64_t size = 0;
    uint32_t flags = 0;
};

struct PullBatch {
    std::vector<PulledMessage> messages;
    std::optional<int64_t> uidvalidity, uidnext, highestmodseq, window_low_uid;
    std::optional<bool> older_on_server;
};

struct SyncState {
    std::optional<int64_t> last_started, last_finished, last_success;
    std::string outcome = "never";   // never, running, success, partial, failed, offline
    std::string problem_key;         // first problem, "" = none
    std::string problem_area;        // "", mail, calendar, outbox, contacts
    std::string counts_json = "{}";
    std::optional<int64_t> clock_skew_s;
};

}  // namespace pbo
