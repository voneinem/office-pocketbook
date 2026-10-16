# Data Model: Email and Calendar MVP

**Feature**: [spec.md](spec.md) | **Plan**: [plan.md](plan.md) | **Research**: [research.md](research.md)

One SQLite database (`pboffice.db`, schema version 1) plus a few files in the app data directory
(`/mnt/ext1/system/pboffice`). The database is created fresh: nothing is migrated from notes-pocketbook, and
a database whose `PRAGMA user_version` is **higher** than the build knows is refused, never modified
(constitution, Development Workflow).

Pragmas at open: `journal_mode=TRUNCATE`, `synchronous=FULL`, `foreign_keys=ON`, `secure_delete=ON`. Every
user action is one transaction committed before any network operation; every sync step commits its data and
its cursor in one transaction.

Dates and times: UTC instants are `INTEGER` seconds since the epoch; all-day and floating values are stored as
`TEXT` local forms (`YYYY-MM-DD` / `YYYY-MM-DDTHH:MM:SS`) next to the instant columns that may be NULL.

## Files outside the database

| File / directory | Content | Notes |
|------------------|---------|-------|
| `credentials` | mail password, SMTP password (optional), calendar password; plain text key=value, version line first | mode 0600 where the file system honors it (research R-13); written atomically; never in the database, logs, backups or errors |
| `/mnt/ext1/Downloads/<name>` | a downloaded attachment, saved under its real (made safe) file name; the path is kept in `attachment.saved_path` | **the user's file**: never overwritten (a taken name gets " (1)", " (2)" ...), never deleted or evicted by the app, not counted in the storage limit; written first as a hidden `.pbo-<id>.part` file and moved when complete (decision of the owner, 2026-10-14). The folder is the reader's Downloads folder on the device, `<data dir>/Downloads` (or `PBO_DOWNLOADS_DIR`) on the host |
| `custom-ca.pem` | optional user CA (FR-003) | read only; validated by the baseline `build_effective_ca` |
| `effective-ca.pem` | base CA + custom CA, rewritten atomically at start | derived |
| `pboffice.log`, `pboffice.log.prev` | diagnostics (FR-041 rules) | written by the launcher, see contracts/device-package.md |

## Entities and tables

### Account (spec: Account)

```sql
CREATE TABLE account_mail(
    id INTEGER PRIMARY KEY CHECK(id = 1),
    imap_host TEXT NOT NULL, imap_port INTEGER NOT NULL,
    imap_security TEXT NOT NULL CHECK(imap_security IN ('tls','starttls')),
    smtp_host TEXT NOT NULL, smtp_port INTEGER NOT NULL,
    smtp_security TEXT NOT NULL CHECK(smtp_security IN ('tls','starttls')),
    username TEXT NOT NULL,
    smtp_username TEXT,                       -- NULL = same as username
    address TEXT NOT NULL,                    -- From address
    display_name TEXT NOT NULL DEFAULT '',
    sent_by_server INTEGER NOT NULL DEFAULT 0 CHECK(sent_by_server IN (0,1)),  -- "server files sent mail itself"
    sync_days INTEGER NOT NULL DEFAULT 30 CHECK(sync_days BETWEEN 1 AND 365),
    attachment_limit INTEGER NOT NULL DEFAULT 10485760 CHECK(attachment_limit BETWEEN 1048576 AND 104857600),
    storage_limit INTEGER NOT NULL DEFAULT 209715200 CHECK(storage_limit >= 20971520),
    sent_folder_id INTEGER, drafts_folder_id INTEGER, trash_folder_id INTEGER,   -- role choices (FR-005)
    capabilities TEXT NOT NULL DEFAULT '',    -- last CAPABILITY line (lowercase tokens), informational
    created INTEGER NOT NULL
);

CREATE TABLE account_calendar(
    id INTEGER PRIMARY KEY CHECK(id = 1),
    caldav_url TEXT,                          -- NULL = no server account
    username TEXT,                            -- password lives in `credentials`
    feed_url TEXT,                            -- NULL = no feed; https only
    feed_name TEXT NOT NULL DEFAULT '',
    created INTEGER NOT NULL,
    CHECK(caldav_url IS NOT NULL OR feed_url IS NOT NULL)
);

CREATE TABLE settings(key TEXT PRIMARY KEY, value TEXT NOT NULL);
-- keys: auto_sync ('1' default), tz_override (the user's chosen IANA zone; absent = ask once, FR-023), tzdata_version, last_integrity_check, sync_all_mail ('0'),
--       first_run_done, ui_language
```

Rules: exactly one mail account and one calendar source (FR-001, FR-002); either may be absent (Assumptions).
Server URLs and user names are validated on entry (scheme, host, port 1-65535, no credentials in URLs, no
spaces/control characters); `imap_security='starttls'` is allowed only for ports where the transport enforces
the upgrade (research R-12). The passwords are **not** columns.

### Folder (spec: Folder)

```sql
CREATE TABLE folder(
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    name TEXT NOT NULL UNIQUE,                -- full mailbox name as on the server (modified UTF-7 decoded)
    raw_name TEXT NOT NULL,                   -- name bytes as sent by the server (for commands)
    delimiter TEXT NOT NULL DEFAULT '/',
    role TEXT NOT NULL DEFAULT 'other' CHECK(role IN ('inbox','sent','drafts','trash','junk','all','archive','other')),
    sync_enabled INTEGER NOT NULL DEFAULT 1,  -- 0 for All Mail / Spam by default (clarification)
    uidvalidity INTEGER, uidnext INTEGER, highestmodseq INTEGER,
    window_low_uid INTEGER,                   -- smallest UID inside the current sync window
    older_on_server INTEGER NOT NULL DEFAULT 0,   -- 1 when messages older than the window exist (FR-006, Edge: huge mailbox)
    unread_count INTEGER NOT NULL DEFAULT 0,  -- cached for the home screen
    last_sync_at INTEGER
);
```

### Message (spec: Message) and Attachment

```sql
CREATE TABLE message(
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    folder_id INTEGER NOT NULL REFERENCES folder(id) ON DELETE CASCADE,
    uid INTEGER NOT NULL,
    stale INTEGER NOT NULL DEFAULT 0,         -- 1 after a UIDVALIDITY change until re-matched (research R-04)
    message_id TEXT,                          -- Message-ID header, <...> form, may be NULL
    in_reply_to TEXT, refs TEXT,              -- for quoting/threading hints only
    from_name TEXT NOT NULL DEFAULT '', from_addr TEXT NOT NULL DEFAULT '',
    to_text TEXT NOT NULL DEFAULT '', cc_text TEXT NOT NULL DEFAULT '',
    subject TEXT NOT NULL DEFAULT '',
    date_utc INTEGER NOT NULL,                -- Date header, else INTERNALDATE
    internal_date INTEGER,
    size INTEGER NOT NULL DEFAULT 0,
    flags_base INTEGER NOT NULL DEFAULT 0,    -- last known server flags (bitmask below)
    flags_local INTEGER NOT NULL DEFAULT 0,   -- what the user wants; = flags_base when nothing pending
    local_state TEXT NOT NULL DEFAULT 'synced'
        CHECK(local_state IN ('synced','pending_flags','pending_move','pending_delete')),
    move_target_folder_id INTEGER REFERENCES folder(id),
    local_error TEXT NOT NULL DEFAULT '' CHECK(local_error IN ('','gone_on_server','moved_on_server','refused','no_trash')),
    body_state TEXT NOT NULL DEFAULT 'headers' CHECK(body_state IN ('headers','text','unreadable','too_large')),
    body_text TEXT,                           -- readable text (HTML already converted), UTF-8, NULL until fetched
    body_note TEXT NOT NULL DEFAULT '',       -- '', 'truncated','charset_unsupported','no_readable_text','html_converted'
    links_json TEXT,                          -- ["https://...", ...] in the order of the [n] markers
    has_attachments INTEGER NOT NULL DEFAULT 0,
    last_opened INTEGER,                      -- LRU for eviction
    UNIQUE(folder_id, uid)
);
CREATE INDEX message_folder_date ON message(folder_id, date_utc DESC);
CREATE INDEX message_state ON message(local_state) WHERE local_state <> 'synced';
CREATE INDEX message_msgid ON message(message_id);

CREATE TABLE attachment(
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    message_id INTEGER NOT NULL REFERENCES message(id) ON DELETE CASCADE,
    section TEXT NOT NULL,                    -- IMAP body section, e.g. "2" or "1.2"
    name TEXT NOT NULL DEFAULT '', mime_type TEXT NOT NULL DEFAULT 'application/octet-stream',
    size INTEGER NOT NULL DEFAULT 0,          -- decoded size if known, else transfer size
    encoding TEXT NOT NULL DEFAULT '',        -- base64 | quoted-printable | 7bit | ...
    state TEXT NOT NULL DEFAULT 'remote' CHECK(state IN ('remote','downloaded')),
    error_key TEXT NOT NULL DEFAULT '',       -- 'too_large','offline','refused', ''
    last_opened INTEGER,
    UNIQUE(message_id, section)
);

-- Full-text index over downloaded text (research R-17)
CREATE VIRTUAL TABLE message_fts USING fts5(
    subject, from_text, to_text, body_text,
    content='', contentless_delete=1, tokenize='unicode61 remove_diacritics 2');
-- rows are written/updated/removed by the store together with `message` (rowid = message.id)
```

**Flag bits** (`flags_base`/`flags_local`): 1 `\Seen`, 2 `\Answered`, 4 `\Flagged`, 8 `\Deleted`, 16 `\Draft`.
Unknown keywords are ignored (not stored, never sent). *Unread* = not `\Seen`.

**Validation / invariants**

- `uid` > 0; `size` >= 0; `date_utc` clamped to [1970, 2100]; header text lengths capped (subject 998 chars,
  address lists 8 KiB) on insert.
- `flags_local = flags_base` and `local_state = 'synced'` and `local_error = ''` unless the user has a pending
  change; the three together are updated in one statement.
- A message with `local_state <> 'synced'` is **protected**: the eviction, the window shortening, the
  server-side-deletion cleanup, and the UIDVALIDITY rebuild never delete it (FR-032).
- `body_text` size <= 2 MiB (truncated beyond, `body_note='truncated'`).

**State: message local change**

```text
synced --(user flags)--------> pending_flags --(push ok; read back flags)--> synced
synced --(user move)---------> pending_move  --(push ok)------------------> [row removed locally; appears in target after pull]
synced --(user delete)-------> pending_move (target = trash) or pending_delete (no Trash, after explicit confirm)
any pending --(UID gone / moved on server)--> same state + local_error set ("could not apply", user acknowledges, row reverts or is removed)
```

### Draft (spec: Draft)

```sql
CREATE TABLE draft(
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    kind TEXT NOT NULL CHECK(kind IN ('new','reply','reply_all','forward')),
    message_id TEXT NOT NULL UNIQUE,          -- generated at creation: stable identity for mirroring and sending
    reply_to_message INTEGER REFERENCES message(id) ON DELETE SET NULL,
    in_reply_to TEXT NOT NULL DEFAULT '', refs TEXT NOT NULL DEFAULT '',
    to_text TEXT NOT NULL DEFAULT '', cc_text TEXT NOT NULL DEFAULT '', bcc_text TEXT NOT NULL DEFAULT '',
    subject TEXT NOT NULL DEFAULT '', body TEXT NOT NULL DEFAULT '',
    created INTEGER NOT NULL, updated INTEGER NOT NULL,
    content_hash TEXT NOT NULL DEFAULT '',    -- over headers+body; detects server-side change
    server_state TEXT NOT NULL DEFAULT 'none'
        CHECK(server_state IN ('none','pending','synced','conflict','pending_delete')),
    server_uid INTEGER, server_hash TEXT NOT NULL DEFAULT '',   -- hash at the last mirror
    conflict_of INTEGER REFERENCES draft(id) ON DELETE SET NULL
);
```

State: `none` (never mirrored) -> `pending` (changed since last mirror) -> `synced` (server confirmed APPEND
and the previous copy was deleted) -> `pending` again on edit. `conflict` marks the second draft created when
a draft changed on both sides (FR-013); `pending_delete` after the user deleted the draft (confirmed) while a
server copy exists; the row is removed after the server copy is gone. A draft is **never deleted before the
server has confirmed its mirrored copy** when `server_state` is `pending` (FR-013/FR-032).
Autosave: the editor calls `save_draft` at most 2 s after the last change and at page leave / suspend; each
call is one transaction.

### Outbox Item (spec: Outbox Item)

```sql
CREATE TABLE outbox(
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    draft_id INTEGER UNIQUE,                  -- NULL after the draft is gone; UNIQUE blocks double Send (FR-015)
    message_id TEXT NOT NULL UNIQUE,          -- same Message-ID as the draft; the duplicate-prevention identity
    envelope_from TEXT NOT NULL,
    envelope_to TEXT NOT NULL,                -- newline-separated recipients incl. Bcc
    subject_preview TEXT NOT NULL DEFAULT '', -- for the list (no body)
    raw_message BLOB NOT NULL,                -- complete RFC 5322 bytes, built once at queue time
    state TEXT NOT NULL CHECK(state IN ('queued','sending','sent_copy_pending','failed','uncertain')),
    error_key TEXT NOT NULL DEFAULT '', error_detail TEXT NOT NULL DEFAULT '',   -- short; never recipients or text
    attempts INTEGER NOT NULL DEFAULT 0,
    queued_at INTEGER NOT NULL, state_at INTEGER NOT NULL, sent_at INTEGER
);
```

**State machine** (research R-08; every arrow is one committed transaction):

```text
            Send tapped (draft -> outbox in one transaction; second tap violates UNIQUE and is ignored)
                |
                v
            queued --(sync/retry: UPDATE ... WHERE state='queued' => 'sending' BEFORE any SMTP byte)--> sending
                                                                                   |
        refused before DATA / connect or TLS failure / auth failure                |  250 after DATA
        / failure before the last body byte left the app                          |
                v                                                                  v
              failed <-- user: Edit (back to draft), Retry (=> queued), Delete   sent_copy_pending --(APPEND ok, or sent_by_server)--> [row deleted]
                                                                                   |  APPEND fails: stays; only APPEND retried
        failure after the last body byte was handed to the transport,
        or app found in 'sending' at start
                v
            uncertain --(user: "Send again" => queued | "It was sent" => sent_copy_pending/deleted | Delete)
```

`uncertain` is never retried automatically and never auto-resolved. `raw_message` is kept as long as the row
exists, so "Send again" sends identical bytes. Rows in `queued|sending|sent_copy_pending|failed|uncertain`
are all protected from eviction.

### Calendar, Event, Conflict (spec: Calendar, Event, Conflict)

```sql
CREATE TABLE calendar(
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    kind TEXT NOT NULL CHECK(kind IN ('caldav','feed')),
    href TEXT NOT NULL UNIQUE,                -- path on the server, or the feed URL
    name TEXT NOT NULL,
    writable INTEGER NOT NULL DEFAULT 0,
    shown INTEGER NOT NULL DEFAULT 1,
    supports_vevent INTEGER NOT NULL DEFAULT 1,
    ctag TEXT, sync_token TEXT, feed_etag TEXT, feed_last_modified TEXT,
    present_on_server INTEGER NOT NULL DEFAULT 1,   -- 0 when the calendar disappeared (research R-18)
    last_sync_at INTEGER
);

CREATE TABLE event(
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    calendar_id INTEGER NOT NULL REFERENCES calendar(id) ON DELETE CASCADE,
    href TEXT,                                -- NULL until created on the server
    etag TEXT,                                -- server version this row was based on
    uid TEXT NOT NULL,
    ics TEXT NOT NULL,                        -- the VCALENDAR as currently known locally (patched on edit)
    summary TEXT NOT NULL DEFAULT '', location TEXT NOT NULL DEFAULT '', description TEXT NOT NULL DEFAULT '',
    attendees TEXT NOT NULL DEFAULT '',
    all_day INTEGER NOT NULL DEFAULT 0,
    tzid TEXT,                                -- IANA name, NULL for floating/UTC/all-day
    start_utc INTEGER, end_utc INTEGER,       -- NULL for floating (resolved at display) and for unresolved zones
    start_local TEXT NOT NULL, end_local TEXT,   -- wall-clock forms, always present
    rrule TEXT NOT NULL DEFAULT '',
    note TEXT NOT NULL DEFAULT '',            -- '', 'rrule_unsupported','tz_unknown','no_end','truncated'
    sync_state TEXT NOT NULL DEFAULT 'synced'
        CHECK(sync_state IN ('synced','pending_create','pending_update','pending_delete','conflict','server_deleted')),
    UNIQUE(calendar_id, uid)
);
CREATE INDEX event_state ON event(sync_state) WHERE sync_state <> 'synced';

CREATE TABLE event_conflict(                 -- the other version, kept side by side (FR-027)
    event_id INTEGER PRIMARY KEY REFERENCES event(id) ON DELETE CASCADE,
    server_ics TEXT NOT NULL, server_etag TEXT, fetched_at INTEGER NOT NULL
);

CREATE TABLE occurrence(                      -- materialized expansion for the window (research R-16)
    event_id INTEGER NOT NULL REFERENCES event(id) ON DELETE CASCADE,
    start_utc INTEGER NOT NULL, end_utc INTEGER NOT NULL,
    local_date TEXT NOT NULL,                 -- date in the user's zone, for day/month marks
    all_day INTEGER NOT NULL DEFAULT 0,
    recurrence_id TEXT NOT NULL DEFAULT '',   -- '' for the master instance
    PRIMARY KEY(event_id, start_utc, recurrence_id)
);
CREATE INDEX occurrence_time ON occurrence(start_utc);
CREATE INDEX occurrence_date ON occurrence(local_date);
```

**State: event sync**

```text
synced --(edit)--------> pending_update --(PUT If-Match ok; new etag)--> synced
(new)  --(create)------> pending_create --(PUT If-None-Match:* ok)------> synced
synced --(delete+confirm)> pending_delete --(DELETE If-Match ok / 404)--> [row removed]
pending_update/pending_delete --(412 or etag differs)--> conflict      [event_conflict row holds the server version]
pending_update --(404 on PUT, or calendar gone)--------> server_deleted  [edited version kept, marked]
conflict --(user: keep mine)--> pending_update (against the new etag)
conflict --(user: keep server's, confirmed)--> synced (ics = server_ics)
conflict --(user: keep both)--> this row = server version (synced) + a new row (new UID) = mine, pending_create
server_deleted --(user: re-create)--> pending_create ; --(user: discard, confirmed)--> row removed
```

Built as: a delete that meets a changed server version ends in `conflict` with `note = 'deleting'` (the user's side is the deletion; "keep mine" sends the DELETE against the new etag); `keep both` on such a conflict keeps only the server's version. `pending_create` that meets `412` is compared by content with the server's resource: equal means the earlier answer was lost and the event is `synced` (no duplicate), different means `conflict`.

A window shortening or window move never removes an event whose `sync_state <> 'synced'` (FR-021, FR-032).
Feed calendars: `writable = 0`, no pending states, events replaced wholesale on each fetch.

### Sync State (spec: Sync State)

```sql
CREATE TABLE sync_state(
    id INTEGER PRIMARY KEY CHECK(id = 1),
    last_started INTEGER, last_finished INTEGER, last_success INTEGER,
    outcome TEXT NOT NULL DEFAULT 'never' CHECK(outcome IN ('never','running','success','partial','failed','offline')),
    problem_key TEXT NOT NULL DEFAULT '',      -- first problem: auth_failed, cert_expired, server_unreachable, ...
    problem_area TEXT NOT NULL DEFAULT '' CHECK(problem_area IN ('','mail','calendar','outbox')),
    counts_json TEXT NOT NULL DEFAULT '{}',
    clock_skew_s INTEGER                       -- device clock vs a server time stamp (date of a message just saved to Sent/Drafts, or the time a calendar server reports); warn above 600 s (Edge: wrong clock)
);
```

"Changes waiting to be synced" (FR-033) is computed, not stored: any outbox row, any draft with
`server_state IN ('pending','pending_delete')`, any message with `local_state <> 'synced'`, any event with
`sync_state IN ('pending_create','pending_update','pending_delete')`.

### Suggested addresses (FR-020)

```sql
CREATE TABLE address_suggestion(
    addr TEXT PRIMARY KEY, name TEXT NOT NULL DEFAULT '', last_seen INTEGER NOT NULL, uses INTEGER NOT NULL DEFAULT 1
);   -- bounded to 2,000 rows (oldest/least used pruned); filled from cached headers and sent mail
```

## Cache eviction and size accounting (SC-013, FR-032)

`used = database file size (page_count * page_size)` (downloaded attachments live in the user's Downloads folder and are not counted).
When `used > storage_limit`, or the window is shortened, or the user requests it, in this order until
`used <= 90 % of limit`:

1. delete downloaded attachment files (least `last_opened` first; set `state='remote'`),
2. clear `body_text` of the oldest synced messages (set `body_state='headers'`; remove from the FTS index),
3. delete the oldest synced messages that are outside the window (`date_utc` older than `sync_days`).

Never selected: any `message` with `local_state <> 'synced'` or `local_error <> ''`, any `draft`, any `outbox`
row, any `event` with `sync_state <> 'synced'`, any `attachment` of a message currently open. If the protected
data alone exceed the limit, sync stops downloading and shows the "storage full" warning.

## Relationships

```text
account_mail 1 --- * folder 1 --- * message 1 --- * attachment
                        message * --- 0..1 folder (move_target_folder_id)
draft 0..1 --- 0..1 outbox          draft * --- 0..1 message (reply_to_message)
account_calendar 1 --- * calendar 1 --- * event 1 --- 0..1 event_conflict
                                          event 1 --- * occurrence
sync_state, settings, address_suggestion: standalone
```

## Migration policy

Schema 1 is the first release. Later upgrades are one-way scripts in `core/src/store_migrations.cpp`, each
run in one transaction after a file copy to `pboffice.db.bak` (removed after `integrity_check` passes, as in
the baseline), each with a test that opens a fixture database of the previous version, and a test that an
older build refuses a newer database. `PRAGMA user_version` is the only version source.

## Note 2026-10-16: settings keys added

`settings.ui_language` (`auto` | `en` | `de`, absent = `auto`) and `settings.window_days_applied` (the days of mail kept at the last sync; when `account_mail.sync_days` is smaller at the next sync, the mail outside the new window is removed, never anything with local work).
