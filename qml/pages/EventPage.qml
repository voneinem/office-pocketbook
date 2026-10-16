import QtQuick
import QtQuick.Controls
import ".."

// One event: title, when (in the user's zone), the event's own zone when it differs, how it repeats, where, who, the description. Paged by whole
// lines like the reader (no kinetic scrolling). The pencil changes the event (a repeating event only offers Delete there); an event of a feed or
// of a read-only calendar has no pencil.
Item {
    id: page

    property var eventId: 0
    property var startUtc: 0
    property var endUtc: 0
    property bool allDay: false
    readonly property int lineHeight: 44
    readonly property int margin: 30

    function readOnlyText() {
        if (!eventView.readOnly) return ""
        return eventView.readOnlyReason === "feed" ? qsTr("This event comes from a calendar feed and cannot be changed here.")
                                                   : qsTr("This calendar does not allow changes.")
    }
    function noteText() {
        switch (eventView.note) {
        case "rrule_unsupported": return qsTr("This event repeats in a way this app cannot show. Only its first time is listed.")
        case "tz_unknown": return qsTr("The time zone of this event is not known; its time is shown as written.")
        case "no_end": return ""
        case "truncated": return qsTr("Some of the text is very long and was cut.")
        case "range_unsupported": return qsTr("A change to \"this and all later\" events is not shown.")
        default: return ""
        }
    }

    Component.onCompleted: eventView.open(page.eventId, page.startUtc, page.endUtc, page.allDay)
    StackView.onActivated: eventView.open(page.eventId, page.startUtc, page.endUtc, page.allDay)   // back from the editor or the conflicts page
    Component.onDestruction: eventView.close()
    Connections { target: timezone; function onZoneChanged() { eventView.open(page.eventId, page.startUtc, page.endUtc, page.allDay) } }

    TopBar {
        id: topBar
        title: qsTr("Event")
        leftItems: [ IconButton { objectName: "eventBack"; text: qsTr("Back"); icon: "chevron-left"; onClicked: if (page.StackView.view) page.StackView.view.pop() } ]
        rightItems: [
            IconButton { objectName: "eventResolve"; text: qsTr("Events to decide"); icon: "info"; visible: eventView.conflict; onClicked: if (page.StackView.view) page.StackView.view.push(Qt.resolvedUrl("ConflictsPage.qml")) },
            IconButton { objectName: "eventEdit"; text: qsTr("Change"); icon: "pencil"; visible: eventView.loaded && !eventView.readOnly && !eventView.conflict
                         onClicked: if (page.StackView.view) page.StackView.view.push(Qt.resolvedUrl("EventEditPage.qml"), { eventId: page.eventId, popAfterDelete: 2 }) }
        ]
    }
    SyncStatusLine { id: status; anchors { top: topBar.bottom; left: parent.left; right: parent.right } }

    Item {
        id: area
        anchors { top: status.bottom; left: parent.left; right: parent.right; bottom: bottomBar.top }
        clip: true

        Flickable {
            id: view
            objectName: "eventView"
            width: parent.width
            height: Math.max(page.lineHeight, Math.floor(area.height / page.lineHeight) * page.lineHeight)
            contentWidth: width
            contentHeight: content.height
            interactive: false
            boundsBehavior: Flickable.StopAtBounds
            clip: true

            Column {
                id: content
                x: page.margin
                width: view.width - 2 * page.margin

                component Line: Text {
                    width: content.width
                    wrapMode: Text.Wrap
                    textFormat: Text.PlainText      // calendar text is never read as markup
                    font.pixelSize: Style.bodySize
                    lineHeight: page.lineHeight
                    lineHeightMode: Text.FixedHeight
                    color: Style.ink
                }
                component Gap: Item { width: 1; height: page.lineHeight }

                Line { objectName: "eventTitle"; text: eventView.summary.length > 0 ? eventView.summary : qsTr("(no title)"); font.bold: true; visible: eventView.loaded }
                Line { objectName: "eventWhen"; text: eventView.whenText; visible: eventView.loaded && eventView.whenText.length > 0 }
                Line { objectName: "eventZone"; text: qsTr("Time zone of the event: %1").arg(eventView.zoneText); visible: eventView.zoneText.length > 0; font.pixelSize: Style.captionSize }
                Line { objectName: "eventRepeat"; text: qsTr("Repeats: %1").arg(eventView.recurrenceText); visible: eventView.recurrenceText.length > 0 }
                Line { objectName: "eventLocation"; text: qsTr("Where: %1").arg(eventView.location); visible: eventView.location.length > 0 }
                Line { objectName: "eventCalendar"; text: qsTr("Calendar: %1").arg(eventView.calendarName); visible: eventView.loaded; font.pixelSize: Style.captionSize }
                Line { objectName: "eventReadOnly"; text: page.readOnlyText(); visible: text.length > 0; font.italic: true }
                Line { objectName: "eventPending"; text: qsTr("Changes to this event have not been sent yet."); visible: eventView.pending; font.italic: true }
                Line { objectName: "eventConflict"; text: qsTr("This event was changed here and on the server; both versions are kept."); visible: eventView.conflict; font.italic: true }
                Line { objectName: "eventNote"; text: page.noteText(); visible: text.length > 0; font.italic: true }
                Line { objectName: "eventNoZone"; text: qsTr("Choose your time zone to see the time."); visible: eventView.loaded && eventView.whenText.length === 0 }
                Line { objectName: "eventMissing"; text: qsTr("This event is no longer available."); visible: !eventView.loaded }
                Gap { visible: eventView.description.length > 0 || eventView.attendees.length > 0 }
                Line { objectName: "eventDescription"; text: eventView.description; visible: eventView.description.length > 0 }
                Gap { visible: eventView.description.length > 0 && eventView.attendees.length > 0 }
                Line { text: qsTr("Invited"); font.bold: true; visible: eventView.attendees.length > 0 }
                Repeater {
                    model: eventView.attendees
                    delegate: Line {
                        required property int index
                        required property string modelData
                        objectName: "eventAttendee" + index
                        font.pixelSize: Style.captionSize
                        text: modelData
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
        PageButtons {
            anchors.centerIn: parent
            target: view
            snap: page.lineHeight
            showIndicator: true
        }
    }
}
