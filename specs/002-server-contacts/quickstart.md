# Quickstart: build and validate contacts on the host

**Feature**: [spec.md](spec.md) | **Plan**: [plan.md](plan.md) | Builds on the
[quickstart of spec 001](../001-email-calendar-mvp/quickstart.md) (prerequisites, configure, build are identical).

Everything below runs on the host against in-process fake servers, **without the device and without any real account**.
The device and live-server checks at the end stay "unverified" until run. Data model: [data-model.md](data-model.md);
interfaces: [contracts/](contracts/).

## 1. Build and run the tests

```bash
cmake -S . -B build-host -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build-host -j
ctest --test-dir build-host --output-on-failure            # everything
ctest --test-dir build-host -R 'contact|vcard|carddav|migration_2'   # this feature only
scripts/pre-merge.sh                                         # the gate: build, tests, secret scan, constitution/fixture/UI/version checks
```

Each new test is written first and must be seen failing before the code it covers (Principle I); `tasks.md` orders
them that way. Tests using the fake CardDAV server print the "UNVERIFIED against real servers" banner.

## 2. Validation scenarios (each maps to spec stories; all are automated)

| # | Scenario | Test | Spec |
|---|----------|------|------|
| 1 | Discover address books, mark read-only ones, download all cards, browse offline in name order | `test_carddav_discovery`, `test_contacts_sync_pull`, `tst_contacts_flow` | US1, FR-001/002/007 |
| 2 | Search by name, organization, number, email (umlauts, case, digits only), offline | `test_contact_sort_search` | US1-3, FR-009 |
| 3 | Open a contact with several labelled entries and an unknown property | `test_vcard_parse`, `test_contacts_bridge` | US1-4/5, FR-010/015 |
| 4 | Create offline, kill the app, restart: contact present and marked "not synced"; sync: one conditional `PUT`, mark gone | `test_contact_push`, `test_contacts_kill` | US2, FR-018 |
| 5 | Refuse an empty contact and a bad email; choose the book | `test_contact_edit_validation`, `test_contact_edit_bridge` | US2-3/4/6, FR-013 |
| 6 | Edit offline; sync; photo and `BDAY` still on the server (GET-merge-PUT) | `test_contact_push`, `test_vcard_edit_roundtrip` (24+ card corpus, over-limit entries kept and removable) | US3-1/2, SC-007 |
| 7 | Same contact changed on the reader and on the server: both kept; resolve each way | `test_contact_conflict` | US3-3, FR-021, SC-006 |
| 8 | Server removed an edited contact / its whole book: edited version kept, re-create or discard | `test_contact_server_deleted` | US3-4, FR-022 |
| 9 | Delete with confirmation, cancel, undo before sync; delete of a server-changed contact becomes a conflict | `test_contact_push`, `test_contact_conflict`, `tst_contacts_flow` | US4, FR-016/019 |
| 10 | Read-only book refuses create/edit/delete with an explanation | `test_contacts_readonly` | US3-5, US4-5, FR-017 |
| 11 | Server refuses a write (403/413/507): contact kept, failed with reason, retry / edit / discard | `test_contact_failed` | FR-024 |
| 12 | Interrupted sync at every step, repeated: no duplicate, nothing lost (100 kills) | `test_contacts_kill`, `test_contacts_sync_pull`, `test_contact_push` (lost answer after a create: retry gets `412`, own card adopted, no second card) | FR-004, SC-005 |
| 13 | A change starts an Auto Sync after a few seconds; an idle app makes no connection | `test_auto_sync`, `test_idle_no_network` | US5, FR-003, SC-009 |
| 14 | Hide an address book: not listed, not suggested, nothing deleted; pending changes still pushed | `test_settings_bridge`, `test_contact_push` | US5-3, FR-025 |
| 15 | No address book on the server: explanation, sync not failed | `test_carddav_discovery`, `test_contacts_bridge` | US5-4 |
| 16 | 20+ malformed cards | `test_malformed_vcard` | FR-026, SC-008 |
| 17 | Certificate failures and redirects to another host | `test_tls_cert_failures`, `test_redirect_credentials` (extended to CardDAV) | FR-006, SC-010 |
| 18 | Recipient suggestions: contacts first, once, labelled; hidden book and pending deletion excluded | `test_address_suggestions`, `test_compose_bridge` | US6, FR-029/030 |
| 19 | 2,000 contacts: page turn, search, suggestions, storage growth, cold start | `test_contacts_perf`, `test_perf_budgets` | SC-004/011/012 (host proxy) |
| 20 | Schema 1 -> 2: every row survives, crash leaves schema 1, older build refuses 2 | `test_store_migration_2` | constitution (migration policy) |
| 21 | Logs hold no names, numbers, addresses, book names | `test_log`, log-capture assertions in `test_contacts_sync_pull` / `test_contact_push` / `test_contacts_bridge`, `check-constitution.sh` (R8 + self-test) | FR-027 |

## 3. Run the host app against a fake server (manual look)

`scripts/run-host-demo.sh` starts the host app with a throw-away data directory and the fake servers (the calendar demo
data gains an address book with fictional people). Never point the host app at a copy of the reader's database
(owner rule).

## 4. Device and live-server steps (UNVERIFIED until run)

Rows to add to `docs/live-server-checklist.md` (LV-K1 ... : discovery from each form of calendar address, `sync-collection`,
`address-data` list, create / edit / delete / conflict / photo kept, cards of other clients) and to
`docs/device-test-checklist.md` (DT-CT-1 the 1 -> 2 upgrade on the reader after the owner says "mounted" and keeps the
backup, DT-CT-2 list and search with a large book, DT-CT-3 keyboard use in the edit form, DT-CT-4 sync after an edit with
Wi-Fi off, DT-CT-5 memory and storage, DT-CT-6 find a contact and read its number in under 20 s, DT-CT-7 create a contact
with name, number and email in under 2 minutes). Install on the reader only after the owner says "Install!"; never touch PB Notes
or InkFeed files.
