#pragma once
// The user's actions on contacts (specs/002-server-contacts/contracts/core-interfaces.md): clean up and check what the user typed, create a contact
// on the reader. Nothing here talks to the network; the push (contacts_sync.h) sends what is waiting. Contacts of a read-only address book,
// unreadable contacts, contacts with two versions and contacts waiting for their deletion cannot be changed.

#include <string>
#include <vector>

#include "pbo/account_input.h"
#include "pbo/contact_types.h"
#include "pbo/store.h"
#include "pbo/types.h"

namespace pbo {

struct ContactEditDeps {
    Store& store;
    Clock& clock;
    Random& random;
};

// Typed text made fit to store: control characters removed (a note keeps its line breaks and tabs, CR LF and CR become LF), a line break in a
// single-line field becomes a space, runs of spaces collapse, the ends are trimmed, text that is not UTF-8 is replaced; entries with nothing in
// them are dropped. Entries marked `locked` are not touched (they are kept byte for byte).
ContactEdit normalize_contact_edit(const ContactEdit& edit);

// Empty = valid. Each error names its field as in the form: "given", "family", "org", "phones[1]", "emails[0]", "addresses[2]", "note", with the key
// "empty" (no name, organization, phone number or email address: FR-013), "email_invalid", "too_long", "too_many" (field "phones", "emails" or
// "addresses") or "locked_changed". The list follows the order of the form, so the first error is the first field to correct.
// `original` is the contact as it was loaded (nullptr for a new one): the limits and the email check apply to values the user adds or changes only;
// an entry or note that is longer than the limit and untouched passes, a locked entry whose value changed is refused.
std::vector<FieldError> validate_contact_edit(const ContactEdit& edit, const ContactEdit* original = nullptr);

// "" when the contact can be changed, else why not: "read_only" (the address book), "unreadable", "in_conflict", "delete_pending", "book_gone".
std::string contact_edit_block_reason(const ContactView& contact);

// A new contact in an address book. The edit is cleaned up and checked first; the card gets a random UID and a random path in the book (stored
// with the contact, so a lost answer to the first request is recognised later). Errors: the key of the first validation error with the field in
// `detail`, "not_found" (no such book, or it is gone), "read_only", "card_too_large", "write_failed".
Result<int64_t> create_contact(ContactEditDeps& deps, int64_t addressbook_id, const ContactEdit& edit);

// A changed contact (research R-06): the edit is cleaned up and checked against the stored card (an entry that was too long to edit stays as it is, or is
// refused with "locked_changed" when it was altered), written into the stored card (only what differs is rewritten) and stored in ONE transaction;
// the push sends it. An edit that changes nothing writes nothing (*changed false). Errors: the key of the first validation error with the field in
// `detail`, "not_found", the reason of contact_edit_block_reason ("unreadable", "book_gone", "read_only", "in_conflict", "delete_pending"),
// "card_too_large", "write_failed". Returns the contact id.
Result<int64_t> change_contact(ContactEditDeps& deps, int64_t contact_id, const ContactEdit& edit, bool* changed = nullptr);

// The user decides a conflict (research R-09). KeepMine: the reader's version is sent again against the server's ETag. KeepServer: the reader's version
// is replaced. KeepBoth: the server's card stays and the reader's version becomes a new contact (new UID, new path). Errors "not_found",
// "not_in_conflict", "unreadable_server_version".
Error resolve_contact_conflict(ContactEditDeps& deps, int64_t contact_id, ContactChoice choice);

// The user deleted the contact (the page asked first): it leaves the list at once and the next sync deletes it on the server; until the server confirmed
// it, undo brings it back as it was (FR-019). Errors "not_found", "read_only", "in_conflict" (undo: "not_found", "nothing_to_undo").
Error delete_contact(Store& store, int64_t contact_id);
Error undo_delete_contact(Store& store, int64_t contact_id);

// A contact the server removed: send it again as a new card under a fresh path ("not_found", "not_server_deleted", "book_gone", "read_only"), or
// remove it for good (the caller asked the user first; "not_found", "not_server_deleted").
Error recreate_contact(ContactEditDeps& deps, int64_t contact_id);
Error discard_server_deleted_contact(Store& store, int64_t contact_id);

// The contact is sent again at the next sync: the failure mark goes. Error "not_found".
Error retry_contact(Store& store, int64_t contact_id);
// The user gives up a change the server did not take: a new contact is removed; a changed one returns to the server's version (the next sync
// downloads it). Errors "not_found", "not_failed" (nothing to discard: the contact has no waiting change).
Error discard_contact_change(Store& store, int64_t contact_id);

}  // namespace pbo
