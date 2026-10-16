import QtQuick
import QtTest

// The calendar pages with the real bridges (offscreen, 800 x 600: two rows fit on a list page).
TestCase {
    id: tc
    name: "calendar_flow"
    visible: true
    when: windowShown
    width: 800
    height: 600

    function page(name, props) {
        var comp = Qt.createComponent(harness.pagesDir + name)
        compare(comp.status, Component.Ready, comp.errorString())
        var initial = props || {}
        initial.width = tc.width
        initial.height = tc.height
        var obj = comp.createObject(tc, initial)
        verify(obj !== null, "page " + name + " could not be created")
        return obj
    }
    function tap(p, name) {
        var b = findChild(p, name)
        verify(b !== null, "no control " + name)
        for (var i = 0; i < 20; ++i) {
            var before = b.mapToItem(null, 0, 0).y
            wait(30)
            if (b.mapToItem(null, 0, 0).y === before) break
        }
        verify(b.enabled, name + " is disabled")
        mouseClick(b)
    }
    function fill(p, name, value) {
        var f = findChild(p, name)
        verify(f !== null, "no field " + name)
        f.text = value
    }
    function texts(p, prefix, count) {
        var out = []
        for (var i = 0; i < count; ++i) {
            var t = findChild(p, prefix + i)
            if (t) out.push(t.text)
        }
        return out
    }

    function init() {
        harness.clearEvents()
        harness.exec("UPDATE settings SET value = 'Europe/Berlin' WHERE key = 'tz_override'")
        if (harness.query("SELECT count(*) FROM settings WHERE key = 'tz_override'") === "0") harness.exec("INSERT INTO settings(key, value) VALUES('tz_override', 'Europe/Berlin')")
    }

    function test_agenda_lists_days_and_events_and_pages() {
        harness.addEvent("Standup", 2026, 10, 15, 9, 30, 30)
        harness.addEvent("Review", 2026, 10, 15, 14, 0, 60)
        harness.addEvent("Dentist", 2026, 10, 16, 8, 0, 45)
        var p = page("AgendaPage.qml")
        verify(p.rowsPerPage >= 2)
        agenda.refresh()
        compare(agenda.total, 3)
        compare(p.emptyKind, "")
        compare(findChild(p, "agendaHeader0").text, "Thursday, 15 October 2026")
        compare(findChild(p, "agendaEvent1Title").text, "Standup")
        compare(findChild(p, "agendaEvent1Time").text, "09:30 – 10:00")
        verify(agenda.pageCount >= 2)
        tap(p, "pageNext")
        verify(agenda.page >= 1)
        p.destroy()
    }

    function test_agenda_without_a_calendar_says_so_and_without_events_too() {
        var p = page("AgendaPage.qml")
        agenda.refresh()
        compare(p.emptyKind, account.calendarConfigured ? "empty" : "no_account")
        verify(findChild(p, "agendaEmpty").visible)
        p.destroy()
    }

    function test_the_agenda_asks_for_the_time_zone_while_none_is_known() {
        harness.addEvent("Standup", 2026, 10, 15, 9, 30, 30)
        harness.clearZone()
        var p = page("AgendaPage.qml")
        agenda.refresh()
        compare(agenda.needsZone, true)
        compare(p.emptyKind, "zone")
        verify(findChild(p, "chooseZoneButton").visible)
        compare(agenda.rows.length, 0)                       // no time with an assumed zone

        var z = page("TimeZonePage.qml")
        compare(timezone.needsZoneChoice, true)
        verify(findChild(z, "zoneIntro").text.indexOf("Choose where you are") >= 0)
        tap(z, "zoneGroup0")                                 // Europe
        compare(timezone.group, "Europe")
        verify(findChild(z, "zonePlace0") !== null)
        // go to the page with Berlin
        var found = false
        for (var i = 0; i < timezone.pageCount && !found; ++i) {
            for (var r = 0; r < timezone.rows.length; ++r)
                if (timezone.rows[r].name === "Europe/Berlin") { tap(z, "zonePlace" + r); found = true; break }
            if (!found) tap(z, "pageNext")
        }
        verify(found)
        tryCompare(timezone, "needsZoneChoice", false, 5000)
        compare(timezone.zoneName, "Europe/Berlin")
        tryCompare(agenda, "needsZone", false, 5000)
        compare(p.emptyKind, "")
        z.destroy()
        p.destroy()
    }

    function test_the_time_zone_page_goes_back_from_the_places_to_the_groups() {
        var z = page("TimeZonePage.qml")
        tap(z, "zoneGroup0")
        verify(z.inGroup)
        tap(z, "zoneBack")
        verify(!z.inGroup)
        z.destroy()
    }

    function test_day_page_shows_the_day_and_moves_by_days() {
        harness.addEvent("Meeting", 2026, 10, 15, 10, 0, 60)
        harness.addEvent("Holiday", 2026, 10, 14, 8, 0, 15)
        dayModel.goToday()
        var p = page("DayPage.qml")
        compare(findChild(p, "dayDate").text, "Wednesday, 14 October 2026")
        compare(findChild(p, "dayEvent0Title").text, "Holiday")
        compare(findChild(p, "todayButton").enabled, false)
        tap(p, "nextDay")
        compare(findChild(p, "dayDate").text, "Thursday, 15 October 2026")
        compare(findChild(p, "dayEvent0Title").text, "Meeting")
        verify(findChild(p, "todayButton").enabled)
        tap(p, "previousDay")
        tap(p, "previousDay")
        compare(findChild(p, "dayDate").text, "Tuesday, 13 October 2026")
        verify(findChild(p, "dayEmpty").visible)
        tap(p, "todayButton")
        compare(dayModel.selectedDate, "2026-10-14")
        p.destroy()
    }

    function test_month_page_marks_days_with_events_and_moves() {
        harness.addEvent("Meeting", 2026, 10, 15, 10, 0, 60)
        monthModel.setMode("month")
        monthModel.goToday()
        var p = page("MonthPage.qml")
        compare(monthModel.title, "October 2026")
        // 15 October 2026 is a Thursday in the third week (rows start on 28 September)
        verify(findChild(p, "cellMark2_3").visible)
        compare(findChild(p, "cellDay2_3").text, "15")
        compare(findChild(p, "cellMark2_2").visible, false)
        tap(p, "nextPeriod")
        compare(monthModel.title, "November 2026")
        tap(p, "previousPeriod")
        tap(p, "previousPeriod")
        compare(monthModel.title, "September 2026")
        tap(p, "monthToday")
        compare(monthModel.title, "October 2026")
        tap(p, "modeButton")
        compare(monthModel.mode, "week")
        compare(monthModel.title, "12 - 18 October 2026")
        tap(p, "modeButton")
        p.destroy()
    }

    // tapping a day rebuilds the grid under the finger; the day must still reach the day page's model (found on the reader)
    function test_tapping_a_day_in_the_month_selects_it_for_the_day_page() {
        monthModel.setMode("month")
        monthModel.goToday()
        dayModel.goToday()
        var p = page("MonthPage.qml")
        tap(p, "cellTap2_4")   // Friday 16 October 2026
        compare(monthModel.selectedDate, "2026-10-16")
        compare(dayModel.selectedDate, "2026-10-16")
        p.destroy()
    }

    function test_event_page_shows_the_event_and_turns_pages_by_whole_lines() {
        var longText = ""
        for (var i = 1; i <= 40; ++i) longText += "Line " + i + " of the agenda that has to be read page by page.\n"
        var id = harness.addEvent("Planning", 2026, 10, 15, 9, 0, 90, longText)
        var p = page("EventPage.qml", { eventId: id })
        tryCompare(eventView, "loaded", true, 2000)
        compare(findChild(p, "eventTitle").text, "Planning")
        compare(findChild(p, "eventWhen").text, "Thursday, 15 October 2026, 09:00 – 10:30")
        compare(findChild(p, "eventCalendar").text, "Calendar: Work")
        compare(findChild(p, "eventReadOnly").visible, false)
        var view = findChild(p, "eventView")
        compare(view.contentY, 0)
        tap(p, "pageDown")
        verify(view.contentY > 0)
        compare(view.contentY % p.lineHeight, 0)             // whole lines only
        p.destroy()
    }

    function test_event_page_shows_the_event_zone_and_the_read_only_reason() {
        var id = harness.addEvent("Call", 2026, 10, 15, 9, 0, 60, "", "America/New_York", false)
        var p = page("EventPage.qml", { eventId: id })
        tryCompare(eventView, "loaded", true, 2000)
        compare(findChild(p, "eventWhen").text, "Thursday, 15 October 2026, 15:00 – 16:00")
        verify(findChild(p, "eventZone").visible)
        verify(findChild(p, "eventZone").text.indexOf("America/New_York") >= 0)
        verify(findChild(p, "eventReadOnly").visible)
        verify(findChild(p, "eventReadOnly").text.indexOf("feed") >= 0)
        p.destroy()
    }

    function test_event_page_for_a_missing_event_says_so() {
        var p = page("EventPage.qml", { eventId: 424242 })
        wait(100)
        verify(findChild(p, "eventMissing").visible)
        p.destroy()
    }

    // typed like a user (textEdited fires)
    function type(p, name, text) {
        var f = findChild(p, name)
        verify(f !== null, "no field " + name)
        var input = f.field !== undefined ? f.field : f
        input.forceActiveFocus()
        for (var i = 0; i < text.length; ++i) keyClick(text.charAt(i))
    }
    function scrollTo(p, name) {
        var sc = findChild(p, "eventEditScroller")
        var f = findChild(p, name)
        sc.contentY = Math.max(0, Math.min(sc.contentHeight - sc.height, f.mapToItem(sc.contentItem, 0, 0).y - 20))
        wait(30)
    }

    function test_a_new_event_is_typed_validated_and_saved_as_not_sent_yet() {
        harness.addEditable("Anchor")   // makes the writable calendar known
        var p = page("EventEditPage.qml", { date: "2026-10-20" })
        verify(findChild(p, "eventEditSave").enabled)
        compare(findChild(p, "eventStartDate").text, "2026-10-20")
        compare(findChild(p, "eventStartTime").text, "15:00")
        tap(p, "eventEditSave")                                                 // no title yet
        compare(findChild(p, "eventTitleField").errorText, "Enter a title.")
        compare(harness.query("SELECT count(*) FROM event WHERE summary = 'Dentist'"), "0")
        type(p, "eventTitleField", "Dentist")
        compare(findChild(p, "eventTitleField").errorText, "")                  // typing clears the message
        scrollTo(p, "eventEndTime")
        findChild(p, "eventEndTime").field.forceActiveFocus()
        findChild(p, "eventEndTime").field.selectAll()
        keyClick("1"); keyClick("7"); keyClick(":"); keyClick("3"); keyClick("0")
        tap(p, "eventEditSave")
        compare(findChild(p, "eventEndTime").errorText, "")
        compare(harness.query("SELECT sync_state FROM event WHERE summary = 'Dentist'"), "pending_create")
        p.destroy()
    }

    function test_a_bad_date_names_the_field() {
        harness.addEditable("Anchor")
        var p = page("EventEditPage.qml", { date: "2026-10-20" })
        type(p, "eventTitleField", "x")
        scrollTo(p, "eventStartDate")
        var f = findChild(p, "eventStartDate").field
        f.forceActiveFocus()
        f.selectAll()
        keyClick("9")
        tap(p, "eventEditSave")
        compare(findChild(p, "eventStartDate").errorText, "Use a date like 2026-10-20.")
        p.destroy()
    }

    function test_an_event_of_a_read_only_calendar_opens_with_the_reason_and_no_save() {
        var id = harness.addEditable("Holiday", false)
        var p = page("EventEditPage.qml", { eventId: id })
        compare(findChild(p, "eventEditBlock").text, "This event comes from a calendar feed and cannot be changed here.")
        verify(!findChild(p, "eventEditSave").enabled)
        verify(!findChild(p, "eventEditDelete").visible)
        p.destroy()
    }

    function test_a_repeating_event_cannot_be_changed_but_can_be_deleted_as_a_whole() {
        var id = harness.addEditable("Weekly", true, "RRULE:FREQ=WEEKLY\r\n")
        var p = page("EventEditPage.qml", { eventId: id })
        compare(findChild(p, "eventEditBlock").text, "Repeating events can be changed in your calendar app.")
        verify(!findChild(p, "eventEditSave").enabled)
        verify(findChild(p, "eventEditDelete").visible)
        p.destroy()
    }

    function test_changing_an_event_marks_it_for_the_next_sync() {
        var id = harness.addEditable("Planning")
        var p = page("EventEditPage.qml", { eventId: id })
        compare(findChild(p, "eventTitleField").text, "Planning")
        type(p, "eventTitleField", " 2")
        tap(p, "eventEditSave")
        compare(harness.query("SELECT sync_state || '|' || summary FROM event WHERE id = " + id), "pending_update|Planning 2")
        p.destroy()
    }

    function test_the_conflicts_page_lists_both_versions_and_keeps_both_on_request() {
        var id = harness.addEditable("Original")
        eventEdit.begin(id)
        eventEdit.title = "Mine"
        verify(eventEdit.save())
        harness.makeConflict(id, "Theirs")
        var p = page("ConflictsPage.qml")
        conflicts.refresh()
        verify(findChild(p, "conflictRows").visible)
        compare(findChild(p, "conflictMine0").text.indexOf("Mine") >= 0, true)
        compare(findChild(p, "conflictServer0").text.indexOf("Theirs") >= 0, true)
        p.chosen = conflicts.rows[0]
        p.perform("both")
        compare(conflicts.total, 0)
        compare(harness.query("SELECT count(*) FROM event WHERE summary IN ('Mine', 'Theirs')"), "2")
        verify(findChild(p, "conflictsEmpty").visible)
        p.destroy()
    }

    function test_keeping_the_servers_version_asks_first() {
        var id = harness.addEditable("Original")
        eventEdit.begin(id)
        eventEdit.title = "Mine"
        verify(eventEdit.save())
        harness.makeConflict(id, "Theirs")
        var p = page("ConflictsPage.qml")
        conflicts.refresh()
        p.chosen = conflicts.rows[0]
        p.perform("server")
        compare(p.confirmAction, "server")
        compare(conflicts.total, 1)                                              // nothing happened yet
        p.apply("server")
        compare(conflicts.total, 0)
        compare(harness.query("SELECT summary FROM event WHERE id = " + id), "Theirs")
        p.destroy()
    }

    function test_the_settings_page_shows_the_options_and_saves_the_switch_at_once() {
        var p = page("SettingsPage.qml")
        compare(findChild(p, "settingsTimeZone").value, timezone.zoneName)
        compare(findChild(p, "settingsAutoSync").checked, true)
        findChild(p, "settingsAutoSync").toggled(false)
        compare(harness.query("SELECT value FROM settings WHERE key = 'auto_sync'"), "0")
        compare(findChild(p, "settingsAutoSync").checked, false)
        findChild(p, "settingsAutoSync").toggled(true)
        compare(harness.query("SELECT value FROM settings WHERE key = 'auto_sync'"), "1")
        // without a mail account the mail options are dimmed
        if (!settings.hasMailAccount) {
            compare(findChild(p, "settingsDays").enabled, false)
            compare(findChild(p, "settingsSentByServer").enabled, false)
        }
        verify(findChild(p, "settingsCustomCa").text.indexOf("usual authorities") > 0)   // no own authority: the check is the usual one
        verify(findChild(p, "settingsDaysNote").text.indexOf("removes the older mail") > 0)
        p.openChoice("days")
        verify(findChild(p, "settingsChoice").visible)
        p.destroy()
    }

    function test_the_home_page_lists_today_and_tomorrow_and_leads_to_the_agenda() {
        harness.addEvent("Standup", 2026, 10, 14, 15, 0, 30)
        harness.addEvent("Dentist", 2026, 10, 15, 8, 0, 45)
        harness.addEvent("Far away", 2026, 10, 20, 8, 0, 45)
        var p = page("HomePage.qml")
        home.refresh()
        compare(findChild(p, "homeEvent0Title").text, "Standup")
        compare(findChild(p, "homeEvent0Time").text, "Today 15:00 – 15:30")
        compare(findChild(p, "homeEvent1Title").text, "Dentist")
        verify(findChild(p, "homeEvent1Time").text.indexOf("Tomorrow") === 0)
        compare(findChild(p, "homeEvent2Title"), null)                          // the day after tomorrow is not listed
        compare(findChild(p, "homeMore").visible, false)
        verify(findChild(p, "homeSettings") !== null)
        verify(findChild(p, "homeQuit") !== null)
        p.destroy()
    }

    function test_the_home_page_says_so_when_nothing_is_planned_or_the_zone_is_missing() {
        harness.clearZone()
        var p = page("HomePage.qml")
        home.refresh()
        if (home.calendarConfigured) compare(findChild(p, "homeNeedsZone").visible, true)
        harness.exec("INSERT OR REPLACE INTO settings(key, value) VALUES('tz_override', 'Europe/Berlin')")
        home.refresh()
        if (home.calendarConfigured) compare(findChild(p, "homeNoEvents").visible, true)
        p.destroy()
    }

    function test_the_calendar_account_page_tests_and_saves_and_lists_the_calendars() {
        var p = page("CalendarAccountPage.qml")
        fill(p, "caldavUrl", "http://insecure.example.org")
        fill(p, "calendarUsername", "alice")
        fill(p, "calendarPassword", "pw-for-tests")
        tap(p, "calendarTestButton")
        tryCompare(p, "statusKey", "invalid_input", 5000)
        compare(p.errorField, "caldav_url")

        fill(p, "caldavUrl", harness.caldavUrl)
        fill(p, "calendarPassword", "wrong")
        tap(p, "calendarTestButton")
        tryCompare(p, "statusKey", "auth_failed", 20000)
        compare(p.failedSide, "caldav")
        verify(findChild(p, "calendarStatus").text.indexOf("Calendar server: ") === 0)

        fill(p, "calendarPassword", "pw-for-tests")
        tap(p, "calendarTestButton")
        tryCompare(p, "statusKey", "ok", 20000)
        compare(p.found.length, 2)
        verify(findChild(p, "foundCalendar0") !== null)
        compare(account.calendarConfigured, false)           // a test saves nothing

        tap(p, "calendarSaveButton")
        tryCompare(account, "calendarConfigured", true, 5000)
        p.destroy()
        var again = page("CalendarAccountPage.qml")
        compare(findChild(again, "caldavUrl").text, harness.caldavUrl)
        compare(findChild(again, "calendarPassword").text, "")   // the password is never shown again
        again.destroy()
    }
}
