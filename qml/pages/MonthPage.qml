import QtQuick
import QtQuick.Controls
import ".."

// A month (or a week) as a grid: days with events are marked, today is underlined, the chosen day is inverted. A tap on a day opens that day.
Item {
    id: page

    readonly property int cellHeight: monthModel.mode === "week" ? 200 : 150
    function push(name, props) { if (page.StackView.view) page.StackView.view.push(Qt.resolvedUrl(name), props || {}) }

    Component.onCompleted: {
        monthModel.refresh()
        if (monthModel.needsZone) page.push("TimeZonePage.qml")
    }
    Connections { target: timezone; function onZoneChanged() { monthModel.refresh() } }
    Connections { target: sync; function onSyncFinished(outcome) { monthModel.refresh() } }

    TopBar {
        id: topBar
        title: monthModel.title
        leftItems: [ IconButton { objectName: "monthBack"; text: qsTr("Back"); icon: "chevron-left"; onClicked: if (page.StackView.view) page.StackView.view.pop() } ]
        rightItems: [
            IconButton { objectName: "modeButton"; text: monthModel.mode === "month" ? qsTr("Week") : qsTr("Month"); icon: monthModel.mode === "month" ? "list" : "calendar"
                         onClicked: monthModel.setMode(monthModel.mode === "month" ? "week" : "month") }
        ]
    }
    SyncStatusLine { id: status; anchors { top: topBar.bottom; left: parent.left; right: parent.right } }

    Row {
        id: names
        objectName: "weekdayNames"
        anchors { top: status.bottom; left: parent.left; right: parent.right; leftMargin: 20; rightMargin: 20 }
        height: 64
        Repeater {
            model: monthModel.weekdayNames
            delegate: Text {
                required property string modelData
                width: names.width / 7
                height: names.height
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
                font.pixelSize: Style.labelSize
                font.bold: true
                color: Style.ink
                text: modelData
            }
        }
    }

    Column {
        id: grid
        objectName: "monthGrid"
        anchors { top: names.bottom; left: parent.left; right: parent.right; leftMargin: 20; rightMargin: 20 }
        visible: !monthModel.needsZone
        Repeater {
            model: monthModel.weeks
            delegate: Row {
                id: week
                required property int index
                required property var modelData
                width: grid.width
                Repeater {
                    model: week.modelData
                    delegate: Item {
                        id: cell
                        required property int index
                        required property var modelData
                        width: grid.width / 7
                        height: page.cellHeight
                        Rectangle { anchors.fill: parent; color: cell.modelData.selected ? Style.ink : Style.paper; border.width: Style.hairlineWidth; border.color: Style.hairline }
                        Rectangle { anchors.fill: parent; color: Style.pressed; visible: cellTap.pressed; opacity: 0.6 }
                        Text {
                            objectName: "cellDay" + week.index + "_" + cell.index
                            anchors { horizontalCenter: parent.horizontalCenter; top: parent.top; topMargin: 20 }
                            font.pixelSize: Style.bodySize
                            font.bold: cell.modelData.today || cell.modelData.marked
                            font.underline: cell.modelData.today
                            color: cell.modelData.selected ? Style.paper : Style.ink
                            opacity: cell.modelData.inMonth ? 1.0 : 0.45
                            text: cell.modelData.day
                        }
                        // the mark: a filled square under the number
                        Rectangle {
                            objectName: "cellMark" + week.index + "_" + cell.index
                            visible: cell.modelData.marked
                            anchors { horizontalCenter: parent.horizontalCenter; bottom: parent.bottom; bottomMargin: 22 }
                            width: 22
                            height: 22
                            color: cell.modelData.selected ? Style.paper : Style.ink
                        }
                        MouseArea {
                            id: cellTap
                            objectName: "cellTap" + week.index + "_" + cell.index
                            anchors.fill: parent
                            onClicked: {
                                // selecting in the month rebuilds this grid and so ends this delegate: it comes last
                                const date = cell.modelData.date
                                dayModel.selectDate(date)
                                page.push("DayPage.qml")
                                monthModel.selectDate(date)
                            }
                        }
                    }
                }
            }
        }
    }

    Item {
        id: bottomBar
        anchors { bottom: parent.bottom; left: parent.left; right: parent.right }
        height: Style.bottomBarHeight
        Rectangle { y: 0; width: parent.width; height: Style.hairlineWidth; color: Style.hairline }
        IconButton { objectName: "previousPeriod"; anchors { left: parent.left; leftMargin: 30; verticalCenter: parent.verticalCenter } text: monthModel.mode === "week" ? qsTr("Previous week") : qsTr("Previous month"); icon: "chevron-left"; onClicked: monthModel.previous() }
        IconButton { objectName: "monthToday"; anchors { horizontalCenter: parent.horizontalCenter; verticalCenter: parent.verticalCenter } text: qsTr("Today"); icon: "home"; onClicked: monthModel.goToday() }
        IconButton { objectName: "nextPeriod"; anchors { right: parent.right; rightMargin: 30; verticalCenter: parent.verticalCenter } text: monthModel.mode === "week" ? qsTr("Next week") : qsTr("Next month"); icon: "chevron-right"; onClicked: monthModel.next() }
    }
}
