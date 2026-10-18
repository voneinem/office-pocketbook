// The contacts side of a synchronisation (contacts_sync.h): see specs/002-server-contacts/research.md R-04, R-10 and data-model.md.

#include "pbo/contacts_sync.h"

#include <functional>
#include <map>
#include <set>

#include "pbo/clock_check.h"
#include "pbo/log.h"
#include "pbo/vcard.h"

namespace pbo {

namespace {

bool stop_class(const Error& e) {
    return e.cls == ErrorClass::Cancelled || e.cls == ErrorClass::Network || e.cls == ErrorClass::Tls || e.cls == ErrorClass::Auth || e.cls == ErrorClass::Storage;
}

// The server's card differs from the one the reader's change is based on: both versions are kept, the user decides (FR-021).
void keep_both_versions(ContactsSyncDeps& d, const PendingContact& pe, const CardResource& theirs, ContactsPushCounts* counts) {
    d.store.contact_conflicted(pe.id, theirs.vcf, theirs.etag, pe.href);
    ++counts->conflicts;
    log::event("contacts.conflict", {log::count("contacts", 1)});
}

// One waiting change of an existing contact. A card the reader holds only in part (everything from the server) is read in full first: the write
// is the reader's edit merged into the server's own card, conditional on the ETag of that look (research R-05, R-06). Returns false when the
// push has to stop (connection, login, storage).
bool push_update(ContactsSyncDeps& d, CardDavClient& client, const PendingContact& pe, ContactsPushCounts* counts, const std::function<void(const Error&)>& note) {
    std::string body = pe.vcf;
    std::string if_match = pe.etag;
    if (pe.partial || pe.etag.empty()) {
        auto got = client.get(pe.href);
        if (!got.ok()) {
            if (got.error.key == "not_found") {   // removed on the server: the edited version is kept, marked
                d.store.contact_gone_on_server(pe.id);
                ++counts->server_deleted;
                return true;
            }
            note(got.error);
            return !stop_class(got.error);
        }
        if (!pe.etag.empty() && got.value.etag != pe.etag) {   // changed on the server since the reader's version: nothing is written
            keep_both_versions(d, pe, got.value, counts);
            return true;
        }
        body = merge_into_full_card(pe.vcf, got.value.vcf);
        if_match = got.value.etag;
    }
    if (body.size() > kMaxCardBytes) {
        d.store.contact_refused(pe.id, "too_large");
        ++counts->refused;
        note(make_error(ErrorClass::Server, "too_large", "the contact is too big"));
        return true;
    }
    const CardWriteResult w = client.put(pe.href, body, if_match.empty() ? nullptr : &if_match);
    switch (w.kind) {
        case CardWriteResult::Kind::Created:
        case CardWriteResult::Kind::Updated:
            d.store.contact_pushed(pe.id, pe.vcf, pe.href, w.etag);
            ++counts->updated;
            return true;
        case CardWriteResult::Kind::PreconditionFailed: {   // changed between the look and the write
            auto got = client.get(pe.href);
            if (!got.ok()) {
                if (got.error.key == "not_found") {
                    d.store.contact_gone_on_server(pe.id);
                    ++counts->server_deleted;
                    return true;
                }
                note(got.error);
                return !stop_class(got.error);
            }
            keep_both_versions(d, pe, got.value, counts);
            return true;
        }
        case CardWriteResult::Kind::NotFound:
            d.store.contact_gone_on_server(pe.id);
            ++counts->server_deleted;
            return true;
        case CardWriteResult::Kind::Refused:
            d.store.contact_refused(pe.id, w.error.key);
            ++counts->refused;
            note(w.error);
            return true;
        case CardWriteResult::Kind::Failed:
            note(w.error);
            return !stop_class(w.error);
    }
    return true;
}

// One waiting deletion. The card goes only against the ETag the reader knows: a card that changed on the server is kept as a conflict (FR-021).
bool push_delete(ContactsSyncDeps& d, CardDavClient& client, const PendingContact& pe, ContactsPushCounts* counts, const std::function<void(const Error&)>& note) {
    auto changed_on_server = [&]() -> bool {   // look at the card: gone = done, else both versions are kept
        auto got = client.get(pe.href);
        if (!got.ok()) {
            if (got.error.key == "not_found") {
                d.store.contact_removed_on_server(pe.id);
                ++counts->deleted;
                return true;
            }
            note(got.error);
            return !stop_class(got.error);
        }
        keep_both_versions(d, pe, got.value, counts);
        return true;
    };
    if (pe.etag.empty()) return changed_on_server();   // the reader never learned the version: it cannot say which one it deletes
    const CardWriteResult w = client.remove(pe.href, pe.etag);
    switch (w.kind) {
        case CardWriteResult::Kind::Created:
        case CardWriteResult::Kind::Updated:
        case CardWriteResult::Kind::NotFound:   // already gone: the goal is reached
            d.store.contact_removed_on_server(pe.id);
            ++counts->deleted;
            return true;
        case CardWriteResult::Kind::PreconditionFailed:
            return changed_on_server();
        case CardWriteResult::Kind::Refused:
            d.store.contact_refused(pe.id, w.error.key);
            ++counts->refused;
            note(w.error);
            return true;
        case CardWriteResult::Kind::Failed:
            note(w.error);
            return !stop_class(w.error);
    }
    return true;
}

}  // namespace

Error pull_contacts(ContactsSyncDeps& d, const std::string& calendar_password, ContactsCounts* counts) {
    ContactsCounts local;
    if (!counts) counts = &local;
    const auto account = d.store.calendar_account();
    if (!account || account->caldav_url.empty()) return {};
    CardDavClient client(d.http, account->caldav_url, account->username, calendar_password);
    auto found = client.discover();
    if (!found.ok()) {
        if (found.error.key == "no_addressbook") {   // nothing to look at; what the reader holds stays as it is
            counts->no_addressbook = true;
            return {};
        }
        return found.error;
    }
    if (const auto t = client.last_server_time()) d.store.set_clock_skew(clock_skew(d.clock.now(), *t));   // a wrong clock is noticed here, for free

    std::set<std::string> present;
    for (const AddressBookInfo& info : found.value) {
        AddressBook b;
        b.href = info.href;
        b.name = info.name;
        b.writable = info.writable;
        auto id = d.store.upsert_addressbook(b);
        if (!id.ok()) return id.error;
        present.insert(info.href);
    }
    if (Error e = d.store.mark_addressbooks_present(present); !e.ok()) return e;

    const int64_t now = d.clock.now();
    Error first;
    for (const AddressBookInfo& info : found.value) {
        if (d.cancel.requested()) return make_error(ErrorClass::Cancelled, "cancelled", "the sync was cancelled");
        AddressBook stored;
        for (const AddressBook& b : d.store.addressbooks())
            if (b.href == info.href) stored = b;
        if (stored.id == 0 || !stored.shown) continue;   // a hidden book is not downloaded; what is stored stays
        ++counts->books;

        const std::string cursor = info.supports_sync_report ? stored.sync_token : stored.ctag;
        auto changes = client.changes(info, cursor);
        if (!changes.ok()) {
            if (first.ok()) first = changes.error;
            if (stop_class(changes.error)) return first;
            continue;
        }
        ContactPullBatch batch;
        batch.synced_at = now;
        std::map<std::string, StoredContactRef> known;
        bool refetch = false;   // a card the user gave up changing: downloaded again whatever the cursor says
        for (const StoredContactRef& r : d.store.contact_refs(stored.id)) {
            known[r.href] = r;
            if (r.refetch && r.sync_state == "synced") refetch = true;
        }
        if (changes.value.unchanged && !refetch) {
            batch.ctag = changes.value.new_ctag;
            if (Error e = d.store.apply_contact_pull(stored.id, batch); !e.ok()) return e;
            continue;
        }

        std::vector<std::string> to_fetch;
        std::set<std::string> queued;
        for (const CardRef& r : changes.value.changed) {
            batch.listed_hrefs.push_back(r.href);
            auto k = known.find(r.href);
            if (k != known.end() && k->second.etag == r.etag && !r.etag.empty() && k->second.sync_state == "synced" && !k->second.refetch) continue;   // already current
            to_fetch.push_back(r.href);
            queued.insert(r.href);
        }
        for (const auto& k : known)
            if (k.second.refetch && k.second.sync_state == "synced" && !queued.count(k.first)) {
                to_fetch.push_back(k.first);
                if (changes.value.full) batch.listed_hrefs.push_back(k.first);
            }
        batch.full = changes.value.full;
        batch.removed_hrefs = changes.value.removed;
        counts->removed += static_cast<int>(changes.value.removed.size());

        bool incomplete = false;
        if (!to_fetch.empty()) {
            auto fetched = client.fetch(info, to_fetch);
            if (!fetched.ok()) {
                if (first.ok()) first = fetched.error;
                if (stop_class(fetched.error)) return first;
                continue;
            }
            for (const CardResource& r : fetched.value.cards) {
                ContactUpsert u = contact_upsert_from_card(r.href, r.etag, r.vcf);
                if (u.unreadable) ++counts->unreadable;   // FR-026: stored as unreadable, the rest is applied
                batch.upserts.push_back(std::move(u));
            }
            for (const std::string& g : fetched.value.gone) batch.removed_hrefs.push_back(g);
            for (const std::string& f : fetched.value.failed) {
                batch.listed_hrefs.push_back(f);   // keep what is stored; it is fetched again at the next sync
                incomplete = true;
            }
        }
        counts->changed += static_cast<int>(batch.upserts.size());
        if (!incomplete) {   // the cursor only moves when everything it covers was fetched
            batch.ctag = changes.value.new_ctag;
            if (info.supports_sync_report) batch.sync_token = changes.value.new_sync_token;   // a book without sync-collection is followed by its ctag
        }
        if (Error e = d.store.apply_contact_pull(stored.id, batch); !e.ok()) return e;
    }
    log::event("contacts.pull", {log::count("books", counts->books), log::count("changed", counts->changed), log::count("removed", counts->removed),
                                 log::count("unreadable", counts->unreadable)});
    return first;
}

Error push_contacts(ContactsSyncDeps& d, const std::string& calendar_password, ContactsPushCounts* counts) {
    ContactsPushCounts local;
    if (!counts) counts = &local;
    const std::vector<PendingContact> pending = d.store.pending_contacts();
    if (pending.empty()) return {};
    const auto account = d.store.calendar_account();
    if (!account || account->caldav_url.empty()) return {};
    CardDavClient client(d.http, account->caldav_url, account->username, calendar_password);

    Error first;
    auto note = [&](const Error& e) {
        if (first.ok()) first = e;
    };

    for (const PendingContact& pe : pending) {
        if (d.cancel.requested()) return make_error(ErrorClass::Cancelled, "cancelled", "the sync was cancelled");
        if (!pe.addressbook_present) continue;               // the book is gone: the store marked what it held
        if (!pe.addressbook_writable) {
            d.store.contact_refused(pe.id, "read_only");
            ++counts->refused;
            note(make_error(ErrorClass::Server, "read_only", "the address book does not accept changes"));
            continue;
        }
        if (pe.sync_state == "pending_update") {
            if (!push_update(d, client, pe, counts, note)) return first;
            continue;
        }
        if (pe.sync_state == "pending_delete") {
            if (!push_delete(d, client, pe, counts, note)) return first;
            continue;
        }
        const CardWriteResult w = client.put(pe.href, pe.vcf, nullptr);
        switch (w.kind) {
            case CardWriteResult::Kind::Created:
            case CardWriteResult::Kind::Updated:
                d.store.contact_pushed(pe.id, pe.vcf, pe.href, w.etag);
                ++counts->created;
                break;
            case CardWriteResult::Kind::PreconditionFailed: {
                // Something is at our path already: an earlier request of ours was accepted and its answer lost. Look at it.
                auto got = client.get(pe.href);
                if (!got.ok()) {
                    note(got.error);
                    if (stop_class(got.error)) return first;
                    break;
                }
                const ParsedContact theirs = parse_vcard(got.value.vcf);
                if (!theirs.readable || theirs.uid != pe.uid) {   // not our card (a path made of 128 random bits cannot clash by chance)
                    note(make_error(ErrorClass::Protocol, "protocol_error", "another card is at the path of a new contact"));
                    break;
                }
                // ours: adopt its ETag, no second card. If the reader's text moved on since (the user edited meanwhile) the contact stays a waiting change
                // against that ETag; an empty text never equals the stored one.
                d.store.contact_pushed(pe.id, same_contact_content(pe.vcf, got.value.vcf) ? pe.vcf : std::string(), pe.href, got.value.etag);
                ++counts->created;
                break;
            }
            case CardWriteResult::Kind::NotFound:
                note(make_error(ErrorClass::Server, "not_found", "the address book is not on the server"));
                break;
            case CardWriteResult::Kind::Refused:
                d.store.contact_refused(pe.id, w.error.key);
                ++counts->refused;
                note(w.error);
                break;
            case CardWriteResult::Kind::Failed:
                note(w.error);
                if (stop_class(w.error)) return first;
                break;
        }
    }
    log::event("contacts.push", {log::count("created", counts->created), log::count("updated", counts->updated), log::count("deleted", counts->deleted),
                                 log::count("conflicts", counts->conflicts), log::count("server_deleted", counts->server_deleted), log::count("refused", counts->refused)});
    return first;
}

}  // namespace pbo
