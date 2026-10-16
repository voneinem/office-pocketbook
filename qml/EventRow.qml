import QtQuick
import "."

// One event in the agenda and in the day list: the time at the left, the title and the calendar's name next to it, marks for an event that is
// read-only, waits to be sent, or has a conflict. A tap emits `clicked`. `tag` makes the object names unique per row.
Item {
    id: root
    property string tag: "event"
    property string timeText
    property string title
    property string calendarName
    property bool readOnly: false
    property bool pending: false
    property bool conflict: false
    signal clicked()

    height: Style.rowHeight
    width: parent ? parent.width : 0

    Rectangle { anchors.fill: parent; color: Style.pressed; visible: tap.pressed }
    Text {
        objectName: root.tag + "Time"
        x: 30
        y: 20
        width: 230
        elide: Text.ElideRight
        font.pixelSize: Style.bodySize
        font.bold: true
        color: Style.ink
        text: root.timeText
    }
    Text {
        objectName: root.tag + "Title"
        x: 280
        y: 20
        width: parent.width - 280 - 30
        elide: Text.ElideRight
        font.pixelSize: Style.bodySize
        color: Style.ink
        textFormat: Text.PlainText
        text: root.title.length > 0 ? root.title : qsTr("(no title)")
    }
    Text {
        objectName: root.tag + "Note"
        x: 280
        y: 20 + Style.bodySize + 14
        width: parent.width - 280 - 30
        elide: Text.ElideRight
        font.pixelSize: Style.captionSize
        color: Style.ink
        textFormat: Text.PlainText
        text: {
            var parts = [root.calendarName]
            if (root.conflict) parts.push(qsTr("two versions"))
            else if (root.pending) parts.push(qsTr("not synced"))
            if (root.readOnly) parts.push(qsTr("read only"))
            return parts.filter(function (p) { return p.length > 0 }).join(" · ")
        }
    }
    RowDivider { anchors.bottom: parent.bottom; x: 30; width: parent.width - 60 }
    MouseArea { id: tap; objectName: root.tag + "Tap"; anchors.fill: parent; onClicked: root.clicked() }
}
