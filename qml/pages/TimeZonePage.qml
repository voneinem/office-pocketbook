import QtQuick
import QtQuick.Controls
import ".."

// The user's time zone (FR-023): first a group (Europe, America, ...), then the place. The choice is kept; the calendar is shown for it.
// Opens by itself while no zone is known. Paged, never scrolled.
Item {
    id: page

    property bool inGroup: false
    property int groupPage: 0
    readonly property int rowHeight: 110
    readonly property int rowsPerPage: Math.max(1, Math.floor((height - topBar.height - intro.height - bottomBar.height) / rowHeight))
    readonly property var allGroups: timezone.groups
    readonly property int groupPages: Math.max(1, Math.ceil(allGroups.length / rowsPerPage))
    readonly property int shownGroupPage: Math.max(0, Math.min(groupPage, groupPages - 1))
    readonly property var groupRows: allGroups.slice(shownGroupPage * rowsPerPage, (shownGroupPage + 1) * rowsPerPage)
    property string message: ""

    onRowsPerPageChanged: timezone.pageSize = rowsPerPage
    Component.onCompleted: timezone.pageSize = rowsPerPage

    Connections {
        target: timezone
        function onZoneSet(ok, key) {
            if (ok) {
                page.message = ""
                if (page.StackView.view) page.StackView.view.pop()
            } else {
                page.message = qsTr("This time zone is not known.")
            }
        }
    }

    function back() {
        if (page.inGroup) page.inGroup = false
        else if (page.StackView.view) page.StackView.view.pop()
    }

    TopBar {
        id: topBar
        title: qsTr("Time zone")
        leftItems: [ IconButton { objectName: "zoneBack"; text: qsTr("Back"); icon: "chevron-left"; onClicked: page.back() } ]
    }

    Text {
        id: intro
        objectName: "zoneIntro"
        anchors { top: topBar.bottom; left: parent.left; right: parent.right; leftMargin: 30; rightMargin: 30 }
        height: implicitHeight + 40
        topPadding: 20
        wrapMode: Text.Wrap
        font.pixelSize: Style.labelSize
        color: Style.ink
        text: page.message.length > 0 ? page.message
              : (timezone.needsZoneChoice ? qsTr("Choose where you are. Times in your calendar are shown for this place.")
                                          : qsTr("Now: %1. Choose another place to change it.").arg(timezone.zoneName))
    }

    Column {
        id: list
        objectName: "zoneRows"
        anchors { top: intro.bottom; left: parent.left; right: parent.right }

        // the groups
        Repeater {
            model: page.inGroup ? [] : page.groupRows
            delegate: InkRow {
                required property int index
                required property string modelData
                objectName: "zoneGroup" + index
                text: modelData
                chevron: true
                onClicked: { timezone.setGroup(modelData); page.inGroup = true }
            }
        }
        // the places of the chosen group
        Repeater {
            model: page.inGroup ? timezone.rows : []
            delegate: InkRow {
                required property int index
                required property var modelData
                objectName: "zonePlace" + index
                text: modelData.label
                onClicked: timezone.setTimeZone(modelData.name)
            }
        }
    }

    Item {
        id: bottomBar
        anchors { bottom: parent.bottom; left: parent.left; right: parent.right }
        height: Style.bottomBarHeight
        Rectangle { y: 0; width: parent.width; height: Style.hairlineWidth; color: Style.hairline }
        ModelPager {
            anchors.centerIn: parent
            page: page.inGroup ? timezone.page : page.shownGroupPage
            pageCount: page.inGroup ? timezone.pageCount : page.groupPages
            onPrevious: if (page.inGroup) timezone.previousPage(); else page.groupPage = Math.max(0, page.shownGroupPage - 1)
            onNext: if (page.inGroup) timezone.nextPage(); else page.groupPage = Math.min(page.groupPages - 1, page.shownGroupPage + 1)
        }
    }
}
