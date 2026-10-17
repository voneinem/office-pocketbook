# Contract: Core interfaces (`core/`, namespace `pbo`)

**Feature**: [../spec.md](../spec.md) | **Plan**: [../plan.md](../plan.md)

These are the C++17 interfaces between the parts of the Qt-free core, and between the core and the bridge.
They are the *seams* that tests replace (Principle II): every network, clock, random and file-system
dependency enters through an interface below, so each module is tested on the host without the device.
Signatures are the contract; bodies belong to the tasks. A change to any interface or to a wire subset
([imap-smtp-subset.md](imap-smtp-subset.md), [caldav-ical-subset.md](caldav-ical-subset.md)) needs a
contract test in the same change (Principle I).

Conventions: no exceptions cross a module boundary (results carry an `Error`); no raw owning pointers;
strings are UTF-8; times are UTC epoch seconds unless named `*_local`; nothing here includes Qt or InkView.

## 1. Shared types (`pbo/types.h`)

```cpp
enum class ErrorClass { None, Network, Tls, Auth, Server, Protocol, Unsupported, Storage, Cancelled };

struct Error {
    ErrorClass cls = ErrorClass::None;
    std::string key;      // stable cause key: "cert_expired", "cert_untrusted", "cert_host_mismatch",
                          // "auth_failed", "server_unreachable", "no_network", "timeout",
                          // "starttls_unavailable", "protocol_error", "refused_recipient", "too_large",
                          // "storage_full", "not_supported", ...
    std::string detail;   // short, English, NEVER credentials, addresses, subjects, hosts or paths (FR-041)
    bool ok() const { return cls == ErrorClass::None; }
};

template <class T> struct Result { Error error; T value; bool ok() const { return error.ok(); } };

struct Clock    { virtual ~Clock();    virtual int64_t now() const = 0; };            // epoch seconds
struct Random   { virtual ~Random();   virtual void bytes(uint8_t* out, size_t n) = 0; };
struct Cancel   { virtual ~Cancel();   virtual bool requested() const = 0; };          // polled between commands
```

## 2. Network transports

### 2.1 `ImapTransport` (`pbo/imap_transport.h`)

One instance = one logical session (one libcurl easy handle reused, so the connection and login are kept for
a whole sync). Not thread-safe; used from the sync thread only.

```cpp
struct ImapEndpoint {
    std::string host; int port = 993;
    enum class Security { Tls, StartTls } security = Security::Tls;   // plaintext does not exist as a value
    std::string username;                                             // password is passed per connect, never stored here
};

struct ImapRaw {                       // raw untagged response data of one command (see imap-smtp-subset.md §3)
    std::string data;                  // exactly the bytes curl delivered, literals included
};

class ImapTransport {
public:
    virtual ~ImapTransport() = default;
    // Verifies TLS (+ enforced STARTTLS), logs in, caches CAPABILITY. Zero credentials leave the device on any TLS failure.
    virtual Error connect(const ImapEndpoint&, const std::string& password) = 0;
    virtual void disconnect() = 0;
    // One IMAP command through a custom request on the selected mailbox (empty = none), e.g. "UID FETCH 1:* (FLAGS)".
    virtual Result<ImapRaw> command(const std::string& mailbox, const std::string& command) = 0;
    // Streams the part `section` ("" = the whole message, digits and dots only) of the message into `sink` with
    // UID FETCH ... BODY.PEEK (never marks the message as read). A literal announced larger than `max_bytes` is refused before
    // any byte is taken (Error key "too_large"); a part the server does not have is "not_found".
    virtual Error fetch_to(const std::string& mailbox, uint32_t uid, const std::string& section,
                           uint64_t max_bytes, ByteSink& sink) = 0;
    // APPEND: uploads `message` to `mailbox`. libcurl sends neither flags nor a date (observed, research R-01), so the
    // server stamps it (clock check, research R-09) and it arrives without \Seen: callers that need flags find the
    // message by Message-ID afterwards and set them with UID STORE.
    virtual Error append(const std::string& mailbox, const std::string& message) = 0;
    virtual const std::set<std::string>& capabilities() const = 0;   // lowercase tokens
};
std::unique_ptr<ImapTransport> make_curl_imap_transport(const std::string& ca_file);   // production
```

### 2.2 `SmtpTransport` (`pbo/smtp_transport.h`)

```cpp
struct SmtpEndpoint { std::string host; int port = 465; ImapEndpoint::Security security = ImapEndpoint::Security::Tls;
                      std::string username; };

enum class SendOutcome {
    Accepted,        // the server answered 250 after DATA: the message is out
    CertainFailure,  // nothing was accepted: refused recipient, auth/TLS/connect failure, or failure before the last body byte was handed over
    Unknown          // failure AFTER the last body byte was handed to the transport: may or may not have been delivered
};
struct SendResult { SendOutcome outcome; Error error; };

class SmtpTransport {
public:
    virtual ~SmtpTransport() = default;
    virtual Error test_connection(const SmtpEndpoint&, const std::string& password) = 0;   // connect, EHLO, STARTTLS/auth only
    // Sends `raw_message` (complete RFC 5322 bytes). `Bcc` recipients are only in `recipients`.
    // MUST return Unknown rather than CertainFailure whenever the outcome is not provable.
    virtual SendResult send(const SmtpEndpoint&, const std::string& password, const std::string& envelope_from,
                            const std::vector<std::string>& recipients, const std::string& raw_message) = 0;
};
std::unique_ptr<SmtpTransport> make_curl_smtp_transport(const std::string& ca_file);
```

### 2.3 `HttpTransport` (baseline, extended)

The baseline interface `HttpTransport::perform(Request) -> Response` [REPO] is kept (HTTPS only, Basic
credentials, size limits, `save_body_to`, no credentials after redirects). Changes: `Request::method` accepts
`PROPFIND`, `REPORT`, `OPTIONS`; `Request::body` is allowed for those; `Response::headers` is multi-valued and
case-insensitive; `Request::max_response_bytes` is mandatory for in-memory responses (default 8 MiB).

## 3. MIME and text (`pbo/mime.h`, `pbo/html_text.h`, `pbo/charset.h`)

```cpp
struct MimeLimits { int max_depth = 8; int max_parts = 200; size_t max_header_block = 64*1024;
                    size_t max_line = 1<<20; size_t max_text_part = 2<<20; size_t max_input = 32<<20; };

struct AttachmentInfo { std::string section, name, mime_type, encoding; uint64_t size = 0; };

struct ParsedMessage {
    std::string from_name, from_addr, to, cc, subject, message_id, in_reply_to, references;
    int64_t date_utc = 0;
    enum class Body { Text, NoReadableText, Unreadable } body_kind = Body::NoReadableText;
    std::string text;                  // UTF-8, HTML already converted
    std::vector<std::string> links;    // [1..n] targets of the "[n]" markers
    std::vector<AttachmentInfo> attachments;
    std::string note;                  // "", "truncated", "charset_unsupported", "html_converted"
};

// Parses a full RFC 5322 message. Never throws, never reads beyond limits; malformed input yields the best
// readable result or Body::Unreadable (FR-040). Deterministic.
ParsedMessage parse_message(std::string_view raw, const MimeLimits& = {});

// Pieces for the IMAP fetch strategy (research R-06)
struct BodyStructure { /* tree of parts: type, subtype, params, encoding, size, disposition, name, section */ };
std::optional<BodyStructure> parse_bodystructure(std::string_view imap_bodystructure);   // nullopt = fall back
std::vector<std::string> choose_text_sections(const BodyStructure&);                    // multipart/alternative rule
ParsedMessage assemble_from_parts(const HeaderBlock&, const BodyStructure&, const std::map<std::string,std::string>& section_bytes);

std::string decode_header_words(std::string_view s);                 // RFC 2047, tolerant
std::string decode_mutf7(std::string_view s);  std::string encode_mutf7(std::string_view utf8);
std::string html_to_text(std::string_view html, std::vector<std::string>* links, const MimeLimits& = {});
// Charset: built-in tables + optional iconv; returns valid UTF-8; `*supported=false` when replaced by fallback.
std::string to_utf8(std::string_view bytes, std::string_view charset, bool* supported = nullptr);

// Building (compose, research R-08)
struct OutgoingMessage { std::string from_name, from_addr, to, cc, subject, body, message_id, in_reply_to, references; int64_t date_utc; };
std::string build_message(const OutgoingMessage&);                   // CRLF, QP, RFC 2047 subject; never writes Bcc
std::string make_message_id(Random&, const std::string& domain);
std::vector<std::string> parse_address_list(std::string_view);       // validated, deduplicated; empty on syntax error
std::string quote_for_reply(const ParsedMessage&, std::string_view attribution);
```

## 4. IMAP responses (`pbo/imap_parse.h`)

```cpp
struct FetchItem { uint32_t uid = 0; std::optional<uint32_t> flags_bits; std::optional<uint64_t> modseq;
                   std::optional<int64_t> internal_date; std::optional<uint64_t> size;
                   std::string header_bytes, bodystructure_text;
                   std::map<std::string, std::string> sections; };
struct FolderListItem { std::string raw_name, name, delimiter; std::set<std::string> attributes; };
struct StatusInfo { uint32_t uidnext = 0, uidvalidity = 0; uint64_t highestmodseq = 0; uint32_t messages = 0; };

// All parsers are total: truncated or hostile input returns what was understood plus `ok=false`; they never read out of bounds,
// never recurse deeper than 32, and cap literals at the size in the command that asked for them.
Parsed<std::vector<FetchItem>>      parse_fetch(const ImapRaw&);
Parsed<std::vector<FolderListItem>> parse_list(const ImapRaw&);
Parsed<StatusInfo>                  parse_status(const ImapRaw&);
Parsed<std::vector<uint32_t>>       parse_search(const ImapRaw&);
std::set<std::string>               parse_capability(const ImapRaw&);
uint32_t flags_to_bits(const std::vector<std::string>&);  std::string bits_to_flag_args(uint32_t add, uint32_t remove);
```

## 5. Time zones, iCalendar, recurrence (`pbo/tz.h`, `pbo/ical.h`, `pbo/rrule.h`)

```cpp
class TzDatabase {                           // bundled IANA data only (research R-16)
public:
    static std::unique_ptr<TzDatabase> open(const std::string& zoneinfo_dir);   // fails if the directory has no usable zones
    std::string version() const;             // e.g. "2026c"
    bool has_zone(std::string_view iana_or_windows_name) const;
    std::string canonical_name(std::string_view name) const;   // Windows names mapped; "" if unknown
    // RFC 5545 conventions: nonexistent local time -> offset before the gap; repeated local time -> first occurrence
    int64_t local_to_utc(std::string_view zone, const LocalDateTime&) const;
    LocalDateTime utc_to_local(std::string_view zone, int64_t utc) const;
    int offset_at(std::string_view zone, int64_t utc) const;
};

struct VEvent { std::string uid, summary, description, location, status, rrule, tzid, recurrence_id;
                std::vector<std::string> attendees, exdates, rdates; bool all_day=false, floating=false;
                std::optional<LocalDateTime> start, end; std::optional<int64_t> start_utc, end_utc; std::string note; };
struct ParsedCalendar { std::vector<VEvent> events; bool ok = true; std::string note; };
ParsedCalendar parse_ical(std::string_view ics, const TzDatabase&, const IcalLimits& = {});   // total, bounded, VTIMEZONE ignored (documented)

struct Occurrence { int64_t start_utc, end_utc; std::string local_date; bool all_day; std::string recurrence_id; };
// Expands one resource (master + overrides) inside [from_utc, to_utc]; at most 5,000 occurrences;
// unsupported rule parts -> single occurrence at DTSTART and note "rrule_unsupported".
std::vector<Occurrence> expand(const ParsedCalendar& resource, const TzDatabase&, const std::string& user_zone,
                               int64_t from_utc, int64_t to_utc);
std::string describe_rrule(const std::string& rrule, const Locale&);        // "every week on Monday until ..." (FR-022)

// Lossless edit: patches changed properties in the raw text, preserves unknown properties/components.
std::string apply_edit(std::string_view raw_ics, const EventEdit&, const TzDatabase&, Clock&);
std::string new_event_ics(const EventEdit&, const std::string& uid, const TzDatabase&, Clock&);
```

## 6. DAV (`pbo/caldav.h`, `pbo/xml_min.h`)

```cpp
struct CalendarInfo { std::string href, name, ctag, sync_token; bool writable, supports_vevent, supports_sync_report; };
struct EventRef     { std::string href, etag; };
struct ChangeSet    { std::vector<EventRef> changed; std::vector<std::string> removed; std::string new_sync_token, new_ctag; bool full; };

class CalDavClient {
public:
    CalDavClient(HttpTransport&, std::string base_url, std::string username, std::string password);
    Result<std::vector<CalendarInfo>> discover();                                          // research R-14 §1-2
    Result<ChangeSet> changes(const CalendarInfo&, const std::string& stored_token_or_ctag,
                              int64_t window_from, int64_t window_to);                     // sync-collection or ctag+query
    Result<std::vector<EventResource>> fetch(const CalendarInfo&, const std::vector<std::string>& hrefs);   // multiget
    enum class WriteKind { Created, Updated, PreconditionFailed, NotFound, Refused };
    struct WriteResult { WriteKind kind; std::string etag; Error error; };
    WriteResult put(const std::string& href_or_new_name, const std::string& ics, const std::string* if_match);  // null = If-None-Match: *
    WriteResult remove(const std::string& href, const std::string& if_match);
};
class FeedClient { Result<FeedResult> fetch(HttpTransport&, const std::string& url, const std::string& etag, const std::string& last_modified); };
```

`xml_min.h`: `parse_xml(std::string_view, XmlLimits) -> Result<XmlNode>`: rejects `<!DOCTYPE`, entities other than the
five predefined and numeric references, processing instructions, depth > 64, nodes > 200,000.

## 7. Storage (`pbo/store.h`)

```cpp
class Store {                                                   // one SQLite connection; all methods are short transactions
public:
    static Result<std::unique_ptr<Store>> open(const std::string& dir, Clock&);   // migrates one way; refuses newer schema (Error key "schema_too_new")
    // accounts / settings
    Error save_mail_account(const MailAccount&);  std::optional<MailAccount> mail_account() const;
    Error save_calendar_account(const CalendarAccount&);  std::optional<CalendarAccount> calendar_account() const;
    // mail
    std::vector<Folder> folders() const;  Error upsert_folders(const std::vector<FolderListItem>&);
    Page<MessageHeader> list_messages(int64_t folder, SortKey, int page, int page_size) const;
    std::optional<MessageView> message(int64_t id) const;                          // marks last_opened
    Error apply_pull(int64_t folder, const PullBatch&);                            // data + cursor in ONE transaction
    Error set_flags_local(int64_t msg, uint32_t add, uint32_t remove);             // pending_flags
    Error request_move(int64_t msg, int64_t target);  Error request_delete(int64_t msg, bool permanent_confirmed);
    std::vector<MessageHeader> search(std::string_view query, int limit) const;     // FTS5, prefix match
    // drafts / outbox
    Result<int64_t> save_draft(const Draft&);  Error delete_draft(int64_t id /*after confirmation*/);
    Result<int64_t> queue_draft(int64_t draft_id, const std::string& raw, const Envelope&);   // ONE transaction; second call -> key "already_queued"
    bool begin_sending(int64_t outbox_id);                                         // conditional queued->sending; false = someone else has it
    Error finish_outbox(int64_t id, OutboxTransition);                             // see data-model state machine
    std::vector<OutboxItem> outbox() const;  int recover_interrupted();            // 'sending' at start -> 'uncertain'
    // calendar
    std::vector<Calendar> calendars() const;  Error apply_calendar_pull(int64_t cal, const CalendarPullBatch&);
    std::vector<OccurrenceRow> agenda(int64_t from, int64_t to, int limit, int offset) const;
    std::vector<OccurrenceRow> day(const std::string& local_date) const;  std::set<std::string> marked_days(const std::string& ym) const;
    Error save_event_local(const EventEdit&);  Error request_event_delete(int64_t id);
    Error resolve_conflict(int64_t event, ConflictChoice);
    // housekeeping
    PendingSummary pending() const;  StorageUsage usage() const;  Error evict_to_limit();  Error rebuild_occurrences();
    // test seam
    Error check_integrity() const;
};
```

Invariants every implementation and test must hold: (a) a failing statement leaves the complete before-state;
(b) rows listed as protected in [data-model.md](../data-model.md) are never removed by `evict_to_limit`,
`apply_pull` or `apply_calendar_pull`; (c) `queue_draft` and `begin_sending` are the only ways to enter the
outbox / `sending`, and both are atomic.

### 7a. Guards and small services (`pbo/uidvalidity_guard.h`, `pbo/clock_check.h`, `pbo/provider_presets.h`)

```cpp
enum class GuardResult { Ok, Mismatch, Failed };   // Failed = could not ask: callers treat it like Mismatch (fail closed)
GuardResult check_uidvalidity(ImapTransport&, const Folder&);   // STATUS immediately before the first write per folder per sync

struct ZoneChoice { enum class State { Resolved, Unresolved } state; std::string name; };
ZoneChoice resolve_user_zone(const std::optional<std::string>& stored_choice, const TzDatabase&);   // no UTC fallback (FR-023)

void note_server_time(int64_t server_epoch);        // clock_check: sets sync_state.clock_skew_s; warning above 600 s
std::optional<SentCopyDefault> preset_for_host(std::string_view imap_host);   // exact hosts only (research R-09)
```

## 8. Sync (`pbo/sync.h`)

```cpp
struct SyncDeps { Store& store; ImapTransport& imap; SmtpTransport& smtp; HttpTransport& http; TzDatabase& tz; Clock& clock; Random& rng; Cancel& cancel; };
struct SyncOptions { bool mail = true, calendar = true; std::function<void(const SyncProgress&)> progress; };
struct SyncReport { SyncOutcome outcome; std::vector<StepResult> steps; Counts counts; Error first_problem; std::string problem_area; };

SyncReport run_sync(SyncDeps&, const Credentials&, const SyncOptions&);
// Steps (research R-18): outbox, mail_push, mail_pull, calendar_push, calendar_pull, finish.
// Guarantees: interrupt at any instruction and re-run => same final state as an uninterrupted run (idempotent);
//             SMTP DATA is attempted at most once per Message-ID unless the user re-queued the item;
//             a failing step is recorded and independent later steps still run; local data stays intact (FR-035).

Result<ConnectionReport> test_mail_connection(ImapTransport&, SmtpTransport&, const MailAccountInput&);   // US1 scenario 1
Result<std::vector<CalendarInfo>> test_calendar_connection(HttpTransport&, const CalendarAccountInput&);   // US2 scenario 1

class SyncScheduler;      // baseline pure decision logic [REPO]; Inputs.has_pending is the PendingSummary any-flag
```

`Credentials` is a plain struct of passwords read from the `credentials` file by `credentials_file.h`
(`load/save/forget`, atomic, mode check reported through `ProtectionStatus {Enforced, NotEnforced, Unknown}`).

## 9. Logging (`pbo/log.h`)

The baseline logger is kept [REPO]: structured events, fields typed (`status`, `count`, `path`, `text`),
name-based redaction. New allowed event codes are listed in `core/src/log_codes.h`; **no field may carry an
address, subject, host, folder name, user name, file name, URL or event title** (FR-041); the constitution
checker rule R6 is extended (R8) to the words `host`, `folder`, `subject`, `from`, `to`, `recipient`, `title`,
`summary`, `attendee`, `location`.

## Implementation notes (2026-10-15): where the code differs from the text above

These are the interfaces as built; the sections above stay as the design record.

- **CalDAV (`caldav.h`)**: `FetchResult { events, gone, failed }` instead of a plain vector; `put(href, ics, if_match)` takes the full resource path (the caller builds it from the calendar href and the UID); `WriteResult::Kind` also has `Failed` (network, login, 5xx); a `403`/`405` is `Refused` with key `read_only`, `413` `too_large`, `507` `storage_full_server`; `409` is read like `412`.
- **Events (`ical.h`, `event_edit.h`)**: `apply_edit` / `new_event_ics` take `now_utc` instead of a `Clock&`; `EditResult { ok, changed, ics, error }`; `is_series`, `validate_event_edit` (keys `title_missing`, `too_long`, `date_invalid`, `end_before_start`, `zone_unknown`), `replace_ical_uid`, `make_event_uid`. The editing service (`create_event`, `change_event`, `delete_event`, `resolve_event_conflict`) sits between the bridges and the store; the store has no `resolve_conflict(event, choice)` but `keep_my_event`, `keep_server_event`, `keep_both_events`, `recreate_event`, `discard_event` (the caller parses the server's version).
- **Event states (`store.h`, `calendar_sync.h`)**: `save_event_local(calendar, event_id, record)`, `request_event_delete`, `pending_events`, `event_pushed(id, pushed_ics, href, etag)` (an edit made while the push was running stays pending), `event_removed_on_server`, `event_conflicted(id, server_ics, server_etag, href)`, `event_gone_on_server`, `event_problems`. A delete that meets a changed server version is a conflict whose local side is the deletion (`event.note = 'deleting'` while in conflict). `push_events` is the `calendar_push` step of `run_sync`, before the pull.
- **Mail organizing (`store.h`, `mail_push.h`)**: `set_flags_local`, `request_move`, `request_delete(id, permanent_confirmed)` (errors `confirm_required`, `no_trash`), `pending_messages`, `message_flags_pushed` (merge: the user's changed bits win, the rest is the server's; a bit the server does not show is `refused`), `message_left_folder`, `set_message_error`, `acknowledge_message_error`, `messages_with_errors`, `search`, `address_suggestions`, `note_address`. `push_message_changes` is part of the `mail_push` step. Messages whose server flags contain `\Deleted` are not listed.
- **Outbox state machine (`data-model.md`)**: `finish_sending` has a fourth kind, `Requeue` (`sending -> queued`), used when the connection was not there at all (`no_network`, `server_unreachable`): nothing was sent, so nothing is uncertain. Every other uncertain case stays `uncertain` and is never resent by itself.
- **Time zone data**: `TzDatabase::open(dir)` reads compact TZif files from `pboffice-zoneinfo/` next to the binary (598 zones, release in `VERSION`); `resolve_user_zone` has no UTC fallback.
- **Window and space (`store.h`, `sync.cpp`)**: `Store::trim_to_window(window_start_utc, keep_message_id, &deleted)` removes synced mail older than the window with the same protection as `evict_to_limit` and sets `older_on_server` on the folders; `run_sync` calls it after the pull only when `account.sync_days` fell below the setting `window_days_applied` (a longer window needs nothing: the pull fetches the added days). Log code `store.trimmed`.
- **Clock (`clock_check.h`)**: two sources feed `sync_state.clock_skew_s`: the HTTP `Date` of the calendar server (`parse_http_date`) and `note_server_stamp(imap, store, clock, mailbox, uid)`, called by the draft mirror and the Sent-copy step after an APPEND that carried no date (`UID FETCH uid (UID INTERNALDATE)` on the open connection). The last measurement stays until the next one replaces it.
- **Words for a repeating rule (`rrule.h`)**: `describe_rrule(rule, Language::English | German)`; the bridges pass the language of the interface.
- **Language (`bridge/include/table_translator.h`)**: not in the core; see `contracts/bridge-qml.md` section 4.
