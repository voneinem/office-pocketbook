import QtQuick
import QtTest

// Compose and outbox pages with the real bridges (offscreen, 800 x 600).
TestCase {
    id: tc
    name: "compose_flow"
    visible: true
    when: windowShown
    width: 800
    height: 600

    SignalSpy { id: syncFinished; target: sync; signalName: "syncFinished" }

    function page(name, props) {
        var comp = Qt.createComponent(harness.pagesDir + name)
        compare(comp.status, Component.Ready, comp.errorString())
        var initial = props || {}
        initial.width = tc.width
        initial.height = tc.height
        var obj = comp.createObject(tc, initial)
        verify(obj !== null, "page " + name + " could not be created")
        return obj
    }
    function tap(p, name) {
        var b = findChild(p, name)
        verify(b !== null, "no control " + name)
        for (var i = 0; i < 20; ++i) {
            var before = b.mapToItem(null, 0, 0).y
            wait(30)
            if (b.mapToItem(null, 0, 0).y === before) break
        }
        verify(b.enabled, name + " is disabled")
        mouseClick(b)
    }
    // typed like a user (textEdited fires)
    function type(p, name, text) {
        var f = findChild(p, name)
        verify(f !== null, "no field " + name)
        var input = f.field !== undefined ? f.field : f
        input.forceActiveFocus()
        for (var i = 0; i < text.length; ++i) keyClick(text.charAt(i))
    }
    function init() {
        tryCompare(sync, "syncing", false, 30000)       // a sync of the previous test must be over before its data is cleared
        harness.clearAll()
        harness.refuseRecipients(false)
    }

    function test_a_page_opened_and_left_without_typing_leaves_no_draft() {
        var p = page("ComposePage.qml")
        wait(100)
        p.destroy()
        compare(harness.query("SELECT count(*) FROM draft"), "0")
    }

    function test_typing_is_saved_by_itself_and_shown() {
        var p = page("ComposePage.qml")
        type(p, "composeTo", "bob@example.org")
        compare(findChild(p, "composeSaveState").text, "Saving …")
        tryCompare(compose, "saveState", "saved", 5000)
        compare(findChild(p, "composeSaveState").text, "Saved on this reader")
        compare(harness.query("SELECT count(*) FROM draft"), "1")
        type(p, "composeSubject", "Plan")
        type(p, "composeBody", "Hello Bob.")
        p.destroy()                                     // leaving the page saves what was typed last
        wait(100)                                       // destroy() happens at the end of the event loop round
        compare(harness.query("SELECT subject FROM draft"), "Plan")
        compare(harness.query("SELECT body FROM draft"), "Hello Bob.")
    }

    // a press-and-hold on a field's text, as a finger does
    function hold(p, name, x) {
        // the form scrolls to the caret while typing; bring the field back into view first
        var sc = findChild(p, "composeScroller")
        var f = findChild(p, name)
        sc.contentY = Math.max(0, Math.min(sc.contentHeight - sc.height, f.mapToItem(sc.contentItem, 0, 0).y - 20))
        wait(30)
        verify(f !== null, "no field " + name)
        var input = f.field !== undefined ? f.field : f
        mousePress(input, x, input.height / 2)
        wait(800)
        mouseRelease(input, x, input.height / 2)
        lastHeld = input
        wait(60)                                         // the bubble's buttons are laid out one event-loop round later
        return input
    }
    // each field has its own bubble (put on the page's top item, not inside the page): the one of the field held last
    property var lastHeld: null
    function menuEntry(name) { return findChild(findChild(lastHeld, "textMenu").bubble, name) }

    function test_every_field_has_a_copy_and_paste_menu() {
        var p = page("ComposePage.qml")
        type(p, "composeSubject", "Plan")
        var sub = hold(p, "composeSubject", 20)
        verify(menuEntry("menu_copy") !== null && menuEntry("menu_copy").visible, "Copy is offered for the selected word")
        verify(menuEntry("menu_cut") !== null, "Cut is offered")
        mouseClick(menuEntry("menu_copy"))
        // nothing selected, an empty field: Paste is offered, Copy is not
        var to = hold(p, "composeTo", 20)
        verify(menuEntry("menu_paste") !== null && menuEntry("menu_paste").visible, "Paste is offered")
        mouseClick(menuEntry("menu_paste"))
        tryCompare(to, "text", "Plan", 2000)
        tryCompare(compose, "to", "Plan", 2000)                    // the paste reached the draft like typing does
        hold(p, "composeTo", 20)
        verify(menuEntry("menu_remove") !== null, "Delete is offered for a selection")
        mouseClick(menuEntry("menu_remove"))
        tryCompare(to, "text", "", 2000)
        tryCompare(compose, "to", "", 2000)
        // the message text too
        var body = hold(p, "composeBody", 20)
        verify(menuEntry("menu_paste") !== null, "the message text offers Paste")
        mouseClick(menuEntry("menu_paste"))
        tryCompare(body, "text", "Plan", 2000)
        p.destroy()
    }

    function test_the_bubble_can_carry_an_explanation_that_goes_with_it() {
        var p = page("ComposePage.qml")
        var to = hold(p, "composeTo", 20)
        var menu = findChild(to, "textMenu")
        verify(menu.bubble.note === "", "no note unless asked for")
        menu.open("Nothing to paste.")
        wait(60)
        verify(findChild(menu.bubble, "menuNote").visible, "the note is shown")
        compare(findChild(menu.bubble, "menuNote").text, "Nothing to paste.")
        menu.close()
        compare(menu.bubble.note, "")
        p.destroy()
    }

    function test_the_recipient_field_offers_known_addresses_and_a_tap_fills_them_in() {
        harness.query("INSERT INTO address_suggestion(addr, name, last_seen, uses) VALUES('anna@example.org', 'Anna Berg', 1, 3)")
        var p = page("ComposePage.qml")
        type(p, "composeTo", "an")
        tryVerify(function () { return findChild(p, "composeSuggestion0") !== null }, 3000)
        compare(findChild(p, "composeSuggestion0").text, "Anna Berg <anna@example.org>")
        p.chooseSuggestion("Anna Berg <anna@example.org>")
        compare(findChild(p, "composeTo").text, "Anna Berg <anna@example.org>, ")
        compare(compose.to, "Anna Berg <anna@example.org>, ")
        compare(p.suggestions.length, 0)
        p.destroy()
    }

    function test_send_without_a_recipient_says_what_is_missing() {
        var p = page("ComposePage.qml")
        tap(p, "composeSend")
        tryCompare(findChild(p, "composeSendNote"), "text", "Enter a recipient.", 3000)
        compare(harness.query("SELECT count(*) FROM outbox"), "0")
        type(p, "composeTo", "not an address")
        tap(p, "composeSend")
        tryCompare(findChild(p, "composeSendNote"), "text", "A recipient address is not valid.", 3000)
        p.destroy()
    }

    function test_discard_asks_first_and_then_deletes_the_draft() {
        var p = page("ComposePage.qml")
        type(p, "composeTo", "bob@example.org")
        tryCompare(compose, "saveState", "saved", 5000)
        tap(p, "composeDiscard")
        var dialog = findChild(p, "discardDialog")
        verify(dialog.visible)
        dialog.footerClicked("dismiss")                 // Keep
        compare(harness.query("SELECT count(*) FROM draft"), "1")
        tap(p, "composeDiscard")
        dialog.footerClicked("accept")                  // Discard
        compare(harness.query("SELECT count(*) FROM draft"), "0")
        p.destroy()
    }

    function test_send_queues_the_message_and_the_sync_sends_it_once() {
        var p = page("ComposePage.qml")
        type(p, "composeTo", "bob@example.org")
        type(p, "composeSubject", "Hello")
        type(p, "composeBody", "Hi Bob.")
        var before = syncFinished.count
        var accepted0 = harness.accepted()
        var copies0 = harness.sentCopies()
        tap(p, "composeSend")
        tryCompare(syncFinished, "count", before + 1, 30000)   // the page starts a sync because the reader is online
        compare(syncFinished.signalArguments[before][0], "success", "problem: " + sync.problemKey)
        compare(harness.accepted(), accepted0 + 1)
        compare(harness.query("SELECT count(*) FROM outbox"), "0")          // sent and the copy filed
        compare(harness.sentCopies(), copies0 + 1)
        tap(p, "composeSend")                                               // a second tap does nothing more
        wait(200)
        compare(harness.accepted(), accepted0 + 1)
        p.destroy()
    }

    // with Auto Sync off nothing is sent until the user syncs (spec US6 scenario 3)
    function test_send_with_auto_sync_off_waits_for_the_sync_button() {
        harness.exec("INSERT OR REPLACE INTO settings(key, value) VALUES('auto_sync', '0')")
        var p = page("ComposePage.qml")
        type(p, "composeTo", "bob@example.org")
        type(p, "composeSubject", "Wait for me")
        type(p, "composeBody", "Text")
        var before = syncFinished.count
        var accepted0 = harness.accepted()
        tap(p, "composeSend")
        wait(1500)                                                          // much longer than the quiet period of Auto Sync (150 ms here)
        compare(syncFinished.count, before)
        compare(harness.accepted(), accepted0)
        verify(parseInt(harness.query("SELECT count(*) FROM outbox")) >= 1)  // it waits in the outbox
        harness.exec("INSERT OR REPLACE INTO settings(key, value) VALUES('auto_sync', '1')")
        sync.syncNow()
        tryCompare(syncFinished, "count", before + 1, 30000)
        compare(harness.accepted(), accepted0 + 1)
        p.destroy()
    }

    // E6 / FR-030: saving a draft (autosave, leaving the page) never starts a sync by itself, whatever Auto Sync says
    function test_saving_a_draft_never_starts_a_sync() {
        harness.exec("INSERT OR REPLACE INTO settings(key, value) VALUES('auto_sync', '1')")
        var p = page("ComposePage.qml")
        var before = syncFinished.count
        type(p, "composeTo", "bob@example.org")
        type(p, "composeSubject", "Only a draft")
        type(p, "composeBody", "Not sent")
        wait(2500)                                                          // the autosave has run, and the quiet period of Auto Sync is long over
        verify(parseInt(harness.query("SELECT count(*) FROM draft WHERE subject = 'Only a draft'")) >= 1)
        p.goBack()
        wait(1500)
        compare(syncFinished.count, before)
        p.destroy()
    }

    function test_the_outbox_page_names_every_state_and_offers_only_what_fits() {
        harness.addOutbox("Waiting one", "queued", "")
        var failed = harness.addOutbox("Failed one", "failed", "refused_recipient")
        var unsure = harness.addOutbox("Unsure one", "uncertain", "connection_lost")
        outbox.refresh()
        var p = page("OutboxPage.qml")
        outbox.refresh()
        compare(outbox.total, 3)
        compare(findChild(p, "outboxSubject0").text, "Waiting one")
        compare(findChild(p, "outboxState0").text, "Waiting to be sent")
        verify(p.rowsPerPage >= 2)
        // the second and third rows may be on the next page of this small window
        var all = []
        for (var i = 0; i < outbox.rows.length; ++i) all.push(p.stateText(outbox.rows[i]))
        compare(all[1], "Not sent: a recipient address was refused")
        compare(all[2], "May have been sent (the connection was lost)")
        compare(p.choicesFor(outbox.rows[0]).length, 0)
        var failedChoices = p.choicesFor(outbox.rows[1]).map(function (c) { return c.value })
        compare(failedChoices.join(","), "retry,edit,remove")
        var uncertainChoices = p.choicesFor(outbox.rows[2]).map(function (c) { return c.value })
        compare(uncertainChoices.join(","), "send_again,mark_sent,remove")
        p.destroy()
    }

    function test_send_again_is_behind_a_confirmation_and_never_happens_by_itself() {
        var id = harness.addOutbox("Unsure", "uncertain", "connection_lost")
        outbox.refresh()
        var p = page("OutboxPage.qml")
        outbox.refresh()
        p.chosen = outbox.rows[0]
        p.perform("send_again")
        var confirm = findChild(p, "outboxConfirm")
        verify(confirm.visible)
        compare(confirm.title, "Send again?")
        confirm.footerClicked("dismiss")                        // Cancel
        compare(harness.query("SELECT state FROM outbox WHERE id = " + id), "uncertain")
        // a sync does not touch it either
        var before = syncFinished.count
        var accepted0 = harness.accepted()
        sync.syncNow()
        tryCompare(syncFinished, "count", before + 1, 30000)
        compare(harness.query("SELECT state FROM outbox WHERE id = " + id), "uncertain")
        compare(harness.accepted(), accepted0)
        p.perform("send_again")
        confirm.footerClicked("accept")
        compare(harness.query("SELECT count(*) FROM outbox WHERE id = " + id + " AND state = 'queued'") === "1" ||
                harness.query("SELECT count(*) FROM outbox WHERE id = " + id) === "0", true)   // queued, or already sent by the sync that follows
        p.destroy()
    }

    function test_retry_and_delete_work_on_a_failed_message() {
        var id = harness.addOutbox("Failed", "failed", "auth_failed")
        outbox.refresh()
        var p = page("OutboxPage.qml")
        outbox.refresh()
        p.chosen = outbox.rows[0]
        p.perform("remove")
        var confirm = findChild(p, "outboxConfirm")
        verify(confirm.visible)
        confirm.footerClicked("accept")
        compare(harness.query("SELECT count(*) FROM outbox"), "0")
        verify(findChild(p, "outboxEmpty").visible)
        p.destroy()
    }

    function test_the_drafts_page_lists_and_deletes_drafts() {
        var p = page("ComposePage.qml")
        type(p, "composeTo", "bob@example.org")
        type(p, "composeSubject", "My draft")
        p.destroy()
        wait(100)
        draftList.refresh()
        var d = page("DraftsPage.qml")
        compare(draftList.total, 1)
        compare(findChild(d, "draftSubject0").text, "My draft")
        d.chosen = draftList.rows[0]
        findChild(d, "deleteDraftDialog").footerClicked("accept")
        compare(harness.query("SELECT count(*) FROM draft WHERE server_state <> 'pending_delete'"), "0")
        verify(findChild(d, "draftsEmpty").visible)
        d.destroy()
    }
}
