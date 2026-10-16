import QtQuick
import QtQuick.Controls
import ".."

// The list of message headers of the inbox (walking skeleton): sender, subject and date, unread rows in bold with a
// mark. Paged, never scrolled. Explicit empty states: no account, or no messages (yet).
Item {
    id: page

    // "" = rows are shown; "no_account" or "empty" otherwise
    readonly property string emptyKind: !messages.empty ? "" : (account.mailConfigured ? "empty" : "no_account")
    readonly property int rowsPerPage: Math.max(1, Math.floor((height - topBar.height - status.height - bottomBar.height) / Style.rowHeight))

    // Sync switches Wi-Fi on first (the system may show its own connect dialog) and starts only when the reader is online.
    property bool autoSync: false          // set by the account page after Save
    property bool connecting: false
    property bool noNetwork: false
    function startSync() {
        page.noNetwork = false
        if (device.wifiConnected()) {
            sync.syncNow()
        } else {
            page.connecting = true
            device.connectNetwork()
        }
    }

    // under the start page (or any page) the list has a Back button; as the first page of the app it has Home, which leaves the app
    readonly property bool hasPageBelow: page.StackView.view !== null && page.StackView.view.depth > 1

    // the title: the role's name for the well-known folders, else the last part of the folder's name
    readonly property string folderTitle: {
        switch (messages.folderRole) {
        case "inbox": return qsTr("Inbox")
        case "drafts": return qsTr("Drafts")
        case "sent": return qsTr("Sent")
        case "archive": return qsTr("Archive")
        case "trash": return qsTr("Trash")
        case "junk": return qsTr("Spam")
        case "all": return qsTr("All mail")
        default: return messages.folderName.length > 0 ? messages.folderName.split("/").pop() : qsTr("Inbox")
        }
    }

    // Older mail on demand ("Load older messages", offered while the server has more): Wi-Fi first, like a sync.
    property string olderNote: ""
    property bool olderPending: false
    function loadOlder() {
        page.olderNote = ""
        page.noNetwork = false
        if (device.wifiConnected()) {
            sync.fetchOlder(messages.folderId)
        } else {
            page.olderPending = true
            page.connecting = true
            device.connectNetwork()
        }
    }

    signal messageOpened(var id)
    function openMessage(id) {
        page.messageOpened(id)
        if (page.StackView.view) page.StackView.view.push(Qt.resolvedUrl("ReaderPage.qml"), { messageId: id })
    }

    onRowsPerPageChanged: messages.pageSize = rowsPerPage
    Component.onCompleted: {
        messages.pageSize = rowsPerPage
        messages.openInbox()
        if (page.autoSync) page.startSync()
    }

    Connections {
        target: device
        function onNetworkResult(connected) {
            if (!page.connecting) return
            page.connecting = false
            const older = page.olderPending
            page.olderPending = false
            if (!connected) page.noNetwork = true
            else if (older) sync.fetchOlder(messages.folderId)
            else sync.syncNow()
        }
    }

    Connections {
        target: sync
        function onSyncFinished(outcome) { messages.refresh() }
        function onOlderFinished(key, added) {
            messages.refresh()
            page.olderNote = key === "ok" ? (added > 0 ? qsTr("%n older message(s) loaded.", "", added) : qsTr("There are no older messages."))
                                         : qsTr("Older messages could not be loaded: %1.").arg(status.causeText(key))
        }
    }

    TopBar {
        id: topBar
        title: page.folderTitle
        leftItems: [
            IconButton { objectName: "listBack"; text: qsTr("Back"); icon: "chevron-left"; visible: page.hasPageBelow; onClicked: if (page.StackView.view) page.StackView.view.pop() },
            IconButton { objectName: "homeButton"; text: qsTr("Home"); icon: "home"; visible: !page.hasPageBelow; onClicked: device.quit() },
            IconButton { objectName: "aboutButton"; text: qsTr("About"); icon: "info"; onClicked: if (page.StackView.view) page.StackView.view.push(Qt.resolvedUrl("AboutPage.qml")) },
            IconButton { objectName: "foldersButton"; text: qsTr("Folders"); icon: "folder"; onClicked: if (page.StackView.view) page.StackView.view.push(Qt.resolvedUrl("FoldersPage.qml")) },
            IconButton { objectName: "calendarButton"; text: qsTr("Calendar"); icon: "calendar"; onClicked: if (page.StackView.view) page.StackView.view.push(Qt.resolvedUrl("AgendaPage.qml")) }
        ]
        rightItems: [
            IconButton { objectName: "newMessageButton"; text: qsTr("New message"); icon: "pencil"; onClicked: { compose.start("new", 0); if (page.StackView.view) page.StackView.view.push(Qt.resolvedUrl("ComposePage.qml"), { prepared: true }) } },
            IconButton { objectName: "settingsButton"; text: qsTr("Settings"); icon: "gear"; onClicked: if (page.StackView.view) page.StackView.view.push(Qt.resolvedUrl("SettingsPage.qml")) },
            IconButton { objectName: "syncButton"; text: qsTr("Sync"); icon: "sync"; enabled: !sync.syncing && !page.connecting; onClicked: page.startSync() }
        ]
    }

    SyncStatusLine {
        id: status
        anchors { top: topBar.bottom; left: parent.left; right: parent.right }
        connecting: page.connecting
        noNetwork: page.noNetwork
        note: page.olderNote
    }

    Column {
        id: rows
        objectName: "rowsColumn"
        property int rowCount: rep.count
        anchors { top: status.bottom; left: parent.left; right: parent.right }
        visible: page.emptyKind === ""

        Repeater {
            id: rep
            model: messages.rows
            delegate: Item {
                id: row
                required property int index
                required property var modelData
                width: rows.width
                height: Style.rowHeight

                Rectangle {
                    objectName: "unreadMark" + row.index
                    visible: row.modelData.unread
                    x: 12
                    anchors.verticalCenter: parent.verticalCenter
                    width: 12
                    height: 12
                    color: Style.ink
                }
                Text {
                    objectName: "rowSender" + row.index
                    x: 40
                    y: 16
                    width: parent.width - 40 - dateText.width - 50 - (row.modelData.hasAttachments ? 46 : 0) - (row.modelData.flagged ? 46 : 0)
                    elide: Text.ElideRight
                    font.pixelSize: Style.bodySize
                    font.bold: row.modelData.unread
                    color: Style.ink
                    text: row.modelData.sender
                }
                Icon {
                    id: rowAttachIcon
                    objectName: "rowAttach" + row.index
                    visible: row.modelData.hasAttachments
                    source: "attach"
                    size: 32
                    anchors { right: dateText.left; rightMargin: 14 }
                    y: 16
                }
                Icon {
                    objectName: "rowStar" + row.index
                    visible: row.modelData.flagged
                    source: "star-filled"
                    size: 32
                    anchors { right: row.modelData.hasAttachments ? rowAttachIcon.left : dateText.left; rightMargin: 14 }
                    y: 16
                }
                Text {
                    id: dateText
                    objectName: "rowDate" + row.index
                    anchors { right: parent.right; rightMargin: 30 }
                    y: 18
                    font.pixelSize: Style.captionSize
                    color: Style.ink
                    text: row.modelData.dateText
                }
                Text {
                    id: pendingText
                    objectName: "rowPending" + row.index
                    visible: row.modelData.errorKey.length > 0 || row.modelData.pendingMark
                    anchors { right: parent.right; rightMargin: 30 }
                    y: 16 + Style.bodySize + 18
                    font.pixelSize: Style.captionSize
                    font.italic: true
                    color: Style.ink
                    text: row.modelData.errorKey.length > 0 ? qsTr("could not apply") : qsTr("not synced")
                }
                Text {
                    objectName: "rowSubject" + row.index
                    x: 40
                    y: 16 + Style.bodySize + 14
                    width: parent.width - 40 - 30 - (pendingText.visible ? pendingText.width + 20 : 0)
                    elide: Text.ElideRight
                    font.pixelSize: Style.labelSize
                    font.bold: row.modelData.unread
                    color: Style.ink
                    text: row.modelData.subject
                }
                RowDivider { anchors.bottom: parent.bottom; x: 30; width: parent.width - 60 }
                Rectangle { anchors.fill: parent; color: Style.pressed; visible: tap.pressed }
                MouseArea { id: tap; objectName: "rowTap" + row.index; anchors.fill: parent; onClicked: page.openMessage(row.modelData.id) }
            }
        }
    }

    Column {
        objectName: "emptyState"
        visible: page.emptyKind !== ""
        anchors { top: status.bottom; topMargin: 80; left: parent.left; right: parent.right }
        spacing: 20
        Text {
            x: 30
            width: parent.width - 60
            wrapMode: Text.Wrap
            horizontalAlignment: Text.AlignHCenter
            font.pixelSize: Style.rowTitleSize
            font.bold: true
            color: Style.ink
            text: page.emptyKind === "no_account" ? qsTr("No mail account yet") : qsTr("No messages")
        }
        Text {
            x: 30
            width: parent.width - 60
            wrapMode: Text.Wrap
            horizontalAlignment: Text.AlignHCenter
            font.pixelSize: Style.bodySize
            color: Style.ink
            text: page.emptyKind === "no_account" ? qsTr("Tap the gear to set up your account.")
                                                  : qsTr("Nothing was found in the last days. Tap the sync button.")
        }
    }

    Item {
        id: bottomBar
        anchors { bottom: parent.bottom; left: parent.left; right: parent.right }
        height: Style.bottomBarHeight
        Rectangle { y: 0; width: parent.width; height: Style.hairlineWidth; color: Style.hairline }
        IconButton {
            objectName: "olderButton"
            visible: messages.olderOnServer
            enabled: !sync.syncing && !page.connecting
            anchors { left: parent.left; leftMargin: 30; verticalCenter: parent.verticalCenter }
            text: qsTr("Load older messages")
            icon: "download"
            onClicked: page.loadOlder()
        }
        Text {
            objectName: "olderCaption"
            visible: messages.olderOnServer
            anchors { left: parent.left; leftMargin: 30 + Style.touchMin + 8; verticalCenter: parent.verticalCenter }
            font.pixelSize: Style.captionSize
            color: Style.ink
            text: qsTr("Older mail")
        }
        Row {
            anchors { right: parent.right; rightMargin: 20; verticalCenter: parent.verticalCenter }
            spacing: 6
            IconButton { id: sortButton; objectName: "sortButton"; text: qsTr("Sort"); icon: "filter"
                         onClicked: { sortChoice.choices = [ { label: qsTr("Newest first"), value: "date" }, { label: qsTr("Oldest first"), value: "dateAsc" }, { label: qsTr("By sender"), value: "sender" }, { label: qsTr("By subject"), value: "subject" } ]
                                      sortChoice.current = messages.sort; sortChoice.anchor = sortButton; sortChoice.open() } }
            IconButton { objectName: "searchButton"; text: qsTr("Search"); icon: "search"; onClicked: { searchResults.search(""); if (page.StackView.view) page.StackView.view.push(Qt.resolvedUrl("SearchPage.qml")) } }
        }
        ModelPager {
            anchors.centerIn: parent
            page: messages.page
            pageCount: messages.pageCount
            onPrevious: messages.previousPage()
            onNext: messages.nextPage()
        }
    }

    ChoiceDialog {
        id: sortChoice
        objectName: "sortChoice"
        title: qsTr("Sort the list")
        onChosen: function (value) { messages.sort = value }
    }
}
