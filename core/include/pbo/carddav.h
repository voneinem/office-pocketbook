#pragma once
// CardDAV client over the HTTP seam (specs/002-server-contacts/contracts/carddav-vcard-subset.md section 1). Basic authentication over https only
// (the transport refuses anything else); user names and passwords are never part of a URL; a server answer that names a resource on another host
// is ignored; every XML body goes through xml_min. It shares the plumbing of the CalDAV client (dav_common.h). Server behaviour is UNVERIFIED
// until a live run.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "pbo/dav_common.h"
#include "pbo/http_transport.h"
#include "pbo/types.h"

namespace pbo {

struct AddressBookInfo {
    std::string href;                  // path on the server, e.g. "/dav/addressbooks/alice/contacts/"
    std::string name;
    std::string ctag, sync_token;      // as the server reported them at discovery
    bool writable = false;             // privileges include write-content or all
    bool supports_sync_report = false;
};

struct CardRef {
    std::string href, etag;
};

struct CardChangeSet {
    std::vector<CardRef> changed;      // new or changed cards (all cards when `full`)
    std::vector<std::string> removed;  // hrefs the server reports as gone (only when !full)
    std::string new_sync_token, new_ctag;
    bool full = false;                 // the list is complete: the caller removes what is not in it
    bool unchanged = false;            // ctag equal to the stored one: nothing to do
};

struct CardResource {
    std::string href, etag, vcf;
};

struct CardFetchResult {
    std::vector<CardResource> cards;
    std::vector<std::string> gone;     // 404 inside the answer
    std::vector<std::string> failed;   // a per-card error (5xx) or a card that was too large: retried at the next sync
};

struct CardWriteResult {
    enum class Kind { Created, Updated, PreconditionFailed, NotFound, Refused, Failed } kind = Kind::Failed;
    std::string etag;                  // the new ETag when known ("" otherwise: the next sync learns it)
    Error error;                       // set for Refused (keys "read_only", "too_large", "storage_full_server", "refused") and Failed (network, auth, server)
};

// The property list sent as card:address-data: everything the app shows or edits, so that photos and other heavy properties are left out by
// a server that honours the list (research R-05).
extern const char* const kCardDataProperties;

// The path of a new card in its book: "<book href>/<32 random hex digits>.vcf". Never made from a name, a number or an address.
std::string new_card_href(const std::string& book_href, Random& random);

class CardDavClient {
public:
    // base_url: what the user entered for the calendar account (a server address, a /dav/ address, or a calendar home). Must be https.
    CardDavClient(HttpTransport& http, std::string base_url, std::string username, std::string password);

    // current-user-principal -> addressbook-home-set -> address books. Error key "no_addressbook" (class Protocol) when the server names no
    // address book home or the home holds none: not a failure of the sync, the Contacts view explains it.
    Result<std::vector<AddressBookInfo>> discover();

    // What changed in an address book. `stored` is the sync token (books with sync-collection) or the ctag. There is no time window: a full
    // list holds every card.
    Result<CardChangeSet> changes(const AddressBookInfo& book, const std::string& stored);

    // addressbook-multiget in batches of 30 cards (at most 4 MiB per answer; a batch that is too big is split)
    Result<CardFetchResult> fetch(const AddressBookInfo& book, const std::vector<std::string>& hrefs);

    // One full card and its ETag (the card is not stripped); at most 1 MiB. Errors: not_found, too_large, the transport keys.
    Result<CardResource> get(const std::string& href);

    // The time the server reported (HTTP Date header) with its latest answer; used to notice a wrong clock on the reader (clock_check.h)
    std::optional<int64_t> last_server_time() const { return dav_.last_server_time(); }

    // Conditional writes (Principle III): if_match null = create (If-None-Match: *), else If-Match: <etag>
    CardWriteResult put(const std::string& href, const std::string& vcf, const std::string* if_match);
    CardWriteResult remove(const std::string& href, const std::string& if_match);

private:
    DavSession dav_;
};

}  // namespace pbo
