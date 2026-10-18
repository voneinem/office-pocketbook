#pragma once
// Contact data as the store keeps and hands it out (specs/002-server-contacts/data-model.md: addressbook, contact, contact_email,
// contact_conflict) and as the editor works with it. Types only; the vCard code is in vcard.h, the user actions in contact_edit.h.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace pbo {

// ---- limits (research R-06, R-14; contracts/carddav-vcard-subset.md section 3) ----
constexpr size_t kMaxCardBytes = 256u << 10;        // a card above this is unreadable (after heavy properties were removed)
constexpr size_t kMaxCardLines = 5000;
constexpr size_t kMaxCardLineBytes = 64u << 10;     // a property value above this is removed at ingest
constexpr size_t kMaxCardEntries = 100;             // phones, emails or addresses read from one card
constexpr size_t kMaxNamePart = 200;                // input limits of the editor (apply to values the user adds or changes only)
constexpr size_t kMaxOrg = 200;
constexpr size_t kMaxPhone = 64;
constexpr size_t kMaxEmail = 254;
constexpr size_t kMaxAddressPart = 200;
constexpr size_t kMaxNote = 8u << 10;
constexpr size_t kMaxEntriesPerKind = 20;
constexpr size_t kMaxWrittenCard = 64u << 10;

// ---- the editor's view of a contact ----
struct LabeledValue {
    std::string label;         // "mobile" | "home" | "work" | "other" | the card's own text (fax, pager, "Mother", ...)
    std::string value;
    int orig_index = -1;       // position among the entries of this kind in the stored card; -1 for an entry the user added
    bool locked = false;       // longer than the input limit: shown read-only, kept byte for byte, removable (data-model.md)
};

struct PostalAddress {
    std::string label, street, city, region, postcode, country;
    int orig_index = -1;
    bool locked = false;
};

struct ContactEdit {
    std::string given, family, org;
    std::vector<LabeledValue> phones, emails;
    std::vector<PostalAddress> addresses;
    std::string note;
    bool note_locked = false;  // the stored note is longer than the limit: the form shows it read-only; removing it is allowed
};

// ---- a card read from text (vcard.h) ----
struct VCardLine {
    std::string raw;           // the physical line(s) exactly as in the text, folds and line end included
    std::string group;         // "item1" of "item1.TEL", as written
    std::string name;          // upper case; "" for a line that is not a property (blank, garbage)
    std::string params;        // everything between the name and the first unquoted ':' (as written, without the leading ';')
    std::string value;         // the unfolded text after the ':' (escapes not removed)
};

struct ParsedContact {
    bool readable = false;
    std::string problem;       // "" | "too_large" | "malformed" | "multiple_cards" | "no_uid" (key; never content)
    std::string version, uid, fn, family, given, additional, prefix, suffix, org, note;
    std::vector<LabeledValue> phones, emails;
    std::vector<PostalAddress> addresses;
    // the card as lines (every logical line of the text, in order) and where the properties are
    std::vector<VCardLine> lines;
    int begin_line = -1, end_line = -1;
    int fn_line = -1, n_line = -1, org_line = -1, note_line = -1, rev_line = -1, version_line = -1;
    std::vector<int> phone_lines, email_lines, address_lines;   // line index per entry of phones / emails / addresses
};

// ---- what the store derives from a card (one function writes it: contact_derive) ----
struct ContactEmailRow {
    std::string addr, addr_key, label;
};

struct ContactFields {
    std::string display_name, family, given, org, subtitle, sort_key, search_text;
    std::vector<ContactEmailRow> emails;
};

// ---- address books and contact rows ----
struct AddressBook {
    int64_t id = 0;
    std::string href, name;
    bool writable = false;
    bool shown = true;
    std::string ctag, sync_token;
    bool present_on_server = true;
    int64_t last_sync_at = 0;
};

struct ContactListItem {
    int64_t id = 0;
    std::string display_name, subtitle;
    std::string sync_state, error_key;   // sync_state: 'synced' or one of the pending / conflict states
    int64_t addressbook_id = 0;
    std::string addressbook_name;
    bool unreadable = false;
};

struct ContactView {
    int64_t id = 0;
    int64_t addressbook_id = 0;
    std::string addressbook_name;
    bool addressbook_writable = false, addressbook_present = true, addressbook_shown = true;
    std::string href, etag, uid, vcf;
    bool partial = false, unreadable = false;
    std::string display_name, family, given, org;
    std::string sync_state, undo_state, error_key;
    ParsedContact card;                  // vcf read again for the detail page and the editor
};

// A card as pulled from the server, ready to store (research R-05: heavy properties are already removed).
struct ContactUpsert {
    std::string href, etag, uid, vcf;
    bool partial = false, unreadable = false;
    ContactFields fields;
};

struct ContactPullBatch {
    std::vector<ContactUpsert> upserts;
    std::vector<std::string> removed_hrefs;
    // A complete list: every synced contact of the book whose href is not in `listed_hrefs` is removed
    bool full = false;
    std::vector<std::string> listed_hrefs;
    std::string ctag, sync_token;        // the cursor, written in the same transaction
    int64_t synced_at = 0;
};

struct StoredContactRef {
    int64_t id = 0;
    std::string href, etag, uid, sync_state;
    bool refetch = false;   // the next pull downloads it whatever the cursor says
};

// A contact with work that the server has not seen yet, as the push needs it.
struct PendingContact {
    int64_t id = 0, addressbook_id = 0;
    std::string addressbook_href;
    bool addressbook_writable = false, addressbook_present = true;
    std::string href, etag, uid, vcf, display_name;
    bool partial = false;
    std::string sync_state;              // 'pending_create' | 'pending_update' | 'pending_delete'
    std::string error_key;
};

// A contact that needs the user's decision: a conflict (both versions kept) or one that was deleted on the server.
struct ContactProblem {
    int64_t contact_id = 0;
    std::string state;                   // 'conflict' | 'server_deleted'
    std::string display_name, addressbook_name;
    bool deleting = false;               // conflict: the user's side is a request to delete
    std::string server_vcf, server_etag; // conflict only
};

enum class ContactChoice { KeepMine, KeepServer, KeepBoth };

}  // namespace pbo
