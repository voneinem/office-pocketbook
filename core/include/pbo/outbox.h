#pragma once
// The outbox step of a synchronisation (research R-08, R-09; FR-014 .. FR-016, Principle III): sends the queued messages, each at most once,
// and files the Sent copy of the accepted ones.
//
// What this step guarantees:
//  - a message is sent only after its row moved queued -> sending in a committed transaction (nobody else can send it then);
//  - the outcome rule of SmtpTransport::send decides the next state: Accepted -> sent_copy_pending, CertainFailure -> failed (or back to
//    queued when no connection ever existed), Unknown -> uncertain; uncertain rows are NEVER sent again by this step;
//  - the Sent copy is APPENDed only when the account does not file sent mail itself, and only after `UID SEARCH HEADER Message-ID` found
//    no copy (an APPEND whose answer was lost is not repeated); a failed APPEND is retried alone, with no further SMTP session.

#include <string>

#include "pbo/credentials_file.h"
#include "pbo/imap_transport.h"
#include "pbo/smtp_transport.h"
#include "pbo/store.h"
#include "pbo/types.h"
#include "pbo/uidvalidity_guard.h"

namespace pbo {

struct OutboxDeps {
    Store& store;
    SmtpTransport& smtp;
    ImapTransport& imap;
    Clock& clock;
    Cancel& cancel;
    UidvalidityGuard& guard;   // the guard of this sync: the flag of the Sent copy is a UID-addressed write
};

struct OutboxCounts {
    int sent = 0;            // accepted by the server
    int failed = 0;          // certain failures that need the user (refused recipient, wrong password, ...)
    int uncertain = 0;       // outcome unknown: shown as "may have been sent"
    int requeued = 0;        // no connection: waits for the next sync
    int copies = 0;          // Sent copies filed (or found) and rows completed
    int copies_failed = 0;   // Sent copies that could not be filed yet
};

// Returns an error only for a problem of the store; everything that happens to a message is recorded on its row and counted.
Error run_outbox(OutboxDeps& deps, const Credentials& credentials, OutboxCounts* counts);

}  // namespace pbo
