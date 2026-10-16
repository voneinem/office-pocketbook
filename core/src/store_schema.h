#pragma once
// Schema 1, copied verbatim from specs/001-email-calendar-mvp/data-model.md (every constraint included).
// Change the data model first, then this file; test_store_open checks the constraints.

namespace pbo {

inline const char* kSchemaSql = R"SQL(
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
--       first_run_done, ui_language, window_days_applied (the days of mail kept at the last sync: a smaller value in the account trims the mail outside it)
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
    saved_path TEXT NOT NULL DEFAULT '',      -- where the downloaded file is (the Downloads folder, real file name)
    error_key TEXT NOT NULL DEFAULT '',       -- 'too_large','offline','refused', ''
    last_opened INTEGER,
    UNIQUE(message_id, section)
);

-- Full-text index over downloaded text (research R-17)
CREATE VIRTUAL TABLE message_fts USING fts5(
    subject, from_text, to_text, body_text,
    content='', contentless_delete=1, tokenize='unicode61 remove_diacritics 2');
-- rows are written/updated/removed by the store together with `message` (rowid = message.id)
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
CREATE TABLE sync_state(
    id INTEGER PRIMARY KEY CHECK(id = 1),
    last_started INTEGER, last_finished INTEGER, last_success INTEGER,
    outcome TEXT NOT NULL DEFAULT 'never' CHECK(outcome IN ('never','running','success','partial','failed','offline')),
    problem_key TEXT NOT NULL DEFAULT '',      -- first problem: auth_failed, cert_expired, server_unreachable, ...
    problem_area TEXT NOT NULL DEFAULT '' CHECK(problem_area IN ('','mail','calendar','outbox')),
    counts_json TEXT NOT NULL DEFAULT '{}',
    clock_skew_s INTEGER                       -- device clock vs a server time stamp (date of a message just saved to Sent/Drafts, or the time a calendar server reports); warn above 600 s (Edge: wrong clock)
);
CREATE TABLE address_suggestion(
    addr TEXT PRIMARY KEY, name TEXT NOT NULL DEFAULT '', last_seen INTEGER NOT NULL, uses INTEGER NOT NULL DEFAULT 1
);   -- bounded to 2,000 rows (oldest/least used pruned); filled from cached headers and sent mail
)SQL";

}  // namespace pbo
