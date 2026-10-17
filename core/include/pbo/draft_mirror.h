#pragma once
// Drafts on the server (FR-013, Principle III, contracts/imap-smtp-subset.md section 6): every draft of the app is mirrored into the account's
// Drafts folder so that it can be continued elsewhere.
//
//  - A new or changed draft is APPENDed FIRST; only after the server confirmed it is the superseded copy removed (\Deleted + UID EXPUNGE of
//    that one UID). An interruption between the two leaves two copies, never none. No bare EXPUNGE is ever sent.
//  - Every UID-addressed command goes through the UIDVALIDITY guard; a folder that was reset gets nothing from this step.
//  - A copy that was changed elsewhere since the last mirror is not overwritten: the server's version becomes a second draft (two drafts).
//  - Messages in the Drafts folder that the app did not put there are ordinary messages and are never touched.

#include <string>

#include "pbo/credentials_file.h"
#include "pbo/imap_transport.h"
#include "pbo/store.h"
#include "pbo/types.h"
#include "pbo/uidvalidity_guard.h"

namespace pbo {

struct DraftMirrorDeps {
    Store& store;
    ImapTransport& imap;
    Clock& clock;
    Cancel& cancel;
    Random& random;
    UidvalidityGuard& guard;
};

struct DraftMirrorCounts {
    int mirrored = 0;    // drafts uploaded
    int removed = 0;     // server copies removed (deleted or sent drafts, superseded copies)
    int conflicts = 0;   // drafts split into two
    int failed = 0;      // drafts that could not be mirrored now
};

// Returns an error only for a problem of the store; the rest is recorded in the counts and tried again at the next sync.
Error run_draft_mirror(DraftMirrorDeps& deps, const Credentials& credentials, DraftMirrorCounts* counts);

}  // namespace pbo
