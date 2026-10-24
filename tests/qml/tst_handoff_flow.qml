import QtQuick
import QtQuick.Controls
import QtTest

// The hand-over navigator (qml/HandoffNavigator.qml) over a StackView, with the real compose page and the real bridges (offscreen, 800 x 600).
TestCase {
    id: tc
    name: "handoff_flow"
    visible: true
    when: windowShown
    width: 800
    height: 600

    Component { id: pageStub; Item { property string label: ""; objectName: label } }

    StackView {
        id: nav
        anchors.fill: parent
        // as in Main.qml: no transitions (e-ink)
        pushEnter: Transition {}
        pushExit: Transition {}
        popEnter: Transition {}
        popExit: Transition {}
        replaceEnter: Transition {}
        replaceExit: Transition {}
    }
    property var navigator: null

    function initTestCase() {
        var comp = Qt.createComponent(harness.qmlDir + "HandoffNavigator.qml")
        compare(comp.status, Component.Ready, comp.errorString())
        navigator = comp.createObject(tc, { nav: nav })
        verify(navigator !== null)
    }
    function init() {
        tryVerify(function() { return compose.saveState !== "pending" }, 3000)
        nav.clear()
        harness.exec("DELETE FROM draft")
        nav.push(pageStub.createObject(nav, { label: "homeStub", width: tc.width, height: tc.height }))
    }
    function current() { return nav.currentItem ? nav.currentItem.objectName : "" }
    // the one of recipient, subject, body that has the cursor
    function focused(p) {
        var names = ["composeTo", "composeSubject", "composeBody"]
        for (var i = 0; i < names.length; ++i) {
            var f = findChild(p, names[i])
            var input = f.field !== undefined ? f.field : f
            if (input.focus) return names[i]   // `focus` (not activeFocus): the offscreen window is never the active one
        }
        return ""
    }
    function openedCompose() {
        tryVerify(function() { return current() === "composePage" }, 5000)
        return nav.currentItem
    }
    // the cursor is set when the page is created; wait until the window has given it
    function focusedOf(p) {
        tryVerify(function() { return focused(p) !== "" }, 3000)
        return focused(p)
    }

    function test_a_handoff_on_the_home_page_opens_the_editor_and_back_returns_home() {
        harness.deliver("Hello from another app\nsecond line", "Plan", "bob@example.org")
        var p = openedCompose()
        compare(compose.body, "Hello from another app\nsecond line")
        compare(compose.subject, "Plan")
        compare(compose.to, "bob@example.org")
        compare(focusedOf(p), "composeBody")                     // recipient and subject are filled: the first empty field is the body
        nav.pop()
        tryVerify(function() { return current() === "homeStub" }, 3000)
    }

    function test_a_handoff_while_a_list_is_open_returns_to_the_same_list() {
        nav.push(pageStub.createObject(nav, { label: "listStub", width: tc.width, height: tc.height }))
        harness.deliver("A link")
        openedCompose()
        nav.pop()
        tryVerify(function() { return current() === "listStub" }, 3000)
        nav.pop()
        tryVerify(function() { return current() === "homeStub" }, 3000)
    }

    function test_the_cursor_is_in_the_first_empty_field() {
        harness.deliver("Only a text")
        var p = openedCompose()
        compare(focusedOf(p), "composeTo")                       // nothing but the text: the recipient comes first
        nav.pop()
        tryVerify(function() { return current() === "homeStub" }, 3000)
        harness.deliver("Text and recipient", "", "bob@example.org")
        p = openedCompose()
        compare(focusedOf(p), "composeSubject")
    }

    function test_after_back_the_draft_is_in_the_drafts_list_with_the_text() {
        harness.deliver("Kept text", "Subject")
        openedCompose()
        nav.pop()
        tryVerify(function() { return current() === "homeStub" }, 3000)
        compare(harness.query("SELECT count(*) FROM draft"), "1")
        compare(harness.query("SELECT body FROM draft"), "Kept text")
        compare(harness.query("SELECT subject FROM draft"), "Subject")
        draftList.refresh()
        compare(draftList.total, 1)
    }

    // ---- a hand-over meets what the user was doing (spec User Story 2; the rules do not depend on how the system delivers) ----

    function test_a_message_being_typed_is_saved_first_and_stays_as_its_own_draft() {
        nav.push(harness.pagesDir + "ComposePage.qml", { width: tc.width, height: tc.height }, StackView.Immediate)   // by URL, as the app does: the stack owns and destroys it
        compose.body = "typed by the user, not saved yet"
        compose.to = "carol@example.org"
        compare(compose.saveState, "pending")
        harness.deliver("Handed over text", "Hand-over subject")
        tryVerify(function() { return compose.body === "Handed over text" }, 5000)
        tryVerify(function() { return current() === "composePage" }, 3000)
        compare(compose.subject, "Hand-over subject")
        compare(compose.to, "")                                                    // nothing of the earlier message is merged in
        compare(harness.query("SELECT count(*) FROM draft"), "2")
        compare(harness.query("SELECT body FROM draft WHERE to_text = 'carol@example.org'"), "typed by the user, not saved yet")
        compare(harness.query("SELECT to_text FROM draft WHERE body = 'Handed over text'"), "")
        compare(nav.depth, 2)                                                      // the home page and the new message; the old one is gone
        nav.pop()                                                                  // Back leaves the new message ...
        tryVerify(function() { return current() === "homeStub" }, 3000)
        var earlier = parseInt(harness.query("SELECT id FROM draft WHERE to_text = 'carol@example.org'"))
        verify(compose.edit(earlier))                                              // ... and the earlier draft opens again with its text
        compare(compose.body, "typed by the user, not saved yet")
        compare(compose.to, "carol@example.org")
    }

    function test_a_dialog_is_not_answered_or_covered_and_the_editor_comes_when_it_closes() {
        var dlg = Qt.createComponent(harness.qmlDir + "ConfirmDialog.qml").createObject(tc, { title: "Delete?", text: "Really?" })
        dlg.open()
        verify(dlg.visible)
        harness.deliver("Waits for the dialog")
        wait(400)
        compare(current(), "homeStub")                                             // the page did not change under the dialog
        verify(dlg.visible)                                                        // ... and the dialog was not answered
        compare(harness.query("SELECT count(*) FROM draft"), "1")                  // the text is already safe in a draft
        dlg.close()
        tryVerify(function() { return current() === "composePage" }, 3000)
        compare(compose.body, "Waits for the dialog")
        dlg.destroy()
    }

    function test_two_handoffs_are_two_drafts_and_the_newest_is_shown() {
        harness.deliver("first text")
        harness.deliver("second text")
        tryVerify(function() { return harness.query("SELECT count(*) FROM draft") === "2" }, 5000)
        tryVerify(function() { return current() === "composePage" && compose.body === "second text" }, 5000)
        compare(harness.query("SELECT count(*) FROM draft WHERE body = 'first text'"), "1")   // neither replaced the other
    }

    function test_two_handoffs_while_a_dialog_is_open_announce_the_one_that_waits_in_the_drafts() {
        var dlg = Qt.createComponent(harness.qmlDir + "ConfirmDialog.qml").createObject(tc, { title: "Delete?", text: "Really?" })
        dlg.open()
        harness.deliver("older text")
        harness.deliver("newer text")
        tryVerify(function() { return harness.query("SELECT count(*) FROM draft") === "2" }, 5000)
        compare(current(), "homeStub")
        dlg.close()
        tryVerify(function() { return current() === "composePage" && compose.body === "newer text" }, 3000)
        compare(navigator.lastMore, 1)                                             // the notice of phase 6 says: 1 more hand-over waits in your drafts
        dlg.destroy()
    }

    function test_a_handoff_during_a_sync_does_not_touch_it() {
        var before = harness.syncCalls()
        harness.deliver("during a sync")
        tryVerify(function() { return current() === "composePage" }, 5000)
        compare(sync.syncing, true)                                                // the sync was neither stopped nor asked for another
        compare(harness.syncCalls(), before)
    }

    // ---- User Story 4: notices, and the account set-up (spec 003 FR-008, FR-009, FR-012, FR-021, FR-022) ----

    function noticeDialog() { return findChild(navigator, "handoffNotice") }
    function noticeText() { var d = noticeDialog(); return d && d.visible ? d.text : "" }
    function acceptNotice() {
        var d = noticeDialog()
        verify(d !== null && d.visible, "no notice is shown")
        d.footerClicked("accept")
    }

    function test_a_refused_handoff_shows_its_notice_and_changes_no_page() {
        harness.deliverRaw(["pbo-mail-v1", "text=" + "x".repeat(8193), "textlen=8193"])
        tryVerify(function() { return noticeText() !== "" }, 3000)
        compare(noticeText(), "The text was too long to take over. Nothing was created. Hand it over again in a shorter form.")
        compare(current(), "homeStub")
        acceptNotice()
        verify(!noticeDialog().visible)
        harness.deliverIncomplete()
        tryVerify(function() { return noticeText() !== "" }, 3000)
        compare(noticeText(), "The text did not arrive completely. Nothing was created. Hand it over again.")
        acceptNotice()
        harness.deliverRaw(["pbo-mail-v1", "to=a@example.org", "to=b@example.org"])
        tryVerify(function() { return noticeText() !== "" }, 3000)
        compare(noticeText(), "The hand-over could not be understood. Nothing was created.")
        compare(current(), "homeStub")
        compare(harness.query("SELECT count(*) FROM draft"), "0")
        acceptNotice()
    }

    function test_no_notice_shows_a_value_of_the_handoff() {
        var word = "ZQXJ4711"
        harness.deliverRaw(["pbo-mail-v1", "text=" + word + "-text", "textlen=1", "subject=" + word + "-subject", "to=" + word + "@example.org"])
        tryVerify(function() { return noticeText() !== "" }, 3000)
        verify(noticeText().indexOf(word) < 0, "the notice repeats a value: " + noticeText())
        verify(noticeDialog().title.indexOf(word) < 0)
        acceptNotice()
        harness.deliver("t", "S".repeat(301), "a@example.org, b@example.org")
        tryVerify(function() { return current() === "composePage" }, 3000)
        tryVerify(function() { return noticeText() !== "" }, 3000)
        verify(noticeText().indexOf("SSSSSSSS") < 0 && noticeText().indexOf("example.org") < 0)
        acceptNotice()
        tryVerify(function() { return noticeText() !== "" }, 3000)
        verify(noticeText().indexOf("example.org") < 0)
        acceptNotice()
    }

    function test_what_was_left_out_is_told_after_the_editor_opened_one_notice_at_a_time() {
        harness.deliver("A text", "S".repeat(301), "a@example.org, b@example.org")
        tryVerify(function() { return current() === "composePage" }, 3000)
        compare(compose.body, "A text")
        compare(compose.subject, "")
        compare(compose.to, "")
        tryVerify(function() { return noticeText() !== "" }, 3000)
        compare(noticeText(), "The subject was too long and was left out.")
        acceptNotice()
        tryVerify(function() { return noticeText() === "The recipient was not used because it is not a single mail address." }, 3000)
        acceptNotice()
        verify(!noticeDialog().visible)
    }

    function test_one_that_waited_in_the_drafts_is_announced_after_the_editor() {
        var dlg = Qt.createComponent(harness.qmlDir + "ConfirmDialog.qml").createObject(tc, { title: "Delete?", text: "Really?" })
        dlg.open()
        harness.deliver("older text")
        harness.deliver("newer text")
        tryVerify(function() { return harness.query("SELECT count(*) FROM draft") === "2" }, 5000)
        dlg.close()
        tryVerify(function() { return current() === "composePage" && compose.body === "newer text" }, 3000)
        tryVerify(function() { return noticeText() !== "" }, 3000)
        compare(noticeText(), "1 more hand-over waits in your drafts.")
        acceptNotice()
        dlg.destroy()
    }

    function test_on_the_account_setup_page_a_handoff_waits_for_the_account_and_then_opens_the_editor() {
        harness.removeMail()
        nav.push(harness.pagesDir + "AccountSetupPage.qml", { width: tc.width, height: tc.height }, StackView.Immediate)
        harness.deliver("Waits for the account", "Plan", "bob@example.org")
        wait(400)
        compare(current(), "accountSetupPage")                                     // no editor over the set-up
        compare(noticeText(), "")
        compare(harness.query("SELECT count(*) FROM draft"), "0")                  // held in memory only
        harness.configureMail()                                                    // the user saved the account
        tryVerify(function() { return current() === "composePage" }, 5000)
        compare(compose.body, "Waits for the account")
        compare(compose.subject, "Plan")
        compare(compose.to, "bob@example.org")
        compare(harness.query("SELECT count(*) FROM draft"), "1")
    }

    function test_leaving_the_account_setup_with_a_held_handoff_says_the_text_was_not_kept() {
        harness.removeMail()
        nav.push(harness.pagesDir + "AccountSetupPage.qml", { width: tc.width, height: tc.height }, StackView.Immediate)
        harness.deliver("Will not be kept")
        wait(300)
        compare(noticeText(), "")
        nav.pop(StackView.Immediate)
        tryVerify(function() { return noticeText() !== "" }, 3000)
        compare(noticeText(), "To mail this text, set up a mail account first. The text was not kept; hand it over again afterwards.")
        acceptNotice()
        harness.configureMail()
        wait(300)
        compare(harness.query("SELECT count(*) FROM draft"), "0")                  // it does not come back
    }

    function test_without_a_mail_account_and_not_on_the_setup_page_the_user_is_told_at_once() {
        harness.removeMail()
        harness.deliver("Calendar-only account")
        tryVerify(function() { return noticeText() !== "" }, 3000)
        compare(noticeText(), "To mail this text, set up a mail account first. The text was not kept; hand it over again afterwards.")
        compare(current(), "homeStub")
        acceptNotice()
        harness.configureMail()
    }

    function cleanup() {
        harness.configureMail()                                                    // every test starts with a mail account, as the others expect
        var d = noticeDialog()
        if (d && d.visible) d.close()
    }
}
