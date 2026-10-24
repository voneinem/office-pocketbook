# Data Model: Receive Shared Text

**Feature**: [spec.md](spec.md) | **Plan**: [plan.md](plan.md) | **Research**: [research.md](research.md)

**There is no schema change.** A hand-over becomes an ordinary draft (`draft` table, schema 2, unchanged), and nothing about a
hand-over is stored once the draft exists. The database stays at schema 2, so no migration and no backup are needed, and an older
build still opens the database.

## Entities

### Hand-over (transient, never stored as such)

One act of another app starting PB Office with the marker `pbo-mail-v1` ([contracts/handoff-contract.md](contracts/handoff-contract.md)).
It exists in three forms, in this order, and in no other:

| Form | Where | Life time |
|------|-------|-----------|
| **Arguments** | the system's start of the launcher, or a request to the running app | until the launcher has written the files and executed itself again (milliseconds), or until the listener copied them |
| **Pending entry** | folder `system/pboffice/handoff/<id>/` with files `0`, `1`, ... (one raw argument each) | from the launcher's write until the draft exists, at most 10 minutes; then removed |
| **Parsed hand-over** | in memory, `pbo::Handoff` | from parsing until the draft is saved (or the notice is shown) |

`pbo::Handoff` (core, [contracts/core-interfaces.md](contracts/core-interfaces.md)):

| Field | Type | Rules |
|-------|------|-------|
| `text` | UTF-8 string | cleaned (control characters, invalid bytes, line ends); at most 8192 bytes before cleaning (R-07) |
| `subject` | UTF-8 string | one line, at most 300 characters; may be empty |
| `to` | string | one plain address or empty |
| `notices` | set of keys | `subject_too_long`, `recipient_ignored` (the hand-over still produced a message) |
| `source` | enum | `file` or `listener`; only for the log |

A refused hand-over has no `Handoff`; it has a reason key (`too_long`, `incomplete`, `malformed`) and no content.

### Hand-over message (an ordinary draft)

A `pbo::Draft` made by `Store::save_draft`, the same record the compose page saves:

| Draft field | Value from a hand-over |
|-------------|------------------------|
| `kind` | `new` |
| `to_text` | the recipient, or empty |
| `cc_text`, `bcc_text` | empty |
| `subject` | the subject, or empty |
| `body` | the text, or empty when only a subject or recipient came |
| `message_id` | `pbo::make_message_id(random, own address)`; needs a mail account |
| `in_reply_to`, `refs`, `reply_to_message` | empty / 0 |

It has no link to the pending entry, no flag that it came from a hand-over, and no sender: from the moment it is saved it is
edited, mirrored to the server's Drafts folder, queued and sent by the unchanged rules of spec 001 (FR-003 of this spec). Because
nothing marks it, it can be deleted like any draft (with the usual confirmation); a text handed over twice is two drafts
(spec edge case).

### Pending entry (folder on the reader's storage)

```text
system/pboffice/handoff/
├── 1761212345-812.tmp/     being written by the launcher: the app ignores every name ending in .tmp
│   ├── 0                   the first argument after the marker, e.g. text=...
│   ├── 1                   textlen=...
│   └── 2                   to=...
└── 1761212401-977/         complete (renamed from .tmp): the app may read it
    └── 0 1 2 ...
```

- `<id>` is `<seconds since the epoch>-<process id>` made by the launcher; it is only a unique name, the app orders entries by name.
- File content: the raw bytes of one argument, no terminator, no escaping.
- Limits the reader enforces: at most 8 files, each at most 16384 bytes; an entry that exceeds them is refused (`malformed`) and removed.
- Permissions: the storage is vfat, so `chmod` has no effect (spec 001 device finding); the folder is created with `umask 077` where it
  has one. The text on the card is the exposure the user is told about (R-02) and is why entries live for seconds.

## State transitions

```text
              launcher                       app
  arguments ───────────▶ pending entry ───────────▶ parsed hand-over ───▶ draft saved ───▶ entry removed
      │  (marker ok)     (.tmp -> renamed)  (read, parse)   │     (Store::save_draft)    (HandoffInbox::remove)
      │                                                     ├──▶ refused (reason key) ──▶ entry removed, notice shown
      │                                                     └──▶ held (no account yet) ──▶ account ready: draft saved
      │                                                                                └─▶ set-up left: discarded, notice
      └─ no marker ──▶ dropped, ordinary start
  pending entry older than 10 minutes ──▶ removed unread, `handoff.expired` counted
  running app, case B1: arguments ───▶ (listener, queued to the Qt thread) ───▶ parsed hand-over   (no entry at all)
```

Invariant (Principle III): the entry is removed only after the draft is saved or the hand-over is refused; the text therefore
exists in at least one place at every moment until the draft exists, and in the two places (entry and draft) for a short time after
a crash, which at worst makes a duplicate draft.

## Validation rules

All rules are in [research.md](research.md) R-07 and are tested by `test_handoff_parse`; the table there is the single source.

## Existing data this feature reads or changes

| Data | Use |
|------|-----|
| `mail_account` | the own address for the Message-ID; presence decides "no account" |
| `draft` | written once per hand-over; read by the compose page and the Drafts list |
| `handoff/` folder | new, transient, under the app's data folder; no other app's folder is touched |
| log | four event codes, counts only (R-09) |
