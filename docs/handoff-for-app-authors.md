# Handing a text to PB Office (for authors of other PocketBook apps)

The InkPad One has no system clipboard, so text cannot be copied from one app and pasted into another. An app can instead start PB Office with the text as a parameter;
PB Office then opens a new message with it. This page is the interface. It is the same as `specs/003-receive-shared-text/contracts/handoff-contract.md`.

**Status of what is claimed here:** see the table at the end. The case "PB Office is not running" follows what was observed on 2026-10-23 for a stand-in app; the case
"PB Office is already running" is **not verified on the reader yet**.

## The call

```c
const char* const argv[] = {
    "pbo-mail-v1",                  // marker and version: required, always first
    "text=<the text>",              // optional: becomes the body of the message
    "textlen=<n>",                  // required when text= is present: the byte length of the text, in decimal
    "subject=<one line>",           // optional
    "to=<one plain address>",       // optional
};
int rv = OpenBook2("/mnt/ext1/applications/pboffice.app", 5, argv, 0);
```

- Each argument is `key=value`; the value is everything after the **first** `=` (it may contain `=`, quotes, line breaks, any UTF-8).
- At least one of `text`, `subject`, `to` must have content. Unknown keys are ignored; a known key given twice makes the whole hand-over malformed.
- Only these three values can be handed over: no Cc or Bcc, no several recipients, no display name, no attachment, no folder, no account, no action.
- PB Office never replies and never learns who called.

## Limits (PB Office enforces them; keep to them)

| Value | Limit | When exceeded |
|-------|-------|---------------|
| text | 8192 bytes of UTF-8 as sent (provisional: lowered to the limit measured on the reader, never below 6000 bytes, which is 1500 characters of any script) | the whole hand-over is refused and the user is told; nothing is created |
| `textlen` | must equal the byte length of the text as received | refused as incomplete: the text was cut or changed on the way |
| subject | 300 characters | the subject is left out, the rest arrives, the user is told |
| to | 254 bytes, exactly one plain `local@domain` address | the recipient is left out, the rest arrives, the user is told (without the address) |

Control characters and invalid UTF-8 are replaced by U+FFFD; line ends become a line feed; the subject becomes one line. The text is plain text: nothing in it is run,
opened or fetched, and web addresses in it are only shown.

## What the sender must do

1. Keep to the limits. A text that is known to be longer than 8192 bytes is not handed over; say so on your own screen.
2. **Check the return value of `OpenBook2`.** 0 means the request was passed on. Anything else means the system did not start or reach PB Office: tell your user and keep
   the text on your screen (PB Office never learns of it).
3. Do not keep the text longer than needed and **never write it to your own log**: a share link can be a key to the user's files.
4. Hand over only what the user chose to mail: everything ends up in a draft and, at the next sync, in the Drafts folder of their mail server.
5. Expect PB Office to come to the front, your app to stay open in the background, and your app to be shown again when the user leaves PB Office.

## What PB Office does

- It saves a draft first, then shows it: nothing is lost if the app is killed. The user adds what is missing and sends it like any message; **nothing is ever sent
  without the user's tap**, however much you handed over.
- A message the user was writing is saved first and stays as its own draft. Two hand-overs are two drafts.
- Without a mail account the hand-over waits (in memory only) while the account set-up is open, and is dropped with a notice if the set-up is left.
- The text is not written to any log of PB Office or its launcher and is not left in the command line of a long-running program. While it travels it waits for
  seconds as a file in `system/pboffice/handoff/` (the card is FAT: it cannot be made private there); an entry that nobody read is removed after 10 minutes.

## Status of the statements

| Statement | Status |
|-----------|--------|
| `OpenBook2` delivers each argument unchanged to a started `.app`, up to 1500 bytes, with non-ASCII letters, quotes and line breaks | observed 2026-10-23 with a stand-in target (not PB Office) |
| The calling app keeps running and is shown again after the target ends | observed 2026-10-23, stand-in target |
| PB Office parses, cleans, bounds and saves as described | tested on the host (`test_handoff_parse`, `test_handoff_bridge`, `test_handoff_header_injection`, `tst_handoff_flow`) |
| The text is not in a process list or log after the launcher's `exec` | tested on the host with a fake launcher layout; **unverified** on the reader (DT-HO-8) |
| Arguments reach a **running** PB Office | **unverified** (DT-HO-2, DT-HO-3) |
| The limit of 8192 bytes arrives complete | **unverified** (DT-HO-4) |
| The sender is shown again after the user leaves a running PB Office | **unverified** (DT-HO-5) |

To try the interface without writing an app, use the test sender in the package (`INSTALL.md`, "Hand-over probe").
