import QtQuick
import QtQuick.Controls
import QtTest

// The contact pages with the real bridges (offscreen, 800 x 600: a few rows fit on a list page).
TestCase {
    id: tc
    name: "contacts_flow"
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
        wait(60)   // the layout settles
        return obj
    }
    function stack(name) {
        // no transitions, as in the app (e-ink): a push or pop is done at once
        var sv = Qt.createQmlObject('import QtQuick.Controls; StackView { pushEnter: null; pushExit: null; popEnter: null; popExit: null; replaceEnter: null; replaceExit: null }', tc)
        sv.width = tc.width
        sv.height = tc.height
        sv.push(harness.pagesDir + name, { width: tc.width, height: tc.height })
        wait(50)
        return sv
    }
    // a long form is moved by screens: bring the control into view first
    function reveal(p, item) {
        var sc = findChild(p, "editScroller")
        if (!sc) return
        wait(60)   // a page just shown has not laid its form out yet
        var y = item.mapToItem(sc.contentItem, 0, 0).y
        sc.contentY = Math.max(0, Math.min(y - 100, sc.contentHeight - sc.height))
    }
    function tap(p, name) {
        tryVerify(function () { return findChild(p, name) !== null }, 1000, "no control " + name)
        var b = findChild(p, name)
        reveal(p, b)
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
        tryVerify(function () { return findChild(p, name) !== null }, 1000, "no field " + name)
        var f = findChild(p, name)
        verify(f !== null, "no field " + name)
        reveal(p, f)
        f.text = value
    }
    function names(p, count) {
        var out = []
        for (var i = 0; i < count; ++i) {
            var t = findChild(p, "contactRow" + i + "Name")
            if (t && t.visible) out.push(t.text)
        }
        return out
    }
    function plainTextEverywhere(item) {
        // no Text of a page shows rich text: nothing in a contact can become a link
        var ok = true
        for (var i = 0; i < item.children.length; ++i) {
            var c = item.children[i]
            if (c.hasOwnProperty("textFormat") && c.textFormat !== Text.PlainText && c.text !== undefined && c.text.length > 0) ok = false
            if (!plainTextEverywhere(c)) ok = false
        }
        return ok
    }

    function init() {
        harness.reset()
        contacts.pageSize = 2
        contacts.clearSearch()
        contact.close()
        sync.refreshState()
    }

    function test_the_list_shows_a_page_in_name_order_with_page_buttons() {
        var book = harness.addBook("Contacts")
        for (var i = 0; i < 7; ++i) harness.addContact(book, "FN:Given Family0" + i + "\r\nN:Family0" + i + ";Given;;;\r\nTEL:+1 555 010" + i + "\r\n")
        var p = page("ContactsPage.qml")
        verify(p.rowsPerPage >= 2)
        contacts.refresh()
        compare(contacts.total, 7)
        compare(p.emptyKind, "")
        compare(names(p, p.rowsPerPage)[0], "Given Family00")
        verify(contacts.pageCount >= 2)
        verify(!findChild(p, "pagePrevious").enabled)
        tap(p, "pageNext")
        compare(contacts.page, 1)
        verify(names(p, p.rowsPerPage)[0] !== "Given Family00")
        tap(p, "pagePrevious")
        compare(contacts.page, 0)
        p.destroy()
    }

    function test_search_narrows_the_list_and_clear_restores_it() {
        var book = harness.addBook("Contacts")
        harness.addContact(book, "FN:Anna Beispiel\r\nN:Beispiel;Anna;;;\r\nORG:Example Works\r\n")
        harness.addContact(book, "FN:Bert Muster\r\nN:Muster;Bert;;;\r\nTEL:+1 555 0100\r\n")
        harness.addContact(book, "FN:Carla Dritte\r\nN:Dritte;Carla;;;\r\n")
        var p = page("ContactsPage.qml")
        contacts.refresh()
        compare(contacts.total, 3)
        fill(p, "contactsSearch", "works")
        tap(p, "contactsSearchButton")
        compare(contacts.total, 1)
        compare(names(p, p.rowsPerPage)[0], "Anna Beispiel")
        fill(p, "contactsSearch", "5550100")
        tap(p, "contactsSearchButton")
        compare(names(p, p.rowsPerPage)[0], "Bert Muster")
        fill(p, "contactsSearch", "nobody at all")
        tap(p, "contactsSearchButton")
        compare(p.emptyKind, "no_match")
        verify(findChild(p, "contactsEmpty").visible)
        tap(p, "contactsSearchClear")
        compare(contacts.total, 3)
        compare(findChild(p, "contactsSearch").text, "")
        compare(p.emptyKind, "")
        p.destroy()
    }

    function test_a_tap_opens_the_detail_page_and_back_returns_to_the_same_list_page() {
        var book = harness.addBook("Contacts")
        for (var i = 0; i < 5; ++i) harness.addContact(book, "FN:Given Family0" + i + "\r\nN:Family0" + i + ";Given;;;\r\nTEL;TYPE=CELL:+1 555 010" + i + "\r\nEMAIL;TYPE=WORK:p" + i + "@example.org\r\n")
        var sv = stack("ContactsPage.qml")
        var list = sv.currentItem
        contacts.refresh()
        tap(list, "pageNext")
        compare(contacts.page, 1)
        var second = names(list, list.rowsPerPage)[0]
        tap(list, "contactRow0")
        wait(100)
        compare(sv.depth, 2)
        var detail = sv.currentItem
        compare(findChild(detail, "contactName").text, second)
        compare(findChild(detail, "contactPhone0Label").text, "mobile")
        verify(findChild(detail, "contactPhone0Value").text.indexOf("+1 555 010") === 0)
        compare(findChild(detail, "contactEmail0Label").text, "work")
        tap(detail, "contactBack")
        wait(100)
        compare(sv.depth, 1)
        compare(contacts.page, 1)
        sv.destroy()
    }

    function test_the_detail_shows_organization_address_note_book_and_unknown_properties_do_not_matter() {
        var book = harness.addBook("Contacts")
        var id = harness.addContact(book, "FN:Anna Beispiel\r\nN:Beispiel;Anna;;;\r\nORG:Example Works\r\nADR;TYPE=HOME:;;1 Example Street;Exampletown;EX;12345;Exampleland\r\nNOTE:Met at the fair.\r\nBDAY:1985-04-12\r\nX-CUSTOM:keep\r\n")
        var p = page("ContactPage.qml", { contactId: id })
        compare(findChild(p, "contactName").text, "Anna Beispiel")
        compare(findChild(p, "contactOrg").text, "Example Works")
        compare(findChild(p, "contactAddress0Label").text, "home")
        verify(findChild(p, "contactAddress0Value").text.indexOf("1 Example Street") >= 0)
        compare(findChild(p, "contactNote").text, "Met at the fair.")
        compare(findChild(p, "contactBookValue").text, "Contacts")
        p.destroy()
    }

    function test_a_long_contact_is_shown_page_by_page_and_nothing_is_cut() {
        var book = harness.addBook("Contacts")
        var note = []
        for (var i = 0; i < 120; ++i) note.push("Line number " + i + " of a long note")
        var id = harness.addContact(book, "FN:Long Note\r\nN:Note;Long;;;\r\nNOTE:" + note.join("\\n") + "\r\n")
        var p = page("ContactPage.qml", { contactId: id })
        tryVerify(function () { return p.pageCount > 1 }, 2000, "a long note needs more than one page")
        var shown = findChild(p, "contactNote").text
        for (var j = 0; j < 120; ++j) verify(shown.indexOf("Line number " + j + " of a long note") >= 0, "line " + j + " is missing")
        var view = findChild(p, "contactView")
        compare(view.contentY, 0)
        verify(!findChild(p, "pageUp").enabled)
        tap(p, "pageDown")
        verify(view.contentY > 0, "the page did not turn")
        verify(findChild(p, "pageUp").enabled)
        // the last page can be reached and then the button is disabled
        for (var k = 0; k < p.pageCount + 2 && findChild(p, "pageDown").enabled; ++k) tap(p, "pageDown")
        verify(!findChild(p, "pageDown").enabled)
        compare(findChild(p, "pageIndicator").text, p.pageCount + " / " + p.pageCount)
        p.destroy()
    }

    function test_the_detail_offers_no_call_mail_or_link_action() {
        var book = harness.addBook("Contacts")
        var id = harness.addContact(book, "FN:Anna Beispiel\r\nN:Beispiel;Anna;;;\r\nTEL:+1 555 0101\r\nEMAIL:anna@example.org\r\nURL:https://www.example.org/anna\r\nNOTE:see https://example.org/x and tel:+1555\r\n")
        var p = page("ContactPage.qml", { contactId: id })
        verify(plainTextEverywhere(findChild(p, "contactView")), "a Text of the contact shows rich text")
        verify(findChild(p, "contactEmail0Value").text === "anna@example.org")
        p.destroy()
    }

    function test_the_empty_states_say_why() {
        var p = page("ContactsPage.qml")
        harness.removeAccount()
        contacts.refresh()
        compare(p.emptyKind, "no_account")
        harness.reset()
        contacts.refresh()
        compare(p.emptyKind, "no_addressbook")
        verify(findChild(p, "contactsEmptyText").text.indexOf("address book") >= 0)
        harness.addBook("Contacts")
        contacts.refresh()
        compare(p.emptyKind, "empty")
        verify(findChild(p, "contactsEmpty").visible)
        p.destroy()
    }

    function test_changes_waiting_for_the_server_are_marked_on_the_row() {
        var book = harness.addBook("Contacts")
        var id = harness.addContact(book, "FN:Anna Beispiel\r\nN:Beispiel;Anna;;;\r\n")
        harness.exec("UPDATE contact SET sync_state = 'pending_update' WHERE id = " + id)
        var p = page("ContactsPage.qml")
        contacts.refresh()
        verify(findChild(p, "contactRow0Note").text.indexOf("not synced") >= 0)
        harness.exec("UPDATE contact SET error_key = 'refused' WHERE id = " + id)
        contacts.refresh()
        verify(findChild(p, "contactRow0Note").text.indexOf("failed") >= 0)
        p.destroy()
    }

    function test_the_status_line_names_the_contacts_when_their_sync_failed() {
        harness.addBook("Contacts")
        var p = page("ContactsPage.qml")
        harness.failContactsSync("server_unreachable")
        sync.refreshState()
        var line = findChild(p, "statusLine")
        verify(line !== null)
        verify(line.text.indexOf("contacts") >= 0, line.text)
        verify(line.text.indexOf("cannot be reached") >= 0, line.text)
        p.destroy()
    }

    // ---- creating a contact (US2) ----------------------------------------------------------------------------------------------------------------

    function test_a_new_contact_is_saved_and_the_list_shows_it_marked_not_synced() {
        var book = harness.addBook("Contacts")
        contacts.refresh()
        var sv = stack("ContactsPage.qml")
        var list = sv.currentItem
        tap(list, "newContactButton")
        wait(100)
        compare(sv.depth, 2)
        var p = sv.currentItem
        verify(!findChild(p, "editBook").visible, "one writable book needs no chooser")
        fill(p, "editGiven", "Anna")
        fill(p, "editFamily", "Beispiel")
        tap(p, "editAddPhone")
        fill(p, "editPhone0Value", "+1 555 0101")
        tap(p, "editPhone0Label")
        var choice = findChild(p, "editLabelChoice")
        verify(choice.visible, "the label list did not open")
        var labels = []
        for (var i = 0; i < choice.choices.length; ++i) labels.push(choice.choices[i].value)
        compare(labels.join(","), "mobile,home,work,other")
        choice.close()
        choice.chosen("work")
        compare(findChild(p, "editPhone0Label").text.toLowerCase().indexOf("work") >= 0, true)
        tap(p, "editAddEmail")
        fill(p, "editEmail0Value", "anna@example.org")
        tap(p, "editSave")
        wait(100)
        compare(sv.depth, 1)   // back at the list
        compare(contacts.total, 1)
        compare(findChild(sv.currentItem, "contactRow0Name").text, "Anna Beispiel")
        verify(findChild(sv.currentItem, "contactRow0Note").text.indexOf("not synced") >= 0)
        verify(harness.query("SELECT vcf FROM contact").indexOf("TEL;TYPE=WORK:+1 555 0101") >= 0)
        sv.destroy()
    }

    function test_saving_an_empty_form_shows_the_message_and_stays() {
        harness.addBook("Contacts")
        var p = page("ContactEditPage.qml")
        tap(p, "editSave")
        compare(contact.validationErrors.length, 1)
        verify(findChild(p, "editMessage").visible)
        verify(findChild(p, "editMessage").text.indexOf("needs a name") >= 0, findChild(p, "editMessage").text)
        compare(harness.query("SELECT count(*) FROM contact"), "0")
        p.destroy()
    }

    function test_a_bad_email_address_marks_its_field_and_nothing_is_saved() {
        harness.addBook("Contacts")
        var p = page("ContactEditPage.qml")
        fill(p, "editGiven", "Anna")
        tap(p, "editAddEmail")
        fill(p, "editEmail0Value", "no-at-sign")
        tap(p, "editSave")
        verify(findChild(p, "editEmail0Value").errorText.length > 0, "the field shows no message")
        compare(findChild(p, "editGiven").errorText, "")
        compare(harness.query("SELECT count(*) FROM contact"), "0")
        fill(p, "editEmail0Value", "anna@example.org")
        tap(p, "editSave")
        compare(harness.query("SELECT count(*) FROM contact"), "1")
        p.destroy()
    }

    function test_cancel_asks_only_when_something_was_changed() {
        harness.addBook("Contacts")
        var sv = stack("ContactsPage.qml")
        sv.push(harness.pagesDir + "ContactEditPage.qml", { width: tc.width, height: tc.height })
        wait(50)
        var p = sv.currentItem
        tap(p, "editCancel")
        wait(50)
        compare(sv.depth, 1)   // nothing typed: leaves at once
        sv.push(harness.pagesDir + "ContactEditPage.qml", { width: tc.width, height: tc.height })
        wait(50)
        p = sv.currentItem
        fill(p, "editGiven", "Anna")
        findChild(p, "editGiven").textEdited()
        tap(p, "editCancel")
        wait(50)
        compare(sv.depth, 2)
        verify(findChild(p, "editLeaveDialog").visible)
        findChild(p, "editLeaveDialog").accepted()
        wait(50)
        compare(sv.depth, 1)
        compare(harness.query("SELECT count(*) FROM contact"), "0")
        sv.destroy()
    }

    function test_with_two_writable_books_the_form_offers_the_choice() {
        var a = harness.addBook("Contacts")
        var b = harness.addBook("Work")
        harness.addBook("Company", false)
        var p = page("ContactEditPage.qml")
        verify(findChild(p, "editBook").visible)
        tap(p, "editBook")
        var choice = findChild(p, "editBookChoice")
        verify(choice.visible)
        compare(choice.choices.length, 2)   // the read-only book is not offered
        choice.close()
        choice.chosen(String(b))
        verify(findChild(p, "editBook").text.indexOf("Work") >= 0)
        fill(p, "editGiven", "Anna")
        tap(p, "editSave")
        compare(harness.query("SELECT addressbook_id FROM contact"), String(b))
        p.destroy()
    }

    function test_without_a_writable_book_new_is_disabled_and_says_why() {
        harness.addBook("Company", false)
        var p = page("ContactsPage.qml")
        contacts.refresh()
        verify(!findChild(p, "newContactButton").enabled)
        harness.addContact(harness.query("SELECT id FROM addressbook"), "FN:Carla Company\r\nN:Company;Carla;;;\r\n")
        contacts.refresh()
        verify(!findChild(p, "newContactButton").enabled)
        verify(findChild(p, "contactsNewHint").visible)
        verify(findChild(p, "contactsNewHint").text.indexOf("read-only") >= 0 || findChild(p, "contactsNewHint").text.indexOf("writable") >= 0)
        p.destroy()
    }

    function test_a_failed_contact_offers_retry_and_discard() {
        var book = harness.addBook("Contacts")
        var id = harness.addContact(book, "FN:Anna Beispiel\r\nN:Beispiel;Anna;;;\r\n")
        harness.exec("UPDATE contact SET sync_state = 'pending_create', error_key = 'refused' WHERE id = " + id)
        var p = page("ContactPage.qml", { contactId: id })
        verify(findChild(p, "contactFailed").visible)
        verify(findChild(p, "contactRetry").visible)
        verify(findChild(p, "contactDiscard").visible)
        tap(p, "contactRetry")
        compare(harness.query("SELECT error_key FROM contact WHERE id = " + id), "")
        harness.exec("UPDATE contact SET error_key = 'refused' WHERE id = " + id)
        contact.open(id)
        tap(p, "contactDiscard")
        var dlg = findChild(p, "contactDiscardDialog")
        verify(dlg.visible)
        dlg.accepted()
        compare(harness.query("SELECT count(*) FROM contact"), "0")
        p.destroy()
    }

    // ---- changing a contact and the contacts that need a decision (US3) --------------------------------------------------------------------------

    function test_a_contact_is_changed_through_the_form_and_the_detail_shows_it_marked() {
        var book = harness.addBook("Contacts")
        var id = harness.addContact(book, "FN:Anna Beispiel\r\nN:Beispiel;Anna;;;\r\nTEL;TYPE=CELL:+1 555 0101\r\nEMAIL;TYPE=WORK:anna@example.org\r\n")
        var sv = stack("ContactsPage.qml")
        contacts.refresh()
        contact.open(id)
        sv.push(harness.pagesDir + "ContactPage.qml", { contactId: id, width: tc.width, height: tc.height })
        wait(60)
        var detail = sv.currentItem
        tap(detail, "contactEdit")
        wait(100)
        compare(sv.depth, 3)
        var p = sv.currentItem
        compare(findChild(p, "editGiven").text, "Anna")
        compare(findChild(p, "editFamily").text, "Beispiel")
        compare(findChild(p, "editPhone0Value").text, "+1 555 0101")
        compare(findChild(p, "editEmail0Value").text, "anna@example.org")
        verify(!findChild(p, "editBook").visible, "an existing contact stays in its address book")
        fill(p, "editPhone0Value", "+1 555 0199")
        tap(p, "editEmail0Remove")
        tap(p, "editAddAddress")
        fill(p, "editAddress0Street", "9 Example Alley")
        fill(p, "editAddress0City", "Exampletown")
        tap(p, "editSave")
        wait(100)
        compare(sv.depth, 2)   // back at the contact
        detail = sv.currentItem
        compare(findChild(detail, "contactPhone0Value").text, "+1 555 0199")
        verify(findChild(detail, "contactEmail0Value") === null || !findChild(detail, "contactEmail0Value").visible)
        verify(findChild(detail, "contactAddress0Value").text.indexOf("9 Example Alley") >= 0)
        verify(findChild(detail, "contactPending").visible)
        compare(harness.query("SELECT sync_state FROM contact WHERE id = " + id), "pending_update")
        sv.destroy()
    }

    function test_a_contact_of_a_read_only_book_shows_the_reason_instead_of_the_editor() {
        var book = harness.addBook("Company", false)
        var id = harness.addContact(book, "FN:Carla Firma\r\nN:Firma;Carla;;;\r\n")
        var p = page("ContactPage.qml", { contactId: id })
        verify(!findChild(p, "contactEdit").visible)
        verify(findChild(p, "contactReadOnly").visible)
        verify(findChild(p, "contactReadOnly").text.indexOf("read-only") >= 0)
        p.destroy()
    }

    function test_an_entry_too_long_for_the_form_is_a_read_only_row_that_can_be_removed() {
        var book = harness.addBook("Contacts")
        var longPhone = ""
        for (var i = 0; i < 90; ++i) longPhone += "9"
        var id = harness.addContact(book, "FN:Quin Rho\r\nN:Rho;Quin;;;\r\nTEL;TYPE=WORK:" + longPhone + "\r\nTEL;TYPE=CELL:+1 555 0213\r\n")
        var p = page("ContactEditPage.qml", { contactId: id })
        var locked = findChild(p, "editPhone0Locked")
        verify(locked.visible)
        verify(locked.text.indexOf("too long") >= 0)
        verify(!findChild(p, "editPhone0Value").visible, "the long number cannot be typed into")
        verify(findChild(p, "editPhone1Value").visible)
        tap(p, "editPhone0Remove")
        compare(findChild(p, "editPhone0Value").text, "+1 555 0213")   // the other one moved up
        tap(p, "editSave")
        verify(harness.query("SELECT vcf FROM contact WHERE id = " + id).indexOf(longPhone) < 0)
        verify(harness.query("SELECT vcf FROM contact WHERE id = " + id).indexOf("+1 555 0213") >= 0)
        p.destroy()
    }

    function test_removing_an_entry_or_deleting_a_contact_shows_the_trash_icon_like_other_screens() {
        var book = harness.addBook("Contacts")
        var id = harness.addContact(book, "FN:Anna Beispiel\r\nN:Beispiel;Anna;;;\r\nTEL;TYPE=CELL:+1 555 0101\r\nEMAIL;TYPE=WORK:anna@example.org\r\nADR;TYPE=HOME:;;9 Example Alley;Exampletown;;12345;\r\n")
        var p = page("ContactEditPage.qml", { contactId: id })
        compare(findChild(p, "editPhone0Remove").icon, "trash")
        compare(findChild(p, "editEmail0Remove").icon, "trash")
        compare(findChild(p, "editAddress0Remove").icon, "trash")
        p.destroy()
        var d = page("ContactPage.qml", { contactId: id })
        compare(findChild(d, "contactDelete").icon, "trash")
        d.destroy()
    }

    function test_the_form_keeps_the_field_being_typed_in_above_the_keyboard() {
        harness.addBook("Contacts")
        var p = page("ContactEditPage.qml", {})
        var sc = findChild(p, "editScroller")
        wait(150)   // the first field takes the focus, which the page follows once
        verify(p.coverPad > 0, "a field being typed in makes room for the keyboard even when Qt reports none")
        var h = sc.contentHeight - p.padHeight
        verify(h > sc.height, "the form is longer than the page, so it is moved by screens")
        // the keyboard covers 300 px of the form: the end of the form can be moved above it
        p.coverPad = 700
        compare(sc.contentHeight, h + 700)
        sc.contentY = 0
        p.keepInView(sc.height - 20, sc.height + 40, sc.height - 300)   // a line below the part the keyboard leaves free
        verify(sc.contentY > 0, "the form moved up")
        compare(sc.contentY, sc.height + 40 - (sc.height - 300) + 20)
        // a line already in the free part stays where it is
        var y = sc.contentY
        p.keepInView(10, 50, sc.height - 300)
        compare(sc.contentY, y)
        // a line above the top comes back into view
        p.keepInView(-50, -10, sc.height - 300)
        compare(sc.contentY, Math.max(0, y - 50 - 20))
        // without the keyboard the form is as long as before and does not stay scrolled past its end
        sc.contentY = sc.contentHeight - sc.height
        p.coverPad = 0
        p.clampScroll()
        compare(sc.contentHeight, h + p.padHeight)
        verify(p.padHeight >= p.Window.height * 0.4 - 1, "room for the keyboard remains at the end of the form")
        verify(sc.contentY <= Math.max(0, sc.contentHeight - sc.height))
        p.destroy()
    }

    function test_the_note_at_the_end_of_the_form_is_moved_above_the_keyboard_when_it_gets_the_focus() {
        harness.addBook("Contacts")
        var p = page("ContactEditPage.qml", {})
        var sc = findChild(p, "editScroller")
        var note = findChild(p, "editNote")
        reveal(p, note)
        note.forceActiveFocus()
        wait(150)
        verify(p.coverPad > 0)
        var r = note.cursorRectangle
        var bottom = note.mapToItem(sc, r.x, r.y).y + r.height
        verify(bottom <= sc.height - p.coverPad, "the line being typed in is above the keyboard: " + bottom + " > " + (sc.height - p.coverPad))
        verify(bottom > 0)
        p.destroy()
    }

    function test_the_decision_page_lists_the_contacts_with_both_versions_and_applies_the_choice() {
        var book = harness.addBook("Contacts")
        var a = harness.addContact(book, "FN:Anna Lokal\r\nN:Lokal;Anna;;;\r\n")
        var b = harness.addContact(book, "FN:Bert Weg\r\nN:Weg;Bert;;;\r\n")
        harness.makeContactConflict(a, "Server")
        harness.makeServerDeleted(b)
        var p = page("ConflictsPage.qml")
        conflicts.refresh()
        compare(conflicts.total, 2)
        // (the page holds one row at this height: the conflict comes first, the removed contact is on the next page)
        compare(findChild(p, "conflictTitle0").text, "Anna Lokal · Contacts")
        compare(findChild(p, "conflictMine0").text, "Yours: Anna Lokal")
        compare(findChild(p, "conflictServer0").text, "On the server: Anna Server")
        // keep the server's version: asks first
        tap(p, "conflictTap0")
        var actions = findChild(p, "conflictActions")
        verify(actions.visible)
        var values = []
        for (var i = 0; i < actions.choices.length; ++i) values.push(actions.choices[i].value)
        compare(values.join(","), "mine,server,both")
        actions.close()
        actions.chosen("server")
        var confirm = findChild(p, "conflictConfirm")
        verify(confirm.visible)
        confirm.close()
        confirm.accepted()
        compare(harness.query("SELECT sync_state FROM contact WHERE id = " + a), "synced")
        compare(harness.query("SELECT display_name FROM contact WHERE id = " + a), "Anna Server")
        compare(conflicts.total, 1)
        // the contact the server removed: send it again
        compare(findChild(p, "conflictTitle0").text, "Bert Weg · Contacts")
        verify(findChild(p, "conflictServer0").text.indexOf("deleted on the server") >= 0)
        tap(p, "conflictTap0")
        actions = findChild(p, "conflictActions")
        values = []
        for (var j = 0; j < actions.choices.length; ++j) values.push(actions.choices[j].value)
        compare(values.join(","), "recreate,discard")
        actions.close()
        actions.chosen("recreate")
        compare(harness.query("SELECT sync_state FROM contact WHERE id = " + b), "pending_create")
        compare(conflicts.total, 0)
        verify(findChild(p, "conflictsEmpty").visible)
        p.destroy()
    }

    function test_a_kept_contact_that_the_server_removed_can_be_discarded_after_a_question() {
        var book = harness.addBook("Contacts")
        var b = harness.addContact(book, "FN:Bert Weg\r\nN:Weg;Bert;;;\r\n")
        harness.makeServerDeleted(b)
        var p = page("ConflictsPage.qml")
        conflicts.refresh()
        tap(p, "conflictTap0")
        var actions = findChild(p, "conflictActions")
        actions.close()
        actions.chosen("discard")
        var confirm = findChild(p, "conflictConfirm")
        verify(confirm.visible)
        verify(confirm.text.indexOf("contact") >= 0)
        confirm.close()
        confirm.accepted()
        compare(harness.query("SELECT count(*) FROM contact"), "0")
        p.destroy()
    }
    // ---- deleting a contact (US4) -----------------------------------------------------------------------------------------------------------------

    function test_delete_asks_first_naming_the_contact_and_cancel_changes_nothing() {
        var book = harness.addBook("Contacts")
        var id = harness.addContact(book, "FN:Anna Beispiel\r\nN:Beispiel;Anna;;;\r\n")
        var p = page("ContactPage.qml", { contactId: id })
        verify(findChild(p, "contactDelete").visible)
        tap(p, "contactDelete")
        var dlg = findChild(p, "contactDeleteDialog")
        verify(dlg.visible)
        verify(dlg.text.indexOf("Anna Beispiel") >= 0)
        dlg.close()
        dlg.rejected()
        compare(harness.query("SELECT sync_state FROM contact WHERE id = " + id), "synced")
        p.destroy()
    }

    function test_a_confirmed_delete_leaves_the_list_and_shows_a_deletion_waiting_with_undo() {
        var book = harness.addBook("Contacts")
        var id = harness.addContact(book, "FN:Anna Beispiel\r\nN:Beispiel;Anna;;;\r\n")
        harness.addContact(book, "FN:Bert Bleibt\r\nN:Bleibt;Bert;;;\r\n")
        var sv = stack("ContactsPage.qml")
        contacts.refresh()
        sv.push(harness.pagesDir + "ContactPage.qml", { contactId: id, width: tc.width, height: tc.height })
        wait(60)
        var detail = sv.currentItem
        tap(detail, "contactDelete")
        var dlg = findChild(detail, "contactDeleteDialog")
        verify(dlg.visible)
        dlg.close()
        dlg.accepted()
        wait(100)
        compare(sv.depth, 1)   // back at the list
        var list = sv.currentItem
        compare(harness.query("SELECT sync_state FROM contact WHERE id = " + id), "pending_delete")
        compare(contacts.total, 1)
        compare(names(list, 5).join("|"), "Bert Bleibt")
        var banner = findChild(list, "deletionWaiting0")
        verify(banner.visible)
        verify(banner.text.indexOf("Anna Beispiel") >= 0)
        tap(list, "deletionUndo0")
        compare(harness.query("SELECT sync_state FROM contact WHERE id = " + id), "synced")
        compare(contacts.total, 2)
        compare(findChild(list, "deletionWaiting0"), null)
        sv.destroy()
    }

    function test_a_read_only_book_offers_no_delete() {
        var book = harness.addBook("Company", false)
        var id = harness.addContact(book, "FN:Carla Firma\r\nN:Firma;Carla;;;\r\n")
        var p = page("ContactPage.qml", { contactId: id })
        verify(!findChild(p, "contactDelete").visible)
        verify(findChild(p, "contactReadOnly").visible)
        p.destroy()
    }
    function test_the_contacts_view_shows_how_many_changes_wait_for_the_server() {
        var book = harness.addBook("Contacts")
        var a = harness.addContact(book, "FN:Anna Beispiel\r\nN:Beispiel;Anna;;;\r\n")
        var b = harness.addContact(book, "FN:Bert Bleibt\r\nN:Bleibt;Bert;;;\r\n")
        harness.exec("UPDATE contact SET sync_state = 'pending_update' WHERE id = " + a)
        harness.exec("UPDATE contact SET sync_state = 'pending_delete', undo_state = 'synced' WHERE id = " + b)
        var p = page("ContactsPage.qml")
        sync.refreshState()
        contacts.refresh()
        var line = findChild(p, "statusLine")
        verify(line.text.indexOf("2 change(s) waiting") >= 0, line.text)
        p.destroy()
    }
}
