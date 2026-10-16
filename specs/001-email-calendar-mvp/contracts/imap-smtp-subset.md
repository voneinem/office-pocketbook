# Contract: IMAP and SMTP subset

**Feature**: [../spec.md](../spec.md) | **Research**: [../research.md](../research.md) R-01, R-03, R-06, R-08, R-09, R-12

What the app sends to, and expects from, a mail server. The fake servers of the tests implement exactly this
subset (`tests/support/fake_imap_server.h`, `fake_smtp_server.h`); servers may do more, the app uses no more.
Behavior shown against real servers is **unverified** until recorded in `docs/live-server-checklist.md`;
hand-written transcripts in `tests/fixtures/` carry the `.unverified` suffix (Principle IX).

## 1. Connection and TLS

| Rule | Detail | Test |
|------|--------|------|
| Schemes | `imaps://host:993`, `smtps://host:465` (implicit TLS); `imap://host:143`, `smtp://host:587` **only** with `CURLOPT_USE_SSL = CURLUSESSL_ALL` | `test_imap_tls_policy`, `test_smtp_tls_policy` |
| Verification | `VERIFYPEER=1`, `VERIFYHOST=2`, CA = effective CA file, TLS >= 1.2; no option turns this off | `test_tls_cert_failures` (imap/smtp variants: expired, wrong host, unknown CA => no `LOGIN`/`AUTH` bytes reach the server) |
| STARTTLS refused | server without `STARTTLS` capability or answering `NO` => `Error{Tls,"starttls_unavailable"}`; **no** credentials sent, no plaintext retry | `test_starttls_refused` |
| `LOGINDISABLED` | treated as "no credentials over this connection" => `starttls_unavailable` | same |
| Redirects | none for IMAP/SMTP | by option set |
| Tracing | no `CURLOPT_VERBOSE`/`DEBUGFUNCTION`/`STDERR` (the trace contains the password); `CURLOPT_ERRORBUFFER` text is not copied into `Error.detail` | rule R10, `test_error_detail_redaction` |
| Minimum libcurl | 8.18.0: older versions drop the contents of IMAP literals (research R-01 item 8). The reader build links its own 8.18.0 statically (`scripts/build-curl-arm.sh`); older or unknown => `connect()` refuses with `libcurl_too_old` | `test_curl_session` |
| Timeouts | connect 15 s; low-speed 1 B/s for 30 s; command 120 s | `test_timeouts` (delayed reply) |
| Authentication | `LOGIN`/`AUTH PLAIN` over the verified channel with the stored password; `AUTH` failure (`NO`/`535`) => `Error{Auth,"auth_failed"}` and local data untouched | `test_auth_failed` |
| Dual-purpose errors | `CURLE_COULDNT_RESOLVE_HOST/CONNECT` => `Network`/`server_unreachable` if a network exists, `no_network` if the device reports Wi-Fi down | `test_error_mapping` |

## 2. IMAP commands used

All run as libcurl custom requests on a selected mailbox unless noted. "Mailbox" is always passed as the
`raw_name` of the folder (modified UTF-7), quoted by the transport.

| Purpose | Command | Notes |
|---------|---------|-------|
| Capabilities | `CAPABILITY` | needs: `IMAP4rev1`; optional: `CONDSTORE`, `UIDPLUS`, `MOVE`, `SPECIAL-USE`, `LITERAL+` |
| Folder list | `LIST "" "*"` | plus `LIST (SPECIAL-USE) "" "*"` when advertised; attributes give roles (research R-10) |
| UIDVALIDITY precondition | `STATUS "<m>" (UIDVALIDITY)` immediately before the first write of a folder in a sync | **Precondition of every row below that contains a UID** (`STORE`, `COPY`, `MOVE`, `EXPUNGE`, draft-mirror delete). A mismatch with the stored value: send nothing for that folder, rebuild it (research R-04), re-apply the pending operations to the re-matched messages. Test: `test_flag_merge`, `test_move_variants`, `test_draft_mirror` |
| Folder state | `STATUS "<m>" (UIDNEXT UIDVALIDITY HIGHESTMODSEQ MESSAGES UNSEEN)` | `HIGHESTMODSEQ` only if `CONDSTORE`; used for the UIDVALIDITY check and cursors |
| Window | `UID SEARCH SINCE <dd-Mon-yyyy>` | server date semantics (day granularity, server zone) accepted; messages slightly outside the window are harmless |
| All UIDs | `UID SEARCH ALL` (or `UID SEARCH UID <low>:*`) | server-side deletion detection |
| Headers | `UID FETCH <uid> (UID FLAGS INTERNALDATE RFC822.SIZE BODYSTRUCTURE BODY.PEEK[HEADER.FIELDS (FROM TO CC SUBJECT DATE MESSAGE-ID IN-REPLY-TO REFERENCES)])` | **one UID per command**: a response with several literals is not delivered completely by libcurl and the rest leaks into the next command (research R-01). Commands without literals (`FLAGS`, `SEARCH`, `STATUS`) take sets. Rows are committed 50 at a time |
| Flags changed | `UID FETCH <set> (FLAGS) (CHANGEDSINCE <modseq>)` or `UID FETCH <set> (FLAGS)` | CONDSTORE optional |
| Text part | `UID FETCH <uid> (BODY.PEEK[<section>]<0.2097152>)` | at most 2 MiB; `BODY.PEEK` so reading never sets `\Seen` |
| Attachment | `UID FETCH <uid> (BODY.PEEK[<s>])` as a custom request, the literal streamed by `fetch_to` into a sink | refused before the first byte when the announced literal exceeds the limit; the decoded size is limited again while writing. NOT the URL form `<m>;UID=<uid>;SECTION=<s>`: libcurl sends `BODY[...]` for it and real servers then set `\Seen` |
| Fallback body | `UID FETCH <uid> (BODY.PEEK[]<0.262144>)` | when `BODYSTRUCTURE` unusable |
| Set flags | `UID STORE <uid> +FLAGS.SILENT (<flags>)` / `-FLAGS.SILENT`, then `UID FETCH <uid> (FLAGS)` | only differences vs `flags_base`; never `FLAGS` replace |
| Move | `UID MOVE <uid> "<target>"` if `MOVE`; else `UID COPY` + `UID STORE +FLAGS.SILENT (\Deleted)` + `UID EXPUNGE <uid>` if `UIDPLUS`; else `UID COPY` + `\Deleted` only | plain `EXPUNGE` is **never** issued |
| Permanent delete | `UID STORE +FLAGS.SILENT (\Deleted)` + `UID EXPUNGE <uid>` (or `\Deleted` only without `UIDPLUS`) | only after the separate explicit confirmation |
| Append | upload to `<m>` with **no flags and no date** (libcurl sends neither; the server stamps the message, I12); afterwards `UID SEARCH HEADER Message-ID` finds it and `UID STORE +FLAGS.SILENT` sets Sent copy `(\Seen)` or draft mirror `(\Draft \Seen)` | no `[APPENDUID]` available (research R-01) |
| Find by id | `UID SEARCH HEADER Message-ID "<id>"` | duplicate avoidance (Sent copy, draft mirror) |

`UID EXPUNGE` is sent only for UIDs the app itself superseded, after the preceding `UID COPY`/`APPEND` succeeded (research R-18, expunge policy).

Never used: `IDLE`, `EXPUNGE` (bare), `CREATE/DELETE/RENAME` of folders, `SUBSCRIBE`, `X-GM-*` extensions,
`ENABLE`/`QRESYNC`, `SORT`/`THREAD`, `AUTHENTICATE` mechanisms other than `PLAIN`/`LOGIN`.

## 3. Response parsing rules (`imap_parse.h`)

- Input is exactly what libcurl delivers for the custom request (research R-01 table): untagged lines, with
  literals (`{n}` CRLF + n bytes). The closing `)` after a literal **may be missing**; parsers rely on the
  literal length and tolerate absence.
- Atoms, quoted strings with `\"` and `\\`, literals, `NIL`, nested parenthesized lists (depth <= 32),
  numbers (32-bit for UIDs; 64-bit for MODSEQ and sizes), modified UTF-7 names.
- A literal larger than the size the command asked for (for example more than 2 MiB for a text part) is
  cut and the item is marked truncated; the total input per command is capped (64 MiB) and over-long
  lines (> 1 MiB) abort the item, not the sync.
- Unknown untagged responses are ignored. A response that makes no sense for the command (`* 5 EXISTS` among
  FETCH data) is ignored. A command whose useful data is missing returns `Error{Protocol,"protocol_error"}` for
  that folder only; the sync continues with the next folder (FR-035).
- `parse_*` are total and deterministic; they have a corpus of malformed inputs (truncated at every offset,
  random bytes, deep nesting, huge counts) in `tests/fixtures/malformed/imap/`.

## 4. Fake IMAP server (test contract)

Scriptable state: folders with attributes, per-folder `UIDVALIDITY`, `UIDNEXT`, `HIGHESTMODSEQ`, messages
(UID, flags, modseq, raw bytes), capability set, per-command delay, drop-after-N-bytes, refusal of `STARTTLS`,
wrong/expired certificate. Hooks used by the contract/integration tests:

| Scenario | Expectation (maps to) |
|----------|------------------------|
| initial window sync, 30 days | only window messages downloaded; `older_on_server=1` set (US1-2, US1-6) |
| incremental sync with and without CONDSTORE | same final store (FR-031) |
| connection dropped after every Nth byte/command | repeated syncs converge; no duplicate rows, no gaps (Edge: network drops) |
| UIDVALIDITY changes | stale rows re-matched by Message-ID; pending flags carried over; nothing pending is lost (R-04) |
| message expunged on the server while a flag change is pending | `local_error='gone_on_server'`, visible note (FR-019) |
| message moved on the server | `moved_on_server` |
| flag changed on both sides | local wins only for the flags the user changed (spec clarification) |
| server without `UIDPLUS`/`MOVE` | no bare `EXPUNGE` is ever received |
| huge folder (50,000 UIDs) | only window downloaded; memory stays bounded; `older_on_server=1` |
| hostile `BODYSTRUCTURE`/literals | item marked unreadable; sync completes |
| APPEND succeeds but reply is lost | the retry first finds the Message-ID and does not append again |

## 5. SMTP

Flow per message (libcurl SMTP handler): `EHLO`, (`STARTTLS` when enforced), `AUTH PLAIN|LOGIN`,
`MAIL FROM:<envelope_from>`, `RCPT TO:<each>` (any refusal aborts: `CURLOPT_MAIL_RCPT_ALLOWFAILS=0`), `DATA`,
message bytes (dot-stuffed by libcurl), `.`, `QUIT`.

**Outcome rule** (FR-015, Principle III) implemented by `SmtpTransport::send`:

| Event | `SendOutcome` | Outbox transition |
|-------|---------------|-------------------|
| `250` after the final `.` | `Accepted` | `sending -> sent_copy_pending` |
| TLS, connect, auth failure; `5xx`/`4xx` for `MAIL`/`RCPT`/`DATA` start; size refusal | `CertainFailure` | `sending -> failed` (+ reason key) |
| Transport loss (timeout, reset, receive error) **after the read callback delivered the last byte**, with no final reply received | `Unknown` | `sending -> uncertain` |
| Failure before the read callback delivered the last byte | `CertainFailure` | `sending -> failed` |
| A final `5xx` reply received after `.` | `CertainFailure` (the server answered; nothing was accepted) | `failed` |
| App killed while `sending` | n/a | at next start `recover_interrupted()`: `sending -> uncertain` |

Reason keys shown to the user: `refused_recipient`, `auth_failed`, `cert_*`, `server_unreachable`,
`no_network`, `timeout`, `too_large`, `server_busy`, `rejected_by_server`. The text of a server reply is never
stored or shown if it can contain addresses; only the numeric class is kept in `error_detail`.

### Fake SMTP server hooks

Accept/refuse per recipient; reply after N ms; drop the connection after the 354, in the middle of the body, after the
final `.` but before the reply (the "unknown" case), after the reply; count accepted messages per `Message-ID`.
Required results: SC-007 — across all injection points and with repeated Send taps and retries, the server
**accepted a given Message-ID at most once** unless the user chose "send again" from `uncertain`; every
message whose outcome is unknown is shown as "may have been sent".

## 6. Sent copy and draft mirror exchanges

| Exchange | Contract |
|----------|----------|
| After `Accepted`, switch off | `UID SEARCH HEADER Message-ID` in Sent (only on retry) -> `APPEND Sent (\Seen) <raw>` (no date) -> outbox row removed |
| After `Accepted`, switch on | no `APPEND`; the outbox row is removed |
| `APPEND` fails | row stays `sent_copy_pending`; next sync retries `APPEND` only; **zero** further SMTP sessions |
| Draft changed | `APPEND Drafts (\Draft \Seen) <raw>` first; then `UID STORE +FLAGS.SILENT (\Deleted)` and `UID EXPUNGE <old uid>`; interruption between the two leaves two server copies, never none |
| Draft deleted/sent | old server copy removed with the same two commands |

## 7. Compatibility targets (unverified until run live)

Dovecot (primary test server), Gmail, Outlook.com/Office 365, and the author's own provider. Provider
presets only set host names, ports (993/465 or 143/587 with STARTTLS) and the sent-copy switch; app-specific
passwords are required for Gmail and Microsoft (spec Assumptions). Quirks recorded when found, each with a
fixture and a test.
