#pragma once
// Drafts and the outbox as the store keeps them (data-model.md: draft, outbox).

#include <cstdint>
#include <string>
#include <vector>

namespace pbo {

struct Draft {
    int64_t id = 0;                         // 0 = not saved yet
    std::string kind = "new";               // new | reply | reply_all | forward
    std::string message_id;                 // "<...>", fixed at creation: the identity for mirroring and sending
    int64_t reply_to_message = 0;           // the message this answers (0 = none)
    std::string in_reply_to, refs;
    std::string to_text, cc_text, bcc_text; // as typed
    std::string subject, body;
    int64_t created = 0, updated = 0;
    std::string content_hash;               // over everything the user can edit; computed by the store
    std::string server_state = "none";      // none | pending | synced | conflict | pending_delete
    int64_t server_uid = 0;                 // UID of the mirrored copy in the Drafts folder (0 = none)
    std::string server_hash;                // content hash at the last mirror
    int64_t conflict_of = 0;
    bool queued = false;                    // has an outbox row (read only)
};

// What the caller hands to Store::queue_draft: the message built once, from the draft as it was when Send was tapped.
struct OutboxRequest {
    std::string expected_hash;              // the draft's content_hash the message was built from
    std::string envelope_from;
    std::vector<std::string> recipients;    // including Bcc
    std::string subject_preview;
    std::string raw_message;                // the complete RFC 5322 bytes
};

struct OutboxItem {
    int64_t id = 0;
    int64_t draft_id = 0;                   // 0 = the draft is gone
    std::string message_id;
    std::string envelope_from;
    std::vector<std::string> recipients;
    std::string subject_preview;
    std::string raw_message;
    std::string state;                      // queued | sending | sent_copy_pending | failed | uncertain
    std::string error_key, error_detail;
    int attempts = 0;
    int64_t queued_at = 0, state_at = 0, sent_at = 0;
};

// How a send attempt ended (SendOutcome of the transport, as the store records it)
struct SendFinish {
    // Requeue: a failure that provably happened before any connection existed (no network, server unreachable): nothing was sent, so the
    // message simply waits for the next sync (sending -> queued); the reason stays visible as the last error
    enum class Kind { Accepted, Failed, Uncertain, Requeue } kind = Kind::Failed;
    std::string error_key, error_detail;    // for Failed and Uncertain: a reason key and a short text without addresses or content
};

}  // namespace pbo
