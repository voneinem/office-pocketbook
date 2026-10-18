import QtQuick
import QtQuick.Controls
import ".."

// Things that need a decision. Events and contacts: changed here and on the server (both versions are kept until you choose), or deleted on the server
// after you changed them here. Messages: a change you made (read, star, move, delete) could not be applied because the message is gone or moved on the server,
// or the server refused it. Nothing is overwritten or dropped by itself. A choice that drops your version asks first. Paged, never scrolled.
Item {
    id: page

    property int pageIndex: 0
    readonly property int rowHeight: 230
    readonly property int rowsPerPage: Math.max(1, Math.floor((height - topBar.height - status.height - bottomBar.height) / rowHeight))
    readonly property int pageCount: Math.max(1, Math.ceil(conflicts.rows.length / rowsPerPage))
    readonly property var pageRows: conflicts.rows.slice(Math.min(pageIndex, pageCount - 1) * rowsPerPage, (Math.min(pageIndex, pageCount - 1) + 1) * rowsPerPage)
    property var chosen: null
    property string confirmAction: ""

    function messageReason(key) {
        switch (key) {
        case "gone_on_server": return qsTr("The message is no longer on the server.")
        case "moved_on_server": return qsTr("The message was moved on the server.")
        case "no_trash": return qsTr("There is no Trash folder, so the message was not deleted.")
        default: return qsTr("The server did not accept your change.")
        }
    }
    function choicesFor(row) {
        if (row.kind === "message") return [ { label: qsTr("OK, I have seen it"), value: "ack" } ]
        if (row.state === "server_deleted") return [ { label: qsTr("Send it again"), value: "recreate" }, { label: qsTr("Discard my version"), value: "discard" } ]
        if (row.deleting) return [ { label: qsTr("Delete it anyway"), value: "mine" }, { label: qsTr("Keep the server's version"), value: "server" } ]
        return [ { label: qsTr("Keep my version"), value: "mine" }, { label: qsTr("Keep the server's version"), value: "server" }, { label: qsTr("Keep both"), value: "both" } ]
    }
    function perform(value) {
        if (!page.chosen) return
        if (value === "ack") {
            conflicts.acknowledgeMessage(page.chosen.messageId)
            sync.refreshState()
            return
        }
        if (value === "server" || value === "discard") {
            page.confirmAction = value
            confirm.open()
            return
        }
        page.apply(value)
    }
    function apply(value) {
        if (page.chosen.kind === "contact") {
            if (value === "recreate") conflicts.recreateContact(page.chosen.contactId)
            else if (value === "discard") conflicts.discardContact(page.chosen.contactId)
            else conflicts.resolveContact(page.chosen.contactId, value)
            contacts.refresh()
        } else {
            if (value === "recreate") conflicts.recreate(page.chosen.eventId)
            else if (value === "discard") conflicts.discard(page.chosen.eventId)
            else conflicts.resolve(page.chosen.eventId, value)
            agenda.refresh()
            dayModel.refresh()
        }
        sync.refreshState()
    }

    Component.onCompleted: conflicts.refresh()
    Connections {
        target: conflicts
        function onActionResult(action, ok, key) { if (ok && action !== "acknowledge") sync.noteChange(device.wifiConnected()) }
    }

    TopBar {
        id: topBar
        title: qsTr("To decide")
        leftItems: [ IconButton { objectName: "conflictsBack"; text: qsTr("Back"); icon: "chevron-left"; onClicked: if (page.StackView.view) page.StackView.view.pop() } ]
    }
    SyncStatusLine { id: status; anchors { top: topBar.bottom; left: parent.left; right: parent.right } }

    Column {
        id: list
        objectName: "conflictRows"
        anchors { top: status.bottom; left: parent.left; right: parent.right }
        visible: !conflicts.empty
        Repeater {
            model: page.pageRows
            delegate: Item {
                id: row
                required property int index
                required property var modelData
                width: list.width
                height: page.rowHeight
                Rectangle { anchors.fill: parent; color: Style.pressed; visible: tap.pressed }
                Text {
                    objectName: "conflictTitle" + row.index
                    x: 30; y: 16
                    width: parent.width - 60
                    elide: Text.ElideRight
                    font.pixelSize: Style.bodySize
                    font.bold: true
                    color: Style.ink
                    textFormat: Text.PlainText
                    text: row.modelData.title.length > 0 ? row.modelData.title : qsTr("(no title)")
                }
                Text {
                    objectName: "conflictMine" + row.index
                    x: 30; y: 16 + Style.bodySize + 14
                    width: parent.width - 60
                    elide: Text.ElideRight
                    font.pixelSize: Style.captionSize
                    color: Style.ink
                    textFormat: Text.PlainText
                    text: row.modelData.kind === "message" ? qsTr("From: %1").arg(row.modelData.line1)
                          : (row.modelData.kind === "contact" ? (row.modelData.deleting ? qsTr("You deleted this contact.") : qsTr("Yours: %1").arg(row.modelData.mineTitle))
                             : (row.modelData.deleting ? qsTr("You deleted this event.") : qsTr("Yours: %1, %2").arg(row.modelData.mineTitle).arg(row.modelData.mineWhen)))
                }
                Text {
                    objectName: "conflictServer" + row.index
                    x: 30; y: 16 + Style.bodySize + Style.captionSize + 28
                    width: parent.width - 60
                    elide: Text.ElideRight
                    font.pixelSize: Style.captionSize
                    color: Style.ink
                    textFormat: Text.PlainText
                    text: row.modelData.kind === "message" ? page.messageReason(row.modelData.errorKey)
                          : (row.modelData.state === "server_deleted" ? qsTr("It was deleted on the server.")
                             : (row.modelData.kind === "contact" ? qsTr("On the server: %1").arg(row.modelData.serverTitle)
                                : qsTr("On the server: %1, %2").arg(row.modelData.serverTitle).arg(row.modelData.serverWhen)))
                }
                Text {
                    x: 30; y: 16 + Style.bodySize + 2 * Style.captionSize + 42
                    width: parent.width - 60
                    font.pixelSize: Style.captionSize
                    font.italic: true
                    color: Style.ink
                    text: qsTr("Tap to choose")
                }
                RowDivider { anchors.bottom: parent.bottom; x: 30; width: parent.width - 60 }
                MouseArea {
                    id: tap
                    objectName: "conflictTap" + row.index
                    anchors.fill: parent
                    onClicked: { page.chosen = row.modelData; actions.choices = page.choicesFor(row.modelData); actions.anchor = row; actions.open() }
                }
            }
        }
    }
    Text {
        objectName: "conflictsEmpty"
        visible: conflicts.empty
        anchors { top: status.bottom; topMargin: 80; left: parent.left; right: parent.right; leftMargin: 30; rightMargin: 30 }
        wrapMode: Text.Wrap
        horizontalAlignment: Text.AlignHCenter
        font.pixelSize: Style.bodySize
        color: Style.ink
        text: qsTr("Nothing needs a decision.")
    }

    Item {
        id: bottomBar
        anchors { bottom: parent.bottom; left: parent.left; right: parent.right }
        height: Style.bottomBarHeight
        Rectangle { y: 0; width: parent.width; height: Style.hairlineWidth; color: Style.hairline }
        ModelPager {
            anchors.centerIn: parent
            page: Math.min(page.pageIndex, page.pageCount - 1)
            pageCount: page.pageCount
            onPrevious: page.pageIndex = Math.max(0, page.pageIndex - 1)
            onNext: page.pageIndex = Math.min(page.pageCount - 1, page.pageIndex + 1)
        }
    }

    ChoiceDialog {
        id: actions
        objectName: "conflictActions"
        title: qsTr("What do you want to do?")
        onChosen: function (value) { page.perform(value) }
    }
    ConfirmDialog {
        id: confirm
        objectName: "conflictConfirm"
        readonly property bool forContact: page.chosen !== null && page.chosen.kind === "contact"
        title: page.confirmAction === "discard" ? qsTr("Discard your version?") : qsTr("Use the server's version?")
        text: page.confirmAction === "discard" ? (forContact ? qsTr("Your changes to this contact are deleted from this reader.") : qsTr("Your changes to this event are deleted from this reader."))
                                               : (forContact ? qsTr("Your changes to this contact are deleted from this reader. The server's version stays.")
                                                             : qsTr("Your changes to this event are deleted from this reader. The server's version stays."))
        acceptText: page.confirmAction === "discard" ? qsTr("Discard") : qsTr("Use the server's")
        cancelText: qsTr("Cancel")
        onAccepted: page.apply(page.confirmAction)
    }
}
