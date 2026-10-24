import QtQuick
import QtQuick.Controls

// Shows a message made by a hand-over from another app (spec 003, contracts/bridge-qml.md). The bridge has already saved the draft; this opens it on the page stack.
// An open message is saved first and stays as its own draft (FR-006), a dialog is never answered or covered (the page waits until it closes), and Back returns
// to the page below. What was refused or left out is told in one short dialog at a time, never with a value of the hand-over (R-08). No animation (e-ink).
Item {
    id: root
    property var nav: null              // the StackView of the window
    property var heldShow: null         // {draftId, more} of a hand-over that waits for a dialog to close
    property int lastMore: 0            // how many older hand-over drafts waited (in the Drafts list) when the last one was shown; the notice of spec 003 phase 6 names it
    anchors.fill: parent

    // a hand-over that waited for a dialog is shown when the last dialog has closed
    Connections {
        target: DialogState
        function onOpenChanged() {
            if (DialogState.open > 0 || !root.heldShow) return
            var h = root.heldShow
            root.heldShow = null
            root.showHandoff(h.draftId, h.more)
        }
    }

    function showHandoff(draftId, more) {
        if (!nav) return
        lastMore = more
        compose.leave()   // the message being typed is saved now: the compose object is reused for the new draft, and nothing typed may be lost
        for (var i = 0; i < 4; ++i) {
            var cur = nav.currentItem
            if (cur && cur.objectName === "composePage") nav.pop(StackView.Immediate)
            else break
        }
        nav.push(Qt.resolvedUrl("pages/ComposePage.qml"), { draftId: draftId }, StackView.Immediate)
        handoff.shown()
        if (more > 0) addNotice("more_waiting", more)
    }

    // ---- notices: one dialog at a time, in the order they arrive ----
    property var notices: []
    function noticeText(key, n) {
        switch (key) {
        case "too_long": return qsTr("The text was too long to take over. Nothing was created. Hand it over again in a shorter form.")
        case "incomplete": return qsTr("The text did not arrive completely. Nothing was created. Hand it over again.")
        case "malformed": return qsTr("The hand-over could not be understood. Nothing was created.")
        case "subject_too_long": return qsTr("The subject was too long and was left out.")
        case "recipient_ignored": return qsTr("The recipient was not used because it is not a single mail address.")
        case "no_account": return qsTr("To mail this text, set up a mail account first. The text was not kept; hand it over again afterwards.")
        case "nothing_handed": return qsTr("Nothing was handed over. Go back and try again.")
        case "more_waiting": return n === 1 ? qsTr("1 more hand-over waits in your drafts.") : qsTr("%1 more hand-overs wait in your drafts.").arg(n)
        default: return ""
        }
    }
    function addNotice(key, n) {
        if (noticeText(key, n).length === 0) return
        var list = notices
        list.push({ key: key, n: n })
        notices = list
        if (!noticeDialog.visible) showNextNotice()
    }
    function showNextNotice() {
        if (notices.length === 0) return
        var list = notices
        var next = list.shift()
        notices = list
        noticeDialog.text = noticeText(next.key, next.n)
        noticeDialog.open()
    }
    ConfirmDialog {
        id: noticeDialog
        objectName: "handoffNotice"
        title: qsTr("Text from another app")
        showCancel: false
        onAccepted: root.showNextNotice()
        onRejected: root.showNextNotice()
    }

    Connections {
        target: typeof handoff !== "undefined" ? handoff : null
        function onShow(draftId, more) {
            if (DialogState.open > 0) root.heldShow = { draftId: draftId, more: more }   // the newest wins; the older ones are in the Drafts list
            else root.showHandoff(draftId, more)
        }
        function onNotice(key, n) { root.addNotice(key, n) }
    }
    // the mail account was saved in the set-up: a hand-over that waited for it continues (it is held in memory only)
    Connections {
        target: typeof account !== "undefined" ? account : null
        function onMailConfiguredChanged() { if (account.mailConfigured) handoff.accountReady() }
    }
}
