import QtQuick
import "."

// One contact in the list: the name in bold, below it the organization (or the first email address or number) and the address book when more than
// one is shown, with marks for a change that waits for the server, a failed one, two versions, or a contact the server removed. A tap emits `clicked`.
// `tag` makes the object names unique per row.
Item {
    id: root
    property string tag: "contact"
    property string name
    property string subtitle
    property string bookName
    property string mark: ""          // "" | "notSynced" | "failed" | "conflict" | "deletedOnServer"
    property bool unreadable: false
    signal clicked()

    objectName: root.tag
    height: Style.rowHeight
    width: parent ? parent.width : 0

    Rectangle { anchors.fill: parent; color: Style.pressed; visible: tap.pressed }
    Text {
        objectName: root.tag + "Name"
        x: 30
        y: 20
        width: parent.width - 60
        elide: Text.ElideRight
        font.pixelSize: Style.bodySize
        font.bold: true
        color: Style.ink
        textFormat: Text.PlainText
        text: root.unreadable ? qsTr("Unreadable contact") : (root.name.length > 0 ? root.name : qsTr("(no name)"))
    }
    Text {
        objectName: root.tag + "Note"
        x: 30
        y: 20 + Style.bodySize + 14
        width: parent.width - 60
        elide: Text.ElideRight
        font.pixelSize: Style.captionSize
        color: Style.ink
        textFormat: Text.PlainText
        text: {
            var parts = [root.subtitle, root.bookName]
            if (root.mark === "conflict") parts.push(qsTr("two versions"))
            else if (root.mark === "deletedOnServer") parts.push(qsTr("deleted on the server"))
            else if (root.mark === "failed") parts.push(qsTr("failed"))
            else if (root.mark === "notSynced") parts.push(qsTr("not synced"))
            return parts.filter(function (p) { return p.length > 0 }).join(" · ")
        }
    }
    RowDivider { anchors.bottom: parent.bottom; x: 30; width: parent.width - 60 }
    MouseArea { id: tap; anchors.fill: parent; onClicked: root.clicked() }
}
