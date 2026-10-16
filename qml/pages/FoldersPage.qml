import QtQuick
import QtQuick.Controls
import ".."

// The folders of the account (FR-005, FR-006): name, number of unread messages, and a switch for whether the folder is
// synchronised. A tap on the name opens the folder. Paged like every list, nothing scrolls.
Item {
    id: page
    signal folderChosen(var id)

    property int pageIndex: 0
    readonly property int rowHeight: 110
    readonly property int rowsPerPage: Math.max(1, Math.floor((height - topBar.height - status.height - bottomBar.height) / rowHeight))
    readonly property int pageCount: Math.max(1, Math.ceil(folders.rows.length / rowsPerPage))
    readonly property var pageRows: folders.rows.slice(pageIndex * rowsPerPage, (pageIndex + 1) * rowsPerPage)
    onPageCountChanged: if (pageIndex >= pageCount) pageIndex = pageCount - 1

    function displayName(row) {
        switch (row.role) {
        case "inbox": return qsTr("Inbox")
        case "drafts": return qsTr("Drafts")
        case "sent": return qsTr("Sent")
        case "archive": return qsTr("Archive")
        case "trash": return qsTr("Trash")
        case "junk": return qsTr("Spam")
        case "all": return qsTr("All mail")
        default: return row.leafName
        }
    }

    function openFolder(id) {
        page.folderChosen(id)
        messages.open(id)
        if (page.StackView.view) page.StackView.view.pop()
    }

    Component.onCompleted: { folders.refresh(); outbox.refresh(); draftList.refresh(); conflicts.refresh() }

    TopBar {
        id: topBar
        title: qsTr("Folders")
        leftItems: [ IconButton { objectName: "foldersBack"; text: qsTr("Back"); icon: "chevron-left"; onClicked: if (page.StackView.view) page.StackView.view.pop() } ]
    }
    SyncStatusLine { id: status; anchors { top: topBar.bottom; left: parent.left; right: parent.right } }

    Column {
        id: list
        objectName: "folderRows"
        anchors { top: status.bottom; left: parent.left; right: parent.right }
        visible: !folders.empty

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
                    objectName: "folderName" + row.index
                    x: 30
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width - 30 - toggle.width - 40 - unread.width - 40
                    elide: Text.ElideRight
                    font.pixelSize: Style.bodySize
                    font.bold: row.modelData.unread > 0
                    color: Style.ink
                    text: page.displayName(row.modelData)
                }
                MouseArea { id: tap; objectName: "folderTap" + row.index; anchors.left: parent.left; anchors.top: parent.top; anchors.bottom: parent.bottom
                            width: parent.width - toggle.width - 60; onClicked: page.openFolder(row.modelData.id) }
                Text {
                    id: unread
                    objectName: "folderUnread" + row.index
                    anchors { right: toggle.left; rightMargin: 30; verticalCenter: parent.verticalCenter }
                    width: Math.max(implicitWidth, 0)
                    font.pixelSize: Style.bodySize
                    color: Style.ink
                    text: row.modelData.unread > 0 ? String(row.modelData.unread) : ""
                }
                InkSwitch {
                    id: toggle
                    objectName: "folderSync" + row.index
                    anchors { right: parent.right; rightMargin: 30; verticalCenter: parent.verticalCenter }
                    checked: row.modelData.syncEnabled
                    onToggled: function (on) { folders.setSyncEnabled(row.modelData.id, on) }
                }
                RowDivider { anchors.bottom: parent.bottom; x: 30; width: parent.width - 60 }
            }
        }
    }

    Text {
        objectName: "foldersEmpty"
        visible: folders.empty
        anchors { top: status.bottom; topMargin: 80; left: parent.left; right: parent.right; leftMargin: 30; rightMargin: 30 }
        wrapMode: Text.Wrap
        horizontalAlignment: Text.AlignHCenter
        font.pixelSize: Style.bodySize
        color: Style.ink
        text: qsTr("There are no folders yet. Sync once to load them.")
    }

    Item {
        id: bottomBar
        anchors { bottom: parent.bottom; left: parent.left; right: parent.right }
        height: Style.bottomBarHeight
        Rectangle { y: 0; width: parent.width; height: Style.hairlineWidth; color: Style.hairline }
        Row {
            anchors { left: parent.left; leftMargin: 20; verticalCenter: parent.verticalCenter }
            spacing: 6
            IconButton { objectName: "outboxButton"; text: qsTr("Outbox"); icon: "send"; onClicked: { outbox.refresh(); if (page.StackView.view) page.StackView.view.push(Qt.resolvedUrl("OutboxPage.qml")) } }
            Text {
                objectName: "outboxCount"
                height: Style.touchMin
                verticalAlignment: Text.AlignVCenter
                font.pixelSize: Style.captionSize
                font.bold: outbox.attentionCount > 0
                color: Style.ink
                text: outbox.total > 0 ? qsTr("Outbox %1").arg(outbox.total) : qsTr("Outbox")
            }
            IconButton { objectName: "draftsButton"; text: qsTr("Drafts"); icon: "pencil"; onClicked: { draftList.refresh(); if (page.StackView.view) page.StackView.view.push(Qt.resolvedUrl("DraftsPage.qml")) } }
            Text {
                height: Style.touchMin
                verticalAlignment: Text.AlignVCenter
                font.pixelSize: Style.captionSize
                color: Style.ink
                text: draftList.total > 0 ? qsTr("Drafts %1").arg(draftList.total) : qsTr("Drafts")
            }
            IconButton { objectName: "decideButton"; visible: !conflicts.empty; text: qsTr("To decide"); icon: "info"; onClicked: { conflicts.refresh(); if (page.StackView.view) page.StackView.view.push(Qt.resolvedUrl("ConflictsPage.qml")) } }
            Text {
                objectName: "decideCount"
                visible: !conflicts.empty
                height: Style.touchMin
                verticalAlignment: Text.AlignVCenter
                font.pixelSize: Style.captionSize
                font.bold: true
                color: Style.ink
                text: qsTr("To decide %1").arg(conflicts.total)
            }
        }
        ModelPager {
            anchors.centerIn: parent
            page: page.pageIndex
            pageCount: page.pageCount
            onPrevious: page.pageIndex = Math.max(0, page.pageIndex - 1)
            onNext: page.pageIndex = Math.min(page.pageCount - 1, page.pageIndex + 1)
        }
    }
}
