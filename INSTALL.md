# Installing PB Office on the PocketBook InkPad One

These steps install the app from the release zip. They are written for the InkPad One
(RK3566); other models are untested. PB Office is a separate app from PB Notes: it uses its own
files and its own data folder and never touches PB Notes.

## 1. Copy the files

1. Connect the reader over USB so its storage appears as a drive.
2. Unzip the package and copy the `applications/` folder into the **root** of the reader's
   storage, merging it with the existing `applications/` folder. You should end up with:

   ```text
   applications/pboffice.app            launcher (shell script)
   applications/pboffice.bin            the program
   applications/pboffice-cacert.pem     bundled CA certificates
   applications/pboffice-emoji.ttf      monochrome emoji font (Noto Emoji)
   applications/pboffice-emoji-OFL.txt  license of the emoji font (SIL OFL 1.1)
   applications/pboffice-zoneinfo/      time zone data for the calendar (copy the whole folder)
   applications/pboffice-plugins/       Qt JPEG and GIF image plugins for the pictures of mails (copy the whole folder)
   applications/pboffice-licenses/      license texts shown by Settings > About (copy the whole folder)
   applications/pboffice-probe.sh       the device probe script (see "Device probe")
   applications/pbo-probe.bin           the device probe (see "Device probe")
   applications/icons/pboffice.bmp
   applications/icons/pboffice_f.bmp
   ```

## 2. Register the app in the Applications menu

The reader only lists apps that are registered in its desktop configuration.

1. On the reader's storage open `system/config/desktop/view.json`. **Back it up first.**
2. Under the top-level `"applications"` object add:

   ```json
   "U_PbOffice": {
     "path": "/mnt/ext1/applications/pboffice.app",
     "title": "PB Office",
     "icon": { "path": "applications/icons/pboffice.bmp" },
     "focused_icon": { "path": "applications/icons/pboffice_f.bmp" }
   }
   ```

3. Add `"U_PbOffice"` to one of the `"groups"` application lists, for example the one
   named `@General`.
4. Rules to keep in mind: the id must start with `U_`, `path` must be absolute, and the icons
   must be opaque BMP files (122x122, 8 bit).
5. Safely eject the reader and **reboot** it.

### Upgrading

Copy the same files over the old ones. The data folder `system/pboffice/` (database, saved passwords,
log) is kept; the app upgrades its database by itself, after making a backup copy, and refuses to start
with a clear message if the data were written by a newer version. PB Office never touches the files of
PB Notes or of InkFeed (it only reads and writes its own files).

## 3. Start the app

Open Applications, then the group you added the entry to, and start **PB Office**. The first time it asks
for your mail account (the gear on the first page); afterwards it opens on the start page. The language of the
interface (English or German) follows the reader and can be changed in **Settings**. The
launcher writes a log to `system/pboffice/pboffice.log`; the app's own data (the database, the saved
passwords file and downloaded attachments) lives in the same folder.

## Passwords

PB Office stores your mail and calendar passwords as plain text in a file in `system/pboffice/` and
tells you on the account page whether the reader's storage can restrict who reads it. Use an
app-specific password (Gmail and Microsoft accounts need one anyway).

## Servers with a private certificate authority

PB Office always verifies the server's certificate and has no switch to skip this. If your server uses
a certificate from a private authority, give the app that authority's certificate:

1. Get the certificate file in PEM format (a text file that starts with `-----BEGIN CERTIFICATE-----`).
2. Connect the reader over USB and copy the file to `system/pboffice/custom-ca.pem` on the reader's
   storage (create the folder `pboffice` inside `system` if it does not exist yet).
3. Safely eject the reader and start PB Office again. **Settings > Security** shows whether a custom CA
   file is loaded. If it says that the file could not be used, it is ignored (it must contain only well-formed certificates
   and be smaller than 1 MB).

## Device probe

The package can contain `pbo-probe.bin` and `pboffice-probe.sh` in `applications/`. They print what this reader's
system can do (supported mail protocols, character sets, certificate files, whether file permissions are kept,
free space, memory) and no personal data. To run them:

1. Create an empty file named `run-probe` in `system/pboffice/` on the reader's storage (create the folder if it
   does not exist yet).
2. Eject the reader safely and start **PB Office** once. The screen stays blank for a moment; the app itself is
   not started on this run and the file is removed.
3. Connect the reader again and read `system/pboffice/probe.txt`.

### IMAP behaviour check (for developers)

To check how this reader's network library behaves against an IMAP server on your home network, run the demo server
on a computer on the same Wi-Fi: `build-host/tests/pbo_fake_imap --listen 0.0.0.0 --host <the computer's address>
--ca-out ca.pem --port 9993`. Copy `ca.pem` to `system/pboffice/imap-selftest-ca.pem` on the reader and create
`system/pboffice/imap-selftest.txt` with one line `<the computer's address> 9993 --bad-login --append Drafts`. Then
start the probe as above (with the reader on Wi-Fi); `probe.txt` gets a section "imap self-test" with one pass/fail line
per check. The demo server holds no real mail; never use these flags against a real account.

## After installing: what has been checked

The tests of this project use fake servers and the host. Work through `docs/device-test-checklist.md` on
the reader and `docs/live-server-checklist.md` against your own providers, and write the dates and results
next to the items; until then every item is **unverified**.

## Troubleshooting

- Nothing happens when starting: read `system/pboffice/pboffice.log` on the reader's storage (the launch before is in
  `pboffice.log.prev`). If it says `failed to open device file /dev/mali0 ... Permission denied` (exit code 134), the
  reader's system has lost access to the graphics chip for new apps. PB Office retries once by itself; if that fails,
  restart the reader. Please keep both log files and note what you did before.
- The entry does not appear: check the JSON of `view.json` is still valid, then reboot.
