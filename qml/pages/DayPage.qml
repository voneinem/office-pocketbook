import QtQuick
import QtQuick.Controls
import ".."

// One day of events. The buttons at the bottom move by a day; the pager pages the rows when a day has many.
Item {
    id: page

    readonly property int rowsPerPage: Math.max(1, Math.floor((height - topBar.height - dateLine.height - status.height - bottomBar.height) / Style.rowHeight))
    function push(name, props) { if (page.StackView.view) page.StackView.view.push(Qt.resolvedUrl(name), props || {}) }

    onRowsPerPageChanged: dayModel.pageSize = rowsPerPage
    Component.onCompleted: {
        dayModel.pageSize = rowsPerPage
        dayModel.refresh()
        if (dayModel.needsZone) page.push("TimeZonePage.qml")
    }
    Connections { target: timezone; function onZoneChanged() { dayModel.refresh() } }
    Connections { target: sync; function onSyncFinished(outcome) { dayModel.refresh() } }

    TopBar {
        id: topBar
        title: qsTr("Day")
        leftItems: [ IconButton { objectName: "dayBack"; text: qsTr("Back"); icon: "chevron-left"; onClicked: if (page.StackView.view) page.StackView.view.pop() } ]
        rightItems: [
            IconButton { objectName: "dayNewEvent"; text: qsTr("New event"); icon: "plus-circle"; onClicked: page.push("EventEditPage.qml", { date: dayModel.selectedDate }) },
            IconButton { objectName: "dayToMonth"; text: qsTr("Month"); icon: "calendar"; onClicked: { monthModel.selectDate(dayModel.selectedDate); page.push("MonthPage.qml") } }
        ]
    }
    SyncStatusLine { id: status; anchors { top: topBar.bottom; left: parent.left; right: parent.right } }

    Text {
        id: dateLine
        objectName: "dayDate"
        anchors { top: status.bottom; left: parent.left; right: parent.right; leftMargin: 30; rightMargin: 30 }
        height: 96
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
        font.pixelSize: Style.rowTitleSize
        font.bold: true
        color: Style.ink
        text: dayModel.dateText
    }

    Column {
        id: rows
        objectName: "dayRows"
        anchors { top: dateLine.bottom; left: parent.left; right: parent.right }
        visible: !dayModel.empty && !dayModel.needsZone
        Repeater {
            model: dayModel.rows
            delegate: EventRow {
                required property int index
                required property var modelData
                width: rows.width
                tag: "dayEvent" + index
                timeText: modelData.timeText
                title: modelData.title
                calendarName: modelData.calendarName
                readOnly: modelData.readOnly === true
                pending: modelData.pending === true
                conflict: modelData.conflict === true
                onClicked: page.push("EventPage.qml", { eventId: modelData.eventId, startUtc: modelData.startUtc, endUtc: modelData.endUtc, allDay: modelData.allDay })
            }
        }
    }
    Text {
        objectName: "dayEmpty"
        visible: dayModel.empty && !dayModel.needsZone
        anchors { top: dateLine.bottom; topMargin: 60; left: parent.left; right: parent.right; leftMargin: 30; rightMargin: 30 }
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.Wrap
        font.pixelSize: Style.bodySize
        color: Style.ink
        text: qsTr("No events on this day.")
    }

    Item {
        id: bottomBar
        anchors { bottom: parent.bottom; left: parent.left; right: parent.right }
        height: Style.bottomBarHeight
        Rectangle { y: 0; width: parent.width; height: Style.hairlineWidth; color: Style.hairline }
        IconButton { objectName: "previousDay"; anchors { left: parent.left; leftMargin: 30; verticalCenter: parent.verticalCenter } text: qsTr("Previous day"); icon: "chevron-left"; onClicked: dayModel.previousDay() }
        IconButton { objectName: "todayButton"; anchors { left: parent.left; leftMargin: 30 + Style.touchMin + 16; verticalCenter: parent.verticalCenter } text: qsTr("Today"); icon: "home"; enabled: !dayModel.isToday; onClicked: dayModel.goToday() }
        IconButton { objectName: "nextDay"; anchors { left: parent.left; leftMargin: 30 + 2 * (Style.touchMin + 16); verticalCenter: parent.verticalCenter } text: qsTr("Next day"); icon: "chevron-right"; onClicked: dayModel.nextDay() }
        ModelPager {
            anchors { right: parent.right; rightMargin: 30; verticalCenter: parent.verticalCenter }
            visible: dayModel.pageCount > 1
            page: dayModel.page
            pageCount: dayModel.pageCount
            onPrevious: dayModel.previousPage()
            onNext: dayModel.nextPage()
        }
    }
}
