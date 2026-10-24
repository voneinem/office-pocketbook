# Contract: handing text to PB Office (v1)

**Feature**: [spec.md](../spec.md) | **Plan**: [plan.md](../plan.md) | **Research**: [R-02, R-03, R-05, R-07](../research.md)

This is the interface between **any app on the reader** (the sender) and PB Office (the receiver). The receiver is built in this
feature; a sender is another project's work (for example the "Mail this link" button of PB Cloud) and follows this page.
Status of every statement about the reader's behaviour: see [Status](#status-of-the-statements-here).

## 1. How the sender calls

```c
const char* const argv[] = {
    "pbo-mail-v1",                 // marker and version: required, always first
    "text=<the text>",             // optional
    "textlen=<n>",                 // required when text= is present: the byte length of the text, in decimal
    "subject=<one line>",          // optional
    "to=<one plain address>",      // optional
};
int rv = OpenBook2("/mnt/ext1/applications/pboffice.app", 5, argv, 0);
```

- The path is the installed launcher of PB Office. The flags are `0` unless experiment E1 ([research.md](../research.md))
  shows that another value is needed to reach a running PB Office (open item O-3).
- Keys are `key=value`. The value is everything after the **first** `=` (a text may contain `=`, quotes, line breaks, any UTF-8).
- Every field is optional, but at least one of `text`, `subject`, `to` must have content after cleaning; otherwise no message is made.
- Unknown keys are ignored (a later version may add keys). A key given twice makes the whole hand-over **malformed**.
- Only the three values above can be handed over. There is no Cc, Bcc, several recipients, display name, attachment, folder,
  account or action (spec Assumptions).

## 2. Limits and cleaning (receiver side; the sender should respect them)

| Value | Limit | When exceeded | Cleaning |
|-------|-------|---------------|----------|
| text | 8192 bytes of UTF-8 as sent (provisional: lowered to the measured system limit by experiment E4, never below 6000) | the whole hand-over is refused; the user is told ("too long"); nothing is created | invalid bytes and control characters except line feed and tab become U+FFFD; `\r\n` and `\r` become `\n` |
| `textlen` | must equal the byte length of the text as received | refused ("incomplete"): the text was cut or changed on the way | none |
| subject | 300 characters after cleaning | the subject is left out; the rest arrives; the user is told | as the text; then line feed, carriage return and tab become one space |
| to | 254 bytes; exactly one plain `local@domain` address | the recipient is left out; the rest arrives; the user is told (without the address) | none; no trimming of anything but surrounding blanks is guessed |

The text is **plain, untrusted text** in every case: nothing in it is run, opened, fetched or interpreted; a web address in it is
only shown (FR-011).

## 3. What the sender must do

1. Keep to the limits above. A text that the sender knows is longer than 8192 bytes is not handed over; the sender says so.
2. **Check the return value.** `OpenBook2` returns 0 when the request was passed on. Any other value means that the system did not
   start or reach PB Office: the sender tells its user and keeps the text on its own screen (PB Office never learns of it).
3. **Not keep the text longer than needed** and never write it to its own log (the reader's logs are readable by the user, and a
   share link can be a key).
4. Expect that PB Office comes to the front and that the sender stays open in the background; when the user leaves PB Office the
   sender is shown again (observed for a stand-in target; for PB Office: **unverified**).
5. Never pass a password or secret that the user did not choose to put into a mail: everything handed over ends up as a draft and,
   at the next sync, in the user's Drafts folder on the mail server.

## 4. What PB Office does with it

| Situation | Behaviour | Spec |
|-----------|-----------|------|
| PB Office is not running | the launcher takes the arguments into a private pending entry and executes itself again without them; the app reads the entry, saves a draft, removes the entry, shows the editor | US1, FR-001..003, FR-016 |
| PB Office is running (case B1) | the running app receives the arguments through a request listener, saves a draft, shows the editor; no file is written | US2, FR-005 |
| PB Office is running (case B2) | the second launcher writes the entry and wakes the running app (or restarts it); the app reads the entry | US2, FR-005 |
| An unsent message is open | it is saved first and stays as its own draft; the new message opens separately | FR-006 |
| No mail account | while the set-up page is shown the hand-over waits and continues after the set-up; otherwise the user is told the text was not kept | FR-008 |
| Refused hand-over | no message; a notice with a key, never with the content | FR-009, FR-012 |

PB Office never replies to the sender and never learns who it is.

## 5. Contract v2 (only if experiments E1 and E2 show case B3)

If the reader drops the arguments of a second start of a running app, a start with arguments cannot carry the text. Then the sender:

1. writes the same files (`0`, `1`, ... one raw argument each) into `system/pboffice/handoff/<id>.tmp/`, renames the folder to
   `<id>`, and
2. calls `OpenBook2(<pboffice.app>, 1, {"pbo-mail-v1"}, 0)`, with no content in any argument.

PB Office reads the folder when it comes to the front; a start with the marker and no entry shows "nothing was handed over"
(FR-009). This is the only variant in which another app writes into PB Office's data folder; it writes only into `handoff/`, and
PB Office removes what it finds. It is **not** built unless needed (open item O-1 of the research).

## Status of the statements here

| Statement | Status |
|-----------|--------|
| `OpenBook2` delivers each argument unchanged to a started `.app`, up to 1500 bytes, with non-ASCII letters, quotes and line breaks | observed 2026-10-23 with a stand-in target (not PB Office) |
| The caller keeps running and is shown again after the target ends | observed 2026-10-23, stand-in target |
| Arguments reach a **running** PB Office through a request listener (case B1) | **unverified**; SDK strings suggest it (research R-01) |
| The text survives to the limit of 8192 bytes | **unverified** (E4) |
| The arguments are not visible to other programs after the launcher's `exec` | by design; **unverified** on the reader (E3) |
| Return of the sender after the user leaves a running PB Office | **unverified** (E5) |
