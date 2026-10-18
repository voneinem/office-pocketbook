import QtQuick
import QtQuick.Controls
import ".."

// The contacts of the shown address books: a search field, then the contacts in name order. Paged, never scrolled. Everything shown comes from the
// reader's own storage; a tap on a contact opens it. Without a calendar server account, without an address book on the server, or with nothing
// found the page says why instead of showing an empty page.
Item {
    id: page

    readonly property int rowsPerPage: Math.max(2, Math.floor((height - topBar.height - searchRow.height - status.height - notes.height - bottomBar.height) / Style.rowHeight))
    // "" = rows are shown; otherwise why not
    readonly property string emptyKind: contacts.state === "ok" ? "" : (contacts.state === "noAccount" ? "no_account" : (contacts.state === "noAddressbook" ? "no_addressbook" : (contacts.state === "noMatch" ? "no_match" : "empty")))
    property bool connecting: false
    property bool noNetwork: false

    function push(name, props) { if (page.StackView.view) page.StackView.view.push(Qt.resolvedUrl(name), props || {}) }
    function startSync() {
        page.noNetwork = false
        if (device.wifiConnected()) {
            sync.syncNow()
        } else {
            page.connecting = true
            device.connectNetwork()
        }
    }
    function openContact(row) { contact.open(row.contactId); page.push("ContactPage.qml", { contactId: row.contactId }) }
    function runSearch() { contacts.search(searchField.text) }
    function clearSearch() {
        searchField.text = ""
        contacts.clearSearch()
    }

    onRowsPerPageChanged: contacts.pageSize = rowsPerPage
    Component.onCompleted: {
        contacts.pageSize = rowsPerPage
        contacts.refresh()
    }
    StackView.onActivated: contacts.refresh()   // back from a contact

    Connections {
        target: device
        function onNetworkResult(connected) {
            if (!page.connecting) return
            page.connecting = false
            if (!connected) page.noNetwork = true
            else sync.syncNow()
        }
    }
    Connections { target: sync; function onSyncFinished(outcome) { contacts.refresh() } }

    TopBar {
        id: topBar
        title: qsTr("Contacts")
        leftItems: [ IconButton { objectName: "contactsBack"; text: qsTr("Back"); icon: "chevron-left"; onClicked: if (page.StackView.view) page.StackView.view.pop() } ]
        rightItems: [
            IconButton { objectName: "newContactButton"; text: qsTr("New contact"); icon: "plus-circle"; enabled: contacts.canCreate; onClicked: page.push("ContactEditPage.qml") },
            IconButton { objectName: "contactsSyncButton"; text: qsTr("Sync"); icon: "sync"; enabled: !sync.syncing && !page.connecting; onClicked: page.startSync() }
        ]
    }

    SyncStatusLine {
        id: status
        anchors { top: topBar.bottom; left: parent.left; right: parent.right }
        connecting: page.connecting
        noNetwork: page.noNetwork
    }

    Item {
        id: searchRow
        visible: contacts.state !== "noAccount" && contacts.state !== "noAddressbook"
        anchors { top: status.bottom; left: parent.left; right: parent.right }
        height: visible ? 110 : 0
        InkField {
            id: searchField
            objectName: "contactsSearch"
            x: 30
            y: 16
            width: parent.width - 60 - 2 * Style.touchMin - 12
            placeholderText: qsTr("Search contacts")
            onAccepted: page.runSearch()
        }
        IconButton { objectName: "contactsSearchButton"; x: parent.width - 30 - 2 * Style.touchMin - 6; y: 8; text: qsTr("Search"); icon: "search"; onClicked: page.runSearch() }
        IconButton { objectName: "contactsSearchClear"; x: parent.width - 30 - Style.touchMin; y: 8; text: qsTr("Clear"); icon: "close"; onClicked: page.clearSearch() }
    }

    Column {
        id: rows
        objectName: "contactsRows"
        anchors { top: searchRow.bottom; left: parent.left; right: parent.right }
        visible: page.emptyKind === ""

        Repeater {
            model: contacts.rows
            delegate: ContactRow {
                required property int index
                required property var modelData
                tag: "contactRow" + index
                name: modelData.displayName
                subtitle: modelData.subtitle
                bookName: modelData.bookName
                mark: modelData.pendingMark
                unreadable: modelData.unreadable === true
                onClicked: page.openContact(modelData)
            }
        }
    }

    Column {
        objectName: "contactsEmpty"
        visible: page.emptyKind !== ""
        anchors { top: searchRow.bottom; topMargin: 80; left: parent.left; right: parent.right }
        spacing: 20
        Text {
            objectName: "contactsEmptyTitle"
            x: 30
            width: parent.width - 60
            wrapMode: Text.Wrap
            horizontalAlignment: Text.AlignHCenter
            font.pixelSize: Style.rowTitleSize
            font.bold: true
            color: Style.ink
            text: page.emptyKind === "no_account" ? qsTr("No contacts yet")
                : (page.emptyKind === "no_addressbook" ? qsTr("No address book")
                : (page.emptyKind === "no_match" ? qsTr("Nothing found") : qsTr("No contacts")))
        }
        Text {
            objectName: "contactsEmptyText"
            x: 30
            width: parent.width - 60
            wrapMode: Text.Wrap
            horizontalAlignment: Text.AlignHCenter
            font.pixelSize: Style.bodySize
            color: Style.ink
            text: page.emptyKind === "no_account" ? qsTr("Contacts come from your calendar server. Set up a calendar server account in the calendar first.")
                : (page.emptyKind === "no_addressbook" ? qsTr("No address book was found on the server yet. Tap the sync button to look for one.")
                : (page.emptyKind === "no_match" ? qsTr("No contact matches your search. Tap the cross to see all contacts.")
                : qsTr("The address books hold no contacts. Tap the sync button to look for news.")))
        }
    }

    Column {
        id: notes
        anchors { bottom: bottomBar.top; left: parent.left; right: parent.right }
        Text {
            objectName: "contactsNewHint"
            visible: contacts.createBlockedReason === "no_writable_book"
            x: 30
            width: parent.width - 60
            wrapMode: Text.Wrap
            font.pixelSize: Style.captionSize
            font.italic: true
            color: Style.ink
            text: qsTr("New contacts need a writable address book. The ones shown here are read-only.")
        }
        // a deletion stays visible as waiting and can be undone until the server confirmed it (FR-019)
        Repeater {
            model: contacts.deletions
            delegate: Item {
                id: del
                required property int index
                required property var modelData
                width: notes.width
                height: Style.touchMin + 10
                Text {
                    objectName: "deletionWaiting" + del.index
                    x: 30
                    width: parent.width - 60 - 200
                    height: parent.height
                    verticalAlignment: Text.AlignVCenter
                    elide: Text.ElideRight
                    textFormat: Text.PlainText
                    font.pixelSize: Style.captionSize
                    font.italic: true
                    color: Style.ink
                    text: qsTr("Deletion waiting: %1").arg(del.modelData.displayName)
                }
                IconButton {
                    objectName: "deletionUndo" + del.index
                    x: parent.width - 30 - 190
                    width: 190
                    text: qsTr("Undo")
                    icon: "redo"
                    onClicked: { contact.undoRemove(del.modelData.contactId); contacts.refresh() }
                }
            }
        }
        Text {
            objectName: "deletionMore"
            visible: contacts.deletionCount > 3
            x: 30
            width: parent.width - 60
            wrapMode: Text.Wrap
            font.pixelSize: Style.captionSize
            font.italic: true
            color: Style.ink
            text: qsTr("%1 more deletions are waiting for the next sync.").arg(contacts.deletionCount - 3)
        }
    }

    Item {
        id: bottomBar
        anchors { bottom: parent.bottom; left: parent.left; right: parent.right }
        height: Style.bottomBarHeight
        Rectangle { y: 0; width: parent.width; height: Style.hairlineWidth; color: Style.hairline }
        ModelPager {
            anchors.centerIn: parent
            page: contacts.page
            pageCount: contacts.pageCount
            onPrevious: contacts.previousPage()
            onNext: contacts.nextPage()
        }
    }
}
