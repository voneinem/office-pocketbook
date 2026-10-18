import QtQuick
import QtQuick.Controls
import ".."

// One contact: name, organization, then the phone numbers, email addresses and postal addresses with their labels, the note and the address book.
// Paged by whole lines like the reader (no kinetic scrolling). Everything is plain text: nothing here is a link, and nothing starts a call, a mail
// or a navigation (FR-011).
Item {
    id: page

    property var contactId: 0
    readonly property int lineHeight: 44
    readonly property int margin: 30
    readonly property int pageCount: pager.pageCount

    function readOnlyText() {
        switch (contact.readOnlyReason) {
        case "unreadable": return qsTr("This contact could not be read and cannot be changed here.")
        case "gone": return qsTr("This address book is gone on the server.")
        case "book": return qsTr("This address book is read-only.")
        case "conflict": return qsTr("This contact was changed here and on the server; both versions are kept.")
        case "deleting": return qsTr("This contact is waiting to be deleted on the server.")
        default: return ""
        }
    }

    Component.onCompleted: contact.open(page.contactId)
    StackView.onActivated: contact.open(page.contactId)
    Component.onDestruction: contact.close()

    Connections {
        target: contact
        function onLocalChange() { contacts.refresh() }   // a retry, a discard or a delete changes what waits for the server (Main.qml asks for the sync)
    }

    TopBar {
        id: topBar
        title: qsTr("Contact")
        leftItems: [ IconButton { objectName: "contactBack"; text: qsTr("Back"); icon: "chevron-left"; onClicked: if (page.StackView.view) page.StackView.view.pop() } ]
        rightItems: [
            IconButton { objectName: "contactEdit"; text: qsTr("Edit"); icon: "pencil"; visible: contact.loaded && !contact.readOnly
                         onClicked: if (page.StackView.view) page.StackView.view.push(Qt.resolvedUrl("ContactEditPage.qml"), { contactId: page.contactId }) },
            IconButton { objectName: "contactDelete"; text: qsTr("Delete"); icon: "trash"; visible: contact.loaded && !contact.readOnly; onClicked: deleteDialog.open() },
            IconButton { objectName: "contactDecide"; text: qsTr("To decide"); icon: "info"; visible: contact.conflict || contact.deletedOnServer
                         onClicked: { conflicts.refresh(); if (page.StackView.view) page.StackView.view.push(Qt.resolvedUrl("ConflictsPage.qml")) } },
            IconButton { objectName: "contactRetry"; text: qsTr("Retry"); icon: "sync"; visible: contact.failedReason.length > 0; onClicked: contact.retry() },
            IconButton { objectName: "contactDiscard"; text: qsTr("Discard"); icon: "close"; visible: contact.failedReason.length > 0; onClicked: discardDialog.open() }
        ]
    }
    SyncStatusLine { id: status; anchors { top: topBar.bottom; left: parent.left; right: parent.right } }

    Item {
        id: area
        anchors { top: status.bottom; left: parent.left; right: parent.right; bottom: bottomBar.top }
        clip: true

        Flickable {
            id: view
            objectName: "contactView"
            width: parent.width
            height: Math.max(page.lineHeight, Math.floor(area.height / page.lineHeight) * page.lineHeight)
            contentWidth: width
            contentHeight: content.height
            interactive: false
            boundsBehavior: Flickable.StopAtBounds
            clip: true

            Column {
                id: content
                x: page.margin
                width: view.width - 2 * page.margin

                component Line: Text {
                    width: content.width
                    wrapMode: Text.Wrap
                    textFormat: Text.PlainText      // a contact is never read as markup, so nothing in it becomes a link
                    font.pixelSize: Style.bodySize
                    lineHeight: page.lineHeight
                    lineHeightMode: Text.FixedHeight
                    color: Style.ink
                }
                component Gap: Item { width: 1; height: page.lineHeight }
                // one entry: the label at the left, the value next to it
                component Entry: Row {
                    id: entry
                    property string tag
                    property string label
                    property string value
                    width: content.width
                    spacing: 20
                    Text {
                        objectName: entry.tag + "Label"
                        width: 190
                        height: page.lineHeight
                        verticalAlignment: Text.AlignVCenter
                        elide: Text.ElideRight
                        textFormat: Text.PlainText
                        font.pixelSize: Style.captionSize
                        color: Style.ink
                        text: entry.label
                    }
                    Text {
                        objectName: entry.tag + "Value"
                        width: content.width - 190 - 20
                        wrapMode: Text.Wrap
                        textFormat: Text.PlainText
                        font.pixelSize: Style.bodySize
                        lineHeight: page.lineHeight
                        lineHeightMode: Text.FixedHeight
                        color: Style.ink
                        text: entry.value
                    }
                }

                Line { objectName: "contactName"; text: contact.displayName; font.bold: true; visible: contact.loaded }
                Line { objectName: "contactOrg"; text: contact.organization; visible: contact.organization.length > 0 }
                Line { objectName: "contactMissing"; text: qsTr("This contact is no longer available."); visible: !contact.loaded }
                Line { objectName: "contactReadOnly"; text: page.readOnlyText(); visible: text.length > 0; font.italic: true }
                Line { objectName: "contactPending"; text: qsTr("Changes to this contact have not been sent yet."); visible: contact.pending && contact.failedReason.length === 0 && !contact.deleting; font.italic: true }
                Line { objectName: "contactFailed"; text: qsTr("The server did not take this contact: %1").arg(contact.failedReason); visible: contact.failedReason.length > 0; font.italic: true }
                Line { objectName: "contactDeletedOnServer"; text: qsTr("This contact was deleted on the server. Your version is kept."); visible: contact.deletedOnServer; font.italic: true }
                Gap { visible: contact.phones.length > 0 || contact.emails.length > 0 || contact.addresses.length > 0 }
                Repeater {
                    model: contact.phones
                    delegate: Entry {
                        required property int index
                        required property var modelData
                        tag: "contactPhone" + index
                        label: modelData.label
                        value: modelData.value
                    }
                }
                Repeater {
                    model: contact.emails
                    delegate: Entry {
                        required property int index
                        required property var modelData
                        tag: "contactEmail" + index
                        label: modelData.label
                        value: modelData.value
                    }
                }
                Repeater {
                    model: contact.addresses
                    delegate: Entry {
                        required property int index
                        required property var modelData
                        tag: "contactAddress" + index
                        label: modelData.label
                        value: modelData.text
                    }
                }
                Gap { visible: contact.note.length > 0 }
                Line { objectName: "contactNote"; text: contact.note; visible: contact.note.length > 0 }
                Gap { visible: contact.loaded }
                Entry { visible: contact.loaded; tag: "contactBook"; label: qsTr("Address book"); value: contact.bookName }
            }
        }
    }

    Item {
        id: bottomBar
        anchors { bottom: parent.bottom; left: parent.left; right: parent.right }
        height: Style.bottomBarHeight
        Rectangle { y: 0; width: parent.width; height: Style.hairlineWidth; color: Style.hairline }
        PageButtons {
            id: pager
            anchors.centerIn: parent
            target: view
            snap: page.lineHeight
            showIndicator: true
        }
    }

    ConfirmDialog {
        id: deleteDialog
        objectName: "contactDeleteDialog"
        title: qsTr("Delete contact?")
        text: qsTr("Delete %1? The deletion is sent to the server at the next sync.").arg(contact.displayName)
        acceptText: qsTr("Delete")
        cancelText: qsTr("Cancel")
        onAccepted: {
            if (contact.remove()) {
                contacts.refresh()
                if (page.StackView.view) page.StackView.view.pop()
            }
        }
    }
    ConfirmDialog {
        id: discardDialog
        objectName: "contactDiscardDialog"
        title: qsTr("Discard this change?")
        text: qsTr("The change the server did not take is removed from this reader.")
        acceptText: qsTr("Discard")
        cancelText: qsTr("Keep")
        onAccepted: {
            contact.discardChange()
            if (page.StackView.view) page.StackView.view.pop()
        }
    }
}
