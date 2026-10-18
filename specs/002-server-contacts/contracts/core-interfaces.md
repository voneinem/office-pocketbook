# Contract: Core interfaces (additions)

**Feature**: [../spec.md](../spec.md) | **Research**: [../research.md](../research.md) | Extends
[the core contract of spec 001](../../001-email-calendar-mvp/contracts/core-interfaces.md)

New and changed C++ seams. `core/` stays Qt-free and InkView-free and every external thing (network, clock, random,
files) is reached through an existing interface (`HttpTransport`, `Clock`, `Random`, `Cancel`). Namespace `pbo`. Only the
shape is fixed here; tasks write the tests first.

## 1. New headers

| Header | Contents |
|--------|----------|
| `dav_common.h` | extracted from `caldav.cpp` (research R-03): `DavSession` (origin, `url_for`, `send` with credentials/cap/server time, `status_error`), `parse_multistatus`, `prop_in_ok_propstat`, `dav_headers`, `xml_escape`. Behaviour of `CalDavClient` unchanged |
| `carddav.h` | `CardDavClient(HttpTransport&, base_url, user, password)`: `discover() -> Result<vector<AddressBookInfo>>` (error `no_addressbook`), `changes(book, stored_token_or_ctag) -> Result<CardChangeSet>`, `fetch(book, hrefs) -> Result<CardFetchResult>`, `get(href) -> Result<CardResource>` (full card with ETag), `put(href, vcf, if_match*) -> CardWriteResult`, `remove(href, if_match) -> CardWriteResult`; `AddressBookInfo{href, name, ctag, sync_token, writable, supports_sync_report}`; `CardRef{href, etag}`; `CardResource{href, etag, vcf}`; `CardWriteResult{Kind Created/Updated/PreconditionFailed/NotFound/Refused/Failed, etag, error}` |
| `vcard.h` | `ParsedContact parse_vcard(string_view text)` (total: never throws, bounded, `unreadable` flag); `string strip_heavy_properties(string text, bool* removed)`; `ContactFields contact_derive(parsed)` (display name, subtitle, sort key, search text, emails); `ContactEdit contact_edit_of(parsed)`; `string new_contact_vcf(const ContactEdit&, uid, now)`; `Result<string> apply_contact_edit(original_text, ContactEdit, now)` |
| `contact_types.h` | `AddressBook`, `Contact` (row), `ContactListItem{id, display_name, subtitle, sync_state, error_key, addressbook_name, unreadable}`, `ContactView{... all fields of the card for the detail page}`, `ContactEdit{given, family, org, phones[], emails[], addresses[], note, note_locked}` with `LabeledValue{label, value, orig_index, locked}` and `PostalAddress{label, street, city, region, postcode, country, orig_index, locked}` (`orig_index` = position in the stored card, -1 for a new entry; `locked` = longer than the input limit: shown read-only, kept byte for byte, removable, a changed value is refused with `locked_changed`), `ContactProblem{contact_id, state, display_name, addressbook_name, deleting, server_vcf, server_etag}`, `ContactUpsert` (a pulled card ready to store), `ContactPullBatch` (as `CalendarPullBatch`), `ContactChoice{KeepMine, KeepServer, KeepBoth}` |
| `contact_edit.h` | validation (`validate_contact_edit(edit) -> vector<FieldError>`, keys `empty`, `email_invalid`, `too_long`, `too_many`, `card_too_large`, `locked_changed`) and the user actions: `create_contact(deps, addressbook_id, edit)`, `change_contact(deps, contact_id, edit)`, `delete_contact(store, contact_id)`, `undo_delete_contact(store, contact_id)`, `retry_contact(store, id)`, `discard_contact_change(store, id)`, `resolve_contact_conflict(deps, id, choice)`, `contact_edit_block_reason(view)` (`""`, `read_only`, `unreadable`, `in_conflict`, `delete_pending`, `book_gone`) |
| `contacts_sync.h` | `ContactsSyncDeps{Store&, HttpTransport&, Clock&, Random&, Cancel&}`; `Error pull_contacts(deps, password, ContactsCounts*)`; `Error push_contacts(deps, password, ContactsPushCounts*)`; `kContactsBatch = 30`. Same guarantees as `calendar_sync.h`: every step commits data and cursor together, an interrupted run converges, local work is never touched by the pull |

## 2. Changed headers

| Header | Change |
|--------|--------|
| `store.h` | `kSchemaVersion = 2`. Address books: `save/upsert_addressbook`, `addressbooks()`, `set_addressbook_shown(id, bool)` (`not_found`), `mark_addressbooks_present(set)`. Reading: `contacts_page(addressbook_ids?, query, offset, limit)`, `contacts_count(query)`, `contact(id) -> optional<ContactView>`, `contact_refs(book_id)`. Pull: `apply_contact_pull(book_id, ContactPullBatch)` (one transaction with the cursor; rows with local work never overwritten or removed). Local work: `save_contact_local(book_id, contact_id, record)` (a new contact gets its `UID` and resource path here) (errors `not_found`, `read_only`, `in_conflict`, `delete_pending`, `duplicate_uid`, `write_failed`), `request_contact_delete`, `undo_contact_delete`, `pending_contacts()` (in the order they were made), `contact_pushed(id, pushed_vcf, href, etag)`, `contact_removed_on_server(id)`, `contact_conflicted(id, server_vcf, server_etag, href)`, `contact_gone_on_server(id)`, `contact_refused(id, error_key)`, `contact_problems()`, `keep_my_contact`, `keep_server_contact`, `keep_both_contacts` (the user's version gets a new `UID` and a new resource path), `recreate_contact` (a fresh resource path), `discard_contact`. `address_suggestions` returns `AddressSuggestion{addr, name, label, from_contact}` (contacts first, deduplicated) |
| `store_migrations.cpp` (new) | built-in `Migration` list used by `Store::open` together with `StoreOptions::extra_migrations`; step 1 -> 2 per [data-model.md](../data-model.md) |
| `sync.h` | `sync_step_names()` = outbox, mail_push, mail_pull, calendar_push, calendar_pull, **contacts_push, contacts_pull**, finish; `SyncOptions::contacts = true`; `SyncReport::contacts_counts`, `contacts_push_counts`; `problem_area` may be `"contacts"`; partial-outcome rule extended so that a failing contacts step with a working mail or calendar step is `Partial` |
| `mail_types.h` | `SyncState::problem_area` comment lists `contacts` |
| `log_codes.h` | `contacts.pull`, `contacts.push`, `contacts.conflict`, `contacts.rejected`, `contacts.unreadable` (counts and codes only; rule R8 extended) |

## 3. Errors (stable keys)

`no_addressbook` (discovery found none: informational), `read_only`, `unreadable`, `in_conflict`, `delete_pending`,
`book_gone`, `empty`, `email_invalid`, `too_long`, `too_many`, `card_too_large`, `locked_changed`, `duplicate_uid`, `not_found`,
`write_failed`, plus the transport keys of the CalDAV client (`auth_failed`, `server_unreachable`, `cert_*`, `too_large`,
`protocol_error`, `malformed_xml`, `server_error`, `forbidden`). The bridge maps a key to a translated text; no key or detail
holds a name, number, address or server name (FR-027).

## 4. Logging (FR-027, Principle IV)

`log::event("contacts.pull", {count("books"), count("changed"), count("removed"), count("unreadable")})` and the like.
**No** log key or value holds a name, organization, number, email, note, address-book name, server address, file name
or path. Rule R8 of `scripts/check-constitution.sh` forbids the log-key words `contact`, `phone`, `email`, `book`, `org`,
`note` (the event *code* `contacts.pull` is a code, not a key) and a self-test proves the rule fires.

## 5. Threading

Unchanged: the store is used from the UI thread and the sync worker as today (`test_store_threads` extended with a
contact write during a pull). `pull_contacts` and `push_contacts` run on the sync worker; list pages, search and
suggestions are plain synchronous store queries bounded by `LIMIT` (one page, at most 100 rows).
