# Contract: Bridge objects and QML pages (additions)

**Feature**: [../spec.md](../spec.md) | **Research**: [../research.md](../research.md) R-10, R-12, R-13 | Extends
[the bridge contract of spec 001](../../001-email-calendar-mvp/contracts/bridge-qml.md); its common rules apply (bridges own no
business rules, no blocking beyond one page, translated keys, empty and error states, e-ink rules of section 3).

## 1. Bridge objects

| Object | Responsibility | Key properties | Key invokables / signals |
|--------|----------------|----------------|--------------------------|
| `ContactsModel` | the paged contact list and search | rows: `displayName`, `subtitle`, `bookName` (empty when only one book is shown), `pendingMark` (`""`, `notSynced`, `failed`, `conflict`, `deletedOnServer`, `deleting`), `errorKey`, `unreadable`; `pageCount`, `page`, `total`, `query`, `state` (`ok`, `noAccount`, `noAddressbook`, `empty`) | `refresh()`, `page(n)`, `search(text)` (runs when the field is confirmed), `clearSearch()` |
| `ContactBridge` | one contact: view, edit, delete, resolve | view: `displayName`, `organization`, `phones[]`, `emails[]`, `addresses[]` (each with `label`), `note`, `bookName`, `readOnly`, `readOnlyReason`, `pending`, `failedReason`, `conflict`, `deleting`; edit: `edit` map (same fields; each phone, email, address entry and the note carries `locked` and `origIndex`), `writableBooks[]` (writable and shown books only), `validationErrors[]` (`field`, `key`) | `open(id)`, `newContact()`, `loadEdit()`, `save(map)` (validates in the core, on success emits `saved(id)`), `remove()` (QML asks first), `undoRemove()`, `retry()`, `discardChange()`, `resolve(choice)`, `recreate()`, `discardServerDeleted()` |
| `SyncBridge` (changed) | global state | `pendingCount` also counts waiting contacts; `hasPending` includes `Store::pending_contacts()`; a saved / deleted contact arms the Auto Sync timer exactly like an event | `noteLocalChange()` called by `ContactBridge` after `saved` / `removed` / `undone` |
| `ConflictsModel` (changed) | "To decide" | contacts section: rows for `conflict` and `server_deleted` contacts with the same actions as events | `resolveContact(id, choice)` |
| `SettingsBridge` (changed) | settings | `addressBooks[]` (`id`, `name`, `shown`, `writable`) | `setAddressBookShown(id, bool)` |
| `ComposeBridge` (changed) | recipient suggestions | `suggestions` rows carry `label` and `fromContact`; shown as "Name <address> (label)" | unchanged call `suggest(word)` |
| `HomeBridge` (changed) | home screen | `contactsAvailable` (a calendar server account is configured; the Contacts page explains an empty or missing address book), `contactsPending` | none |

## 2. Pages and navigation

```text
Home ──► Contacts ──► Contact ──► ContactEdit
  │          ├──► ContactEdit (New)
  │          └──► (search result = the same list)
  ├──► Conflicts (adds: contact conflicts and "deleted on the server")
  └──► Settings ──► Address books (shown / hidden)
Compose: recipient field ─ suggestions (contacts first, then cached mail)
```

- **ContactsPage**: top bar (title "Contacts", offline mark, unsynced count, last sync as on every page); a search
  field (`InkField`, confirm key or Search button runs the search, a Clear button restores the list); the list of
  `InkRow`s, 8-10 rows per page by the baseline paging; `PageButtons`; footer `FooterButton` "New" (disabled with an
  explanation when no book is writable). Empty states: no account / no address book found on the server (`noAddressbook`:
  "No address book was found on the server."), empty book, no search result. Waiting changes are visible per row and as a
  count in the footer (FR-008, FR-028).
- **ContactPage**: name, organization, then phones, emails, addresses (each "label: value", plain text, no link, no call
  or mail action: FR-011), note; page buttons when the text is longer than a page; footer **Edit**, **Delete** (opens
  `ConfirmDialog` that names the contact; cancelling changes nothing); a banner for read-only (explains the reason),
  failed (reason, Retry / Edit / Discard), conflict (leads to "To decide"), deleted on the server, and "Deletion waiting:
  Undo" (FR-019).
- **ContactEditPage**: fields for given name, family name, organization; for phone, email and address a list of rows each
  with a label button (`ChoiceDialog`: the labels of [the vCard contract](carddav-vcard-subset.md) section 3), the value
  fields and a remove button, an "Add phone / email / address" button each; an entry or note that is over the input limit is a **read-only row** ("Too long to edit here, kept unchanged") with a remove button only; note; the address-book chooser when creating
  and more than one writable, shown book exists (the last used one is preselected, kept in `settings` key `contacts_last_book`);
  Save runs the core validation and shows the first error next to its field (`empty`: "A contact needs a name, an
  organization, a phone number or an email address."); Cancel asks only when something was changed.
- **Home**: a "Contacts" button (visible when a calendar server account is configured) and the waiting-changes line already shown.
- **Settings**: a section "Address books" listing each book with an on/off switch (like the calendar visibility).
- **Compose**: unchanged page; the suggestion popup shows the extra label text.

## 3. E-ink rules

All rules of section 3 of the spec 001 contract apply and are enforced by `scripts/check-ui-style.sh` as today: black and
white, large touch targets, no animation, no `Flickable` kinetic flicking, paged lists, one repaint per page change, text
input with the on-screen keyboard (the field kept above it). The search does not run on every key (research R-11). No
new widget kind, so no new style rule.

## 4. Language

English `qsTr` source texts and German entries in `qml/strings_de.tsv`; `scripts/extract-strings.py --check`
(`check_translations`) must pass. New texts (not exhaustive): Contacts, New contact, Edit contact, Delete contact?,
"Delete %1? The deletion is sent to the server at the next sync.", Name, Organization, Phone, Email, Address, Note, the
label names, Add phone/email/address, Search contacts, Clear, "Not synced", "Failed: %1", "Deleted on the server",
"Undo", "No address book was found on the server.", "This address book is read-only.", "Unreadable contact", the
validation messages, Address books.
