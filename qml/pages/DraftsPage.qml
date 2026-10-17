import QtQuick
import QtQuick.Controls
import ".."

// The drafts on this reader (also kept in the Drafts folder of the account once there is a connection). A tap continues a draft; a long list is paged.
Item {
    id: page

    property int pageIndex: 0
    readonly property int rowHeight: 132
    readonly property int rowsPerPage: Math.max(1, Math.floor((height - topBar.height - status.height - bottomBar.height) / rowHeight))
    readonly property int pageCount: Math.max(1, Math.ceil(draftList.rows.length / rowsPerPage))
    readonly property var pageRows: draftList.rows.slice(Math.min(pageIndex, pageCount - 1) * rowsPerPage, (Math.min(pageIndex, pageCount - 1) + 1) * rowsPerPage)
    property var chosen: null

    Component.onCompleted: draftList.refresh()

    TopBar {
        id: topBar
        title: qsTr("Drafts")
        leftItems: [ IconButton { objectName: "draftsBack"; text: qsTr("Back"); icon: "chevron-left"; onClicked: if (page.StackView.view) page.StackView.view.pop() } ]
        rightItems: [ IconButton { objectName: "draftsNew"; text: qsTr("New message"); icon: "pencil"; onClicked: { compose.start("new", 0); if (page.StackView.view) page.StackView.view.push(Qt.resolvedUrl("ComposePage.qml"), { prepared: true }) } } ]
    }
    SyncStatusLine { id: status; anchors { top: topBar.bottom; left: parent.left; right: parent.right } }

    Column {
        id: list
        objectName: "draftRows"
        anchors { top: status.bottom; left: parent.left; right: parent.right }
        visible: !draftList.empty
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
                    objectName: "draftSubject" + row.index
                    x: 30
                    y: 16
                    width: parent.width - 60 - dateText.width - 20
                    elide: Text.ElideRight
                    font.pixelSize: Style.bodySize
                    color: Style.ink
                    textFormat: Text.PlainText
                    text: row.modelData.subject.length > 0 ? row.modelData.subject : qsTr("(no subject)")
                }
                Text {
                    id: dateText
                    anchors { right: parent.right; rightMargin: 30 }
                    y: 20
                    font.pixelSize: Style.captionSize
                    color: Style.ink
                    text: row.modelData.updatedText
                }
                Text {
                    x: 30
                    y: 16 + Style.bodySize + 12
                    width: parent.width - 60
                    elide: Text.ElideRight
                    font.pixelSize: Style.labelSize
                    color: Style.ink
                    textFormat: Text.PlainText
                    text: row.modelData.to.length > 0 ? qsTr("To: %1").arg(row.modelData.to) : qsTr("No recipient yet")
                }
                RowDivider { anchors.bottom: parent.bottom; x: 30; width: parent.width - 60 }
                MouseArea {
                    id: tap
                    objectName: "draftTap" + row.index
                    anchors.fill: parent
                    onClicked: { if (page.StackView.view) page.StackView.view.push(Qt.resolvedUrl("ComposePage.qml"), { draftId: row.modelData.id }) }
                    onPressAndHold: { page.chosen = row.modelData; deleteDialog.open() }
                }
            }
        }
    }
    Text {
        objectName: "draftsEmpty"
        visible: draftList.empty
        anchors { top: status.bottom; topMargin: 80; left: parent.left; right: parent.right; leftMargin: 30; rightMargin: 30 }
        wrapMode: Text.Wrap
        horizontalAlignment: Text.AlignHCenter
        font.pixelSize: Style.bodySize
        color: Style.ink
        text: qsTr("No drafts.")
    }

    Item {
        id: bottomBar
        anchors { bottom: parent.bottom; left: parent.left; right: parent.right }
        height: Style.bottomBarHeight
        Rectangle { y: 0; width: parent.width; height: Style.hairlineWidth; color: Style.hairline }
        Text {
            anchors { left: parent.left; leftMargin: 30; verticalCenter: parent.verticalCenter }
            font.pixelSize: Style.captionSize
            color: Style.ink
            text: qsTr("Press and hold a draft to delete it.")
        }
        ModelPager {
            anchors { right: parent.right; rightMargin: 30; verticalCenter: parent.verticalCenter }
            visible: page.pageCount > 1
            page: Math.min(page.pageIndex, page.pageCount - 1)
            pageCount: page.pageCount
            onPrevious: page.pageIndex = Math.max(0, page.pageIndex - 1)
            onNext: page.pageIndex = Math.min(page.pageCount - 1, page.pageIndex + 1)
        }
    }

    ConfirmDialog {
        id: deleteDialog
        objectName: "deleteDraftDialog"
        title: qsTr("Delete this draft?")
        text: qsTr("The draft is deleted from this reader and from your Drafts folder.")
        acceptText: qsTr("Delete")
        cancelText: qsTr("Keep")
        onAccepted: if (page.chosen) draftList.remove(page.chosen.id)
    }
}
