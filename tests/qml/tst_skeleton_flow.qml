import QtQuick
import QtTest

// The walking skeleton through the real pages: fill the account form, Test, Save, Sync, read the list.
// The window is the size of the offscreen screen (800 x 600), so the list has 2 rows per page.
TestCase {
    id: tc
    name: "skeleton_flow"
    visible: true      // a TestCase is invisible by default, and an invisible page gets no clicks
    when: windowShown
    width: 800
    height: 600

    SignalSpy { id: syncFinished; target: sync; signalName: "syncFinished" }

    function page(name, props) {
        var comp = Qt.createComponent(harness.pagesDir + name)
        compare(comp.status, Component.Ready, comp.errorString())
        // the size is given at creation: a page resized afterwards does not take clicks in its bottom panel
        var initial = props || {}
        initial.width = tc.width
        initial.height = tc.height
        var obj = comp.createObject(tc, initial)
        verify(obj !== null, "page " + name + " could not be created")
        return obj
    }

    function fill(p, name, value) {
        var f = findChild(p, name)
        verify(f !== null, "no field " + name)
        f.text = value
    }

    // typed like a user (textEdited fires)
    function type(p, name, text) {
        var f = findChild(p, name)
        verify(f !== null, "no field " + name)
        var input = f.field !== undefined ? f.field : f
        input.forceActiveFocus()
        for (var i = 0; i < text.length; ++i) keyClick(text.charAt(i))
    }

    function tap(p, name) {
        var b = findChild(p, name)
        verify(b !== null, "no control " + name)
        // an item inside the reader's text area that lies below the visible lines: turn the page first, like a reader would
        var view = findChild(p, "readerView")
        if (view) {
            var inside = false
            for (var o = b.parent; o; o = o.parent) if (o === view.contentItem) inside = true
            if (inside) {
                for (var t = 0; t < 20; ++t) {
                    var pt = b.mapToItem(view.contentItem, 0, 0)
                    if (pt.y + b.height <= view.contentY + view.height) break
                    view.contentY = Math.min(view.contentHeight - view.height, view.contentY + view.height - p.lineHeight)   // one page, whole lines
                    wait(30)
                }
            }
        }
        // positioners (Column) arrange their children one event-loop round after creation: let the position settle first
        for (var i = 0; i < 20; ++i) {
            var before = b.mapToItem(null, 0, 0).y
            wait(30)
            if (b.mapToItem(null, 0, 0).y === before) break
        }
        verify(b.enabled, name + " is disabled")
        mouseClick(b)
    }

    function test_the_password_field_offers_delete_but_never_copy() {
        var form = page("AccountSetupPage.qml")
        var f = findChild(form, "password")
        verify(f !== null, "no password field")
        f.text = "secret"
        var input = f.field
        var sc = findChild(form, "accountScroller")
        sc.contentY = Math.max(0, Math.min(sc.contentHeight - sc.height, f.mapToItem(sc.contentItem, 0, 0).y - 20))
        wait(50)
        mousePress(input, 20, input.height / 2)
        wait(800)
        mouseRelease(input, 20, input.height / 2)
        var top = findChild(input, "textMenu").bubble
        verify(findChild(top, "menu_remove") !== null && findChild(top, "menu_remove").visible, "Delete is offered")
        compare(findChild(top, "menu_copy"), null)
        compare(findChild(top, "menu_cut"), null)
        form.destroy()
    }

    function test_account_form_to_message_list() {
        var form = page("AccountSetupPage.qml")
        compare(form.statusKey, "")
        fill(form, "imapHost", "localhost")
        fill(form, "imapPort", String(harness.port))
        fill(form, "smtpHost", "localhost")
        fill(form, "smtpPort", String(harness.smtpPort))
        fill(form, "username", "alice")
        fill(form, "password", "pw-for-tests")
        fill(form, "address", "alice@example.org")

        tap(form, "testButton")
        tryCompare(form, "statusKey", "ok", 20000)
        compare(account.mailConfigured, false)          // a test saves nothing

        tap(form, "saveButton")
        tryCompare(account, "mailConfigured", true, 5000)
        compare(form.saved, true)

        var list = page("MessageListPage.qml")
        compare(list.emptyKind, "empty")                // nothing synced yet: an explicit empty state
        tap(list, "syncButton")
        tryCompare(syncFinished, "count", 1, 30000)
        compare(syncFinished.signalArguments[0][0], "success")

        compare(messages.total, 12)
        var rows = findChild(list, "rowsColumn")
        verify(rows !== null)
        tryCompare(rows, "rowCount", messages.pageSize, 5000)
        compare(messages.pageSize, 2)
        compare(findChild(list, "rowSubject0").text, "Subject 12")
        compare(findChild(list, "rowSubject1").text, "Subject 11")
        compare(findChild(list, "rowSubject0").font.bold, false)   // message 12 is read
        compare(findChild(list, "rowSubject1").font.bold, true)    // message 11 is unread
        compare(findChild(list, "rowSender1").text, "Sender 11")
        compare(findChild(list, "pageIndicator").text, "1 / 6")

        tap(list, "pageNext")
        compare(findChild(list, "pageIndicator").text, "2 / 6")
        compare(findChild(list, "rowSubject0").text, "Subject 10")
        list.destroy()
        form.destroy()
    }

    // FR-003 / FR-004: every cause the connection test can name has its own sentence, and the password statement is always on the page
    function test_every_cause_has_a_message_and_the_password_statement_is_visible() {
        var form = page("AccountSetupPage.qml")
        var causes = ["connecting", "ok", "saved", "auth_failed", "cert_expired", "cert_untrusted", "cert_host_mismatch", "tls_failed", "starttls_unavailable",
                      "server_unreachable", "no_network", "timeout", "libcurl_too_old", "invalid_input"]
        var seen = {}
        for (var i = 0; i < causes.length; ++i) {
            var text = form.causeText(causes[i])
            verify(text.length > 0, "no message for " + causes[i])
            verify(text.indexOf("test failed (") < 0, causes[i] + " falls into the general message")
            verify(!seen[text], "two causes share the message " + text)
            seen[text] = true
        }
        compare(form.causeText(""), "")
        verify(form.causeText("something_new").indexOf("something_new") > 0)         // an unknown cause is named, never hidden
        var statement = findChild(form, "protectionText")
        verify(statement !== null && statement.visible && statement.text.length > 0)
        verify(statement.text.indexOf("app-specific password") > 0 || statement.text.indexOf("only this app") > 0)
        verify(form.protectionText("not_enforced").indexOf("app-specific password") > 0)
        verify(form.protectionText("unknown").indexOf("app-specific password") > 0)
        form.destroy()
    }

    function test_a_wrong_password_shows_its_cause() {
        var form = page("AccountSetupPage.qml")
        fill(form, "imapHost", "localhost")
        fill(form, "imapPort", String(harness.port))
        fill(form, "smtpHost", "localhost")
        fill(form, "smtpPort", String(harness.smtpPort))
        fill(form, "username", "alice")
        fill(form, "password", "not-the-password")
        fill(form, "address", "alice@example.org")
        tap(form, "testButton")
        tryCompare(form, "statusKey", "auth_failed", 20000)
        verify(findChild(form, "statusText").text.length > 0)
        form.destroy()
    }

    // Tapping a row opens the message; the reader shows the header, the text in blocks (quotes marked), the attachments and
    // the links, and pages by whole lines.
    function test_reader_a_tap_on_a_row_opens_the_message() {
        var list = page("MessageListPage.qml")
        var opened = []
        list.messageOpened.connect(function (id) { opened.push(id) })
        tryCompare(findChild(list, "rowSubject0"), "text", messages.rows[0].subject)
        // the Column places its rows in the next event-loop round: wait until they are apart
        tryVerify(function () { return findChild(list, "rowSubject1").mapToItem(null, 0, 0).y > findChild(list, "rowSubject0").mapToItem(null, 0, 0).y }, 3000)
        var row = findChild(list, "rowTap0")
        verify(row !== null)
        mouseClick(row)
        compare(opened.length, 1)
        compare(opened[0], messages.rows[0].id)
        list.destroy()
    }

    function test_list_marks_messages_with_attachments() {
        harness.addRichMessage()
        var list = page("MessageListPage.qml")
        tryCompare(findChild(list, "rowSubject0"), "text", "Re: Planung")
        compare(findChild(list, "rowAttach0").visible, true)
        compare(findChild(list, "rowAttach1").visible, false)
        for (var i = 0; i < messages.rows.length; ++i) compare(findChild(list, "rowAttach" + i).visible, messages.rows[i].hasAttachments)
        list.destroy()
    }

    function test_reader_page_shows_the_message_in_blocks() {
        var id = harness.addRichMessage()
        var p = page("ReaderPage.qml", { messageId: id })
        compare(reader.loaded, true)
        compare(findChild(p, "readerSubject").text, "Re: Planung")
        verify(findChild(p, "readerFrom").text.indexOf("Anna Beispiel") >= 0)
        compare(findChild(p, "block0").text, "Yes, Thursday works for me [1].")
        compare(findChild(p, "block0").quoted, false)
        compare(findChild(p, "blockMark0").visible, false)
        compare(findChild(p, "block1").text, "On Mon, 05 Oct 2026, Ben wrote:")
        compare(findChild(p, "block2").text, "Can we meet on Thursday?\nI could do 10:00.")
        compare(findChild(p, "block2").quoted, true)
        compare(findChild(p, "blockMark2").visible, true)
        verify(findChild(p, "block3").x > findChild(p, "block2").x)         // a deeper quote is indented further
        compare(findChild(p, "attachmentName0").text, "report.pdf")
        compare(findChild(p, "attachmentSize0").text, "2.5 KB")
        // the attachments come right after the header, before the text: they are on the first page
        tryVerify(function () { return findChild(p, "attachmentName0").mapToItem(p, 0, 0).y < findChild(p, "block0").mapToItem(p, 0, 0).y }, 3000)
        verify(findChild(p, "attachmentName0").mapToItem(p, 0, 0).y < findChild(p, "readerView").height + findChild(p, "readerView").mapToItem(p, 0, 0).y)
        compare(findChild(p, "linkText0").text, "[1] example.org/a")                // the list shows the short form: no scheme, no "www."
        compare(findChild(p, "linkText0").textFormat, Text.PlainText)            // never interpreted as markup, so never a live link
        compare(findChild(p, "block0").textFormat, Text.PlainText)
        compare(findChild(p, "readerSubject").textFormat, Text.PlainText)
        p.destroy()
    }

    function test_opening_a_message_marks_it_read_and_the_actions_change_it() {
        var id = harness.addRichMessage()
        harness.exec("UPDATE message SET flags_base = 0, flags_local = 0, local_state = 'synced' WHERE id = " + id)
        var p = page("ReaderPage.qml", { messageId: id })
        compare(reader.unread, false)                                           // opening reads it
        compare(harness.query("SELECT local_state FROM message WHERE id = " + id), "pending_flags")
        p.perform("toggle_read")
        compare(reader.unread, true)
        compare(harness.query("SELECT local_state FROM message WHERE id = " + id), "synced")   // back to what the server has
        p.perform("toggle_star")
        compare(reader.flagged, true)
        compare(harness.query("SELECT flags_local & 4 FROM message WHERE id = " + id), "4")
        var labels = p.actionChoices().map(function (c) { return c.label })
        compare(labels[1], "Remove the star")
        p.destroy()
    }

    SignalSpy { id: autoSyncDone; target: sync; signalName: "syncFinished" }

    function test_the_reader_starts_a_sync_by_itself_after_a_change() {
        var id = harness.addRichMessage()
        harness.exec("UPDATE message SET flags_base = 0, flags_local = 0, local_state = 'synced' WHERE id = " + id)
        var p = page("ReaderPage.qml", { messageId: id })      // opening reads the message: that change is the first
        harness.exec("INSERT OR REPLACE INTO settings(key, value) VALUES('auto_sync', '1')")
        autoSyncDone.clear()
        p.perform("toggle_star")                               // star
        compare(autoSyncDone.count, 0)                         // not at once: a few seconds later
        tryVerify(function () { return autoSyncDone.count > 0 }, 8000)
        harness.exec("INSERT OR REPLACE INTO settings(key, value) VALUES('auto_sync', '0')")
        tryCompare(sync, "syncing", false, 8000)
        p.destroy()
    }

    function test_delete_asks_first_and_moves_the_message_to_the_trash() {
        harness.exec("INSERT OR IGNORE INTO folder(name, raw_name, role) VALUES('Trash', 'Trash', 'trash')")
        var id = harness.addRichMessage()
        var p = page("ReaderPage.qml", { messageId: id })
        compare(reader.deleteKind, "to_trash")
        compare(findChild(p, "readerDeleteDialog").title, "Move to the Trash?")
        p.perform("delete")
        verify(findChild(p, "readerDeleteDialog").visible)
        compare(harness.query("SELECT local_state FROM message WHERE id = " + id) === "pending_move", false)   // not before the answer
        p.destroy()
        harness.exec("DELETE FROM folder WHERE role = 'trash'")
    }

    function test_the_search_page_finds_mail_offline_and_says_when_there_is_nothing() {
        harness.addRichMessage()
        var p = page("SearchPage.qml")
        verify(findChild(p, "searchNote").visible)
        type(p, "searchField", "planung")
        tap(p, "searchGo")
        compare(searchResults.searching, true)
        tryVerify(function () { return findChild(p, "searchSubject0") !== null }, 3000)
        compare(findChild(p, "searchSubject0").text, "Re: Planung")
        findChild(p, "searchField").field.selectAll()
        type(p, "searchField", "zzzzqqq")
        tap(p, "searchGo")
        compare(searchResults.empty, true)
        compare(findChild(p, "searchNote").text, "Nothing found in the mail on this reader.")
        p.destroy()
    }

    function test_the_list_and_the_reader_mark_a_change_that_waits_for_the_sync() {
        var id = harness.addRichMessage()
        harness.exec("UPDATE message SET flags_base = 0, flags_local = 0, local_state = 'synced', local_error = '' WHERE id = " + id)
        var list = page("MessageListPage.qml")
        tryCompare(findChild(list, "rowSubject0"), "text", "Re: Planung")
        compare(findChild(list, "rowPending0").visible, false)
        harness.exec("UPDATE message SET flags_local = 1, local_state = 'pending_flags' WHERE id = " + id)
        messages.refresh()
        tryCompare(findChild(list, "rowPending0"), "visible", true)
        compare(findChild(list, "rowPending0").text, "not synced")
        harness.exec("UPDATE message SET local_error = 'gone_on_server' WHERE id = " + id)
        messages.refresh()
        tryCompare(findChild(list, "rowPending0"), "text", "could not apply")
        list.destroy()
        harness.exec("UPDATE message SET flags_local = 1, local_state = 'pending_flags', local_error = '' WHERE id = " + id)
        var p = page("ReaderPage.qml", { messageId: id })
        verify(findChild(p, "readerDate").text.indexOf("not synced") > 0)
        p.destroy()
        harness.exec("UPDATE message SET flags_base = 1, flags_local = 1, local_state = 'synced' WHERE id = " + id)
    }

    // a star or "unread" set offline must show on the message and in the list at once (reported from the reader)
    function test_the_reader_and_the_list_show_the_star_and_the_unread_state() {
        var id = harness.addRichMessage()
        harness.exec("UPDATE message SET flags_base = 1, flags_local = 1, local_state = 'synced', local_error = '' WHERE id = " + id)
        var p = page("ReaderPage.qml", { messageId: id })
        compare(findChild(p, "readerStarIcon").visible, false)
        verify(findChild(p, "readerDate").text.indexOf("starred") < 0)
        reader.setFlagged(true)
        tryCompare(findChild(p, "readerStarIcon"), "visible", true)
        verify(findChild(p, "readerDate").text.indexOf("starred") > 0)
        verify(findChild(p, "readerDate").text.indexOf("unread") < 0)
        reader.markUnread()
        tryVerify(function () { return findChild(p, "readerDate").text.indexOf("unread") > 0 })
        var list = page("MessageListPage.qml")
        messages.refresh()
        tryCompare(findChild(list, "rowStar0"), "visible", true)
        list.destroy()
        p.destroy()
        harness.exec("UPDATE message SET flags_base = 1, flags_local = 1, local_state = 'synced' WHERE id = " + id)
    }

    // pictures of an HTML mail: a part of the mail shows at once, a picture on the sender's server waits for the tap, and the links are short
    function test_html_picture_mail_shows_its_picture_offers_the_rest_and_shortens_the_links() {
        var id = harness.addPictureMessage()
        var p = page("ReaderPage.qml", { messageId: id })
        compare(findChild(p, "block0").text, "Hello Volker")
        var pic = findChild(p, "picture1")
        verify(pic !== null)
        tryCompare(pic, "status", Image.Ready, 5000)
        verify(pic.visible)
        verify(pic.paintedHeight > 0)
        compare(findChild(p, "pictureNote1").visible, false)                       // the loaded picture needs no note
        compare(findChild(p, "picture3").visible, false)                           // the picture on the sender's server is not loaded
        verify(findChild(p, "pictureNote3").visible)
        compare(findChild(p, "pictureNote3").visible, true)
        verify(findChild(p, "imageOffer").visible)
        verify(findChild(p, "imageOfferText").text.indexOf("sender") >= 0)         // it says whom it tells
        verify(findChild(p, "loadImages") !== null)
        // whole lines: every block is a multiple of the line height, so that a page turn never cuts one
        for (var i = 0; i < 5; ++i) {
            var b = findChild(p, "block" + i)
            if (b) verify(b.parent.height % p.lineHeight === 0, "block " + i + " is not a whole number of lines")
        }
        // links: short, and a tap shows the whole address
        var longLink = findChild(p, "linkText1")
        verify(longLink.text.length < 70, longLink.text)
        verify(longLink.text.indexOf("tracking/redirect?campaign=autumn&user=0123") < 0)
        compare(findChild(p, "linkText0").text, "[1] shop.example.org/a")
        p.toggleLink(1)           // what a tap on the link does (the link is below the visible page in this small window)
        verify(findChild(p, "linkText1").text.indexOf("campaign=autumn&user=0123456789abcdef") > 0)
        p.toggleLink(1)           // what a tap on the link does (the link is below the visible page in this small window)
        verify(findChild(p, "linkText1").text.length < 70)
        p.destroy()
        harness.exec("DELETE FROM message WHERE uid = 901")   // the picture mail was only for this test
    }

    function test_link_labels_keep_the_start_and_the_end_of_a_long_address() {
        var p = page("ReaderPage.qml", { messageId: 0 })
        compare(p.linkLabel("https://www.example.org/a"), "example.org/a")
        compare(p.linkLabel("http://example.org/short?x=1"), "example.org/short?x=1")
        var l = p.linkLabel("https://shop.example.org/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa/end1234")
        compare(l.length, 34 + 1 + 8)
        verify(l.indexOf("…") === 34 && l.slice(-8) === "/end1234")
        compare(p.linkLabel("https://" + "h".repeat(60) + ".example.org/x").slice(-1), "…")        // a very long host name is cut
        p.destroy()
    }

    function test_the_list_can_be_sorted() {
        var list = page("MessageListPage.qml")
        verify(findChild(list, "sortButton") !== null)
        messages.sort = "sender"
        compare(messages.sort, "sender")
        messages.sort = "date"
        list.destroy()
    }

    function test_the_folders_page_offers_the_things_to_decide_only_when_there_are_some() {
        harness.exec("UPDATE message SET local_error = '' ")
        var p = page("FoldersPage.qml")
        compare(findChild(p, "decideButton").visible, false)
        var id = harness.addRichMessage()
        harness.exec("UPDATE message SET local_error = 'gone_on_server', local_state = 'pending_flags' WHERE id = " + id)
        conflicts.refresh()
        compare(findChild(p, "decideButton").visible, true)
        var c = page("ConflictsPage.qml")
        compare(findChild(c, "conflictTitle0").text, "Re: Planung")
        compare(findChild(c, "conflictServer0").text, "The message is no longer on the server.")
        c.chosen = conflicts.rows[0]
        c.perform("ack")
        compare(conflicts.empty, true)
        c.destroy()
        p.destroy()
    }

    function test_reader_pages_by_whole_lines_and_never_cuts_a_line() {
        var id = harness.addRichMessage()
        var p = page("ReaderPage.qml", { messageId: id })
        var view = findChild(p, "readerView")
        tryVerify(function () { return view.contentHeight > view.height }, 3000)   // the message is longer than the screen (the Column lays out in the next round)
        compare(view.height % p.lineHeight, 0)                             // the viewport shows whole lines only
        compare(view.interactive, false)                                    // no kinetic flicking on e-ink
        compare(view.contentY, 0)
        tap(p, "pageDown")
        verify(view.contentY > 0)
        compare(view.contentY % p.lineHeight, 0)                           // a page turn moves by whole lines
        var first = view.contentY
        tap(p, "pageDown")
        verify(view.contentY > first)
        compare(view.contentY % p.lineHeight, 0)
        tap(p, "pageUp")
        compare(view.contentY, first)
        // the last page ends on the last line, not below it
        for (var i = 0; i < 20; ++i) if (findChild(p, "pageDown").enabled) tap(p, "pageDown")
        verify(view.contentY + view.height >= view.contentHeight - 1)
        verify(view.contentY % p.lineHeight === 0)
        p.destroy()
    }

    // The rows are rebuilt when the message is reloaded, so the text is looked up again on every poll.
    function waitForText(root, name, text, timeout) {
        tryVerify(function () { var o = findChild(root, name); return o !== null && o.text === text }, timeout)
    }

    // Downloading an attachment: tap, Wi-Fi first, a status line, the size limit stated when it blocks.
    function syncAndFind(fixtureName, subject) {
        harness.addServerMessage(fixtureName)
        var n = syncFinished.count
        sync.syncNow()
        tryCompare(syncFinished, "count", n + 1, 30000)
        return harness.messageIdBySubject(subject)
    }

    function test_reader_attachment_is_downloaded_on_a_tap() {
        var id = syncAndFind("with_attachments", "With attachments")
        var p = page("ReaderPage.qml", { messageId: id })
        compare(findChild(p, "attachmentName0").text, "report.pdf")
        compare(findChild(p, "attachmentStatus0").text, "")                 // nothing is downloaded by opening the message
        compare(harness.fileExists("report.pdf"), false)
        tap(p, "attachmentTap0")
        waitForText(p, "attachmentStatus0", "Saved in Downloads: report.pdf", 20000)
        compare(harness.fileExists("report.pdf"), true)         // in the Downloads folder, under its real name
        // the second attachment of the same message, with Wi-Fi off: the page switches it on first
        device.wifiUp = false
        device.connectWorks = true
        var calls = device.connectCalls
        tap(p, "attachmentTap1")
        tryCompare(device, "connectCalls", calls + 1, 3000)
        waitForText(p, "attachmentStatus1", "Saved in Downloads: gr\u00fc\u00dfe.png", 20000)
        compare(harness.fileExists("gr\u00fc\u00dfe.png"), true)
        p.destroy()
    }

    function test_reader_attachment_over_the_limit_says_so_and_fetches_nothing() {
        var id = syncAndFind("nested_multipart", "Nested")
        harness.setAttachmentSize(harness.attachmentIdOf(id, 0), 20000000)
        var p = page("ReaderPage.qml", { messageId: id })
        tap(p, "attachmentTap0")
        waitForText(p, "attachmentStatus0", "Larger than the limit of 10 MiB. Not downloaded.", 20000)
        compare(harness.fileExists("archive.zip"), false)
        p.destroy()
    }

    function test_reader_attachment_without_wifi_says_so() {
        var id = syncAndFind("message_rfc822_attachment", "Fwd: attached message")
        var p = page("ReaderPage.qml", { messageId: id })
        device.wifiUp = false
        device.connectWorks = false
        tap(p, "attachmentTap0")
        waitForText(p, "attachmentStatus0", "No network connection. Turn on Wi-Fi and try again.", 5000)
        compare(harness.fileExists("forwarded.eml"), false)
        device.wifiUp = true
        device.connectWorks = true
        p.destroy()
    }

    function test_reader_a_message_without_its_body_says_so() {
        var id = parseInt(harness.addHeaderOnlyMessage())
        var p = page("ReaderPage.qml", { messageId: id })
        compare(reader.bodyState, "headers")
        verify(findChild(p, "readerNotice").visible)
        verify(findChild(p, "readerNotice").text.length > 0)
        p.destroy()
    }

    // Sync switches Wi-Fi on first, and only syncs when the reader is online.
    function test_sync_asks_for_wifi_first() {
        var list = page("MessageListPage.qml")
        device.wifiUp = false
        device.connectWorks = true
        var calls = device.connectCalls
        var finished = syncFinished.count
        tap(list, "syncButton")
        compare(findChild(list, "statusLine").text, "Connecting to Wi-Fi …")
        tryCompare(device, "connectCalls", calls + 1, 2000)
        tryCompare(syncFinished, "count", finished + 1, 30000)
        compare(device.wifiUp, true)
        list.destroy()
    }

    function test_sync_does_not_start_when_wifi_cannot_be_switched_on() {
        var list = page("MessageListPage.qml")
        device.wifiUp = false
        device.connectWorks = false
        var finished = syncFinished.count
        tap(list, "syncButton")
        tryCompare(findChild(list, "statusLine"), "text", "No network connection. Turn on Wi-Fi and try again.", 5000)
        wait(300)
        compare(syncFinished.count, finished)           // nothing was started
        compare(sync.syncing, false)
        device.wifiUp = true
        device.connectWorks = true
        list.destroy()
    }

    function test_the_connection_test_asks_for_wifi_too() {
        var form = page("AccountSetupPage.qml")
        device.wifiUp = false
        device.connectWorks = false
        fill(form, "imapHost", "localhost")
        fill(form, "imapPort", String(harness.port))
        fill(form, "username", "alice")
        fill(form, "password", "pw-for-tests")
        fill(form, "address", "alice@example.org")
        tap(form, "testButton")
        tryCompare(form, "statusKey", "no_network", 5000)
        device.wifiUp = true
        device.connectWorks = true
        form.destroy()
    }

    function test_an_invalid_field_is_named_and_nothing_is_sent() {
        var form = page("AccountSetupPage.qml")
        fill(form, "imapHost", "bad host")
        fill(form, "username", "alice")
        fill(form, "password", "x")
        fill(form, "address", "alice@example.org")
        tap(form, "testButton")
        tryCompare(form, "statusKey", "invalid_input", 5000)
        compare(form.errorField, "imap_host")
        form.destroy()
    }

    function test_the_folders_page_lists_the_folders_and_the_switch_changes_the_choice() {
        var p = page("FoldersPage.qml")
        verify(folders.rows.length >= 1)
        compare(findChild(p, "folderName0").text, "Inbox")
        compare(folders.rows[0].syncEnabled, true)
        tap(p, "folderSync0")
        compare(folders.rows[0].syncEnabled, false)
        compare(harness.query("SELECT sync_enabled FROM folder WHERE role = 'inbox'"), "0")
        tap(p, "folderSync0")
        compare(folders.rows[0].syncEnabled, true)
        verify(findChild(p, "foldersEmpty").visible === false)
        p.destroy()
    }

    function test_the_folders_page_has_a_back_button_like_the_other_pages() {
        var p = page("FoldersPage.qml")
        var back = findChild(p, "foldersBack")
        verify(back !== null && back.visible)
        tap(p, "foldersBack")          // no stack around the page in this test: nothing to pop, nothing breaks
        p.destroy()
    }

    function test_the_home_button_of_the_list_leaves_the_app() {
        var list = page("MessageListPage.qml")
        var before = device.quitCalls
        tap(list, "homeButton")
        compare(device.quitCalls, before + 1)
        list.destroy()
    }

    function test_the_folder_tap_opens_it_in_the_list() {
        var list = page("MessageListPage.qml")
        compare(messages.folderRole, "inbox")
        var p = page("FoldersPage.qml")
        var id = folders.rows[0].id
        tap(p, "folderTap0")
        compare(messages.folderId, id)
        p.destroy()
        list.destroy()
    }

    function test_the_status_line_shows_offline_and_the_number_of_waiting_changes() {
        device.wifiUp = false
        var list = page("MessageListPage.qml")
        harness.exec("UPDATE message SET local_state = 'pending_flags' WHERE id IN (SELECT id FROM message LIMIT 2)")
        sync.refreshState()
        var line = findChild(list, "statusLine")
        tryVerify(function () { return line.text.indexOf("Offline") >= 0 }, 3000)
        verify(line.text.indexOf("2 change(s) waiting") >= 0, line.text)
        harness.exec("UPDATE message SET local_state = 'synced'")
        sync.refreshState()
        device.wifiUp = true
        list.destroy()
        var again = page("MessageListPage.qml")
        line = findChild(again, "statusLine")
        verify(line.text.indexOf("Offline") < 0, line.text)
        verify(line.text.indexOf("waiting") < 0, line.text)
        again.destroy()
    }

    function test_older_mail_is_offered_only_while_the_server_has_more() {
        var list = page("MessageListPage.qml")
        compare(findChild(list, "olderButton").visible, false)
        harness.exec("UPDATE folder SET older_on_server = 1 WHERE role = 'inbox'")
        messages.refresh()
        compare(findChild(list, "olderButton").visible, true)
        compare(findChild(list, "olderCaption").visible, true)
        harness.exec("UPDATE folder SET older_on_server = 0")
        messages.refresh()
        compare(findChild(list, "olderButton").visible, false)
        list.destroy()
    }
}
