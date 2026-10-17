import QtQuick

// "Previous page" / "next page" with "n / m" between them, for lists that the model pages itself (the message list).
// E-ink: the list jumps by a whole page, nothing scrolls or animates. `page` is 0-based.
Row {
    id: root
    property int page: 0
    property int pageCount: 1
    signal previous()
    signal next()
    spacing: 24

    IconButton { objectName: "pagePrevious"; text: qsTr("Previous page"); icon: "chevron-left"; enabled: root.page > 0; onClicked: root.previous() }
    Text {
        objectName: "pageIndicator"
        width: 150
        height: Style.touchMin
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        font.pixelSize: Style.bodySize
        color: Style.ink
        text: (root.page + 1) + " / " + root.pageCount
    }
    IconButton { objectName: "pageNext"; text: qsTr("Next page"); icon: "chevron-right"; enabled: root.page + 1 < root.pageCount; onClicked: root.next() }
}
