// The built-in schema upgrades (data-model.md of spec 001, "Migration policy", and specs/002-server-contacts/data-model.md). Every step is a
// script that Store::open runs inside ONE transaction after a copy of the database file, followed by an integrity check. Steps only go forward;
// an older build refuses a database of a newer version (schema_too_new). `PRAGMA user_version` is the only version source.
//
// Change the data model document first, then this file.

#include "pbo/store.h"

namespace pbo {

namespace {

// Schema 1 -> 2 (spec 002): address books and contacts, and the problem area "contacts" of the sync state.
const char* kMigration1To2 = R"SQL(
CREATE TABLE addressbook(
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    href TEXT NOT NULL UNIQUE,                -- path on the server, e.g. "/dav/addressbooks/alice/contacts/"
    name TEXT NOT NULL,
    writable INTEGER NOT NULL DEFAULT 0,      -- privileges include write-content or all
    shown INTEGER NOT NULL DEFAULT 1,         -- the user's choice (Settings); the first discovery shows all
    ctag TEXT, sync_token TEXT,               -- the cursor, written in the same transaction as the cards it covers
    present_on_server INTEGER NOT NULL DEFAULT 1,   -- 0 when the book disappeared
    last_sync_at INTEGER
);

CREATE TABLE contact(
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    addressbook_id INTEGER NOT NULL REFERENCES addressbook(id) ON DELETE CASCADE,
    href TEXT NOT NULL,                       -- the card's resource path: from the server, or chosen by the app BEFORE the first PUT of a new contact
    etag TEXT,                                -- the server version this row is based on; NULL while a created contact is not confirmed
    uid TEXT NOT NULL,                        -- the card's UID; 'href:<href>' for a downloaded card without a usable UID or with a duplicate UID
    vcf TEXT NOT NULL,                        -- the card as known locally; PHOTO/LOGO/SOUND/KEY and values > 64 KiB removed
    partial INTEGER NOT NULL DEFAULT 0 CHECK(partial IN (0,1)),   -- 1: the stored card may lack properties (every card from the server): a write merges into the server's full card
    unreadable INTEGER NOT NULL DEFAULT 0 CHECK(unreadable IN (0,1)),   -- shown as "unreadable contact", never edited
    display_name TEXT NOT NULL DEFAULT '',
    family TEXT NOT NULL DEFAULT '', given TEXT NOT NULL DEFAULT '', org TEXT NOT NULL DEFAULT '',
    subtitle TEXT NOT NULL DEFAULT '',
    sort_key TEXT NOT NULL DEFAULT '',
    search_text TEXT NOT NULL DEFAULT '',
    sync_state TEXT NOT NULL DEFAULT 'synced'
        CHECK(sync_state IN ('synced','pending_create','pending_update','pending_delete','conflict','server_deleted')),
    undo_state TEXT NOT NULL DEFAULT '' CHECK(undo_state IN ('','synced','pending_update')),
    error_key TEXT NOT NULL DEFAULT '' CHECK(error_key IN ('','read_only','too_large','refused','storage_full_server')),
    refetch INTEGER NOT NULL DEFAULT 0 CHECK(refetch IN (0,1)),
    UNIQUE(addressbook_id, uid),
    UNIQUE(addressbook_id, href)
);
CREATE INDEX contact_sort ON contact(addressbook_id, sort_key);
CREATE INDEX contact_state ON contact(sync_state) WHERE sync_state <> 'synced';

CREATE TABLE contact_email(
    contact_id INTEGER NOT NULL REFERENCES contact(id) ON DELETE CASCADE,
    idx INTEGER NOT NULL,
    addr TEXT NOT NULL,
    addr_key TEXT NOT NULL,
    label TEXT NOT NULL DEFAULT '',
    PRIMARY KEY(contact_id, idx)
);
CREATE INDEX contact_email_key ON contact_email(addr_key);

CREATE TABLE contact_conflict(
    contact_id INTEGER PRIMARY KEY REFERENCES contact(id) ON DELETE CASCADE,
    server_vcf TEXT NOT NULL, server_etag TEXT, fetched_at INTEGER NOT NULL,
    deleting INTEGER NOT NULL DEFAULT 0 CHECK(deleting IN (0,1))   -- 1: the user's side of the conflict is a request to delete
);

ALTER TABLE sync_state RENAME TO sync_state_old;
CREATE TABLE sync_state(
    id INTEGER PRIMARY KEY CHECK(id = 1),
    last_started INTEGER, last_finished INTEGER, last_success INTEGER,
    outcome TEXT NOT NULL DEFAULT 'never' CHECK(outcome IN ('never','running','success','partial','failed','offline')),
    problem_key TEXT NOT NULL DEFAULT '',
    problem_area TEXT NOT NULL DEFAULT '' CHECK(problem_area IN ('','mail','calendar','outbox','contacts')),
    counts_json TEXT NOT NULL DEFAULT '{}',
    clock_skew_s INTEGER
);
INSERT INTO sync_state SELECT * FROM sync_state_old;
DROP TABLE sync_state_old;
)SQL";

}  // namespace

const std::vector<Migration>& builtin_migrations() {
    static const std::vector<Migration> list = {{1, 2, kMigration1To2}};
    return list;
}

}  // namespace pbo
