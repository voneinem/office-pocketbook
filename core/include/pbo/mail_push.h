#pragma once
// What the user did with mail on the reader goes to the server (US5; constitution Principle III, research R-05..R-07, tasks I8/K1/K2):
//  - Flags: only the difference between what the server last showed and what the user wants is sent, as UID STORE +FLAGS.SILENT / -FLAGS.SILENT
//    (never a full FLAGS replace, so a flag someone else set is never removed); the result is read back (UID FETCH FLAGS) and merged: the bits the
//    user changed win, everything else is the server's.
//  - Move: UID MOVE when the server has MOVE; else UID COPY, then \Deleted on the original, then UID EXPUNGE <uid> when the server has UIDPLUS (the
//    original is only touched after the copy succeeded). A bare EXPUNGE is never sent.
//  - Permanent delete: \Deleted + UID EXPUNGE <uid> (UIDPLUS), else \Deleted only.
//  - Every folder with work is checked with the UIDVALIDITY guard first; a folder that was reset gets no UID-addressed command at all (its work is
//    re-applied after the next pull has matched the messages by Message-ID, or reported as "gone on the server").
//  - A message that is no longer on the server is reported (local_error), never dropped silently.

#include "pbo/credentials_file.h"
#include "pbo/imap_transport.h"
#include "pbo/store.h"
#include "pbo/types.h"
#include "pbo/uidvalidity_guard.h"

namespace pbo {

struct MessagePushDeps {
    Store& store;
    ImapTransport& imap;
    Cancel& cancel;
    UidvalidityGuard& guard;
};

struct MessagePushCounts {
    int flags = 0;      // flag changes sent
    int moved = 0;
    int deleted = 0;
    int errors = 0;     // changes the server did not accept or whose message is gone (marked on the message)
    int skipped = 0;    // folders left alone because their UIDVALIDITY changed or could not be checked
};

// Connects by itself, only when something is waiting. Returns an error only for the connection ("auth_failed", "server_unreachable", ...) or the
// store; what happens to single messages is recorded on them.
Error push_message_changes(MessagePushDeps& deps, const Credentials& credentials, MessagePushCounts* counts);

}  // namespace pbo
