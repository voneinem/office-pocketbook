import QtQuick
import QtQuick.Controls
import ".."

// Create or change one event (US4). Dates are typed as 2026-10-20 and times as 09:30 in the zone of the event; an all-day event has a last day and no
// times. Nothing is sent from here: the event is saved on the reader (marked "not synced") and the next sync sends it. A repeating event, an event
// of a feed or of a read-only calendar opens with the reason and no Save; a repeating event can still be deleted as a whole, after a question.
Item {
    id: page

    property var eventId: 0           // 0 = a new event
    property string date: ""          // a new event: the day to start on ("" = today)
    property int popAfterDelete: 1    // how many pages to leave after a delete (2 when opened from the event page)
    property bool dirty: false
    property string note: ""

    function messageFor(key) {
        switch (key) {
        case "title_missing": return qsTr("Enter a title.")
        case "too_long": return qsTr("The text is too long.")
        case "date_invalid": return qsTr("Use a date like 2026-10-20.")
        case "time_invalid": return qsTr("Use a time like 09:30.")
        case "end_before_start": return qsTr("The end is before the start.")
        case "zone_unknown": return qsTr("The time zone of this event is not known.")
        case "read_only": return qsTr("This calendar does not allow changes.")
        case "series_read_only": return qsTr("Repeating events can be changed in your calendar app.")
        case "in_conflict": return qsTr("This event has two versions. Choose one first.")
        case "delete_pending": return qsTr("This event is being deleted.")
        case "duplicate_uid": return qsTr("An event with this identity exists already.")
        case "zone_unresolved": return qsTr("Choose your time zone first.")
        case "not_found": return qsTr("This event is no longer available.")
        default: return key.length > 0 ? qsTr("The event could not be saved on this reader.") : ""
        }
    }
    function blockText() {
        switch (eventEdit.blockReason) {
        case "": return ""
        case "no_calendar": return qsTr("There is no calendar here that accepts new events.")
        case "feed": return qsTr("This event comes from a calendar feed and cannot be changed here.")
        case "read_only": return qsTr("This calendar does not allow changes.")
        case "series_read_only": return qsTr("Repeating events can be changed in your calendar app.")
        default: return messageFor(eventEdit.blockReason)
        }
    }
    function fieldError(field) { return eventEdit.errorField === field ? page.messageFor(eventEdit.errorKey) : "" }
    function goBack() {
        if (page.dirty) leaveDialog.open()
        else page.leave(1)
    }
    function leave(n) {
        Qt.inputMethod.hide()
        if (!page.StackView.view) return
        for (var i = 0; i < n; ++i) page.StackView.view.pop()
    }
    function refreshLists() {
        agenda.refresh()
        dayModel.refresh()
        monthModel.refresh()
    }

    Component.onCompleted: {
        if (page.eventId > 0) eventEdit.begin(page.eventId)
        else eventEdit.beginNew(page.date)
        titleField.text = eventEdit.title
        locationField.text = eventEdit.location
        descriptionArea.text = eventEdit.description
        startDateField.text = eventEdit.startDate
        startTimeField.text = eventEdit.startTime
        endDateField.text = eventEdit.endDate
        endTimeField.text = eventEdit.endTime
        if (eventEdit.canSave && page.eventId === 0) titleField.forceFocus()
    }
    Component.onDestruction: eventEdit.close()

    Connections {
        target: eventEdit
        function onSaved(id) {
            sync.noteChange(device.wifiConnected())   // the change reaches the calendar server at the next sync, which starts by itself shortly
            page.refreshLists()
            page.leave(1)
        }
        function onRemoved() {
            sync.noteChange(device.wifiConnected())
            page.refreshLists()
            page.leave(page.popAfterDelete)
        }
    }

    TopBar {
        id: topBar
        title: page.eventId > 0 ? qsTr("Change event") : qsTr("New event")
        leftItems: [ IconButton { objectName: "eventEditBack"; text: qsTr("Back"); icon: "chevron-left"; onClicked: page.goBack() } ]
        rightItems: [ IconButton { objectName: "eventEditSave"; text: qsTr("Save"); icon: "check"; enabled: eventEdit.canSave; onClicked: eventEdit.save() } ]
    }

    Flickable {
        id: scroller
        objectName: "eventEditScroller"
        anchors { left: parent.left; right: parent.right; top: topBar.bottom; bottom: tools.top }
        contentWidth: width
        contentHeight: form.implicitHeight + 40
        boundsBehavior: Flickable.StopAtBounds
        interactive: false
        clip: true

        Column {
            id: form
            x: 30
            y: 20
            width: parent.width - 60
            spacing: 20

            Text {
                objectName: "eventEditBlock"
                visible: text.length > 0
                width: parent.width
                wrapMode: Text.Wrap
                font.pixelSize: Style.bodySize
                font.bold: true
                color: Style.ink
                text: page.blockText()
            }
            InkField { id: titleField; objectName: "eventTitleField"; width: parent.width; label: qsTr("Title"); enabled: eventEdit.canSave
                       errorText: page.fieldError("title")
                       onTextEdited: { eventEdit.title = text; page.dirty = true } }
            Item {
                width: parent.width
                height: 80
                Text { anchors { left: parent.left; right: allDaySwitch.left; rightMargin: 20; verticalCenter: parent.verticalCenter } text: qsTr("All day"); font.pixelSize: Style.labelSize; color: Style.ink }
                InkSwitch {
                    id: allDaySwitch
                    objectName: "eventAllDay"
                    anchors { right: parent.right; verticalCenter: parent.verticalCenter }
                    checked: eventEdit.allDay
                    enabled: eventEdit.canSave
                    onToggled: function (on) { eventEdit.allDay = on; page.dirty = true }
                }
            }
            InkField { id: startDateField; objectName: "eventStartDate"; width: parent.width; label: eventEdit.allDay ? qsTr("First day") : qsTr("Starts (date)")
                       enabled: eventEdit.canSave; placeholderText: "2026-10-20"; errorText: page.fieldError("startDate")
                       field.inputMethodHints: Qt.ImhPreferNumbers | Qt.ImhNoPredictiveText
                       onTextEdited: { eventEdit.startDate = text; page.dirty = true } }
            InkField { id: startTimeField; objectName: "eventStartTime"; visible: !eventEdit.allDay; width: parent.width; label: qsTr("Starts (time)")
                       enabled: eventEdit.canSave; placeholderText: "09:30"; errorText: page.fieldError("startTime")
                       field.inputMethodHints: Qt.ImhPreferNumbers | Qt.ImhNoPredictiveText
                       onTextEdited: { eventEdit.startTime = text; page.dirty = true } }
            InkField { id: endDateField; objectName: "eventEndDate"; width: parent.width; label: eventEdit.allDay ? qsTr("Last day") : qsTr("Ends (date)")
                       enabled: eventEdit.canSave; placeholderText: "2026-10-20"; errorText: page.fieldError("endDate")
                       field.inputMethodHints: Qt.ImhPreferNumbers | Qt.ImhNoPredictiveText
                       onTextEdited: { eventEdit.endDate = text; page.dirty = true } }
            InkField { id: endTimeField; objectName: "eventEndTime"; visible: !eventEdit.allDay; width: parent.width; label: qsTr("Ends (time)")
                       enabled: eventEdit.canSave; placeholderText: "10:30"; errorText: page.fieldError("endTime")
                       field.inputMethodHints: Qt.ImhPreferNumbers | Qt.ImhNoPredictiveText
                       onTextEdited: { eventEdit.endTime = text; page.dirty = true } }
            Text {
                objectName: "eventEditZone"
                visible: !eventEdit.allDay && eventEdit.zoneName.length > 0
                width: parent.width
                wrapMode: Text.Wrap
                font.pixelSize: Style.captionSize
                color: Style.ink
                text: qsTr("Times are in the time zone %1.").arg(eventEdit.zoneName)
            }
            InkRow {
                objectName: "eventEditCalendar"
                visible: eventEdit.isNew && eventEdit.calendars.length > 0
                text: qsTr("Calendar: %1").arg(eventEdit.calendarName)
                chevron: eventEdit.calendars.length > 1
                enabled: eventEdit.calendars.length > 1
                onClicked: {
                    var c = []
                    for (var i = 0; i < eventEdit.calendars.length; ++i) c.push({ label: eventEdit.calendars[i].name, value: String(eventEdit.calendars[i].id) })
                    calendarChoice.choices = c
                    calendarChoice.current = String(eventEdit.calendarId)
                    calendarChoice.open()
                }
            }
            Text {
                objectName: "eventEditCalendarName"
                visible: !eventEdit.isNew && eventEdit.calendarName.length > 0
                width: parent.width
                font.pixelSize: Style.captionSize
                color: Style.ink
                text: qsTr("Calendar: %1").arg(eventEdit.calendarName)
            }
            InkField { id: locationField; objectName: "eventLocationField"; width: parent.width; label: qsTr("Where"); enabled: eventEdit.canSave
                       onTextEdited: { eventEdit.location = text; page.dirty = true } }
            Text { text: qsTr("Notes"); font.pixelSize: Style.labelSize; color: Style.ink }
            TextArea {
                id: descriptionArea
                objectName: "eventDescriptionField"
                width: parent.width
                height: Math.max(240, contentHeight + 40)
                enabled: eventEdit.canSave
                font.pixelSize: Style.bodySize
                color: Style.ink
                wrapMode: TextEdit.Wrap
                inputMethodHints: Qt.ImhNoPredictiveText
                background: Rectangle { color: Style.paper; radius: Style.radiusField; border.width: Style.ruleWidth; border.color: Style.ink }
                TextMenu { target: descriptionArea; multiline: true }
                onTextChanged: if (activeFocus) { eventEdit.description = text; page.dirty = true }
                onCursorRectangleChanged: {
                    const y = form.y + descriptionArea.y + cursorRectangle.y
                    if (y + cursorRectangle.height > scroller.contentY + scroller.height) scroller.contentY = Math.min(scroller.contentHeight - scroller.height, y + cursorRectangle.height - scroller.height + 20)
                    else if (y < scroller.contentY) scroller.contentY = Math.max(0, y - 20)
                }
            }
            Text {
                objectName: "eventEditNote"
                visible: text.length > 0
                width: parent.width
                wrapMode: Text.Wrap
                font.pixelSize: Style.captionSize
                font.italic: true
                color: Style.ink
                text: eventEdit.errorKey.length > 0 && eventEdit.errorField.length === 0 ? page.messageFor(eventEdit.errorKey) : ""
            }
        }
    }

    BottomPanel {
        id: tools
        anchors.bottom: parent.bottom
        tools: [
            ToolButton { objectName: "eventEditSaveTool"; text: qsTr("Save"); icon: "check"; enabled: eventEdit.canSave; onClicked: eventEdit.save() },
            ToolButton { objectName: "eventEditDelete"; text: qsTr("Delete"); icon: "trash"; visible: eventEdit.canDelete; onClicked: deleteDialog.open() }
        ]
    }

    ChoiceDialog {
        id: calendarChoice
        objectName: "eventCalendarChoice"
        title: qsTr("Calendar")
        onChosen: function (value) { eventEdit.calendarId = Number(value) }
    }
    ConfirmDialog {
        id: leaveDialog
        objectName: "eventLeaveDialog"
        title: qsTr("Leave without saving?")
        text: qsTr("Your changes to this event are lost.")
        acceptText: qsTr("Leave")
        cancelText: qsTr("Keep editing")
        onAccepted: page.leave(1)
    }
    ConfirmDialog {
        id: deleteDialog
        objectName: "eventDeleteDialog"
        title: qsTr("Delete this event?")
        text: eventEdit.blockReason === "series_read_only" ? qsTr("All repeats of this event are deleted from the calendar at the next sync.")
                                                           : qsTr("The event is deleted from the calendar at the next sync.")
        acceptText: qsTr("Delete")
        cancelText: qsTr("Cancel")
        onAccepted: eventEdit.remove()
    }
}
