# Device findings

Facts observed on the PocketBook InkPad One. Everything not listed here is **unverified** (constitution,
Principle IX). Each entry has a date and the way it was observed.

## 2026-10-14 - device probe (T029)

Observed with `pbo-probe` 0.1.0 started through the launcher's `run-probe` flag (`INSTALL.md`, "Device probe");
the reader was not on Wi-Fi during the run.

### What the output means for the open items

| Item | Result | Status |
|---|---|---|
| O-1 device libcurl has IMAP/SMTP | libcurl **8.12.0** (OpenSSL 3.5.4) lists `imap imaps smtp smtps`; the SDK libcurl is 8.16.0 | Protocol list **verified**. Behaviour (custom commands, STARTTLS, APPEND, timeouts) is **unverified**: T054 (`--imap-selftest`) decides. The OpenSSL-client fallback is not needed unless T054 fails |
| O-6 `chmod` / protected location | data folder is **vfat**; `chmod 0600` is **NOT observable** (mode reads back 0777) | **Verified.** The credentials file cannot be made private on this storage; the in-app warning must say so (spec, clarification on passwords). No protected location was found by the probe |
| O-7 `/etc/localtime`, fonts | `TZ` is unset; `/etc/localtime` is a link to a relative target named `timezone`; the probe did not check what that file contains | **Partly open.** It must not be relied on: the app uses bundled tzdata and asks for a zone (spec). Font coverage for German and Eastern European letters is not probed: device checklist |
| O-8 gconv modules | `/usr/lib/gconv` present; iconv to UTF-8 works for all 16 charsets tried (ISO-8859-1/2/15, Windows-1250/1251/1252, KOI8-R, ISO-2022-JP, Shift_JIS, EUC-JP, GB2312, GBK, Big5, EUC-KR, UTF-7, UTF-16LE) | **Verified** for this firmware. iconv can be used for charset conversion |

### Other facts

- Kernel `5.10.226pocketbook aarch64` with a 32-bit ARM user space (the app and the probe are armhf binaries).
- Memory: `MemTotal` 1,987,872 kB. Free storage on the data partition: 23,321 MiB.
- Frame buffer: 1872 x 1404 pixels as reported by sysfs (the panel is 1872x1404; the app's Qt window is 1404x1872 portrait).
- System CA store: `/etc/ssl/certs/ca-certificates.crt` with 150 certificates, 151 `.pem`/`.crt` files, all with a certificate. The
  bundled CA file in the package stays as the fallback (no HTTPS handshake was tested: no network).
- **Qt on the device is 6.10.3** (`/usr/lib/libQt6Core.so.6.10.3`); the SDK sysroot is Qt 6.8.2. The app built against the SDK runs on it
  (About page, rotation, Back verified by the user on 2026-10-14), but anything Qt-version-sensitive is device-unverified.
- QML modules present on the device include `QtWebView`/`QtWebEngine`; PB Office never uses them (rule U5).
- Verified by the user, same day: PB Office 0.1.0 starts on the About page and the Back key works; rotation works (log).

### 2026-10-14 (evening) - first sync on the reader failed with `protocol_error`: libcurl drops literal contents

Observed: login and the folder list worked (6 folders stored); the message headers did not arrive; the app reported
`protocol_error`. Reproduced on the development host with libcurl built from the official sources, using
`pbo-probe --imap-selftest` against the demo server (`pbo_fake_imap`):

| libcurl | custom `UID FETCH n (... BODY.PEEK[HEADER.FIELDS (...)])` | URL fetch `;UID=n;SECTION=...` |
|---|---|---|
| 8.12.0 (the reader's version) | the answer ends right after `{43}\r\n`: **the literal's contents are not delivered**, no closing `)` | works (sends `BODY[...]`, no `PEEK`) |
| 8.16.0 (the SDK's version) | same as 8.12.0 | works |
| 8.18.0 (the host's version) | complete answer | works |

Consequences: reading headers and bodies through custom requests cannot work with the reader's own libcurl. Decision
(owner, 2026-10-14): the app carries libcurl 8.18.0 built for ARM from the unmodified official source
(`scripts/build-curl-arm.sh`, SHA-256 and release-key signature checked) and linked statically; the minimum libcurl
version in the code is now 8.18.0. What the bundled library does on the reader is **unverified** until
`pbo-probe --imap-selftest` has run there (T054). Also observed (host, all versions): the URL form always sends `BODY[...]`
(never `PEEK`), which real servers answer by setting `\Seen`; the design therefore keeps using custom `BODY.PEEK` requests
(possible with 8.18.0) and uses the URL form only where the section is an attachment the reader chose to open.

**Verified by the owner, 2026-10-14 (evening): after the change, Sync on the reader fetched the inbox of a real mail account
over the bundled libcurl 8.18.0 and the message list shows the messages.** This confirms TLS verification, login, folder
list, `STATUS`, `UID SEARCH` and header `FETCH` against one real provider (not recorded here) on the InkPad One. Not yet
verified: bodies, attachments, flags, move/delete, `APPEND`, SMTP, other providers, the IMAP self-test (T054).

**Verified by the owner, 2026-10-14 (night): message bodies and the reader.** With the build that fetches only the readable part
(`BODY.PEEK`), the message list opens, messages read page by page, quoted text is marked. The device database confirms for the two
messages in the 30-day window of one real account: both have their text (`body_state = text`); one is an HTML mail converted to
text, one has an attachment (listed, not downloaded). Not yet verified: more providers, large mailboxes, attachment download,
flags, SMTP, the IMAP self-test (T054).

### 2026-10-14 (night) - folder pull on the reader (owner-verified: "Works!")

The build with `pull_folder` (window only, deletions, flag changes, UIDVALIDITY rebuild logic, all sync-enabled folders) was installed on the InkPad One and the owner reported that it works with their real account. What exactly was exercised (deletions, flag changes, larger mailbox) was not itemised, so only "sync with the new pull works on one real account" is claimed; the UIDVALIDITY rebuild has only fake-server evidence (a real server rarely resets).

### 2026-10-14 (late night) - connection test of both servers on the reader

The account form's Test button (`test_mail_connection`) ran on the InkPad One against the owner's real account: log line `connection.tested imap=ok smtp=ok`, then the account was saved and a sync finished with `success`. So the SMTP test (connect, TLS, `EHLO localhost`, AUTH, `NOOP`, no mail) works against this one provider. Other providers and sending are unverified. Observation: the sync at app start (before the Test) ended `offline` / `server_unreachable` once, so "Wi-Fi connected" at start did not yet mean the server was reachable.

### 2026-10-18 - contacts (0.3.0) on the reader, first owner test

PB Office 0.3.0 was installed over 0.2.0 on the InkPad One (Nextcloud account with three address books). The log shows: the first start migrated the database (`pboffice.db.bak` written at launch, the app started and synced with `outcome=success`), `contacts.pull books=3 changed=160`, one contact deleted (`contacts.push ... deleted=1`), one created (`contacts.push created=1`), each followed by a sync a few seconds after the change (Wi-Fi was brought up by the app). The new contact is in the reader's database with the server's ETag, in the writable book "Kontakte", with a complete card (name, organisation, two phone numbers, email, note), and the pull after the push found nothing new. The owner reported: create, change and delete work, and "everything else is good", plus four points:

| Point | Finding | Action |
|-------|---------|--------|
| The delete icon was a minus in a circle | Other screens use the trash can | Contact delete and the "remove entry" buttons of the form now use the trash icon |
| The on-screen keyboard covers the text field when creating or changing a note | Three rounds on the reader (screenshots by the owner). The form scrolled against the full page and ignored the keyboard; then the keyboard height from Qt (`kb.y=840`, `kb.h=495` of a 1335 px window) was used in window pixels while the page is drawn on a stage scaled by the device pixel ratio (1.30 here), so the form moved about 30 % too little | The form ends with room for the keyboard (40 % of the page), and while a text field has the focus the field (the cursor line of the note) is moved above the keyboard; the overlap is converted by the pixel ratio and 60 px are added for the keyboard's top edge. **Owner-verified 2026-10-18: "viel besser"**, the note and its cursor line stay above the keyboard. |
| The created contact did not appear in the owner's other mail clients | The reader's data showed a confirmed write (ETag, complete card). **Owner-verified 2026-10-18: the test contact is on Nextcloud**; the other clients had not synced or read another account. | Closed |
| A sync should follow a create, change or delete when Auto Sync is on | The log showed it for create and delete; the request for a sync is now made in `Main.qml` instead of on the page. **Owner-verified 2026-10-18: "Sync funktioniert auch"** (after the change of a contact). | Closed |

### Raw output

```text
== system (shell)
Linux 5.10.226pocketbook aarch64

== pbo-probe
pbo-probe (prints no personal data)

== libcurl
version: 8.12.0
ssl: OpenSSL/3.5.4
protocols: dict file ftp ftps gopher gophers http https imap imaps mqtt pop3 pop3s rtmp rtmpe rtmps rtmpt rtmpte rtmpts rtsp smtp smtps telnet tftp
imaps: yes
smtps: yes
imap: yes
smtp: yes
https: yes
features: SSL libz IPv6 HTTP2 threadsafe

== charsets (iconv to UTF-8)
ISO-8859-1: yes
ISO-8859-2: yes
ISO-8859-15: yes
WINDOWS-1250: yes
WINDOWS-1251: yes
WINDOWS-1252: yes
KOI8-R: yes
ISO-2022-JP: yes
SHIFT_JIS: yes
EUC-JP: yes
GB2312: yes
GBK: yes
BIG5: yes
EUC-KR: yes
UTF-7: yes
UTF-16LE: yes

== CA files
/etc/ssl/certs/ca-certificates.crt: 224449 bytes, 150 certificates
/etc/ssl/cert.pem: not present
/etc/ssl/certs/ca-bundle.crt: not present
/etc/ssl/certs: 151 pem/crt files, 151 with a certificate

== data directory
path: /mnt/ext1/system/pboffice
filesystem: vfat (0x4d44)
chmod 0600: NOT observable (mode read back: 0777)
free: 23321 MiB

== system
TZ: (unset)
localtime: -> timezone
MemTotal: 1987872 kB
gconv: /usr/lib/gconv present
screen: frame buffer 1872,1404 (width,height in pixels)

== Qt runtime (R-2)
/usr/lib/libQt6Core.so
/usr/lib/libQt6Core.so.6
/usr/lib/libQt6Core.so.6.10.3
/usr/lib/libQt6Qml.so
/usr/lib/libQt6Qml.so.6
/usr/lib/libQt6Qml.so.6.10.3
/usr/lib/libQt6Quick.so
/usr/lib/libQt6Quick.so.6
/usr/lib/libQt6Quick.so.6.10.3
-- QML modules in /usr/qml:
QML
Qt
Qt5Compat
QtCore
QtNetwork
QtQml
QtQuick
QtTest
QtWebChannel
QtWebEngine
QtWebSockets
QtWebView
builtins.qmltypes
com
jsroot.qmltypes
-- QPA platform plugins:
libqlinuxfb.so
libqminimal.so
libqoffscreen.so
libqpocketbook2.so
libqpocketbook2egl.so
libqvnc.so

== HTTPS handshake with each existing CA candidate (verification ON)
/etc/ssl/certs/ca-certificates.crt -> curl: (6) Could not resolve host: curl.se
000

== note
QueryNetwork() (R-11) cannot be called from a shell: it is read by the app and logged as network.connect.
The IMAP behaviour check against a server is a separate step (pbo-probe --imap-selftest, not part of this output).
```
