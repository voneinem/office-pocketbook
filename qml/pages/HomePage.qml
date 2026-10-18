import QtQuick
import QtQuick.Controls
import ".."

// The start page: the way into mail and calendar at a glance - unread mail, what is on today and tomorrow (up to six, then "+ n more"), and the sync state.
// Everything shown comes from the reader's own storage; nothing is fetched when the page opens.
Item {
    id: page

    property bool autoSync: false     // set after the first account was saved
    property bool connecting: false
    property bool noNetwork: false

    function push(name, props) { if (page.StackView.view) page.StackView.view.push(Qt.resolvedUrl(name), props || {}) }
    function startSync() {
        page.noNetwork = false
        if (device.wifiConnected()) {
            sync.syncNow()
        } else {
            page.connecting = true
            device.connectNetwork()
        }
    }
    function openInbox() { messages.openInbox(); page.push("MessageListPage.qml") }
    function openEvent(row) { page.push("EventPage.qml", { eventId: row.eventId, startUtc: row.startUtc, endUtc: row.endUtc, allDay: row.allDay }) }

    Component.onCompleted: {
        home.refresh()
        if (page.autoSync) page.startSync()
    }
    StackView.onActivated: home.refresh()
    Connections {
        target: sync
        function onSyncFinished(outcome) { home.refresh() }
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
    Connections { target: timezone; function onZoneChanged() { home.refresh() } }

    TopBar {
        id: topBar
        title: qsTr("PB Office")
        leftItems: [ IconButton { objectName: "homeQuit"; text: qsTr("Home"); icon: "home"; onClicked: device.quit() } ]
        rightItems: [
            IconButton { objectName: "homeSettings"; text: qsTr("Settings"); icon: "gear"; onClicked: page.push("SettingsPage.qml") },
            IconButton { objectName: "homeSync"; text: qsTr("Sync"); icon: "sync"; enabled: !sync.syncing && !page.connecting; onClicked: page.startSync() }
        ]
    }
    SyncStatusLine {
        id: status
        anchors { top: topBar.bottom; left: parent.left; right: parent.right }
        connecting: page.connecting
        noNetwork: page.noNetwork
    }

    Flickable {
        anchors { top: status.bottom; left: parent.left; right: parent.right; bottom: parent.bottom }
        contentWidth: width
        contentHeight: column.implicitHeight + 40
        boundsBehavior: Flickable.StopAtBounds
        interactive: false
        clip: true

        Column {
            id: column
            width: parent.width

            component Head: Text {
                x: 30
                width: parent.width - 60
                topPadding: 24
                bottomPadding: 6
                font.pixelSize: Style.labelSize
                font.bold: true
                color: Style.ink
            }

            InkRow {
                objectName: "homeMail"
                visible: home.mailConfigured
                text: qsTr("Mail")
                value: home.unreadCount > 0 ? qsTr("%n unread", "", home.unreadCount) : qsTr("no unread mail")
                chevron: true
                onClicked: page.openInbox()
            }
            InkRow {
                objectName: "homeNewMessage"
                visible: home.mailConfigured
                text: qsTr("Write a message")
                chevron: true
                onClicked: { compose.start("new", 0); page.push("ComposePage.qml", { prepared: true }) }
            }
            InkRow {
                objectName: "homeSetupMail"
                visible: !home.mailConfigured
                text: qsTr("Set up your mail account")
                chevron: true
                onClicked: page.push("AccountSetupPage.qml")
            }
            InkRow {
                objectName: "homeCalendar"
                visible: home.calendarConfigured
                text: qsTr("Calendar")
                chevron: true
                onClicked: page.push("AgendaPage.qml")
            }
            InkRow {
                objectName: "homeContacts"
                visible: home.contactsAvailable
                text: qsTr("Contacts")
                chevron: true
                onClicked: { contacts.refresh(); page.push("ContactsPage.qml") }
            }
            InkRow {
                objectName: "homeSetupCalendar"
                visible: !home.calendarConfigured
                text: qsTr("Set up a calendar")
                chevron: true
                onClicked: page.push("CalendarAccountPage.qml")
            }

            Head { objectName: "homeEventsHead"; visible: home.calendarConfigured; text: qsTr("Today and tomorrow") }
            Text {
                objectName: "homeNeedsZone"
                x: 30
                width: parent.width - 60
                visible: home.calendarConfigured && home.needsZone
                wrapMode: Text.Wrap
                font.pixelSize: Style.bodySize
                color: Style.ink
                text: qsTr("Choose your time zone to see the times.")
            }
            Text {
                objectName: "homeNoEvents"
                x: 30
                width: parent.width - 60
                visible: home.calendarConfigured && !home.needsZone && home.events.length === 0
                wrapMode: Text.Wrap
                font.pixelSize: Style.bodySize
                color: Style.ink
                text: qsTr("Nothing is planned for today or tomorrow.")
            }
            Repeater {
                model: home.events
                delegate: EventRow {
                    required property int index
                    required property var modelData
                    tag: "homeEvent" + index
                    timeText: modelData.dayText + " " + modelData.timeText
                    title: modelData.title
                    calendarName: modelData.calendarName
                    pending: modelData.pending
                    conflict: modelData.conflict
                    onClicked: page.openEvent(modelData)
                }
            }
            InkRow {
                objectName: "homeMore"
                visible: home.moreCount > 0
                text: qsTr("+ %n more", "", home.moreCount)
                chevron: true
                onClicked: page.push("AgendaPage.qml")
            }
        }
    }
}
