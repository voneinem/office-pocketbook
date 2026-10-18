# Data Model: Server Contacts

**Feature**: [spec.md](spec.md) | **Plan**: [plan.md](plan.md) | **Research**: [research.md](research.md) R-05 to R-09, R-11, R-12

Extends the SQLite database of spec 001 ([data-model.md](../001-email-calendar-mvp/data-model.md), schema 1) to
**schema 2**. `core/src/store_schema.h` keeps schema 1 verbatim; the changes below are the built-in migration
1 -> 2 in `core/src/store_migrations.cpp`, written from this file ("change the data model first, then the code").
`Store::kSchemaVersion` becomes 2. A fresh database is created as schema 1 and then migrated.

## Schema 2 (migration 1 -> 2, one transaction)

```sql
CREATE TABLE addressbook(
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    href TEXT NOT NULL UNIQUE,                -- path on the server, e.g. "/dav/addressbooks/alice/contacts/"
    name TEXT NOT NULL,
    writable INTEGER NOT NULL DEFAULT 0,      -- privileges include write-content or all
    shown INTEGER NOT NULL DEFAULT 1,         -- the user's choice (Settings); the first discovery shows all
    ctag TEXT, sync_token TEXT,               -- the cursor, written in the same transaction as the cards it covers
    present_on_server INTEGER NOT NULL DEFAULT 1,   -- 0 when the book disappeared (R-09)
    last_sync_at INTEGER
);

CREATE TABLE contact(
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    addressbook_id INTEGER NOT NULL REFERENCES addressbook(id) ON DELETE CASCADE,
    href TEXT NOT NULL,                       -- the card's resource path: from the server, or chosen by the app BEFORE the first PUT of a new contact (F2)
    etag TEXT,                                -- the server version this row is based on; NULL while a created contact is not confirmed
    uid TEXT NOT NULL,                        -- the card's UID; 'href:<href>' for a downloaded card without a usable UID or with a duplicate UID (F1)
    vcf TEXT NOT NULL,                        -- the card as known locally; PHOTO/LOGO/SOUND/KEY and values > 64 KiB removed (R-05)
    partial INTEGER NOT NULL DEFAULT 0 CHECK(partial IN (0,1)),   -- 1: the stored card may lack properties (every card from the server: the request names a property list, heavy ones are removed): a write merges into the server's full card; 0 only for a card the app created itself
    unreadable INTEGER NOT NULL DEFAULT 0 CHECK(unreadable IN (0,1)),   -- shown as "unreadable contact", never edited (FR-026)
    display_name TEXT NOT NULL DEFAULT '',    -- derived: FN, else "given family", else organization, else email, else phone
    family TEXT NOT NULL DEFAULT '', given TEXT NOT NULL DEFAULT '', org TEXT NOT NULL DEFAULT '',
    subtitle TEXT NOT NULL DEFAULT '',        -- derived for the list: organization (if a name exists), else first email, else first phone
    sort_key TEXT NOT NULL DEFAULT '',        -- derived: folded family, given; nameless cards: organization, email, phone (R-11)
    search_text TEXT NOT NULL DEFAULT '',     -- derived: folded name, organization, emails, phone numbers (digits and +)
    sync_state TEXT NOT NULL DEFAULT 'synced'
        CHECK(sync_state IN ('synced','pending_create','pending_update','pending_delete','conflict','server_deleted')),
    undo_state TEXT NOT NULL DEFAULT '' CHECK(undo_state IN ('','synced','pending_update')),   -- what a pending deletion returns to (FR-019)
    error_key TEXT NOT NULL DEFAULT '' CHECK(error_key IN ('','read_only','too_large','refused','storage_full_server')),   -- a refusal: the row stays pending_* (FR-024)
    refetch INTEGER NOT NULL DEFAULT 0 CHECK(refetch IN (0,1)),   -- 1: the next pull downloads this href whatever the cursor says (R-08)
    UNIQUE(addressbook_id, uid),
    UNIQUE(addressbook_id, href)              -- one row per resource path: the pull matches by href; the synthetic uid cannot collide (N2)
);
CREATE INDEX contact_sort ON contact(addressbook_id, sort_key);
CREATE INDEX contact_state ON contact(sync_state) WHERE sync_state <> 'synced';

CREATE TABLE contact_email(                   -- derived index for recipient suggestions (R-12); rewritten with every contact write
    contact_id INTEGER NOT NULL REFERENCES contact(id) ON DELETE CASCADE,
    idx INTEGER NOT NULL,
    addr TEXT NOT NULL,                       -- as written (shown)
    addr_key TEXT NOT NULL,                   -- lower case (compared)
    label TEXT NOT NULL DEFAULT '',           -- 'home' | 'work' | 'other' | the card's own text
    PRIMARY KEY(contact_id, idx)
);
CREATE INDEX contact_email_key ON contact_email(addr_key);

CREATE TABLE contact_conflict(                -- the other version, kept side by side (FR-021, R-09)
    contact_id INTEGER PRIMARY KEY REFERENCES contact(id) ON DELETE CASCADE,
    server_vcf TEXT NOT NULL, server_etag TEXT, fetched_at INTEGER NOT NULL
);

-- sync_state: the CHECK of problem_area gains 'contacts' (a CHECK cannot be altered in place: rebuild, copy the single row)
ALTER TABLE sync_state RENAME TO sync_state_old;
CREATE TABLE sync_state(  -- identical to schema 1 except for the problem_area CHECK
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
```

`settings` gets two keys (no table change): `contacts_last_book` (the address book id used last for a new contact, preselected
in the form) and `schema_backup_pending` (`1` from a 1 -> 2 migration that wrote a `pboffice.db.bak` copy, i.e. never for a fresh database, until the next successful start removes
`pboffice.db.bak`; research R-07). Address-book visibility is a column. The mail and calendar tables are not touched.

## Entities and the spec

| Spec entity | Table | Notes |
|-------------|-------|-------|
| Address Book | `addressbook` | one per collection found by discovery; hiding sets `shown = 0` and deletes nothing (FR-025) |
| Contact | `contact` (+ `contact_email`) | the vCard text is the source of truth; every column except state/identity is derived from it by `vcard.h` at write time |
| Contact Conflict | `contact_conflict` | server version beside the local row; for `server_deleted` there is no second version |
| Sync State | `sync_state` (existing) | `problem_area = 'contacts'`; counts in `counts_json` |

## Validation rules

- A contact is written only if `validate_contact_edit` accepts it (research R-14): at least one of name, organization,
  phone, email (FR-013); every email well formed; sizes bounded.
- `href` is accepted only if it names the account's own host (`dav_common`); a *new* card gets a random 128-bit `UID` and a
  random resource path (`<book href><32 hex>.vcf`) when the contact is created, both stored in the same transaction;
  a downloaded card without `UID` is `unreadable` (synthetic uid, see below).
- Limits of research R-14 apply only to values the user **adds or changes**. An entry or note that is longer than the limit
  and untouched is passed through byte for byte; the edit form shows it read-only ("too long to edit here, kept unchanged")
  and lets the user remove it (F4, N3). The edit carries a per-entry `locked` flag (and `note_locked`): a locked entry is
  submitted with its original index and is neither validated nor rewritten; removing it removes its line and its whole group;
  a submitted change to a locked entry is refused with `locked_changed`.
- `UNIQUE(addressbook_id, uid)`: two cards with the same `UID` in one book: the first stays; the second is stored as
  `unreadable` with the **synthetic uid `href:<its href>`** (never silently dropped, never edited). A downloaded card with no
  usable `UID` is stored the same way. A synthetic uid cannot collide: an `href` is unique in a book.
- Derived columns are recomputed by one function (`contact_derive(parsed)`) used by pull, edit, conflict resolution
  and the migration test; no other code writes them.

## State machine: `contact.sync_state`

```text
(server)          pull                 user edit                 push ok
   ───────────► synced ──────────────► pending_update ────────► synced   (etag/href/vcf from the answer; refetch learns the etag if absent)
                  │ │                      │  │ 412 / pre-GET etag differs
                  │ │ user delete          │  └──────────────────────────► conflict ──(resolve)──► pending_update | synced | (+ new pending_create)
                  │ └──────────────► pending_delete ──push ok (204 / 404)──► row removed
                  │                    │ undo (before the server confirmed)
                  │ 404 on update      └──► undo_state (synced | pending_update)
                  └─ pull: gone ─ with local work ─► server_deleted ──(re-create)──► pending_create ──push ok──► synced
                                                      └──(discard, confirmed)────► row removed
user create ──► pending_create (href chosen and stored) ──push ok (201/204, If-None-Match: *)──► synced
                    └── answer lost: retry PUT -> 412 -> GET href -> own UID ──► synced | pending_update (no second card)
                    └── user deletes before any sync ──► row removed (never reaches the server)
any pending_* + refusal (403 / 413 / 507) ──► same state, error_key set ──(retry)──► error_key = '' ; (discard) ──► create: removed | update: refetch=1, synced
```

Rules (all with tests, research R-17):

1. **Persist before network** (constitution III): `save_contact_local`, `request_contact_delete`, `undo_contact_delete`,
   `resolve_contact_conflict` commit in one transaction before any request; a kill at any instruction leaves either the old
   or the new state (kill matrix).
2. **The pull never touches a row with local work.** A pulled card for a row that is not `synced` is compared with the
   row's `etag`: equal -> nothing; different -> `conflict` (for `pending_update` / `pending_delete`) or `server_deleted`
   handling as above. **A pulled card whose `href` equals the `href` of a `pending_create` row is the app's own card** (the
   `UID` was generated by the app): the row takes the card's `etag`; if the pulled text equals the local text it becomes
   `synced`, otherwise `pending_update` (the next push merges against that etag). It is never a conflict. A card for a
   `synced` row replaces it. A card absent from a *complete* list or reported removed
   removes a `synced` row and marks a row with local work `server_deleted`.
3. **Writes are conditional**: create `If-None-Match: *`; update `If-Match: <etag of the GET for partial rows, else the
   stored etag>`; delete `If-Match: <stored etag>`. No unconditional write exists.
4. **Idempotence of a create** (F2): the resource path is stored before the first `PUT`, so a retry is the same request:
   `PUT <href>` with `If-None-Match: *`. If the first `PUT` was accepted and its answer lost, the retry answers `412`; the push
   then does `GET <href>`: a card with the row's `UID` is the app's own card, so the row takes its `ETag` and becomes `synced`
   (equal text) or `pending_update` (the user edited meanwhile) and **no second card is created**. A card at that path with a
   different `UID` cannot happen with a random 128-bit path; if it does, the create gets a new path and is sent again.
5. **Hidden address book**: pull skipped; push still runs for its pending rows; list, search and suggestions exclude it.
6. **Never evicted**: `evict_to_limit` does not select contacts; a `contact` row with `sync_state <> 'synced'` is
   protected in every maintenance function (test `test_eviction_sync` extended).
7. **Cascade**: deleting an `addressbook` row (only done when it is gone on the server and holds no local work) cascades
   to its contacts, emails and conflicts; a book with local work stays with `present_on_server = 0`.

## Relationships

```text
account_calendar 1 --- * addressbook 1 --- * contact 1 --- * contact_email
                                              contact 1 --- 0..1 contact_conflict
sync_state, settings: standalone (as before)
contact_email * --- (read only) --- Store::address_suggestions, merged with address_suggestion (cached mail)
```

## Migration policy (applies the policy of spec 001)

- Built-in step `{from: 1, to: 2}` in `core/src/store_migrations.cpp`; `Store::open` runs it in one transaction after a copy
  to `pboffice.db.bak`, then `PRAGMA integrity_check` (existing code). **Change:** the `.bak` file is removed at the *next*
  successful start of the new build, not at once (research R-07), so a rollback right after the first start loses nothing.
- Tests: `test_store_migration_2` opens a schema-1 fixture database created by the 0.2.0 schema (with mail, drafts, an outbox
  item, events) and checks that every row survives and that the new tables are empty; a crash injected during the migration
  leaves schema 1 intact; the previous schema-1 build code path refuses schema 2 (`schema_too_new`); `PRAGMA
  user_version` is the only version source.
- The owner's reader database is never copied to the host for this (owner rule); the live upgrade is device checklist
  item DT-CT-1 (unverified until run).
