# Contract: CardDAV and vCard subset

**Feature**: [../spec.md](../spec.md) | **Research**: [../research.md](../research.md) R-01 to R-06, R-09 | Extends
[the CalDAV contract of spec 001](../../001-email-calendar-mvp/contracts/caldav-ical-subset.md)

What the app sends to, and expects from, a contact server, and which parts of vCard it understands. Server
behaviour is **unverified** (Nextcloud first, OpenCloud second) until `docs/live-server-checklist.md` records a
run; fixtures imitating servers carry `.unverified`. The rules of the CalDAV contract apply unchanged: HTTPS only,
Basic authentication, no user name or password in a URL, an `href` naming another host is ignored, bodies capped
(8 MiB in memory), every XML body through `xml_min`, `Depth` and `Content-Type` set explicitly.

## 1. Requests

| Step | Request | Expectation |
|------|---------|-------------|
| Principal | `PROPFIND <entered URL or /.well-known/carddav> Depth: 0` body `current-user-principal` | `207`; redirects stay on `https` and the same host for credentials |
| Home set | `PROPFIND <principal> Depth: 0` body `card:addressbook-home-set` | `207`; none -> error `no_addressbook` (not a failure of the sync) |
| Books | `PROPFIND <home> Depth: 1` body `resourcetype, displayname, d:sync-token, cs:getctag, d:current-user-privilege-set, d:supported-report-set` | `207`; a collection is an address book when `resourcetype` has `card:addressbook`; **writable** when privileges include `d:write-content` or `d:all` |
| Changes (preferred) | `REPORT <book> Depth: 1` `d:sync-collection` with the stored token, `d:sync-level 1`, `getetag` | `207` with changed `href/getetag` and removed `href` (status 404); `403` / `409 valid-sync-token` -> full list |
| Changes (fallback) | `PROPFIND <book> Depth: 0 getctag`; when changed `PROPFIND Depth: 1 getetag` (full list) | compare with the stored hrefs/etags; hrefs not listed are removed (rows with local work are not) |
| Fetch | `REPORT <book>` `card:addressbook-multiget` for the changed hrefs, batches of 30, at most 4 MiB per answer, with `D:getetag` and `card:address-data` listing the properties `VERSION UID FN N NICKNAME ORG TITLE TEL EMAIL ADR NOTE REV X-ABLabel` (so `PHOTO` etc. are left out; research R-05) | `207` with `address-data`; a server that ignores the list sends full cards: the ingest strips them |
| Pre-write read | `GET <href>` before the `PUT` of a **partial** contact | `200` with the full card and `ETag`; the ETag must equal the stored one or the write is a conflict |
| Create | `PUT <book>/<random>.vcf` `If-None-Match: *`, `Content-Type: text/vcard; charset=utf-8`; the path is stored with the row before the first attempt | `201` / `204`; `ETag` header stored, else the next pull learns it. A retry after a lost answer gets `412`; `GET <path>` then shows the app's own `UID` -> adopted, no second card |
| Update | `PUT <href>` `If-Match: "<etag>"` | `204` / `200` -> new `ETag`; `412` -> conflict; `404` -> `server_deleted` |
| Delete | `DELETE <href>` `If-Match: "<etag>"` | `204` -> removed; `412` -> conflict (the user's side is a deletion); `404` -> already gone (done) |

Writes are **always conditional** (Principle III). The resource name of a new card is `<32 hex digits>.vcf` (random
128-bit, never derived from a name, address or number).

### Status mapping (same keys as the CalDAV client)

| HTTP / situation | Core result |
|------------------|-------------|
| `401` | `Error{Auth, "auth_failed"}` |
| `403` on write | row stays `pending_*`, `error_key = refused` (`read_only` if the book is known read-only) |
| `404` on update | `server_deleted` |
| `409`, `412` | `PreconditionFailed` -> `conflict` |
| `413`, `507` | `error_key = too_large` / `storage_full_server` |
| `5xx`, timeouts | `Error{Server|Network}`, retry at the next sync; local state unchanged |
| `207` with a per-resource `5xx` | those cards skipped and retried; the others applied |
| malformed XML / vCard | that card stored as `unreadable` (or skipped when it has no usable `href`); the sync continues (FR-026) |

## 2. Fake CardDAV server (test contract)

`tests/support/fake_carddav_server.h`, TLS, same scaffolding as the fake CalDAV server. Scriptable: principals,
homes, address books (ctag, sync token, privileges, supported reports with / without `sync-collection`), cards (href,
ETag, vCard text, including photos), per-request fault injection (drop, delay, `412`, `404`, `403`, `413`, `507`,
`5xx`, malformed XML, redirect to another host), a hook that edits the server copy between the app's read and
write, a switch "ignores the `address-data` property list". Required scenarios:

| Scenario | Expectation (maps to) |
|----------|------------------------|
| discovery, two books, one read-only | books listed, writability correct (US1-1, FR-001, FR-017) |
| no address book on the server | `no_addressbook`, sync step reported as done, Contacts view explains (US5-4) |
| full download, then a change by token, then a removal | rows match the server; removed card removed (FR-002) |
| ctag fallback without `sync-collection` | same result as the token path |
| cards with photo, server honours / ignores the list | no photo in `vcf` either way; `partial = 1` (FR-002) |
| create, update, delete offline then sync | exactly one conditional `PUT` / `DELETE` each (FR-018, FR-020) |
| edit of a partial contact | one `GET`, then one `PUT` with the GET's ETag; the photo and the `BDAY`/`X-*` lines are still on the server (FR-015, SC-007) |
| same card changed on both sides | `412` or differing pre-`GET` ETag -> both versions kept (FR-021, SC-006) |
| card deleted on the server, edited locally | `server_deleted`, edited version kept (FR-022) |
| delete of a card changed on the server | not deleted; `conflict` with `deleting` (US4-4) |
| book removed on the server | contacts with local work kept as `server_deleted` (FR-022) |
| refusal `403` / `413` / `507` on write | `error_key` set, retry / edit / discard work (FR-024) |
| interruption after `PUT`, before the answer is processed | next push finds equal content, no duplicate (FR-004, SC-005) |
| redirect to another host | credentials not sent there (as `test_redirect_credentials`) |
| 20+ malformed cards in one answer | sync completes, each shown as far as readable or as unreadable (SC-008) |

## 3. vCard understood by the parser

Limits: card 256 KiB, 5,000 lines, line 64 KiB, 100 entries per kind, display strings cut at 64 KiB (the raw text keeps
everything). A card over the limits is `unreadable` (`too_large`).

| Property | Handling |
|----------|----------|
| `BEGIN:VCARD` / `END:VCARD`, `VERSION` | exactly one card per resource; 2.1 / 3.0 / 4.0 accepted for reading; a resource with more than one card is `unreadable` (shown as such, never edited, nothing lost) |
| `UID` | identity; absent or duplicate in the book -> `unreadable`, stored under the synthetic uid `href:<href>` |
| `FN`, `N` | display name and sort; `N` components: family, given, additional, prefix, suffix |
| `ORG` | first component (organization); the rest is preserved |
| `TEL`, `EMAIL`, `ADR` | each entry with its label (`TYPE=` in the 3.0 and 4.0 spellings, bare 2.1 types, `itemN.X-ABLabel`); `PREF` only orders; `ADR` parts street, city, region, postcode, country (PO box and extended address are kept in the text, shown joined to the street) |
| `NOTE` | text with newlines |
| `PHOTO`, `LOGO`, `SOUND`, `KEY`, any value over 64 KiB | removed at ingest, `partial = 1` |
| everything else (`BDAY`, `URL`, `TITLE`, `CATEGORIES`, `X-*`, `KIND`, `MEMBER`, `IMPP`, `RELATED`, ...) | not interpreted, **preserved** in the stored text and in every write |

Charsets: UTF-8; `CHARSET=` parameters of 2.1 cards and quoted-printable values decoded through the existing charset
tables (`charset.h`); an undecodable value is kept as raw bytes in the text and shown with replacement characters.

### Labels

Editable labels: phone `mobile`, `home`, `work`, `other`; email `home`, `work`, `other`; address `home`, `work`,
`other`. On read, `TYPE` tokens map to the nearest (`cell` -> mobile, `voice`/`pref`/`internet` ignored for the
label); any other text (`fax`, `pager`, an `X-ABLabel` such as "Mother") is shown as written and kept unchanged unless
the user changes that entry's label. Written as `TYPE=CELL|HOME|WORK` (phone), `TYPE=HOME|WORK` (email, address);
"other" writes no `TYPE`.

## 4. Writing contacts (lossless edit)

- `new_contact_vcf`: `BEGIN:VCARD`, `VERSION:3.0`, `PRODID:-//office-pocketbook//EN`, `UID:<32 hex>@pboffice.invalid`,
  `FN`, `N` (family;given;;;), `ORG`, one line per phone / email / address, `NOTE`, `REV`, `END:VCARD`; text escaped
  (`\\ \, \; \n`), lines folded at 75 octets, CRLF line ends.
- `apply_contact_edit(original_text, edit)`: rules of research R-06: unchanged entries keep their original lines byte for
  byte; changed entries are rewritten; removed entries take their whole group with them; added entries are appended; `FN`
  rewritten only when name or organization changed; `N` replaces only family and given; `REV` set; every other line is
  untouched. For a `partial` contact the function runs on the **fetched full card** (research R-05).
- Round-trip tests: parse -> edit one field -> serialize keeps the content-line multiset of everything else, for fixtures
  from several client styles (`.unverified`).
