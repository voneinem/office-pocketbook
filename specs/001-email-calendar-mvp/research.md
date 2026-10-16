# Research: Email and Calendar MVP (office-pocketbook)

**Feature**: [spec.md](spec.md) | **Plan**: [plan.md](plan.md) | **Date**: 2026-10-13

This file records what was found out before the design was fixed, and the decisions that follow. Each
finding carries an evidence label (constitution Principle IX):

- **[SDK]** checked by reading files of the PocketBook SDK on the development machine (header, library,
  `curl-config`, `strings`). Proves what is *built in*, not how a server or the device behaves.
- **[SPIKE]** observed on 2026-10-13 with a throw-away program (host libcurl 8.18.0) against a throw-away
  fake IMAP server written for the occasion. Not committed; it is re-done properly as the contract tests
  of tasks. Proves libcurl *can* do the thing, on the host.
- **[REPO]** read from this repository (the notes-pocketbook baseline) or its `docs/reference/`.
- **[UNVERIFIED]** assumed; needs the device, a real server, or both. Listed again in
  [Open items](#open-items-to-verify).

---

## R-01 Mail transport: libcurl for IMAP and SMTP (Phase-0 gate of the constitution)

The constitution (Technology Stack) requires that the device's libcurl IMAP/SMTP support is verified and
recorded here before the email design is fixed.

**Findings**

| Fact | Evidence |
|------|----------|
| SDK sysroot libcurl is **8.16.0**, shared `libcurl.so.4.8.0` and static `libcurl.a`, OpenSSL 3.5.4 backend | [SDK] `curl-config --version`, `curlver.h`, `libcurl.pc` |
| Built with `--enable-imap --enable-smtp --enable-pop3 --with-openssl --with-nghttp2 --enable-ares --with-libpsl`; **without** `--with-libssh2`, `--with-libgsasl`; NTLM disabled | [SDK] configure line printed by `curl-config --configure` |
| The library contains the IMAP and SMTP protocol handlers (`imap`, `imaps`, `smtp`, `smtps` scheme strings, `imap.c`, "Got unexpected imap-server response", "STARTTLS denied") | [SDK] `strings libcurl.so.4.8.0` |
| `CURLUSESSL_ALL`, `CURLOPT_MAIL_FROM/RCPT`, `CURLOPT_MAIL_RCPT_ALLOWFAILS`, `CURLOPT_LOGIN_OPTIONS` exist in the SDK header | [SDK] `curl/curl.h` |
| Default CA path of the SDK libcurl is `/etc/ssl/certs`; what the *device* has there is unknown | [SDK] configure line; [REPO] security review open item O1 |
| Authentication available without extra libraries: `LOGIN`, `PLAIN` (cleartext inside TLS), `XOAUTH2` bearer (unused, out of scope), CRAM-MD5/DIGEST-MD5 (built in, not needed) | [SDK] configure line (no gsasl); curl docs |

**Behavior of libcurl's IMAP handler** (host libcurl 8.18.0 against a throw-away fake server) [SPIKE]:

| Experiment | Result |
|------------|--------|
| `CURLOPT_CUSTOMREQUEST = LIST "" "*"` | Untagged `* LIST ...` lines arrive in the write callback, tagged status stripped |
| `STATUS INBOX (UIDNEXT UIDVALIDITY HIGHESTMODSEQ)` | Untagged `* STATUS ...` line delivered. This is the way to read UIDVALIDITY/UIDNEXT/HIGHESTMODSEQ, because the `SELECT` response that curl issues itself is not exposed |
| `UID FETCH 1:* (FLAGS) (CHANGEDSINCE 5)` | Command passed through unchanged; untagged `* n FETCH (...)` lines delivered. CONDSTORE modifiers therefore work through custom requests |
| `UID FETCH 1 (BODY.PEEK[HEADER])` | The response line `* 1 FETCH (UID 1 BODY[] {98}` followed by the literal bytes arrives; the closing `)` was *not* delivered. A response parser must tolerate a missing closing parenthesis and rely on the literal length |
| URL form `imap://host/INBOX;UID=1` | Returns exactly the raw RFC 5322 message, ideal for streaming into a size-limited buffer or file. `;SECTION=` selects a body part |
| URL form `INBOX;UID=1;UIDVALIDITY=99` with a different real UIDVALIDITY | Fails with `CURLE_REMOTE_FILE_NOT_FOUND (78)`: usable as a cheap "mailbox was reset" check, but the same code also means "no such UID", so the sync engine confirms with `STATUS` |
| `UID STORE 1 +FLAGS (\Flagged)` as custom request | Works; the resulting untagged FETCH is delivered |
| Upload (`CURLOPT_UPLOAD`) to `imap://host/Sent` | Issues `APPEND Sent {n}` with the given bytes; returns success. `[APPENDUID ...]` from the tagged reply is not exposed |
| Each new easy handle opens a new connection and runs `CAPABILITY` + `LOGIN` (+ `SELECT` per mailbox change); one easy handle reused for the whole sync keeps the connection | Observed in the fake server's log |

**Decision**: use libcurl (IMAP custom requests + URL-addressed fetches + `CURLOPT_UPLOAD` for APPEND, and
the SMTP handler for sending) behind two small interfaces, `ImapTransport` and `SmtpTransport`
([contracts/core-interfaces.md](contracts/core-interfaces.md)). A response parser of our own interprets the
untagged lines (about 300 lines: atoms, quoted strings, literals, parenthesized lists, `BODYSTRUCTURE`).

**Rationale**: libcurl is already a constitution-approved dependency, is linked on the device, has the
protocols compiled in, and already provides TLS verification, STARTTLS enforcement and redirect/credential
rules that the notes baseline relies on. No new dependency, no new binary size.

**Alternatives considered**

| Alternative | Why not (now) |
|-------------|---------------|
| Minimal IMAP client over OpenSSL (own socket, own state machine) | Fully controllable (pipelining, `IDLE`, `[APPENDUID]`, exact SELECT data), but 1,500+ lines of security-critical network code to write and test, and TLS verification, timeouts, proxies and STARTTLS would be re-implemented. Rejected for v1 (Principle V). **It stays the documented fallback**: `ImapTransport` is the seam, and the decision is revisited only if the device probe (R-02) shows that the device libcurl lacks IMAP or behaves differently from the host |
| libetpan, VMime, mailcore2, c-client | Extra dependencies (licenses, size, ARM build, maintenance). VMime is GPL (excluded by Principle VII). Rejected |
| POP3 | Cannot meet the folder, flag and move requirements |

**Known limitations accepted** (and where the design copes with them)

1. `curl` does not expose the tagged reply, so `[APPENDUID]` is unavailable: the Sent copy is verified, if
   at all, by `UID SEARCH HEADER Message-ID` (R-09). `[UIDVALIDITY]` and `HIGHESTMODSEQ` are read with `STATUS`.
2. No `IDLE`, no pipelining: not needed, because the app must not hold connections or poll (Principle VI).
3. A custom request is one command at a time; throughput is bounded by round trips. Commands without
   literals take UID sets; commands that return literals address one UID (item 6) and the SC-014 estimate in R-06 still holds.
4. Compile-time `CURLUSESSL_ALL` semantic is what the constitution calls "enforced STARTTLS" (R-12).
5. **Minimum libcurl version: 8.12.0** (decided 2026-10-14 after the device probe: the reader ships libcurl 8.12.0, the SDK's 8.16.0 is only what the app is built against; a 8.16.0 floor would have blocked all mail on the reader). `connect()` refuses an older run-time library
   (`curl_version_info()->version_num`), because older releases had STARTTLS-handling defects. The behavior
   above was observed with 8.18.0 on the host; the device runtime is checked by `pbo-probe --imap-selftest` (R-02).
6. **A response with several literals is not delivered completely** [TEST 2026-10-14, host libcurl 8.18.0 against the fake server]:
   `UID FETCH 1:3 (... BODY.PEEK[HEADER.FIELDS (...)])` returned only the first message; the rest stayed in libcurl's buffer
   and appeared at the start of the NEXT command's answer (a `UID SEARCH` then returned a FETCH line instead of its own result).
   One literal-bearing FETCH per command works, and commands without literals (`UID FETCH 1:3 (UID FLAGS)`, `SEARCH`,
   `STATUS`, `LIST`) take sets. Decision: header and body fetches address ONE UID per command (about 1 request per
   message instead of 1 per 50; the SC-014 estimate in R-06 has room for it), and a returned item whose UID was not asked for
   is dropped. [UNVERIFIED on the reader: libcurl 8.12.0; T054 re-runs it there.] A full IMAP client of our own would remove the
   limitation (the documented fallback).
7a. **libcurl's URL form for parts (`;UID=n;SECTION=s`) fetches `BODY[s]` without `PEEK`** [TEST 2026-10-14; the fake server models RFC 3501
   6.4.5 and marks the message read]: real servers set `\Seen` for it. Attachments are therefore streamed from a custom
   `UID FETCH n (BODY.PEEK[s])` request (possible with libcurl 8.18.0, R-01 item 8): the `{N}` literal is recognized in the write
   callback, a literal over the limit is refused before a byte is taken, and the bytes are decoded into a file while they arrive.
7. **`APPEND` carries no flags and no date** [TEST 2026-10-14, 8.18.0]: the server log shows `APPEND INBOX {39}`. The earlier
   assumption that libcurl adds `(\Seen)` is wrong for this version. Flags are set afterwards (contracts/imap-smtp-subset.md section 2).
8. **Custom requests drop the contents of literals in libcurl 8.12.0 and 8.16.0** [TEST 2026-10-14, versions built from the
   official sources and run against the fake server]: a `UID FETCH` custom request answers `... BODY[...] {43}\r\n` and nothing more
   (no literal, no closing parenthesis). 8.18.0 delivers the literal. The reader ships 8.12.0 (device probe), the SDK has 8.16.0.
   **Decision (owner, 2026-10-14): the app carries libcurl 8.18.0**, built for ARM from the unmodified source with
   `scripts/build-curl-arm.sh` (pinned SHA-256, release-key signature) and linked statically; the run-time floor is 8.18.0
   (`kMinCurlVersionNum`). Rejected: an own IMAP client over OpenSSL (more code, kept as the fallback), and workarounds on the
   reader's libcurl (ENVELOPE lists plus URL fetches that mark mail read on the server). Unverified on the reader until T054.

## R-02 Device probe before the first device build

[UNVERIFIED] The SDK's libcurl is what the app is *built* against. The device image's own `libcurl.so.4`
(the one used at run time, since `libinkview.so` itself lists it as NEEDED [SDK]) may differ in version and
protocols. Decision: extend `scripts/probe-device.sh` (exists in the baseline [REPO]) with a tiny probe
binary `pbo-probe` (built with the same SDK toolchain; prints `curl_version_info()` protocol list and
features, the device time zone sources, file-system type and `chmod` behavior of the data directory,
`/proc/meminfo`, free storage, whether gconv modules exist, whether `/etc/ssl/certs` is usable). Its output
is recorded under `docs/device-findings.md` when obtained. **No device-dependent decision is final until
that file exists**; the code paths that depend on it (R-05 charset, R-13 credential file, R-16 time zone)
each have a conservative fallback that works without the probe result. The probe also carries
`pbo-probe --imap-selftest <host> <port> <ca.pem>`, which runs the connection, `LIST`, `STATUS`, header and
`;UID=` fetch, `APPEND` and missing-parenthesis checks of `test_imap_transport` against a server on the LAN
(`pbo_fake_imap --listen 0.0.0.0`), so the libcurl *behavior* on the device (not only its protocol list) is
checked; a failing self-test re-opens R-01 and the OpenSSL-client fallback.

## R-03 Incremental IMAP sync model

**Decision**

- Per folder the store remembers `uidvalidity`, `uidnext`, `highestmodseq` (NULL if CONDSTORE is absent) and
  `window_low_uid`.
- `CAPABILITY` is requested once per sync (custom request) to learn `CONDSTORE`, `UIDPLUS`, `MOVE`,
  `SPECIAL-USE`, `LITERAL+`, `ENABLE`, `UTF8=ACCEPT` (ignored), `STARTTLS`/`LOGINDISABLED`.
- Sync of one folder:
  1. `STATUS (UIDNEXT UIDVALIDITY HIGHESTMODSEQ MESSAGES)`.
  2. If `UIDVALIDITY` differs from the stored one: **rebuild** (R-04).
  3. New mail: `UID SEARCH SINCE <date>` (initial) or `UID FETCH <stored uidnext>:* (...)` (incremental),
     restricted to the sync window.
  4. Flag changes: with CONDSTORE, `UID FETCH 1:* (FLAGS) (CHANGEDSINCE <modseq>)`; without it,
     `UID FETCH <low>:* (FLAGS)` for the windowed range.
  5. Deletions on the server: `UID SEARCH ALL` (windowed range) compared with the local UID set; missing
     UIDs are removed locally unless the message has a pending local change (then the "could not apply"
     note of FR-019, spec clarification).
  6. Headers in batches of 50 UIDs per command; header and `BODYSTRUCTURE` come together.
- No QRESYNC (`ENABLE QRESYNC` changes untagged response formats and curl's handling of them is unverified);
  CONDSTORE alone is enough for flags, and the UID-set comparison covers expunges. Revisit only if SC-014
  is missed on a real server.

**Rationale**: UID + UIDVALIDITY (+ optional CONDSTORE) is the minimum that is idempotent and resumable;
every step commits its result and its cursor in one SQLite transaction, so interrupting at any point and
repeating gives the same state (Principle III).

**Alternatives**: full re-list per sync (too slow for 5,000 headers, wasteful on e-ink battery); QRESYNC
(more complex, less supported); `IDLE` (forbidden by Principle VI).

## R-04 UIDVALIDITY change and mailbox reset (Principle III)

**Decision**: when UIDVALIDITY changes, all synced messages of that folder are marked `stale` (not
deleted), the folder cursor is reset, and the window is re-downloaded. Each downloaded message is matched
with a stale row by `Message-ID` header (+ date + size as tie-break); a match transfers the local
state (pending flags, pending move/delete) to the new row, the stale row is then removed. Stale rows that
carry pending local work and found no match are kept visibly flagged "no longer on the server" until the
user dismisses them. Drafts, outbox, and unsynced events are independent tables and are never touched.

**Alternatives**: delete and re-download everything (violates FR-019/FR-032 for pending flags); trusting
UIDs across a reset (corrupt state).

## R-05 MIME parsing and charsets

**Findings**

- glibc `iconv.h` and `usr/lib/gconv/` (255 converter modules) exist in the SDK sysroot [SDK]; there is no
  separate libiconv. glibc loads `gconv` modules at run time from the *device* root file system, which may
  not have them [UNVERIFIED, probe R-02]. ICU 73 exists in the sysroot too [SDK], but only as a Qt
  dependency; the core must stay Qt-free (Principle II) and adding ICU as a direct dependency is not
  justified.

**Decision**

- **In-house MIME parser** in `core/src/mime_*.cpp` (RFC 5322 headers with folding, RFC 2045-2049 structure,
  RFC 2047 encoded words, RFC 2231 parameters including continuation and charset, `quoted-printable`, `base64`,
  `7bit/8bit/binary`, `multipart/*` with boundary search, `message/rfc822` treated as an attachment only).
  Hard limits: nesting depth 8, parts 200, header block 64 KiB, single line 1 MiB, decoded text part 2 MiB
  (truncated with a visible note), total input 32 MiB; anything over a limit is treated as unreadable, never
  crashes (FR-040, Principle I).
- `multipart/alternative`: prefer `text/plain`; else `text/html` converted to text; else the last
  alternative that is readable. Inside `multipart/mixed` the text parts before the first attachment form
  the body; `Content-Disposition: attachment` and non-text parts become attachments with name, size, type.
- **Charset conversion**: built-in tables for UTF-8, US-ASCII, ISO-8859-1/-2/-15 (and aliases), Windows-1250/1252,
  UTF-16/32 (BOM-less and with BOM). These cover the spec's "German umlauts, Western and Eastern
  European sets" with no external module. Any other charset goes through glibc `iconv` when it is
  available (`iconv_open` succeeds); otherwise the text is decoded as Windows-1252 with undecodable bytes
  replaced visibly (U+FFFD) and the message is marked "charset not supported".
- Output is always valid UTF-8 (reuse the baseline `utf8` helpers [REPO]).

**Rationale**: a small parser is easier to bound and fuzz than a general library; the subset needed to *read*
mail and to *build* simple plain-text mail is small. The only risky piece, base64/QP decoding and recursion,
is covered by the malformed-input corpus (R-17).

**Alternatives**: GMime (LGPL-2.1, pulls GLib; ARM build and size unjustified), mimetic (MIT, old, C++ heavy,
unmaintained), mailio, Qt's `QMailMessage` (Qt in core is forbidden), libetpan.

## R-06 Fetching strategy for bodies and attachments

**Decision**

- The first batch command fetches `UID FLAGS INTERNALDATE RFC822.SIZE BODYSTRUCTURE
  BODY.PEEK[HEADER.FIELDS (FROM TO CC SUBJECT DATE MESSAGE-ID IN-REPLY-TO REFERENCES)]` for up to 50 UIDs.
- From `BODYSTRUCTURE` the readable parts are chosen (R-05 rule). The readable part is fetched with
  `UID FETCH <uid> (BODY.PEEK[<section>]<0.2097152>)`: at most 2 MiB (SC-005's 1 MB message fits).
  Messages whose chosen part is larger are stored truncated with a "message continues on the server" note.
  The text is decoded and (for HTML) converted at once; **only the readable text is stored**, never the
  original HTML (smaller, and nothing active is kept on the device, Principle IV).
- Attachments are listed from `BODYSTRUCTURE` (name, size, type, section) and fetched only on tap, through the
  URL form `imap://host/<folder>;UID=<n>;SECTION=<s>`, streamed into a file with a hard byte limit
  (the attachment limit, default 10 MiB, FR-010). The transfer is aborted as soon as the running total
  exceeds the limit; no file stays behind on failure (pattern of the baseline `save_body_to` [REPO]).
- If `BODYSTRUCTURE` cannot be parsed (hostile or unusual server), fall back to
  `BODY.PEEK[]<0.262144>` and parse the MIME locally.

**Throughput estimate against SC-014** (500 messages in under 10 minutes) [UNVERIFIED]: ceiling is round
trips. 10 header batches + 500 body fetches + folder commands is about 520 requests; at an assumed
150 ms per request on home Wi-Fi that is about 80 s, with a large reserve. Batching several UIDs that share
the same section number into one `UID FETCH` is a later optimization and is not needed to meet SC-014.

## R-07 HTML to text

**Decision**: in-house tolerant tokenizer and converter (about 400 lines): skips `head/script/style/svg/
template`, comments and CDATA; block elements produce line breaks, `li` bullets, `h1-h6` blank-line-framed
headings, `blockquote` quote markers (so quoted text is distinguishable, FR-008), `br`, `hr`, tables as
one line per row with ` | ` between cells; `a href` becomes `text [n]` and the list of `[n] address` is kept
with the message (links visible as text, never opened, FR-009); `img` is replaced by its `alt` text only if
present; entities: all numeric plus the ~250 HTML 4 named entities; depth and output size capped
(same limits as R-05). **Nothing is ever fetched** and no style or script is interpreted.

**Alternatives**: libxml2's HTML parser (present in the SDK sysroot [SDK]; larger, 2.13 has a history of
parser CVEs, not in the default dependency set); a web engine (forbidden by Principle IV); stripping tags
with a regex (breaks on malformed input).

## R-08 Composing and sending mail

**Decision**

- The core builds RFC 5322 messages itself: headers (`Date`, `From`, `To`, `Cc`, `Subject` as RFC 2047
  encoded words when non-ASCII, `Message-ID`, `In-Reply-To`, `References`, `MIME-Version`, `User-Agent`
  omitted), body `text/plain; charset=utf-8` with `Content-Transfer-Encoding: quoted-printable`, CRLF
  line ends, lines of at most 76 characters. Replies quote the original with `> `; forwards inline the original
  text ("-----Original Message-----" block). `Bcc` is used only for the SMTP envelope and never written to
  the message headers.
- `Message-ID` is generated once at queue time (`<random-128-bit.pbo@<sender-domain>>`), stored in the
  outbox row and in the bytes; the **raw message is stored in the outbox row** so a retry sends identical
  bytes and the id stays unique.
- SMTP through libcurl: `CURLOPT_MAIL_FROM`, `CURLOPT_MAIL_RCPT` list, `CURLOPT_UPLOAD` with a read callback,
  `CURLOPT_MAIL_RCPT_ALLOWFAILS = 0` (any refused recipient aborts the send before `DATA`: a *certain*
  failure). Authentication uses the same user name/password as IMAP unless the user sets separate SMTP
  credentials.

**Outcome-unknown rule (FR-015, Principle III)**: the read callback sets `data_fully_sent = true` when it
has returned the last byte. A transport failure (timeout, reset, receive error) **before** that flag is a
certain failure (`failed`); **after** it, the outcome is unknown (`uncertain`). Independently, the
outbox row is moved to `sending` (committed, synchronous=FULL) **before** the first SMTP byte is written.
If the app dies in `sending`, the next start can only conclude "unknown" and moves the row to `uncertain`.
A `250` reply after `DATA` is the only path to `sent`. Auto-resend never happens from `uncertain`; the user
chooses between "send again" and "delete". Pressing Send twice is blocked because the transition
`queued -> sending` is a single conditional `UPDATE ... WHERE state='queued'` and the draft-to-outbox
conversion is unique on the draft id.

**Alternatives**: SMTP `CHUNKING/BDAT`, `DSN` (unverified support, no need); a locally generated `.eml`
export for manual rescue (out of scope).

## R-09 Sent copy (spec clarification: default APPEND, per-account switch)

**Decision**

- After SMTP `250`, the outbox row becomes `sent_copy_pending` (committed). If the account switch
  "server files sent mail itself" is off, the sync does `APPEND <Sent> (\Seen) <date> <raw message>`
  by upload (R-01) and then removes the outbox row. A failed APPEND keeps the row in `sent_copy_pending`;
  only the APPEND is retried, never the SMTP send.
- To avoid a duplicate when an APPEND succeeded but its reply was lost, before retrying the APPEND the sync
  runs `UID SEARCH HEADER Message-ID <id>` in the Sent folder; a hit completes the row.
- Switch presets: on for hosts matching Gmail (`imap.gmail.com`) and Outlook.com/Office 365
  (`outlook.office365.com`, `imap-mail.outlook.com`); off otherwise. [UNVERIFIED] That these servers file
  copies on SMTP submission is widely documented behavior but is not checked against live accounts here.
- When the switch is on, the app never appends. The Sent folder content arrives with the normal folder sync.

**Alternatives**: always APPEND (duplicates on Gmail/Outlook); never APPEND (no Sent copy on Dovecot-type
servers); a local-only copy (user data not in the account). Decided by the user in clarification.

## R-10 Special folders and Gmail labels

**Decision** (spec clarifications): roles from `LIST` attributes (`\Sent`, `\Drafts`, `\Trash`, `\Junk`,
`\All`, `\Archive`; RFC 6154) when present; otherwise names, compared case-insensitively after removing
prefixes `INBOX.` and `INBOX/`: `Sent`, `Sent Items`, `Sent Messages`, `Gesendet`, `Gesendete Elemente`,
`Gesendete Objekte`; `Drafts`, `Entwürfe`, `Entwurf`; `Trash`, `Deleted Items`, `Deleted Messages`,
`Papierkorb`, `Gelöscht`, `Gelöschte Elemente`; otherwise the account setup asks once and stores the choice
in `account_mail.role_*`. Gmail needs no code path: its `[Gmail]/All Mail` and `[Gmail]/Spam` carry `\All`
and `\Junk` and are created with `sync_enabled = 0`; all other labels are ordinary folders.
Folder names are modified-UTF-7 decoded for display (RFC 3501 §5.1.3), own decoder (about 60 lines).

## R-11 Draft mirroring (spec clarification: drafts are mirrored to the server)

**Clock check (clarification 2026-10-13, analysis finding I12)**: the `APPEND` of a Sent copy or a draft
carries **no date argument**, so the server stamps the message. At the end of the APPEND the sync runs
`UID SEARCH HEADER Message-ID "<id>"` and `UID FETCH <uid> (INTERNALDATE)` once (two extra commands in the same
session; curl hides `[APPENDUID]`) and passes the stamp to `note_server_time`, which warns above 600 s.
Accounts whose server files sent mail itself (no Sent APPEND) get a stamp only from draft mirroring or from a
calendar server's response time; with no stamp there is no warning (spec Edge Cases).

**Decision**: the device is the master while a draft is being edited. The draft row is saved locally first,
at most 2 s after the last keystroke and at every page leave/app suspend (SC: at most seconds lost, story 3).
At sync: a changed draft is `APPEND`ed to the Drafts folder as a new message (`\Draft \Seen`, same
`Message-ID`), the previous server copy (known `server_uid`) is then `UID STORE \Deleted` and expunged with
`UID EXPUNGE` (needs UIDPLUS; without it the old copy is only flagged and hidden, never mass-expunged).
Order is APPEND first, delete second, so an interruption leaves two copies, never zero. A server-side draft
that the user did not write here (foreign `Message-ID`) is shown as an ordinary message in the Drafts folder
view and can be opened for reading; editing a foreign draft is out of scope. A draft changed both locally
and on the server (same `Message-ID`, different content hash seen since the last mirror) becomes two drafts
(spec FR-013). Sending or deleting a draft (after confirmation) marks its server copy for deletion at the
next sync.

**Alternatives**: local-only drafts (rejected by user); mirror on every keystroke (battery, network
against Principle VI).

## R-12 TLS policy and endpoint rules (Principle IV)

**Decision**: one shared `CurlSession` configuration used by IMAP, SMTP and HTTP(S) transports:

- `CURLOPT_SSL_VERIFYPEER=1`, `CURLOPT_SSL_VERIFYHOST=2`, `CURLOPT_CAINFO=<effective CA file>` from the
  baseline `ca_selector`/`build_effective_ca` [REPO] (device store if usable, else bundled Mozilla bundle,
  plus `custom-ca.pem`), `CURLOPT_SSLVERSION=TLSv1.2 or newer`. Re-verified by the existing
  `scripts/check-constitution.sh` rule R1, extended (R7) with `CURLOPT_USE_SSL` set to anything but
  `CURLUSESSL_ALL`, plus plain `imap://`/`smtp://` literals outside the enforced-STARTTLS factory.
- Allowed schemes per transport: `imaps` and `smtps` (implicit TLS, ports 993/465); `imap` and `smtp` only
  together with `CURLOPT_USE_SSL=CURLUSESSL_ALL` (enforced STARTTLS, ports 143/587): curl then fails the
  transfer if the server does not offer STARTTLS or the upgrade fails, and does not send credentials before
  the upgrade. A fake server that refuses STARTTLS must produce `starttls_unavailable` with zero credentials
  seen (contract test). **[UNVERIFIED]** until `test_starttls_refused` passes: the claim that curl sends no
  credentials before the upgrade is documented libcurl behavior, not yet observed here (the spike used plaintext
  `imap://` in a scratch directory only).
- `CURLOPT_PROTOCOLS_STR`/`REDIR_PROTOCOLS_STR` restrict each transport to its own scheme; IMAP/SMTP follow no
  redirects; HTTP redirects stay `https` only and never carry credentials to another host (baseline test).
- Error mapping (`CURLE_PEER_FAILED_VERIFICATION`, `CURLE_SSL_CACERT_BADFILE`, `CURLE_LOGIN_DENIED`,
  `CURLE_COULDNT_RESOLVE_HOST`, `CURLE_COULDNT_CONNECT`, `CURLE_OPERATION_TIMEDOUT`, ...) to
  `ErrorClass` plus a cause key: `cert_untrusted`, `cert_expired`, `cert_host_mismatch` (distinguished with
  `CURLINFO_SSL_VERIFYRESULT` values `X509_V_ERR_CERT_HAS_EXPIRED` etc.), `auth_failed`, `server_unreachable`,
  `no_network`, `timeout`, `protocol_error`, `starttls_unavailable`.
- Timeouts: connect 15 s, per-command low-speed limit (1 byte/s for 30 s), whole command 120 s; whole sync is
  cancelable at command boundaries.

**Not used**: `CURLOPT_SSL_OPTIONS` revocation bypass flags, `CURLOPT_PROXY` (none configured), TLS 1.0/1.1, and any libcurl tracing (`CURLOPT_VERBOSE`, `DEBUGFUNCTION`,
`STDERR`): the trace contains the `LOGIN`/`AUTH` password. Mechanical rule R10 forbids it; `CURLOPT_ERRORBUFFER`
text is never copied into `Error.detail` because it contains host names.

## R-13 Credential storage on the device (Principle IV requires the evaluation to be recorded)

**Options evaluated**

| Option | Assessment |
|--------|------------|
| A. Password in the SQLite database (notes baseline) | Works, but the database is copied for migration backups, may be attached to bug reports, and the freed pages keep old secrets unless `secure_delete` is on |
| **B. Separate file `credentials` in the app data directory, mode 0600, directory 0700, written atomically (temp + rename + fsync); never in the database, logs, backups or error text** | **Chosen** (user decision: "plain text in a file readable only by the app"). Easy to delete ("Forget account" overwrites then unlinks), keeps the database shareable |
| C. Obfuscation (base64/XOR) | Rejected: false sense of security |
| D. Passphrase/PIN-derived encryption (PBKDF2 + AES-GCM with OpenSSL, already linked) | Technically cheap, but needs a PIN at every start; rejected by the user for e-ink usability. Can be added later (format has a version byte) |
| E. OS keystore | The SDK has none [SDK: no keyring libraries, no secret-service]; [UNVERIFIED] on the device |

**Honesty limits** [UNVERIFIED]: `/mnt/ext1` on PocketBook readers is usually a FAT-family file system
mounted for all users, where `chmod` has no effect. The app therefore **checks at run time** whether the
mode it set is observable (`stat` after `chmod`); if not, the account setup page and the About page say
"this storage cannot protect the file: anyone who connects the reader over USB can read it". The text is
chosen from the measured result, not assumed. Whether a better location exists (an internal ext4
partition writable by the app) is a probe item (R-02); until then the data directory stays
`/mnt/ext1/system/pboffice` like the baseline.

The password is held in memory only while a sync or connection test runs and is zeroed afterwards
(`OPENSSL_cleanse`). Redaction of secrets in logs is inherited from the baseline logger [REPO].

## R-14 CalDAV with libcurl

**Findings**: the baseline `CurlTransport` supports GET/HEAD/POST/PUT/DELETE over HTTPS with Basic auth,
redirect and credential rules and size limits [REPO]. libcurl accepts any method name through
`CURLOPT_CUSTOMREQUEST` and any request body through `CURLOPT_POSTFIELDS`/upload, so `PROPFIND` and
`REPORT` need an extended request type, not a new library. HTTP/2 via nghttp2 is available [SDK] but
unnecessary.

**Decision**: extend `HttpTransport::Request` with a free `method` string (already a string), `Depth`
via headers, and a body for any method; keep the single TLS-verifying `CurlTransport` for HTTPS endpoints.
CalDAV uses (details in [contracts/caldav-ical-subset.md](contracts/caldav-ical-subset.md)):

1. Discovery: `PROPFIND /.well-known/caldav` or the entered URL, `current-user-principal`, then
   `calendar-home-set`, then `PROPFIND Depth:1` for the calendars (`resourcetype`, `displayname`,
   `supported-calendar-component-set` (VEVENT required), `getctag`/`sync-token`, `current-user-privilege-set`
   for writability).
2. Capability: `DAV:supported-report-set` decides whether `sync-collection` (RFC 6578) is used; otherwise
   ctag + `calendar-query` returning only `getetag`, then `calendar-multiget` for changed hrefs.
3. Reading events: `REPORT calendar-query` with `time-range` [today-30d, today+365d] (the spec window); the
   server returns master events whose recurrences touch the window. No server-side `expand`.
4. Writing: `PUT` with `If-None-Match: *` (create) or `If-Match: "<etag>"` (update); `DELETE` with
   `If-Match`. HTTP `412` (and `404` on update) is a conflict; `403/507/413` is a server refusal; any
   `2xx` stores the returned `ETag` (if missing, an immediate `HEAD`/`PROPFIND` fetches it).
5. A `Location` after PUT is ignored unless it is on the same host; credentials are never sent elsewhere.

**Server targets**: Nextcloud first, OpenCloud second [UNVERIFIED]. Nothing in the plan hardcodes a server
product: behavior is decided by capability detection. Nextcloud-specific facts to verify live: `well-known`
redirect, `sync-token` support, handling of `If-Match` on edited recurring events. For OpenCloud/oCIS the
CalDAV availability itself is [UNVERIFIED] and gets a "not available" error path with a clear message.

**Read-only feed** (spec: calendar feed address): plain `GET` with `If-None-Match`/`If-Modified-Since`,
size limit 8 MiB, `https` only (webcal:// is rewritten to https://), parsed with the same iCalendar code and
marked read-only.

**Alternatives**: libical/libcurl-based `libcaldav` (stale, GPL-ish), `neon`/`libdav` (extra deps).

## R-15 XML for WebDAV

**Decision**: a small in-house non-validating XML reader (`core/src/xml_min.cpp`, ~350 lines): elements,
attributes, namespaces (prefix resolution for `DAV:`, `urn:ietf:params:xml:ns:caldav`, Apple/Nextcloud
extension namespaces tolerated and ignored), text and CDATA, the five predefined entities and numeric
character references. **DTDs, external entities and processing instructions are rejected**, so XXE and
entity-expansion attacks are impossible by construction. Depth cap 64, document cap 8 MiB, node cap
200,000. Builds request bodies from fixed templates (no general writer).

**Alternatives**: libxml2 (in the SDK [SDK]; powerful, but a dependency to justify and to keep hardened),
expat (also in the SDK [SDK]; small and fine, but still a new dependency; its entity-expansion protection
would need configuration). The in-house reader is the smaller risk for a fixed protocol subset.

## R-16 iCalendar, recurrence and time zones

**Decision**

- **iCalendar parser in the core** (RFC 5545 subset): content-line unfolding, parameters with quoting, text
  unescaping, `VEVENT`, `VTIMEZONE` (kept, mostly ignored), `VALARM` (kept, ignored: no reminders in this
  release), properties `UID, DTSTAMP, DTSTART, DTEND, DURATION, SUMMARY, DESCRIPTION, LOCATION, RRULE, RDATE,
  EXDATE, RECURRENCE-ID, STATUS, ATTENDEE, ORGANIZER, TRANSP, SEQUENCE, LAST-MODIFIED, CLASS`. Unknown
  properties are preserved verbatim.
- **Edit without loss**: the raw `VCALENDAR` text is stored; an edit patches only the changed properties
  in the raw text and bumps `SEQUENCE`/`DTSTAMP`; unknown properties, `VALARM`s and other components
  survive the round trip (important for events created in other clients).
- **RRULE expansion** in the core for `FREQ=DAILY|WEEKLY|MONTHLY|YEARLY`, `INTERVAL`, `COUNT`, `UNTIL`,
  `BYDAY` (with ordinals in monthly/yearly), `BYMONTHDAY`, `BYMONTH`, `BYSETPOS`, `WKST`, plus `EXDATE`, `RDATE` and
  `RECURRENCE-ID` overrides (moved and cancelled instances). Expansion is bounded to the window and capped
  (at most 5,000 occurrences per event). Unsupported parts (`SECONDLY/MINUTELY/HOURLY`, `BYWEEKNO`,
  `BYYEARDAY`, `BYHOUR/BYMINUTE/BYSECOND`, `RSCALE`) yield a single occurrence at `DTSTART` plus the visible
  note "repeat rule not fully supported" (spec: unusual data is shown as far as possible).
- **Evaluate libical**: MPL-2.0/LGPL-2.1 dual license, allows MIT distribution, but ~150-200 kB of code
  plus its own time-zone handling, needs `perl`/host code generation steps in some versions (cross-build
  friction) [UNVERIFIED version-specific], and its zone handling would conflict with the bundled-IANA
  decision (R-16 time zones). Estimated reuse gain is small because the spec only needs the subset above.
  Rejected; revisited only if the subset proves insufficient on real calendars.

**Time zones (spec clarification: bundled IANA database, never the device's)**

- Data source: the IANA tz database (public domain) pinned in `third_party/tzdata/` (release `2026c` is what
  the development host carries [`/usr/share/zoneinfo/tzdata.zi` header, host check]; the repository pins
  whatever release `scripts/update-tzdata.sh` imports and records its version and SHA-256 in
  `third_party/tzdata/VERSION`). The script builds **compact ("slim") TZif v2+ files** with `zic -b slim`
  for the canonical zones plus their links and commits the result, so builds need no `zic`. Estimated size
  about 400 kB [UNVERIFIED until built]; installed under `applications/pboffice-zoneinfo/`.
- A reader for TZif (RFC 9636): transitions plus the POSIX-TZ footer string for dates beyond the table
  (`M3.5.0`-style rules, ~200 lines). It resolves local-to-UTC and UTC-to-local, with the RFC 5545
  conventions: a **nonexistent** local time (spring-forward gap) is interpreted with the offset before the
  gap; an **ambiguous** local time (fall-back) is the first occurrence; recurring events are generated in
  local wall-clock time of their `TZID` and converted per occurrence, which keeps "same local time on each
  date" (spec story 2 scenario 6).
- Names: IANA names are used directly. `TZID` values like `W. Europe Standard Time` (Outlook) go through
  a compact built-in mapping table (about 40 common Windows names -> IANA; facts, not copyrightable
  text; source noted in `NOTICE`); an unknown name is treated as floating with a visible "time zone
  unknown" note. Embedded `VTIMEZONE` definitions are **not** interpreted in v1 (YAGNI; the server's
  IANA-named zones cover Nextcloud, Google, Apple); this is a stated limitation.
- The user's own zone (FR-023, clarification 2026-10-13): 1) the choice stored in Settings, 2) the reader's
  setting when readable (`TZ`, then the target of `/etc/localtime`), 3) otherwise **no zone**: the first time a
  calendar is used the app asks the user to pick one from a list and remembers it. Until a zone exists the
  calendar views show "Choose your time zone" and no times; the app never displays times in an assumed zone,
  and there is no UTC fallback. InkView exposes no time-zone API (`inkview.h` searched [SDK]), so (2) is the
  expected path where it works; whether the device keeps `/etc/localtime` current is [UNVERIFIED] and covered
  by the probe (R-02).
- Because rules change (the database date is shown in About), a **time-zone data version** is stored in
  settings; when it differs from the compiled-in version, the occurrence cache is rebuilt.

## R-17 Storage design

**Decision**: one SQLite database `pboffice.db` (SQLite 3.54.0 vendored, `SQLITE_THREADSAFE=1` [REPO]),
`journal_mode=TRUNCATE`, `synchronous=FULL`, `foreign_keys=ON`, `secure_delete=ON` as in the baseline [REPO];
one-way migrations driven by `PRAGMA user_version` with a backup before upgrade and an integrity check
after, and a hard refusal of a database with a newer `user_version` (constitution). Full schema in
[data-model.md](data-model.md). Points that differ from the notes baseline:

- **Full-text search**: SQLite FTS5 (already inside the vendored amalgamation, 2,778 `fts5` references
  [REPO]) enabled with `SQLITE_ENABLE_FTS5`, `unicode61 remove_diacritics 2`
  tokenizer, external-content table over `message` (subject, sender, recipients, body text). Prefix queries
  (`word*`) make "a word" search fast for 5,000 headers (SC-004); substring-in-the-middle search is not
  offered. Cost: index size roughly 30-60% of text; counted in the storage budget.
- **Occurrence cache** for calendars (R-16): materialized for the window, rebuilt per event on change.
- **Bodies as text** only; downloaded attachments are saved in the reader's Downloads folder under their real
  (made safe) file name (decision of the owner, 2026-10-14: the reader's own viewer can open them); they are the
  user's files: the app never overwrites, deletes or evicts them. The `attachment` row keeps the path. File names stay out of logs.
- **Eviction** (SC-013, FR-032): the limit applies to the database (attachments saved in Downloads are the user's
  files and not counted). Order: body text of the oldest synced messages (headers kept,
  body re-fetchable online); then oldest synced messages outside the window. Rows with any pending local
  state, all drafts, all outbox rows, and events with `sync_state <> 'synced'` are never selected. If the
  protected set alone exceeds the limit, the app warns and stops downloading.
- **Crash safety**: every local user action is one transaction committed before any network call;
  every sync step commits data and cursor together.

## R-18 Sync engine

**Decision**: a single `SyncRunner` executes an ordered list of idempotent steps; each step has a name,
runs inside its own transactions and records progress in the store:

1. `outbox`: send `queued` items; complete `sent_copy_pending` items (R-08, R-09).
2. `mail_push`: for each folder with pending work, **first** the `UidvalidityGuard` (a `STATUS`, comparing
   `UIDVALIDITY` with the stored value, failing closed; Principle III: keyed by UID *and* UIDVALIDITY). On a mismatch no UID-addressed command is sent for
   that folder (no `STORE`, `COPY`, `MOVE`, `EXPUNGE`, no draft-mirror delete); the folder is rebuilt (R-04) and
   the pending operations are re-applied to the re-matched messages afterwards. Then: draft mirror (R-11),
   flags, moves, deletes (pending message state).
3. `mail_pull`: folders (R-03), per-folder window, deletions, eviction.
4. `calendar_push`: pending event creates/updates/deletes with conditional requests.
5. `calendar_pull`: ctag/sync-token, changed hrefs, window, occurrence cache.
6. `finish`: record outcome, counts, the first problem found.

Outgoing work goes first so that the user's actions land even when the pull fails later; a failure in one
step is recorded and the next independent step still runs (mail and calendar are independent, FR-035).
Cancellation is checked between commands; the app killed at any point resumes with the same steps because
nothing is held only in memory. The `SyncScheduler` from the baseline [REPO] (pure decision logic, no clock)
is kept; its input `has_pending` now means "outbox item, pending message change, draft to mirror or pending
event" and `quiet_ms`/`gap_ms` keep their defaults (4 s/15 s). `has_pending` only feeds the status display: the
arming rule is separate (FR-030) - saving a draft (autosave, page leave, suspend) never arms Auto Sync; only a
send, an event save and an event delete do, and a pending draft mirror goes with the next sync that starts for
another reason.

**Expunge policy (Principle III, analysis finding K2)**: `UID EXPUNGE` is only ever sent (a) for the source of
a move the user requested, after the `UID COPY` succeeded, and (b) for the superseded server copy of the user's
own draft, after the new `APPEND` succeeded; in both cases exactly the one UID that the app itself superseded,
never a bare `EXPUNGE`. Neither removes user content that exists nowhere else, so no extra confirmation is
asked; every deletion of content the user can still want (Trash delete, permanent delete, events) keeps its
confirmation. A reviewer who reads the constitution's "expunge" more strictly can require a confirmation
instead; the tests (`test_sent_copy`, `test_draft_mirror`, `test_move_variants`) pin the narrow behavior either way.

**Mail flag merge (spec clarification)**: each message stores `flags_base` (last server state) and
`flags_local`. Push sends only the differences between `flags_local` and `flags_base`
(`UID STORE +FLAGS/-FLAGS`), then reads the resulting flags back; flags the user did not change follow the
server. A UID that vanished or moved yields `local_error = 'gone_on_server'` or `'moved_on_server'`, shown as
"could not apply" and acknowledged by the user; nothing is dropped silently.

**Move**: `UID MOVE` when `MOVE` is advertised; else `UID COPY` + `UID STORE \Deleted` + `UID EXPUNGE` when
`UIDPLUS` is advertised; else `UID COPY` + `\Deleted` only (the original is hidden locally and never
mass-expunged) [UNVERIFIED on live servers]. **Delete**: move to the Trash role folder; with no Trash folder
the app first asks for the separate permanent-delete confirmation and then uses `\Deleted` + `UID EXPUNGE`
(or only `\Deleted` without UIDPLUS).

**Calendar conflicts (spec clarification)**: push uses `If-Match`. A `412`, a changed etag on the
pre-push check, or a `404` for an update produces `conflict` (both versions kept; server copy fetched and
stored in `event_conflict`) or `server_deleted` (the edited event kept, marked). The user resolves with
**keep mine** (re-PUT against the new etag), **keep the server's** (discard mine after confirmation) or
**keep both** (store mine as a new event with a new UID). A removed calendar turns its unsynced events into
`server_deleted` the same way.

## R-19 UI architecture and e-ink

**Decision** (details in [contracts/bridge-qml.md](contracts/bridge-qml.md)):

- Keep the layers of the baseline: `core/` (Qt-free) -> `bridge/` (QObject facades, background execution with
  results marshalled to the UI thread; the baseline bridges already do this for sync [REPO]) -> `qml/`
  (Qt Quick, pages and the existing generic widgets) -> `app/` (InkView glue, orientation, Wi-Fi connect).
- Pages (stack navigation with an always-visible top bar showing offline/unsynced/last-sync state):
  Home, Folders, MessageList, Reader, Compose, Outbox, Search, Agenda, DayView, MonthView (with a week
  mode), EventView, EventEdit, ConflictView, AccountSetup (mail + calendar tabs), Settings, About.
- Reuse from the baseline without change: `Style.qml`, `TopBar`, `BottomPanel`, `PageButtons`, dialogs,
  `InkField`, `InkSwitch`, `InkRow`, `DateTimePicker`, `EmojiPicker`, `SelectableText` (reader text
  selection), `IconButton`. Remove: Markdown block views, slash menu, attachment image views, note pages.
- Lists are paged with `PageButtons`; no `Flickable` kinetic scrolling (`scripts/check-ui-style.sh`
  already enforces this [REPO]); the reader pages by the baseline `page_target` logic (text never cut
  mid-line).
- Fonts: system text font as in the baseline, bundled monochrome Noto Emoji for emoji (SIL OFL, already in
  `third_party/noto-emoji` [REPO]).

**Budgets** (Principle V; all [UNVERIFIED] until measured on the InkPad One; device RAM is a probe item):

| Budget | Target | Source |
|--------|--------|--------|
| Cold start to home screen | < 5 s | SC-003 |
| Page turn in a 5,000-header list | < 1 s | SC-004 |
| Open a downloaded 1 MB message | < 2 s | SC-005 |
| Resident memory, browsing | <= 150 MB | design target |
| Resident memory peak during a 500-message sync | <= 250 MB | design target |
| Install package | <= 25 MB (binary, CA bundle 0.2 MB, emoji font 2 MB, zoneinfo ~0.4 MB, licenses) | design target |
| Local data | <= 200 MB default, user-changeable, shown in Settings | SC-013 |

## R-20 Test strategy

**Decision** (Principle I; framework is the baseline's `test_support.h` + CTest, labels per directory [REPO]):

- `tests/unit`: parsers (IMAP responses, `BODYSTRUCTURE`, MIME, RFC 2047/2231, charsets, HTML text,
  mUTF-7), iCalendar, RRULE, TZif/DST, XML, compose, store, migrations, eviction, outbox state machine,
  merge rules, scheduler, status.
- `tests/contract`: each wire exchange against **fake servers**; fixtures carry `.unverified` until a real
  server confirmed them.
- `tests/integration`: **in-process fake servers** (new `tests/support/fake_imap_server.h`,
  `fake_smtp_server.h`, `fake_caldav_server.h`) built on the baseline TLS harness [REPO] (throw-away PKI via
  the `openssl` CLI), so the *real* `CurlImapTransport`/`CurlSmtpTransport`/`CurlTransport` run end to end.
  Failure injection hooks: drop the connection after N bytes/N commands, delay a reply past the timeout,
  refuse STARTTLS, wrong/expired certificate, change UIDVALIDITY, expunge under the client, return `412`.
- **Interruption tests** (SC-006/SC-007): a harness that runs a core operation in a child process, kills
  it (`SIGKILL`) at a random point, reopens the store, and checks the invariants (no lost draft/outbox/
  event; at most one `250` accepted by the fake SMTP server per Message-ID; every `uncertain` visible).
  100 iterations per scenario in CI-less local gate, seeded and reproducible.
- **Malformed-input corpus** (SC-010, FR-040): at least 50 mail and 20 calendar items in
  `tests/fixtures/malformed/` plus generated variants (truncate at every offset of a seed message, byte
  flips with a fixed PRNG seed, nesting/size bombs); run in the normal test run and in a sanitizer
  build (`-DPBO_SANITIZE=ON`, ASan+UBSan). Optional libFuzzer harnesses are a later addition, not a
  requirement.
- **Idle network test** (SC-011): counts connection attempts on the fake servers while the scheduler runs
  with a fake clock.
- Fixtures: anonymized, reserved domains only (`scripts/check-fixtures.sh` already enforces this [REPO]);
  `.unverified` marker for hand-written server transcripts.
- **Device checklist** `docs/device-test-checklist.md`: every item marked "unverified" until run on the
  InkPad One, with the result and date recorded.

## R-21 Third-party components, licenses and packaging

**Decision**: components after this feature (manifest/NOTICE/About stay in sync, Principle VII):

| Component | Use | License | Status |
|-----------|-----|---------|--------|
| libcurl (system/SDK) | IMAP, SMTP, HTTPS | curl license | existing |
| OpenSSL (system/SDK) | TLS (via libcurl), random ids | Apache-2.0 | existing |
| SQLite 3.54.0 incl. FTS5 | storage, search | public domain | existing; compile flag added |
| Mozilla CA bundle | trust anchors | MPL-2.0 | existing |
| Noto Emoji | emoji font | OFL-1.1 | existing |
| Qt 6.8 (Core, Gui, Qml, Quick) | UI | LGPL-3.0 | existing |
| InkFeed patterns | build/launcher/packaging | MIT | existing attribution |
| IANA tz database | time zones | public domain | **new data asset** (see Complexity Tracking) |
| Windows-to-IANA zone name table | name mapping | facts (source: CLDR windowsZones, Unicode license) | **new**, noted in NOTICE |

Packaging (device layout, per baseline `scripts/package-device.sh` [REPO], renamed): `applications/pboffice.app`
(launcher), `pboffice.bin`, `pboffice-cacert.pem`, `pboffice-emoji.ttf` and OFL text, `pboffice-zoneinfo/`,
`pboffice-licenses/`, icons; registration in `system/config/desktop/view.json` as `U_PbOffice`; runtime data
under `/mnt/ext1/system/pboffice` (database, `credentials`, `attachments/`, `custom-ca.pem`, log). The new
names guarantee office-pocketbook never overwrites or shares data with notes-pocketbook (constitution
Technology Stack).

## Open items to verify

| ID | Item | How it gets verified |
|----|------|----------------------|
| O-1 | Device libcurl has IMAP/SMTP and behaves like the SDK's 8.16.0 | `pbo-probe` on the InkPad One (R-02); then the contract tests are re-run against the same binary if possible **Protocol list verified 2026-10-14 (8.12.0 has imap/imaps/smtp/smtps, `docs/device-findings.md`); behaviour still open (T054).** |
| O-2 | libcurl IMAP/SMTP behavior with Dovecot (primary test server), Gmail, Outlook.com, the author's provider | live checklist `docs/live-server-checklist.md`; fixtures stay `.unverified` until then |
| O-3 | Gmail/Outlook.com file Sent copies on SMTP submission | live check, per provider |
| O-4 | `SPECIAL-USE` attributes and folder names of the target providers | live `LIST` transcripts, anonymized into fixtures |
| O-5 | CalDAV: discovery, `sync-collection`, `If-Match` on recurring events on Nextcloud and OpenCloud | live checklist; capability detection protects meanwhile |
| O-6 | Device file system supports `chmod`; where a protected location exists | probe (R-13) **Answered 2026-10-14: vfat, `chmod 0600` not observable, no protected location found (`docs/device-findings.md`).** |
| O-7 | Device keeps a valid `/etc/localtime` or equivalent; fonts cover German and Eastern European letters | probe, device checklist **Partly answered 2026-10-14: `/etc/localtime` exists but its content is unchecked; fonts unprobed (`docs/device-findings.md`).** |
| O-8 | gconv modules present on the device | probe (R-05) **Answered 2026-10-14: gconv present, 16 charsets convert (`docs/device-findings.md`).** |
| O-9 | RAM, startup, page-turn, search, 5,000-header and 1 MB message timings | device checklist (budgets in R-19) |
| O-10 | SC-014 first-sync time on real Wi-Fi | device checklist |
| O-11 | `UID MOVE`/`UIDPLUS` availability and behavior without them | live checklist |
| O-12 | Wi-Fi reconnect, sleep and wake behavior during sync | device checklist |
