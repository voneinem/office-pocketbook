import QtQuick
import QtQuick.Controls
import ".."

// Writing a message. The text is saved by itself (2 seconds after the last change, when the page is left, when the app is suspended), so nothing is
// lost; "Send" puts the message into the outbox, and the sync sends it as soon as the reader is online. Discard asks first.
Item {
    id: page
    objectName: "composePage"   // the hand-over navigator (HandoffNavigator.qml) finds an open message by this name

    property var draftId: 0                // an existing draft to continue, 0 for a new message or a prepared reply (compose.start was called)
    property bool prepared: false          // compose.start() was called by the opener
    property string sendNote: ""
    property bool showCcBcc: false

    function causeText(key, field) {
        switch (key) {
        case "no_recipient": return qsTr("Enter a recipient.")
        case "recipient_invalid": return field === "cc" ? qsTr("A Cc address is not valid.") : (field === "bcc" ? qsTr("A Bcc address is not valid.") : qsTr("A recipient address is not valid."))
        case "from_invalid": return qsTr("Your own address in the account settings is not valid.")
        case "no_account": return qsTr("Set up your mail account first.")
        case "too_large": return qsTr("The text is too long to send.")
        case "write_failed": return qsTr("The message could not be saved.")
        default: return qsTr("The message could not be queued (%1).").arg(key)
        }
    }
    function stateText() {
        switch (compose.saveState) {
        case "pending": return qsTr("Saving …")
        case "saved": return qsTr("Saved on this reader")
        case "error": return qsTr("Not saved: the reader's storage may be full.")
        default: return ""
        }
    }
    function title() {
        switch (compose.kind) {
        case "reply": return qsTr("Reply")
        case "reply_all": return qsTr("Reply to all")
        case "forward": return qsTr("Forward")
        default: return qsTr("New message")
        }
    }
    // ---- address suggestions for the recipient fields ----
    property var suggestions: []
    property string suggestWhich: ""
    function suggestFor(which, text) {
        page.suggestWhich = which
        page.suggestions = compose.suggestions(text)
    }
    function chooseSuggestion(entry) {
        var f = suggestWhich === "cc" ? ccField : (suggestWhich === "bcc" ? bccField : toField)
        var t = compose.applySuggestion(f.text, entry)
        f.text = t
        if (suggestWhich === "cc") compose.cc = t
        else if (suggestWhich === "bcc") compose.bcc = t
        else compose.to = t
        page.suggestions = []
        f.forceFocus()
    }
    function goBack() {
        compose.leave()
        if (page.StackView.view) page.StackView.view.pop()
    }

    Connections {
        target: compose
        function onSendResult(ok, key, field) {
            if (ok) {
                page.sendNote = ""
                sync.refreshState()
                sync.noteChange(device.wifiConnected())   // Auto Sync: shortly after, switching Wi-Fi on if needed; with Auto Sync off the message waits for the Sync button
                if (page.StackView.view) page.StackView.view.pop()
            } else {
                page.sendNote = page.causeText(key, field)
                if (key === "recipient_invalid" || key === "no_recipient") { page.showCcBcc = page.showCcBcc || field === "cc" || field === "bcc"; toField.forceFocus() }
            }
        }
        function onFieldsChanged() {
            if (toField.text !== compose.to) toField.text = compose.to
            if (ccField.text !== compose.cc) ccField.text = compose.cc
            if (bccField.text !== compose.bcc) bccField.text = compose.bcc
            if (subjectField.text !== compose.subject) subjectField.text = compose.subject
            if (bodyArea.text !== compose.body) bodyArea.text = compose.body
        }
    }

    Component.onCompleted: {
        if (page.draftId > 0) compose.edit(page.draftId)
        else if (!page.prepared) compose.start("new", 0)
        toField.text = compose.to
        ccField.text = compose.cc
        bccField.text = compose.bcc
        subjectField.text = compose.subject
        bodyArea.text = compose.body
        page.showCcBcc = compose.cc.length > 0 || compose.bcc.length > 0
        focusFirstEmpty()
    }
    // The cursor goes to the first empty one of recipient, subject, body (a message from a hand-over has the text, and maybe more, already). The StackView
    // gives the focus to the page it has just pushed (measured in tst_handoff_flow), which takes it from a field set while the page is created: so it is set
    // again one event-loop turn after the page became the current one (StackView.onActivated), once.
    property bool focusSet: false
    function focusFirstEmpty() {
        if (compose.to.length === 0) toField.forceFocus()
        else if (compose.subject.length === 0) subjectField.forceFocus()
        else { bodyArea.forceActiveFocus(); bodyArea.cursorPosition = 0 }
    }
    StackView.onActivated: { if (!page.focusSet) { page.focusSet = true; Qt.callLater(page.focusFirstEmpty) } }
    Component.onDestruction: compose.leave()

    TopBar {
        id: topBar
        title: page.title()
        leftItems: [ IconButton { objectName: "composeBack"; text: qsTr("Back"); icon: "chevron-left"; onClicked: page.goBack() } ]
        rightItems: [ IconButton { objectName: "composeSend"; text: qsTr("Send"); icon: "send"; onClicked: compose.send() } ]
    }

    Flickable {
        id: scroller
        objectName: "composeScroller"
        anchors { left: parent.left; right: parent.right; top: topBar.bottom; bottom: statusArea.top }
        contentWidth: width
        contentHeight: form.implicitHeight + 40
        boundsBehavior: Flickable.StopAtBounds
        interactive: false
        clip: true

        Column {
            id: form
            x: 30
            y: 20
            width: parent.width - 60
            spacing: 20

            InkField { id: toField; objectName: "composeTo"; width: parent.width; label: qsTr("To"); placeholderText: "name@example.org"
                       field.inputMethodHints: Qt.ImhEmailCharactersOnly | Qt.ImhNoPredictiveText
                       onTextEdited: { compose.to = text; page.suggestFor("to", text) } }
            Column {
                id: suggestionList
                objectName: "composeSuggestions"
                width: parent.width
                visible: page.suggestions.length > 0
                Repeater {
                    model: page.suggestions
                    delegate: InkRow {
                        required property int index
                        required property var modelData
                        objectName: "composeSuggestion" + index
                        text: modelData.display
                        onClicked: page.chooseSuggestion(modelData.text)
                    }
                }
            }
            InkRow { objectName: "ccBccRow"; visible: !page.showCcBcc; text: qsTr("Cc and Bcc"); chevron: true; onClicked: page.showCcBcc = true }
            InkField { id: ccField; objectName: "composeCc"; visible: page.showCcBcc; width: parent.width; label: qsTr("Cc")
                       field.inputMethodHints: Qt.ImhEmailCharactersOnly | Qt.ImhNoPredictiveText
                       onTextEdited: { compose.cc = text; page.suggestFor("cc", text) } }
            InkField { id: bccField; objectName: "composeBcc"; visible: page.showCcBcc; width: parent.width; label: qsTr("Bcc")
                       field.inputMethodHints: Qt.ImhEmailCharactersOnly | Qt.ImhNoPredictiveText
                       onTextEdited: { compose.bcc = text; page.suggestFor("bcc", text) } }
            InkField { id: subjectField; objectName: "composeSubject"; width: parent.width; label: qsTr("Subject")
                       onTextEdited: compose.subject = text }
            Text {
                visible: compose.forwardNote
                width: parent.width
                wrapMode: Text.Wrap
                font.pixelSize: Style.captionSize
                font.italic: true
                color: Style.ink
                text: qsTr("The text of the message is included. Attachments are not.")
            }
            Text { text: qsTr("Message"); font.pixelSize: Style.labelSize; color: Style.ink }
            TextArea {
                id: bodyArea
                objectName: "composeBody"
                width: parent.width
                height: Math.max(420, contentHeight + 40)
                font.pixelSize: Style.bodySize
                color: Style.ink
                wrapMode: TextEdit.Wrap
                inputMethodHints: Qt.ImhNoPredictiveText
                background: Rectangle { color: Style.paper; radius: Style.radiusField; border.width: Style.ruleWidth; border.color: Style.ink }
                TextMenu { target: bodyArea; multiline: true }
                onTextChanged: if (activeFocus || text !== compose.body) compose.body = text
                onCursorRectangleChanged: {
                    // keep the line being typed in view: the form moves by whole steps, no animation
                    const y = form.y + bodyArea.y + cursorRectangle.y
                    if (y + cursorRectangle.height > scroller.contentY + scroller.height) scroller.contentY = Math.min(scroller.contentHeight - scroller.height, y + cursorRectangle.height - scroller.height + 20)
                    else if (y < scroller.contentY) scroller.contentY = Math.max(0, y - 20)
                }
            }
        }
    }

    Column {
        id: statusArea
        anchors { left: parent.left; right: parent.right; bottom: tools.top }
        padding: 14
        spacing: 4
        Text {
            objectName: "composeSendNote"
            width: parent.width - 28
            visible: text.length > 0
            wrapMode: Text.Wrap
            font.pixelSize: Style.bodySize
            font.bold: true
            color: Style.ink
            text: page.sendNote
        }
        Text {
            objectName: "composeSaveState"
            width: parent.width - 28
            font.pixelSize: Style.captionSize
            color: Style.ink
            text: page.stateText()
        }
    }

    BottomPanel {
        id: tools
        anchors.bottom: parent.bottom
        tools: [
            ToolButton { objectName: "composeSendTool"; text: qsTr("Send"); icon: "send"; onClicked: compose.send() },
            ToolButton { objectName: "composeDiscard"; text: qsTr("Discard"); icon: "trash"; onClicked: discardDialog.open() }
        ]
    }

    ConfirmDialog {
        id: discardDialog
        objectName: "discardDialog"
        title: qsTr("Discard this message?")
        text: qsTr("The text is deleted from this reader and from your Drafts folder.")
        acceptText: qsTr("Discard")
        cancelText: qsTr("Keep")
        onAccepted: { compose.discard(); if (page.StackView.view) page.StackView.view.pop() }
    }
}
