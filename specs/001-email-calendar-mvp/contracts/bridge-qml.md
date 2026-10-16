# Contract: Bridge objects and QML pages

**Feature**: [../spec.md](../spec.md) | **Research**: [../research.md](../research.md) R-19

The bridge (`bridge/`, Qt Core only, no QML types) exposes the core to QML as `QObject` facades registered
as context objects. QML never touches the core, SQL or the network. Long operations (sync, connection
test, attachment download, search over a large list) run on one worker thread; results come back as signals
on the UI thread. Bridges own no business rules: every rule lives in the core and has a core test; the bridge
tests (`tests/bridge/`) check the translation and the threading, the QML tests (`tests/qml/`, offscreen)
check the pages with stub bridges.

## 1. Bridge objects

| Object | Responsibility | Key properties | Key invokables / signals |
|--------|----------------|----------------|--------------------------|
| `SyncBridge` | sync control and global state (top bar, home) | `online`, `syncing`, `progressText`, `lastSyncText`, `pendingCount`, `problemText`, `clockWarning` | `syncNow()`, `cancel()`; `syncFinished(outcome)`; Auto Sync decisions via the baseline `SyncScheduler` |
| `AccountBridge` | account setup and tests | `mailConfigured`, `calendarConfigured`, `protectionText` (honest credential-storage statement), `customCaText` | `testMail(map)`, `saveMail(map)`, `testCalendar(map)`, `saveCalendar(map)`, `chooseCalendars(list)`, `forget()`; `testResult(area, key, detailKey)` |
| `FolderModel` | folder list | role, display name, unread, `olderOnServer`, `syncEnabled` | `refresh()`, `setSyncEnabled(id, bool)` |
| `MessageListModel` | paged list for a folder / search result | rows: sender, subject, date text, unread, flagged, `hasAttachments`, `pendingMark`, `errorKey` | `open(folder, sort)`, `page(n)`, `pageCount`, `fetchOlder()`, `search(text)` |
| `ReaderBridge` | one message | `from`, `to`, `cc`, `subject`, `date`, `blocks` (paragraphs with `quoted` flag), `links`, `attachments`, `bodyNote` | `open(id)`, `markRead/Unread`, `toggleFlag`, `moveTo(folder)`, `remove(permanent=false)`, `downloadAttachment(i)`, `reply()`, `replyAll()`, `forward()` |
| `ComposeBridge` | draft editing | `to`, `cc`, `bcc`, `subject`, `body`, `suggestions`, `savedText` | `start(kind, messageId)`, `edit(draftId)`, `autosave()` (2 s after last change, page leave, suspend), `send()`, `discard()` (confirmation in QML) |
| `OutboxModel` | outbox list | state key, reason key, subject preview, attempts | `retry(id)`, `edit(id)`, `remove(id)`, `sendAgain(id)`, `markSent(id)` |
| `AgendaModel` | upcoming occurrences, paged | date header rows, time text, title, `calendarName`, `readOnly`, `pending`, `conflict` | `page(n)`, `pageCount` |
| `DayModel` / `MonthModel` | day list; month/week grid | marked days, today, selected date | `selectDate(d)`, `nextMonth()`, `setMode(week|month)` |
| `EventBridge` | event view / edit / conflict | all event fields, `recurrenceText`, `zoneText`, `readOnly`, `readOnlyReason`, `conflictOther` | `open(id)`, `new(date)`, `save(map)`, `remove()`, `resolve(choice)` |
| `TimeZoneBridge` | the user's time zone (FR-023) | `needsZoneChoice`, `zoneName` | `zoneList()` (paged canonical zones), `setTimeZone(name)` (rejects names that are not in the bundled database); opened automatically by Agenda/Day/Month while no zone is known |
| `SettingsBridge` | settings | sync days, attachment limit, storage limit + usage, Auto Sync, sent-by-server switch, folder roles, All Mail/Spam sync, time-zone override, calendar visibility | setters validate in the core; `usageText`, `evictNow()` |
| `AboutBridge` | About page (baseline, extended) | version, license texts (offline), credential-storage statement, tz database version | reads `licenses/manifest.txt` as in the baseline [REPO] |
| `DeviceBridge` (`app/`) | InkView glue | `orientation`, Wi-Fi connect request, keyboard, `quit()` | baseline [REPO] |

Common rules: models never block the UI thread for more than one page (<= 100 ms target, unverified);
all user-visible strings are keys translated in QML; no object exposes an address, subject or title to the
log; every list has an explicit empty state and an explicit error state (`errorKey`).

## 2. Pages and navigation

```text
Home ──► Folders ──► MessageList ──► Reader ──► Compose (reply/forward)
  │          │            │  └─► Search          └─► Attachment note / link list
  │          │            └─► Compose (new)
  │          └─► Outbox
  ├──► Agenda ──► EventView ──► EventEdit
  │       ├──► DayView ──► EventView
  │       └──► MonthView(week/month) ──► DayView
  ├──► Conflicts (messages "could not apply", event conflicts, uncertain sends) ──► resolution dialogs
  └──► Settings ──► AccountSetup (mail | calendar tabs) , TimeZone , About
       (TimeZone also opens by itself from Agenda/Day/Month while no zone is known)
```

- First start: `AccountSetup` (mail or calendar, either may be skipped).
- A top bar on every page shows: back, title, an **offline** indicator, the **unsynced count**, the time of the
  last sync (FR-033). The home page repeats them in words.
- The Home page (FR-036): unread count (taps to Inbox), today's and tomorrow's events (soonest first, up to 6, taps to
  event; with more, a row "+ n more" opens the Agenda - FR-029), last sync time, offline indication, pending changes, `Sync` button, links to Mail, Agenda, Outbox,
  Settings. No timers or auto-refresh: it updates on signals only.
- Lists page with `PageButtons` (previous/next page, "page n of m"); no kinetic scrolling; the reader
  turns pages with the baseline `page_target` logic so lines are not cut.
- Destructive actions use `ConfirmDialog`: delete message, delete draft, delete event, permanent delete
  (separate, stronger wording), discard a conflict version, "send again".
- Error/empty/loading states: empty folder, no account, offline-only data ("showing the last synced state"),
  sync progress with a Cancel button, storage-full warning, clock warning.

## 3. E-ink rules (enforced by `scripts/check-ui-style.sh`, extended)

Black/white only; touch targets >= 9 mm (>= 120 px at 227 dpi) [design value, device-unverified]; no
animations or transitions; no `Flickable`/`ListView` kinetic flicking (`flickableDirection` disabled, paged
instead); no timers that repaint continuously; text input uses the on-screen keyboard with the field kept
above it; a page change causes at most one full repaint. New rules for this app: no `Image` with a remote
source, no `WebView`/`WebEngine` import, no `Qt.openUrlExternally` (links are text only, FR-009).

## 4. Accessibility and language

Interface strings are English in the QML (`qsTr`), with a German translation (spec Assumptions). The baseline loads no `.qm` files
because `lrelease` is unusable with the SDK [REPO/SDK], so the translation is a plain table, `qml/strings_de.tsv` (English source,
TAB, German; `||` separates the singular from the plural of a `%n` text), read by `TableTranslator` (a `QTranslator`, installed with
`QCoreApplication::installTranslator`; `QQmlEngine::retranslate()` shows a change at once). It is selected by the setting `ui_language`
(`auto` | `en` | `de`; `auto` = the device language, `currentLang()` on the reader; fallback English; changed in Settings > Language,
`SettingsBridge.language`). `scripts/extract-strings.py --check` (CTest `check_translations`) fails when a text has no translation, a
translation has other placeholders, or an entry is stale. Dates are formatted in C++ (`caltext`, `describe_rrule`) with German names
when `caltext::set_german(true)` has been called by the app after the language change. Dates/times use 24-hour format and the user's zone.
