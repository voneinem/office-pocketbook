import QtQuick
import QtQuick.Controls
import ".."

// The agenda: the next events in time order under a header per day. Paged, never scrolled. Without a known time zone the zone picker opens first
// (no time is shown with an assumed zone, FR-023).
Item {
    id: page

    readonly property int rowsPerPage: Math.max(2, Math.floor((height - topBar.height - status.height - bottomBar.height) / Style.rowHeight))
    // "" = rows are shown; otherwise why not
    readonly property string emptyKind: agenda.needsZone ? "zone" : (!agenda.empty ? "" : (account.calendarConfigured ? "empty" : "no_account"))
    property bool connecting: false
    property bool noNetwork: false

    function push(name, props) { if (page.StackView.view) page.StackView.view.push(Qt.resolvedUrl(name), props || {}) }
    function chooseZone() { page.push("TimeZonePage.qml") }
    function startSync() {
        page.noNetwork = false
        if (device.wifiConnected()) {
            sync.syncNow()
        } else {
            page.connecting = true
            device.connectNetwork()
        }
    }
    function openEvent(row) { page.push("EventPage.qml", { eventId: row.eventId, startUtc: row.startUtc, endUtc: row.endUtc, allDay: row.allDay }) }

    onRowsPerPageChanged: agenda.pageSize = rowsPerPage
    Component.onCompleted: {
        agenda.pageSize = rowsPerPage
        agenda.refresh()
        conflicts.refresh()
        if (agenda.needsZone) page.chooseZone()
    }

    Connections {
        target: device
        function onNetworkResult(connected) {
            if (!page.connecting) return
            page.connecting = false
            if (!connected) page.noNetwork = true
            else sync.syncNow()
        }
    }
    Connections {
        target: sync
        function onSyncFinished(outcome) {
            agenda.refresh()
            conflicts.refresh()
            if (sync.needsZone && page.StackView.view && page.StackView.status === StackView.Active) page.chooseZone()
        }
    }
    Connections { target: timezone; function onZoneChanged() { agenda.refresh() } }

    TopBar {
        id: topBar
        title: qsTr("Agenda")
        leftItems: [ IconButton { objectName: "agendaBack"; text: qsTr("Back"); icon: "chevron-left"; onClicked: if (page.StackView.view) page.StackView.view.pop() } ]
        rightItems: [
            IconButton { objectName: "newEventButton"; text: qsTr("New event"); icon: "plus-circle"; onClicked: page.push("EventEditPage.qml", { date: "" }) },
            IconButton { objectName: "conflictsButton"; text: qsTr("Events to decide"); icon: "info"; visible: !conflicts.empty; onClicked: page.push("ConflictsPage.qml") },
            IconButton { objectName: "dayButton"; text: qsTr("Day"); icon: "list"; onClicked: { dayModel.goToday(); page.push("DayPage.qml") } },
            IconButton { objectName: "monthButton"; text: qsTr("Month"); icon: "calendar"; onClicked: { monthModel.goToday(); page.push("MonthPage.qml") } },
            IconButton { objectName: "calendarAccountButton"; text: qsTr("Calendar account"); icon: "gear"; onClicked: page.push("CalendarAccountPage.qml") },
            IconButton { objectName: "agendaSyncButton"; text: qsTr("Sync"); icon: "sync"; enabled: !sync.syncing && !page.connecting; onClicked: page.startSync() }
        ]
    }

    SyncStatusLine {
        id: status
        anchors { top: topBar.bottom; left: parent.left; right: parent.right }
        connecting: page.connecting
        noNetwork: page.noNetwork
    }

    Column {
        id: rows
        objectName: "agendaRows"
        anchors { top: status.bottom; left: parent.left; right: parent.right }
        visible: page.emptyKind === ""

        Repeater {
            model: agenda.rows
            delegate: Item {
                id: row
                required property int index
                required property var modelData
                width: rows.width
                height: Style.rowHeight

                // a day header
                Item {
                    anchors.fill: parent
                    visible: row.modelData.kind === "header"
                    Text {
                        objectName: "agendaHeader" + row.index
                        x: 30
                        anchors.verticalCenter: parent.verticalCenter
                        width: parent.width - 60
                        elide: Text.ElideRight
                        font.pixelSize: Style.rowTitleSize
                        font.bold: true
                        color: Style.ink
                        text: row.modelData.dateText || ""
                    }
                    Rectangle { x: 30; y: parent.height - 3; width: parent.width - 60; height: Style.ruleWidth; color: Style.ink }
                }
                EventRow {
                    anchors.fill: parent
                    visible: row.modelData.kind === "event"
                    tag: "agendaEvent" + row.index
                    timeText: row.modelData.timeText || ""
                    title: row.modelData.title || ""
                    calendarName: row.modelData.calendarName || ""
                    readOnly: row.modelData.readOnly === true
                    pending: row.modelData.pending === true
                    conflict: row.modelData.conflict === true
                    onClicked: page.openEvent(row.modelData)
                }
            }
        }
    }

    Column {
        objectName: "agendaEmpty"
        visible: page.emptyKind !== ""
        anchors { top: status.bottom; topMargin: 80; left: parent.left; right: parent.right }
        spacing: 20
        Text {
            x: 30
            width: parent.width - 60
            wrapMode: Text.Wrap
            horizontalAlignment: Text.AlignHCenter
            font.pixelSize: Style.rowTitleSize
            font.bold: true
            color: Style.ink
            text: page.emptyKind === "zone" ? qsTr("Which time zone are you in?") : (page.emptyKind === "no_account" ? qsTr("No calendar yet") : qsTr("No upcoming events"))
        }
        Text {
            x: 30
            width: parent.width - 60
            wrapMode: Text.Wrap
            horizontalAlignment: Text.AlignHCenter
            font.pixelSize: Style.bodySize
            color: Style.ink
            text: page.emptyKind === "zone" ? qsTr("Times are shown for the place you choose. Tap the button below.")
                : (page.emptyKind === "no_account" ? qsTr("Tap the gear to set up your calendar.") : qsTr("Nothing is planned from today on. Tap the sync button to look for news."))
        }
        IconButton {
            objectName: "chooseZoneButton"
            visible: page.emptyKind === "zone"
            anchors.horizontalCenter: parent.horizontalCenter
            text: qsTr("Choose time zone")
            icon: "list"
            onClicked: page.chooseZone()
        }
    }

    Item {
        id: bottomBar
        anchors { bottom: parent.bottom; left: parent.left; right: parent.right }
        height: Style.bottomBarHeight
        Rectangle { y: 0; width: parent.width; height: Style.hairlineWidth; color: Style.hairline }
        ModelPager {
            anchors.centerIn: parent
            page: agenda.page
            pageCount: agenda.pageCount
            onPrevious: agenda.previousPage()
            onNext: agenda.nextPage()
        }
    }
}
