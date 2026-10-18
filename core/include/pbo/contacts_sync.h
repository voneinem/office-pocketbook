#pragma once
// The contacts side of a synchronisation (specs/002-server-contacts/research.md R-04, R-10): discovery of the address books of the calendar server
// account, change detection, fetching, and the push of what the user changed on the reader. Every step commits data and cursor together; an
// interrupted run converges when it is run again. Local work (contacts with sync_state not 'synced') is never touched by the pull.

#include <string>
#include <vector>

#include "pbo/carddav.h"
#include "pbo/http_transport.h"
#include "pbo/store.h"
#include "pbo/types.h"

namespace pbo {

struct ContactsSyncDeps {
    Store& store;
    HttpTransport& http;
    Clock& clock;
    Cancel& cancel;
};

struct ContactsCounts {
    int books = 0;            // shown address books looked at
    int changed = 0;          // contacts written
    int removed = 0;          // contacts removed because the server removed them
    int unreadable = 0;       // cards stored as unreadable (FR-026)
    bool no_addressbook = false;   // the server names no address book: not a failure
};

struct ContactsPushCounts {
    int created = 0, updated = 0, deleted = 0;
    int conflicts = 0;         // 412 or a changed card: both versions kept, the user decides
    int server_deleted = 0;    // the contact or its address book is gone on the server: the edited version is kept and marked
    int refused = 0;           // 403 / 413 / 507: the change stays waiting, marked failed
};

// The pull: the address books of the calendar server account (the same address, user and password) and the cards of the SHOWN books. Without a
// calendar server account it does nothing. A server without address books gives counts->no_addressbook and no error. Errors: the first problem
// ("auth_failed", "server_unreachable", ...); a problem in one book does not stop the others unless the connection or the login is the problem.
Error pull_contacts(ContactsSyncDeps& deps, const std::string& calendar_password, ContactsCounts* counts);

// The push: every contact with local work (Store::pending_contacts) goes to its address book with a conditional request. The mark clears only when
// the server confirmed. Errors: the first problem; a connection or login problem stops the push.
Error push_contacts(ContactsSyncDeps& deps, const std::string& calendar_password, ContactsPushCounts* counts);

}  // namespace pbo
