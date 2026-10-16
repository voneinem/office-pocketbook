#pragma once
// Mail pull steps (research R-06, R-18). Each step talks to the server through ImapTransport and writes through
// Store; a batch is committed together with its cursor, so an interrupted run resumes without gaps or duplicates.

#include <map>
#include <string>

#include "pbo/imap_transport.h"
#include "pbo/store.h"
#include "pbo/uidvalidity_guard.h"
#include "pbo/types.h"

namespace pbo {

struct MailCounts {
    int folders = 0;
    int new_messages = 0;
    int bodies = 0;   // message bodies fetched
    int removed = 0;        // messages that are gone from the server (deleted, or kept with a mark when local work is pending)
    int flags_updated = 0;  // messages whose flags changed on the server
};

// LIST "" "*" -> Store::upsert_folders. Error "protocol_error" when the answer cannot be used.
Error sync_folder_list(ImapTransport& imap, Store& store, MailCounts* counts = nullptr);

// One folder, pulled completely (research R-04, R-06): the folder's STATUS (the UIDVALIDITY guard's answer, before anything is
// written), the window (UID SEARCH SINCE), messages that disappeared from the server (removed; ones with pending local work are
// kept and marked "gone_on_server"), changed flags (CONDSTORE: only what changed since the stored HIGHESTMODSEQ), and the headers
// of the messages the store does not have, at most 50 UIDs per transaction together with the cursor. A changed UIDVALIDITY
// starts a rebuild (Store::begin_rebuild) that is finished after a clean pull; an unreadable STATUS writes nothing.
// `cancel` is checked between commands.
Error pull_folder(ImapTransport& imap, Store& store, const Folder& folder, int window_days, Clock& clock, Cancel& cancel,
                  UidvalidityGuard& guard, MailCounts* counts = nullptr);

// Older mail on demand: the next `count` (newest first) messages of `folder` below the ones the store has, headers and bodies. The
// window setting is not changed; `older_on_server` is cleared when nothing older is left. The folder's UIDVALIDITY is checked first
// (a reset writes nothing and returns Error{Protocol,"uidvalidity_changed"}). Messages are ordered by UID, which is the arrival
// order on every server this app is meant for.
Error fetch_older(ImapTransport& imap, Store& store, const Folder& folder, int count, Cancel& cancel, UidvalidityGuard& guard,
                  MailCounts* counts = nullptr);

// The readable part of the messages of `folder` that have only their header (research R-06): BODYSTRUCTURE, then ONLY the
// chosen text part with BODY.PEEK and a 2 MiB limit, attachments are listed and not downloaded; an unusable structure falls
// back to the first 256 KiB of the whole message. Newest first, at most 1000 per call, every message in its own transaction,
// so an interrupted run resumes. `known_structures` (UID -> BODYSTRUCTURE text, optional) saves a command per message.
Error fetch_bodies(ImapTransport& imap, Store& store, const Folder& folder, Cancel& cancel, MailCounts* counts = nullptr,
                   const std::map<uint32_t, std::string>* known_structures = nullptr);

}  // namespace pbo
