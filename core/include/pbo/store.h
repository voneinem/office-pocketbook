#pragma once
// The SQLite store (data-model.md). One connection; every method is a short transaction. Grown story by story;
// this first slice opens the database, creates schema 1 and runs one-way migrations.
//
// Principle III: a database with a newer schema than this build knows is refused and never modified; upgrades are
// one-way, run in one transaction after a backup copy, and are followed by an integrity check.

#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <functional>

#include "pbo/account.h"
#include "pbo/calendar_types.h"
#include "pbo/draft_types.h"
#include "pbo/imap_parse.h"
#include "pbo/mail_types.h"
#include "pbo/types.h"

struct sqlite3;

namespace pbo {

struct Migration {
    int from = 0;
    int to = 0;
    std::string sql;   // run inside the migration transaction
};

struct StoreOptions {
    int target_version = 0;                 // 0 = the version of this build (Store::kSchemaVersion)
    std::vector<Migration> extra_migrations;  // the registered upgrade steps (tests add their own)
};

class Store {
public:
    static constexpr int kSchemaVersion = 1;

    // Creates `dir` (mode 0700) and `dir/attachments` (0700) if needed and opens `dir/pboffice.db` (0600).
    // Errors: "schema_too_new" (the file is left untouched), "migration_failed" (rolled back, backup kept),
    // "open_failed", "integrity_failed".
    static Result<std::unique_ptr<Store>> open(const std::string& dir, Clock& clock, const StoreOptions& options = {});

    ~Store();
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;

    int schema_version() const;
    Error check_integrity() const;
    const std::string& directory() const { return dir_; }

    // ---- account (data-model.md: account_mail) ----
    // Validates the account first (account_input.h); a rejected value changes nothing. Errors: "invalid_account"
    // (detail names the first field and key), "write_failed".
    Error save_mail_account(const MailAccount& account);
    std::optional<MailAccount> mail_account() const;

    // ---- mail ----
    std::vector<Folder> folders() const;
    // Inserts new folders and refreshes name/delimiter/role of known ones; cursors and sync flags are kept.
    Error upsert_folders(const std::vector<FolderListItem>& items);
    std::optional<Folder> folder(int64_t folder_id) const;
    // The user's choice whether a folder is synchronised (kept when the folder list is read again). Error "not_found" for an unknown folder.
    Error set_folder_sync_enabled(int64_t folder_id, bool enabled);
    // How much waits for the next sync: messages with a local change that is not on the server yet, plus the outbox entries that
    // are not sent (queued, sending, failed, uncertain, copy pending). Drafts are the user's own text, not counted.
    int unsynced_count() const;
    // The UIDs of the messages of the folder's CURRENT numbering (rows kept from before a UIDVALIDITY reset are not included).
    std::set<uint32_t> known_uids(int64_t folder_id) const;
    // New server flags of known messages: flags_base always, flags_local too unless the user has a pending change on the message.
    Error apply_flags(int64_t folder_id, const std::vector<std::pair<uint32_t, uint32_t>>& uid_and_flags);
    // Messages that are gone from the server: a row without local work is deleted; a row with pending work is kept and marked
    // local_error = "gone_on_server" (FR-019). *removed / *kept (optional) count both.
    Error remove_gone(int64_t folder_id, const std::vector<uint32_t>& uids, int* removed = nullptr, int* kept = nullptr);

    // ---- UIDVALIDITY reset (research R-04) ----
    // The folder was reset on the server: all its rows become "stale" (their UIDs are renumbered to negative values so they can
    // never meet a new UID), the cursor is cleared, the new UIDVALIDITY is stored and the rebuild is marked as running.
    // Calling it again (a second reset during the rebuild) turns the rows downloaded so far stale as well.
    Error begin_rebuild(int64_t folder_id, int64_t new_uidvalidity);
    bool rebuild_in_progress(int64_t folder_id) const;
    // The window has been downloaded: stale rows without local work are removed; stale rows with pending work stay visible with
    // local_error "gone_on_server" (they had no match among the downloaded messages). Ends the rebuild.
    Error finish_rebuild(int64_t folder_id, int* removed = nullptr, int* kept = nullptr);
    // Message rows and the folder cursor in ONE transaction. A row the user changed locally keeps its local flags.
    Error apply_pull(int64_t folder_id, const PullBatch& batch);
    // Messages of a folder whose body has not been fetched yet (body_state "headers"), newest first.
    std::vector<MissingBody> messages_missing_body(int64_t folder_id, int limit) const;
    // The body and the attachment rows of one message and the search index, in ONE transaction. A rejected state
    // ("invalid_body_state") or an unknown message ("not_found") changes nothing. Attachments the user already downloaded
    // keep their state; sections that are no longer listed are removed.
    Error apply_body(int64_t message_id, const BodyUpdate& update);
    std::optional<AttachmentRef> attachment(int64_t attachment_id) const;

    // ---- the pictures of a message's HTML body (data-model.md: message_image) ----
    std::vector<StoredImage> message_images(int64_t message_id) const;        // by number
    std::optional<MessageLocation> message_location(int64_t message_id) const;   // the UID and the folder on the server
    std::optional<std::string> image_data(int64_t message_id, int idx) const;   // the bytes of a loaded picture
    // Keeps the bytes of a loaded picture (only picture media types; "not_found" for a picture that is not listed, "too_large" over 8 MiB).
    Error set_image_loaded(int64_t message_id, int idx, const std::string& mime_type, const std::string& bytes);
    // "pending" | "failed" | "too_large" | "unavailable" (not "loaded": that comes with the bytes); the bytes of the picture are dropped.
    Error set_image_state(int64_t message_id, int idx, const std::string& state);
    // state "remote" | "downloaded"; error_key "", "too_large", "offline" or "refused". Unknown id: "not_found".
    // `saved_path` belongs to "downloaded"; any other state clears it.
    Error set_attachment_state(int64_t attachment_id, const std::string& state, const std::string& error_key, const std::string& saved_path = std::string());
    // One message for reading; marks it as opened now (the least recently opened go first when space is needed).
    std::optional<MessageView> message(int64_t message_id);
    Page<MessageHeader> list_messages(int64_t folder_id, SortKey sort, int page, int page_size) const;

    // ---- organizing mail (US5; data-model.md: message.local_state) ----
    // The user's flag change on the reader: flags_local = (flags_local | add) & ~remove. 'pending_flags' while it differs from the server's flags_base, 'synced'
    // again when it does not. Errors: "not_found", "bad_state" (a move or a delete is waiting).
    Error set_flags_local(int64_t message_id, uint32_t add, uint32_t remove);
    // Moves the message to another folder at the next sync. It leaves the list of its folder at once. Errors: "not_found", "same_folder", "bad_target", "bad_state".
    Error request_move(int64_t message_id, int64_t target_folder_id);
    // Deleting. In a folder that is not the Trash the message is moved to the Trash (nothing is lost). In the Trash, or when there is no Trash folder,
    // the deletion is permanent and needs `permanent_confirmed` (the page asked a second, separate question); the app never creates a Trash folder.
    // Errors: "not_found", "confirm_required" (in the Trash), "no_trash" (no Trash folder known), "bad_state".
    Error request_delete(int64_t message_id, bool permanent_confirmed);
    // Messages with a change waiting and no error, in folder order (the push). Rows of an earlier UIDVALIDITY (stale) are not listed.
    std::vector<PendingMessage> pending_messages() const;
    // The server applied the flag change and answered with its flags: flags_base = that, flags_local = the server's flags except the bits the user changed
    // (the user's choice wins for those). If the server does not show a changed bit afterwards, the change is marked 'refused'.
    Error message_flags_pushed(int64_t message_id, uint32_t server_flags);
    // The move or the delete is done on the server: the row goes (a moved message arrives with the pull of its new folder).
    Error message_left_folder(int64_t message_id);
    // A change could not be applied: local_error = "gone_on_server" | "moved_on_server" | "refused" | "no_trash"; the change stays visible as "could not apply".
    Error set_message_error(int64_t message_id, const std::string& key);
    // The user saw the message "could not apply": gone/moved on the server -> the row is removed; refused / no_trash -> the local change is dropped.
    Error acknowledge_message_error(int64_t message_id);
    std::vector<MessageHeader> messages_with_errors() const;
    // Full-text search over sender, recipients, subject and the stored body text of every folder (research R-17). The input is taken literally: quotes,
    // % _ * and FTS syntax characters are ordinary characters; every word matches as a prefix; all words must match. Newest first.
    std::vector<MessageHeader> search(const std::string& query, int limit) const;
    // Addresses seen in cached and sent mail, for the recipient fields: those that start with `prefix` (address or name), most used first.
    struct AddressSuggestion {
        std::string addr, name;
    };
    std::vector<AddressSuggestion> address_suggestions(const std::string& prefix, int limit) const;
    Error note_address(const std::string& addr, const std::string& name, int weight = 1);

    // Decodes RFC 2047 words that an earlier version stored as they came (subject, sender name, recipients) and keeps the
    // search index in step. Idempotent; *repaired (optional) gets the number of rows changed.
    Error repair_encoded_headers(int* repaired = nullptr);

    // ---- drafts (data-model.md: draft) ----
    // Inserts (id 0) or updates one draft in ONE transaction. The content hash is computed here. Errors: "invalid_draft" (kind, message id),
    // "too_large" (body over 1 MiB, recipients over 16 KiB), "not_found", "already_queued" (a draft in the outbox is not edited), "write_failed".
    // A changed draft that was mirrored (server_state "synced") becomes "pending".
    Result<int64_t> save_draft(const Draft& draft);
    std::optional<Draft> draft(int64_t draft_id) const;
    // The drafts the user sees: not in the outbox, not deleted; the most recently changed first.
    std::vector<Draft> drafts() const;
    // The user deletes a draft (after the confirmation). A mirrored copy on the server goes first: the row then stays hidden as
    // "pending_delete" until the mirror has removed the copy. A draft in the outbox is not deleted here ("already_queued").
    Error delete_draft(int64_t draft_id);
    // Removes the row of a draft once nothing depends on it (the server copy is gone). Refused ("draft_pending") while the draft has
    // changes the server has not seen (server_state "pending"): a draft is never lost before its mirrored copy is confirmed (FR-013/FR-032).
    Error drop_draft_row(int64_t draft_id);
    // The server confirmed the copy with `uid` of the content `hash`: server_state becomes "synced" if the draft still has that content, else stays "pending".
    // `raw_hash` (optional) is the fingerprint of the bytes that were uploaded; it is kept as server_hash to notice a change made elsewhere.
    Error set_draft_mirrored(int64_t draft_id, int64_t uid, const std::string& hash, const std::string& raw_hash = std::string());
    // The draft changed on the server and here (FR-013): the server's version becomes a second draft (fresh Message-ID, conflict_of = original,
    // it IS the server copy `uid`), the original keeps the user's text and goes up again as a new copy. One transaction; returns the new draft's id.
    Result<int64_t> split_draft_conflict(int64_t original_id, const Draft& server_version, int64_t uid, const std::string& raw_hash);
    std::vector<Draft> drafts_to_mirror() const;       // server_state none or pending, not in the outbox
    std::vector<Draft> drafts_to_remove_on_server() const;   // server_state pending_delete

    // ---- outbox (data-model.md: outbox, state machine of research R-08) ----
    // Draft -> outbox in ONE transaction. A second call for the same draft changes nothing ("already_queued"); a draft that changed since the
    // message was built is refused ("draft_changed"). Returns the outbox id.
    Result<int64_t> queue_draft(int64_t draft_id, const OutboxRequest& request);
    std::vector<OutboxItem> outbox() const;
    std::optional<OutboxItem> outbox_item(int64_t outbox_id) const;
    // queued -> sending, committed BEFORE the first SMTP byte; true only for the caller that made the change.
    Result<bool> begin_sending(int64_t outbox_id);
    // sending -> sent_copy_pending | failed | uncertain | queued (Requeue). "bad_state" if the row is not in "sending".
    Error finish_sending(int64_t outbox_id, const SendFinish& finish);
    // The reason the Sent copy could not be filed yet, kept on the row of a message in "sent_copy_pending" ("bad_state" for other rows).
    Error set_outbox_error(int64_t outbox_id, const std::string& key, const std::string& detail);
    // sent_copy_pending -> row removed (the Sent copy is filed or not wanted); the draft goes with it.
    Error complete_outbox(int64_t outbox_id);
    Error retry_outbox(int64_t outbox_id);        // failed -> queued
    Error send_again(int64_t outbox_id);          // uncertain -> queued (only the user's choice)
    Error mark_was_sent(int64_t outbox_id);       // uncertain -> sent_copy_pending (the user says it was sent)
    Error edit_outbox(int64_t outbox_id);         // failed -> back to a draft ("draft_gone" if the draft no longer exists)
    Error delete_outbox(int64_t outbox_id);       // failed | uncertain -> the message and its draft are deleted (the user confirmed)
    // At the start of the app: every row still in "sending" becomes "uncertain" (error_key "interrupted"); never resent by itself.
    Error recover_interrupted(int* recovered = nullptr);

    // ---- calendar (data-model.md: account_calendar, calendar, event, occurrence) ----
    // Validates first (calendar_account_input.h); a rejected value changes nothing. Errors: "invalid_account", "write_failed".
    Error save_calendar_account(const CalendarAccount& account);
    std::optional<CalendarAccount> calendar_account() const;
    std::vector<Calendar> calendars() const;
    // Inserts the calendar or refreshes name, writability and VEVENT support of the one with this href; the user's choice (shown) and
    // the cursor are kept, and present_on_server is set again. Returns the id.
    Result<int64_t> upsert_calendar(const Calendar& calendar);
    Error set_calendar_shown(int64_t calendar_id, bool shown);   // "not_found" for an unknown calendar
    // Calendars of this kind that are not in `present_hrefs` are marked present_on_server = 0 (their events stay).
    Error mark_calendars_present(const std::string& kind, const std::set<std::string>& present_hrefs);
    std::vector<StoredEventRef> event_refs(int64_t calendar_id) const;
    // Events, their occurrences and the calendar's cursor in ONE transaction. An event with local work (sync_state not 'synced') is
    // never overwritten or removed; an event without any occurrence in the batch is not stored (and a stored copy is removed).
    Error apply_calendar_pull(int64_t calendar_id, const CalendarPullBatch& batch);
    // Replaces the occurrences of every event in one transaction (the window moved, the zone or the tz data changed). The callback
    // gets the stored ics of an event; no occurrence for a synced event removes it, an event with local work stays.
    Error rebuild_occurrences(const std::function<std::vector<OccurrenceRow>(const std::string& ics)>& expander);
    // Occurrences that have not ended before `from_utc` (and have not started before it), in time order, of the shown calendars.
    Page<AgendaItem> agenda(int64_t from_utc, int page, int page_size) const;
    // Occurrences overlapping [from_utc, to_utc) of the shown calendars, in time order.
    std::vector<AgendaItem> occurrences_between(int64_t from_utc, int64_t to_utc) const;
    // The distinct local dates ("YYYY-MM-DD", from..to inclusive) on which a shown calendar has an occurrence.
    std::vector<std::string> marked_days(const std::string& from_date, const std::string& to_date) const;
    std::optional<EventView> event(int64_t event_id) const;
    // The occurrence of an event to show when the caller names none: the first that has not ended at `from_utc`, else the last one.
    std::optional<OccurrenceRow> next_occurrence(int64_t event_id, int64_t from_utc) const;

    // ---- event editing (data-model.md: state "event sync") ----
    // Writes a new (event_id 0, in calendar_id) or an edited event in ONE transaction, with its occurrences. `record` comes from the new text
    // (ics, fields, occurrences; href/etag are ignored). A new event is 'pending_create'; a synced one becomes 'pending_update'; a pending one stays
    // pending; a 'server_deleted' one stays marked. Errors: "not_found", "read_only" (feed, calendar without write access, calendar gone and
    // nothing yet created there), "in_conflict", "delete_pending", "duplicate_uid", "write_failed". Returns the event id.
    Result<int64_t> save_event_local(int64_t calendar_id, int64_t event_id, const EventUpsert& record);
    // The user deleted the event (already confirmed). pending_create / server_deleted: the row is removed; synced / pending_update:
    // 'pending_delete' (its occurrences go, the text stays for the push). Errors: "not_found", "read_only", "in_conflict".
    Error request_event_delete(int64_t event_id);
    std::vector<PendingEvent> pending_events() const;   // in the order they were made
    // The server accepted `pushed_ics` (PUT) under `href` with `etag`. If the event was edited meanwhile it stays 'pending_update' (against the
    // new etag); otherwise it is 'synced'.
    Error event_pushed(int64_t event_id, const std::string& pushed_ics, const std::string& href, const std::string& etag);
    // DELETE accepted, or the resource was already gone: a 'pending_delete' row is removed.
    Error event_removed_on_server(int64_t event_id);
    // 412: the server's version differs (`href`: where the resource is, kept for a created event). The state becomes 'conflict' and the server's version is kept beside the local one (nothing is lost).
    // For a 'pending_delete' row the user's side is the deletion (EventProblem::deleting).
    Error event_conflicted(int64_t event_id, const std::string& server_ics, const std::string& server_etag, const std::string& href);
    // 404 on an update, or the calendar is gone: 'pending_update'/'pending_create' becomes 'server_deleted' (the edited version is kept); a
    // 'pending_delete' row is removed (the goal is reached).
    Error event_gone_on_server(int64_t event_id);
    std::vector<EventProblem> event_problems() const;
    // Resolutions of a conflict (all in one transaction). KeepMine: the local text goes up again against the server's etag (a deletion stays a
    // deletion). KeepServer: the local row becomes the server's version (`server`). KeepBoth: the row becomes the server's version and the
    // local text is kept as a new event (`mine_copy`, new UID, 'pending_create').
    Error keep_my_event(int64_t event_id);
    Error keep_server_event(int64_t event_id, const EventUpsert& server);
    Error keep_both_events(int64_t event_id, const EventUpsert& server, const EventUpsert& mine_copy);
    Error recreate_event(int64_t event_id);   // server_deleted -> pending_create
    Error discard_event(int64_t event_id);    // server_deleted -> row removed (the user confirmed)

    // ---- storage (SC-013, FR-032) ----
    struct Usage {
        int64_t database_bytes = 0;   // the used part of the database file (free pages do not count)
    };
    Usage usage() const;
    struct Eviction {
        int deleted = 0;              // messages removed
        int64_t used_after = 0;
        bool over_limit = false;      // still above the limit although everything that may go has gone: the protected data alone are too big
    };
    // Above `limit_bytes` (0 = no limit) the OLDEST synced messages that are older than `window_start_utc` are deleted, down to 90 % of the limit.
    // Never chosen: a message with a pending change or an error, the message `keep_message_id` (the one that is open), anything inside the window;
    // drafts, the outbox and events are not touched at all. Downloaded attachments live in the Downloads folder and are never deleted by the app.
    Eviction evict_to_limit(int64_t limit_bytes, int64_t window_start_utc, int64_t keep_message_id = 0);
    // The window was shortened: deletes the synced messages that are older than `window_start_utc`, with the same protection as evict_to_limit (a message
    // with a pending change or an error and the one that is open stay; drafts, the outbox and events are not touched). The folders learn that older mail
    // waits on the server. Returns the number of messages removed in *deleted.
    Error trim_to_window(int64_t window_start_utc, int64_t keep_message_id = 0, int* deleted = nullptr);

    // ---- settings (data-model.md: settings) ----
    std::string setting(const std::string& key) const;   // "" if not set
    Error set_setting(const std::string& key, const std::string& value);
    // Makes the next sync fetch the bodies of all messages again (a change in how bodies are read); downloaded attachments keep
    // their state. Returns the number of messages marked in *marked (optional).
    Error mark_bodies_for_refetch(int* marked = nullptr);

    // ---- sync state (one row) ----
    SyncState sync_state() const;
    // The device clock minus a server's time, measured during a sync (clock_check.h); nullopt forgets it.
    Error set_clock_skew(std::optional<int64_t> skew_seconds);
    Error write_sync_state(const SyncState& state);

    // Test seams (the product code uses the typed methods that arrive with the stories). debug_exec returns the
    // SQLite error text ("" on success); debug_query returns the first column of the first row as text.
    std::string debug_exec(const std::string& sql);
    std::string debug_query(const std::string& sql) const;

private:
    Store(sqlite3* db, std::string dir, Clock& clock) : db_(db), dir_(std::move(dir)), clock_(&clock) {}

    // One SQLite connection is shared by the UI thread (reads) and the worker thread (sync). Every public method
    // holds this mutex for its whole transaction, so a reader never sees a half-applied batch.
    mutable std::mutex mu_;
    sqlite3* db_ = nullptr;
    std::string dir_;
    Clock* clock_ = nullptr;
};

}  // namespace pbo
