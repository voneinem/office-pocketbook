import QtQuick
import QtQuick.Controls
import ".."

// Shown instead of the mail pages when the database cannot be opened. It says what happened and that nothing was
// changed or deleted; the About page stays reachable.
Item {
    id: page

    function message(key) {
        switch (key) {
        case "schema_too_new": return qsTr("The data on this reader was written by a newer version of PB Office. Please install the newer version. Nothing was changed.")
        case "integrity_failed": return qsTr("The database failed its check. Nothing was changed or deleted. Please keep the folder system/pboffice and report this.")
        case "migration_failed": return qsTr("The data could not be upgraded to this version. Your data is unchanged and a backup copy was kept.")
        default: return qsTr("The data folder could not be opened (%1). Nothing was changed.").arg(key)
        }
    }

    TopBar { id: topBar; title: qsTr("PB Office") }

    Column {
        anchors { top: topBar.bottom; topMargin: 80; left: parent.left; right: parent.right }
        spacing: 30
        Text {
            objectName: "storageErrorText"
            x: 30
            width: parent.width - 60
            wrapMode: Text.Wrap
            font.pixelSize: Style.bodySize
            color: Style.ink
            text: page.message(startupError)
        }
        InkRow {
            text: qsTr("About")
            chevron: true
            onClicked: page.StackView.view.push(Qt.resolvedUrl("AboutPage.qml"))
        }
    }
}
