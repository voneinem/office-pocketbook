import QtQuick
import QtQuick.Controls
import ".."

// Search over sender, recipients, subject and the downloaded text of every folder. It works offline: it only reads what is on the reader. Words match as
// beginnings ("quart" finds "Quarterly"), every word must be found. The result list is paged like every list.
Item {
    id: page

    readonly property int fieldBlock: 130
    readonly property int rowsPerPage: Math.max(1, Math.floor((height - topBar.height - fieldBlock - bottomBar.height) / Style.rowHeight))

    onRowsPerPageChanged: searchResults.pageSize = rowsPerPage
    Component.onCompleted: {
        searchResults.pageSize = rowsPerPage
        input.forceFocus()
    }
    Component.onDestruction: searchResults.search("")
    function runSearch() {
        Qt.inputMethod.hide()
        searchResults.search(input.text)
    }

    TopBar {
        id: topBar
        title: qsTr("Search")
        leftItems: [ IconButton { objectName: "searchBack"; text: qsTr("Back"); icon: "chevron-left"; onClicked: { Qt.inputMethod.hide(); if (page.StackView.view) page.StackView.view.pop() } } ]
        rightItems: [ IconButton { objectName: "searchGo"; text: qsTr("Search"); icon: "search"; onClicked: page.runSearch() } ]
    }

    InkField {
        id: input
        objectName: "searchField"
        anchors { top: topBar.bottom; left: parent.left; right: parent.right; leftMargin: 30; rightMargin: 30; topMargin: 16 }
        placeholderText: qsTr("Sender, subject, words in the text")
        onAccepted: page.runSearch()
    }

    Column {
        id: rows
        objectName: "searchRows"
        anchors { top: topBar.bottom; topMargin: page.fieldBlock; left: parent.left; right: parent.right }
        visible: searchResults.searching && !searchResults.empty

        Repeater {
            model: searchResults.rows
            delegate: Item {
                id: row
                required property int index
                required property var modelData
                width: rows.width
                height: Style.rowHeight

                Text {
                    objectName: "searchSender" + row.index
                    x: 30
                    y: 16
                    width: parent.width - 30 - dateText.width - 50
                    elide: Text.ElideRight
                    font.pixelSize: Style.bodySize
                    font.bold: row.modelData.unread
                    color: Style.ink
                    text: row.modelData.sender
                }
                Text {
                    id: dateText
                    anchors { right: parent.right; rightMargin: 30 }
                    y: 18
                    font.pixelSize: Style.captionSize
                    color: Style.ink
                    text: row.modelData.dateText
                }
                Text {
                    objectName: "searchSubject" + row.index
                    x: 30
                    y: 16 + Style.bodySize + 14
                    width: parent.width - 60
                    elide: Text.ElideRight
                    font.pixelSize: Style.labelSize
                    font.bold: row.modelData.unread
                    color: Style.ink
                    text: row.modelData.subject
                }
                RowDivider { anchors.bottom: parent.bottom; x: 30; width: parent.width - 60 }
                Rectangle { anchors.fill: parent; color: Style.pressed; visible: tap.pressed }
                MouseArea {
                    id: tap
                    objectName: "searchTap" + row.index
                    anchors.fill: parent
                    onClicked: { Qt.inputMethod.hide(); if (page.StackView.view) page.StackView.view.push(Qt.resolvedUrl("ReaderPage.qml"), { messageId: row.modelData.id }) }
                }
            }
        }
    }
    Text {
        objectName: "searchNote"
        visible: !rows.visible
        anchors { top: topBar.bottom; topMargin: page.fieldBlock + 60; left: parent.left; right: parent.right; leftMargin: 30; rightMargin: 30 }
        wrapMode: Text.Wrap
        horizontalAlignment: Text.AlignHCenter
        font.pixelSize: Style.bodySize
        color: Style.ink
        text: searchResults.searching ? qsTr("Nothing found in the mail on this reader.") : qsTr("Type a word and tap the magnifier. Only mail that is on this reader is searched.")
    }

    Item {
        id: bottomBar
        anchors { bottom: parent.bottom; left: parent.left; right: parent.right }
        height: Style.bottomBarHeight
        Rectangle { y: 0; width: parent.width; height: Style.hairlineWidth; color: Style.hairline }
        ModelPager {
            anchors.centerIn: parent
            page: searchResults.page
            pageCount: searchResults.pageCount
            onPrevious: searchResults.previousPage()
            onNext: searchResults.nextPage()
        }
    }
}
