import QtQuick
import QtQuick.Controls
import ".."

// Create or change a contact: the name and organization, phone numbers, email addresses and postal addresses with a label each, and a note.
// Nothing is sent from here: the contact is saved on the reader (marked "not synced") and the next sync sends it. Text that is longer than this
// form accepts and comes from another app shows as a read-only row that can be removed (and is kept unchanged otherwise). With more than one
// writable address book a new contact can be put in the one you choose; an existing contact stays in its own.
Item {
    id: page

    property var contactId: 0        // 0 = a new contact, else the contact to change
    property bool dirty: false
    // the entries of the form: [{label, value, origIndex, locked}], addresses [{label, street, city, region, postcode, country, origIndex, locked}]
    property var phones: []
    property var emails: []
    property var addresses: []
    property bool noteLocked: false
    property var bookId: 0
    property string bookName: ""
    // what the on-screen keyboard covers of the form (0 while it is hidden): the form gets that much room below its end, so even
    // the note at the bottom can be moved above the keyboard
    property real coverPad: 0
    // the form always ends with room for the keyboard (about 40 % of the window), so the note can be paged up above where the keyboard will be
    readonly property real padHeight: Math.max(page.coverPad, page.height * 0.4)
    // the page is drawn on a stage that is the window scaled by the device pixel ratio (Main.qml): this many page pixels per window pixel
    readonly property real dpr: page.Window.width > 0 ? page.width / page.Window.width : 1
    property Item typingIn: null     // the text control that has the focus

    readonly property var phoneLabels: ["mobile", "home", "work", "other"]
    readonly property var otherLabels: ["home", "work", "other"]

    function labelText(label) {
        switch (label) {
        case "mobile": return qsTr("Mobile")
        case "home": return qsTr("Home")
        case "work": return qsTr("Work")
        case "other": return qsTr("Other")
        default: return label.length > 0 ? label : qsTr("Other")
        }
    }
    function messageFor(key) {
        switch (key) {
        case "empty": return qsTr("A contact needs a name, an organization, a phone number or an email address.")
        case "email_invalid": return qsTr("This email address is not valid.")
        case "too_long": return qsTr("The text is too long.")
        case "too_many": return qsTr("There are too many entries.")
        case "locked_changed": return qsTr("This entry is too long to change here.")
        case "read_only": return qsTr("This address book is read-only.")
        case "in_conflict": return qsTr("This contact was changed here and on the server. Decide that first.")
        case "delete_pending": return qsTr("This contact is waiting to be deleted.")
        case "unreadable": return qsTr("This contact could not be read and cannot be changed here.")
        case "book_gone": return qsTr("This address book is gone on the server.")
        case "not_found": return qsTr("This address book is no longer available.")
        case "card_too_large": return qsTr("The contact is too big.")
        default: return key.length > 0 ? qsTr("The contact could not be saved on this reader.") : ""
        }
    }
    function fieldError(field) {
        var list = contact.validationErrors
        for (var i = 0; i < list.length; ++i)
            if (list[i].field === field) return page.messageFor(list[i].key)
        return ""
    }
    function firstError() {
        var list = contact.validationErrors
        if (list.length > 0) return page.messageFor(list[0].key)
        return page.messageFor(contact.saveError)
    }
    function goBack() {
        if (page.dirty) leaveDialog.open()
        else page.leave()
    }
    function leave() {
        Qt.inputMethod.hide()
        if (page.StackView.view) page.StackView.view.pop()
    }
    function copyList(list) {
        var out = []
        for (var i = 0; i < list.length; ++i) out.push(Object.assign({}, list[i]))
        return out
    }
    function bookNameOf(id) {
        for (var i = 0; i < contact.writableBooks.length; ++i)
            if (contact.writableBooks[i].id === id) return contact.writableBooks[i].name
        return ""
    }
    function addPhone() { page.phones = page.phones.concat([{ label: "mobile", value: "", origIndex: -1, locked: false }]); page.dirty = true; page.focusLast(phoneRepeater) }
    function addEmail() { page.emails = page.emails.concat([{ label: "home", value: "", origIndex: -1, locked: false }]); page.dirty = true; page.focusLast(emailRepeater) }
    function addAddress() {
        page.addresses = page.addresses.concat([{ label: "home", street: "", city: "", region: "", postcode: "", country: "", origIndex: -1, locked: false }])
        page.dirty = true
        page.focusLast(addressRepeater)
    }
    function remove(list, index) {
        var copy = page.copyList(list)
        copy.splice(index, 1)
        page.dirty = true
        return copy
    }
    // what the form holds now, in the shape ContactBridge.save wants
    function formMap() {
        return {
            contactId: page.contactId, bookId: page.bookId,
            given: givenField.text, family: familyField.text, org: orgField.text,
            note: page.noteLocked ? "" : noteArea.text, noteLocked: page.noteLocked,
            phones: page.phones, emails: page.emails, addresses: page.addresses
        }
    }
    // The page up/down buttons move the form by screens; a field that is about to be typed in is brought into view.
    function reveal(item) {
        if (!item) return
        var y = item.mapToItem(scroller.contentItem, 0, 0).y
        var bottom = y + item.height
        if (y < scroller.contentY) scroller.contentY = Math.max(0, y - 20)
        else if (bottom > scroller.contentY + scroller.height) scroller.contentY = Math.min(Math.max(0, scroller.contentHeight - scroller.height), bottom - scroller.height + 20)
    }
    function clampScroll() { scroller.contentY = Math.max(0, Math.min(scroller.contentY, scroller.contentHeight - scroller.height)) }
    // Moves the form by whole steps (no animation) so that the line from lineTop to lineBottom (in the page's view of the form) lies
    // within the first viewHeight pixels, the part the keyboard leaves free.
    function keepInView(lineTop, lineBottom, viewHeight) {
        var max = Math.max(0, scroller.contentHeight - scroller.height)
        if (lineBottom > viewHeight) scroller.contentY = Math.min(max, scroller.contentY + lineBottom - viewHeight + 20)
        else if (lineTop < 0) scroller.contentY = Math.max(0, scroller.contentY + lineTop - 20)
    }
    // the top of the keyboard in window coordinates: the system's figure when Qt reports a believable one, else 62 % of the window
    // (the reader's keyboard starts at about 66 % of the screen)
    function keyboardTop() {
        var kb = Qt.inputMethod.keyboardRectangle
        var h = page.Window.height
        return (kb.height > 0 && kb.y > h * 0.3 && kb.y < h) ? kb.y : h * 0.62
    }
    function fieldFocus(item, gained) {
        if (gained) page.typingIn = item
        else if (page.typingIn === item) page.typingIn = null
        Qt.callLater(page.followFocus)
    }
    // The field being typed in (a text area: the line with the cursor) stays above the keyboard. The keyboard is taken to be there
    // while a text field of the form has the focus: the system's keyboard is not always reported to Qt.
    function followFocus() {
        var item = page.typingIn
        var typing = item !== null && item !== undefined
        var covered = 0
        // (window pixels from here on, turned into page pixels at the end; 60 px more for the keyboard's own top edge)
        if (Qt.inputMethod.visible || typing) {
            var overlap = scroller.mapToItem(null, 0, scroller.height).y - page.keyboardTop()
            if (overlap > 0) covered = overlap * page.dpr + 60
        }
        page.coverPad = covered
        if (covered <= 0) { page.clampScroll(); return }
        if (!typing) return
        var r = item.height > 100 ? item.cursorRectangle : Qt.rect(0, 0, item.width, item.height)
        var top = item.mapToItem(scroller, r.x, r.y).y
        page.keepInView(top, top + r.height, scroller.height - covered)
    }
    function focusLast(repeater) {
        Qt.callLater(function () {
            var it = repeater.itemAt(repeater.count - 1)
            if (!it) return
            page.reveal(it)
            var f = it.firstField
            if (f) f.forceFocus()
        })
    }
    function save() {
        Qt.inputMethod.hide()
        contact.save(page.formMap())
    }

    function fill() {
        page.bookId = contact.edit.bookId
        page.bookName = page.bookNameOf(page.bookId)
        page.phones = page.copyList(contact.edit.phones)
        page.emails = page.copyList(contact.edit.emails)
        page.addresses = page.copyList(contact.edit.addresses)
        page.noteLocked = contact.edit.noteLocked
        givenField.text = contact.edit.given
        familyField.text = contact.edit.family
        orgField.text = contact.edit.org
        noteArea.text = contact.edit.note
        page.dirty = false
    }

    Component.onCompleted: {
        if (page.contactId > 0) {
            contact.open(page.contactId)
            if (contact.loadEdit()) {
                page.fill()
                givenField.forceFocus()
            }
        } else if (contact.newContact()) {
            page.fill()
            givenField.forceFocus()
        }
    }

    Connections {
        target: contact
        function onSaved(id) {
            page.dirty = false
            contacts.refresh()
            page.leave()
        }
    }
    Connections {
        target: Qt.inputMethod
        function onVisibleChanged() { Qt.callLater(page.followFocus) }
        function onKeyboardRectangleChanged() { Qt.callLater(page.followFocus) }
    }

    TopBar {
        id: topBar
        title: page.contactId > 0 ? qsTr("Change contact") : qsTr("New contact")
        leftItems: [ IconButton { objectName: "editCancel"; text: qsTr("Cancel"); icon: "chevron-left"; onClicked: page.goBack() } ]
        rightItems: [ IconButton { objectName: "editSave"; text: qsTr("Save"); icon: "check"; enabled: contact.editing; onClicked: page.save() } ]
    }

    Flickable {
        id: scroller
        objectName: "editScroller"
        anchors { left: parent.left; right: parent.right; top: topBar.bottom; bottom: bottomBar.top }
        contentWidth: width
        contentHeight: form.implicitHeight + 40 + page.padHeight
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
                objectName: "editBlock"
                visible: !contact.editing
                width: parent.width
                wrapMode: Text.Wrap
                font.pixelSize: Style.bodySize
                font.bold: true
                color: Style.ink
                text: page.contactId > 0 ? page.messageFor(contact.editBlockedReason)
                      : (contact.createBlockedReason === "no_writable_book" ? qsTr("There is no address book here that accepts new contacts.")
                         : qsTr("No address book was found on the server yet."))
            }
            Text {
                objectName: "editMessage"
                visible: text.length > 0
                width: parent.width
                wrapMode: Text.Wrap
                font.pixelSize: Style.bodySize
                font.bold: true
                color: Style.ink
                text: page.firstError()
            }
            InkRow {
                objectName: "editBook"
                x: -30
                width: parent.width + 60
                visible: page.contactId === 0 && contact.writableBooks.length > 1
                text: qsTr("Address book: %1").arg(page.bookName)
                chevron: true
                onClicked: {
                    var c = []
                    for (var i = 0; i < contact.writableBooks.length; ++i) c.push({ label: contact.writableBooks[i].name, value: String(contact.writableBooks[i].id) })
                    bookChoice.choices = c
                    bookChoice.current = String(page.bookId)
                    bookChoice.open()
                }
            }
            InkField { onFieldFocus: function (item, gained) { page.fieldFocus(item, gained) } id: givenField; objectName: "editGiven"; width: parent.width; label: qsTr("First name"); enabled: contact.editing
                       errorText: page.fieldError("given")
                       onTextEdited: page.dirty = true }
            InkField { onFieldFocus: function (item, gained) { page.fieldFocus(item, gained) } id: familyField; objectName: "editFamily"; width: parent.width; label: qsTr("Last name"); enabled: contact.editing
                       errorText: page.fieldError("family"); onTextEdited: page.dirty = true }
            InkField { onFieldFocus: function (item, gained) { page.fieldFocus(item, gained) } id: orgField; objectName: "editOrg"; width: parent.width; label: qsTr("Organization"); enabled: contact.editing
                       errorText: page.fieldError("org"); onTextEdited: page.dirty = true }

            // ---- phone numbers ----
            Text { text: qsTr("Phone"); font.pixelSize: Style.labelSize; font.bold: true; color: Style.ink; visible: contact.editing }
            Repeater {
                id: phoneRepeater
                model: page.phones
                delegate: Item {
                    id: phoneRow
                    readonly property var firstField: phoneValue
                    required property int index
                    required property var modelData
                    width: form.width
                    height: phoneValue.height + (phoneValue.errorText.length > 0 ? 0 : 0)
                    IconButton { id: phoneLabel; objectName: "editPhone" + phoneRow.index + "Label"; text: page.labelText(phoneRow.modelData.label); width: 200; visible: !phoneRow.modelData.locked
                                 onClicked: { labelChoice.target = "phones"; labelChoice.index = phoneRow.index; labelChoice.openFor(phoneLabel, page.phoneLabels, phoneRow.modelData.label) } }
                    InkField { onFieldFocus: function (item, gained) { page.fieldFocus(item, gained) }
                        id: phoneValue
                        objectName: "editPhone" + phoneRow.index + "Value"
                        x: 200
                        width: parent.width - 200 - Style.touchMin
                        visible: !phoneRow.modelData.locked
                        text: phoneRow.modelData.value
                        errorText: page.fieldError("phones[" + phoneRow.index + "]")
                        field.inputMethodHints: Qt.ImhDialableCharactersOnly
                        onTextChanged: if (phoneRow.index < page.phones.length && !page.phones[phoneRow.index].locked) page.phones[phoneRow.index].value = text
                        onTextEdited: page.dirty = true
                    }
                    Text {
                        objectName: "editPhone" + phoneRow.index + "Locked"
                        visible: phoneRow.modelData.locked
                        width: parent.width - Style.touchMin - 10
                        height: Style.touchMin
                        verticalAlignment: Text.AlignVCenter
                        wrapMode: Text.Wrap
                        font.pixelSize: Style.captionSize
                        font.italic: true
                        color: Style.ink
                        textFormat: Text.PlainText
                        text: qsTr("%1: too long to edit here, kept unchanged").arg(page.labelText(phoneRow.modelData.label))
                    }
                    IconButton { objectName: "editPhone" + phoneRow.index + "Remove"; text: qsTr("Remove"); icon: "trash"; x: parent.width - Style.touchMin
                                 onClicked: page.phones = page.remove(page.phones, phoneRow.index) }
                }
            }
            InkRow { objectName: "editAddPhone"; x: -30; width: parent.width + 60; visible: contact.editing && page.phones.length < 20; text: qsTr("Add phone"); chevron: false; onClicked: page.addPhone() }

            // ---- email addresses ----
            Text { text: qsTr("Email"); font.pixelSize: Style.labelSize; font.bold: true; color: Style.ink; visible: contact.editing }
            Repeater {
                id: emailRepeater
                model: page.emails
                delegate: Item {
                    id: emailRow
                    readonly property var firstField: emailValue
                    required property int index
                    required property var modelData
                    width: form.width
                    height: emailValue.height
                    IconButton { id: emailLabel; objectName: "editEmail" + emailRow.index + "Label"; text: page.labelText(emailRow.modelData.label); width: 200; visible: !emailRow.modelData.locked
                                 onClicked: { labelChoice.target = "emails"; labelChoice.index = emailRow.index; labelChoice.openFor(emailLabel, page.otherLabels, emailRow.modelData.label) } }
                    InkField { onFieldFocus: function (item, gained) { page.fieldFocus(item, gained) }
                        id: emailValue
                        objectName: "editEmail" + emailRow.index + "Value"
                        x: 200
                        width: parent.width - 200 - Style.touchMin
                        visible: !emailRow.modelData.locked
                        text: emailRow.modelData.value
                        errorText: page.fieldError("emails[" + emailRow.index + "]")
                        field.inputMethodHints: Qt.ImhEmailCharactersOnly | Qt.ImhNoPredictiveText
                        onTextChanged: if (emailRow.index < page.emails.length && !page.emails[emailRow.index].locked) page.emails[emailRow.index].value = text
                        onTextEdited: page.dirty = true
                    }
                    Text {
                        objectName: "editEmail" + emailRow.index + "Locked"
                        visible: emailRow.modelData.locked
                        width: parent.width - Style.touchMin - 10
                        height: Style.touchMin
                        verticalAlignment: Text.AlignVCenter
                        wrapMode: Text.Wrap
                        font.pixelSize: Style.captionSize
                        font.italic: true
                        color: Style.ink
                        textFormat: Text.PlainText
                        text: qsTr("%1: too long to edit here, kept unchanged").arg(page.labelText(emailRow.modelData.label))
                    }
                    IconButton { objectName: "editEmail" + emailRow.index + "Remove"; text: qsTr("Remove"); icon: "trash"; x: parent.width - Style.touchMin
                                 onClicked: page.emails = page.remove(page.emails, emailRow.index) }
                }
            }
            InkRow { objectName: "editAddEmail"; x: -30; width: parent.width + 60; visible: contact.editing && page.emails.length < 20; text: qsTr("Add email"); chevron: false; onClicked: page.addEmail() }

            // ---- postal addresses ----
            Text { text: qsTr("Address"); font.pixelSize: Style.labelSize; font.bold: true; color: Style.ink; visible: contact.editing }
            Repeater {
                id: addressRepeater
                model: page.addresses
                delegate: Column {
                    id: addressRow
                    readonly property var firstField: streetField
                    required property int index
                    required property var modelData
                    width: form.width
                    spacing: 10
                    Item {
                        width: parent.width
                        height: Style.touchMin
                        IconButton { id: addressLabel; objectName: "editAddress" + addressRow.index + "Label"; text: page.labelText(addressRow.modelData.label); width: 200
                                     visible: !addressRow.modelData.locked
                                     onClicked: { labelChoice.target = "addresses"; labelChoice.index = addressRow.index; labelChoice.openFor(addressLabel, page.otherLabels, addressRow.modelData.label) } }
                        Text {
                            objectName: "editAddress" + addressRow.index + "Locked"
                            visible: addressRow.modelData.locked
                            width: parent.width - Style.touchMin - 10
                            height: Style.touchMin
                            verticalAlignment: Text.AlignVCenter
                            wrapMode: Text.Wrap
                            font.pixelSize: Style.captionSize
                            font.italic: true
                            color: Style.ink
                            text: qsTr("%1: too long to edit here, kept unchanged").arg(page.labelText(addressRow.modelData.label))
                        }
                        IconButton { objectName: "editAddress" + addressRow.index + "Remove"; text: qsTr("Remove"); icon: "trash"; x: parent.width - Style.touchMin
                                     onClicked: page.addresses = page.remove(page.addresses, addressRow.index) }
                    }
                    InkField { onFieldFocus: function (item, gained) { page.fieldFocus(item, gained) } id: streetField; objectName: "editAddress" + addressRow.index + "Street"; width: parent.width; visible: !addressRow.modelData.locked; placeholderText: qsTr("Street")
                               text: addressRow.modelData.street; errorText: page.fieldError("addresses[" + addressRow.index + "]")
                               onTextChanged: if (addressRow.index < page.addresses.length && !page.addresses[addressRow.index].locked) page.addresses[addressRow.index].street = text
                               onTextEdited: page.dirty = true }
                    InkField { onFieldFocus: function (item, gained) { page.fieldFocus(item, gained) } objectName: "editAddress" + addressRow.index + "Postcode"; width: parent.width; visible: !addressRow.modelData.locked; placeholderText: qsTr("Postcode")
                               text: addressRow.modelData.postcode
                               onTextChanged: if (addressRow.index < page.addresses.length && !page.addresses[addressRow.index].locked) page.addresses[addressRow.index].postcode = text
                               onTextEdited: page.dirty = true }
                    InkField { onFieldFocus: function (item, gained) { page.fieldFocus(item, gained) } objectName: "editAddress" + addressRow.index + "City"; width: parent.width; visible: !addressRow.modelData.locked; placeholderText: qsTr("City")
                               text: addressRow.modelData.city
                               onTextChanged: if (addressRow.index < page.addresses.length && !page.addresses[addressRow.index].locked) page.addresses[addressRow.index].city = text
                               onTextEdited: page.dirty = true }
                    InkField { onFieldFocus: function (item, gained) { page.fieldFocus(item, gained) } objectName: "editAddress" + addressRow.index + "Region"; width: parent.width; visible: !addressRow.modelData.locked; placeholderText: qsTr("Region")
                               text: addressRow.modelData.region
                               onTextChanged: if (addressRow.index < page.addresses.length && !page.addresses[addressRow.index].locked) page.addresses[addressRow.index].region = text
                               onTextEdited: page.dirty = true }
                    InkField { onFieldFocus: function (item, gained) { page.fieldFocus(item, gained) } objectName: "editAddress" + addressRow.index + "Country"; width: parent.width; visible: !addressRow.modelData.locked; placeholderText: qsTr("Country")
                               text: addressRow.modelData.country
                               onTextChanged: if (addressRow.index < page.addresses.length && !page.addresses[addressRow.index].locked) page.addresses[addressRow.index].country = text
                               onTextEdited: page.dirty = true }
                }
            }
            InkRow { objectName: "editAddAddress"; x: -30; width: parent.width + 60; visible: contact.editing && page.addresses.length < 20; text: qsTr("Add address"); chevron: false; onClicked: page.addAddress() }

            // ---- the note ----
            Text { text: qsTr("Note"); font.pixelSize: Style.labelSize; font.bold: true; color: Style.ink; visible: contact.editing }
            TextArea {
                id: noteArea
                objectName: "editNote"
                width: parent.width
                height: Math.max(200, contentHeight + 40)
                visible: contact.editing && !page.noteLocked
                font.pixelSize: Style.bodySize
                color: Style.ink
                wrapMode: TextEdit.Wrap
                inputMethodHints: Qt.ImhNoPredictiveText
                background: Rectangle { color: Style.paper; radius: Style.radiusField; border.width: Style.ruleWidth; border.color: Style.ink }
                TextMenu { target: noteArea; multiline: true }
                onTextChanged: if (activeFocus) page.dirty = true
                onActiveFocusChanged: page.fieldFocus(noteArea, activeFocus)
                onCursorRectangleChanged: Qt.callLater(page.followFocus)   // (a line added at the end of a long note)
            }
            Item {
                visible: page.noteLocked
                width: parent.width
                height: Style.touchMin
                Text {
                    objectName: "editNoteLocked"
                    width: parent.width - Style.touchMin - 10
                    height: Style.touchMin
                    verticalAlignment: Text.AlignVCenter
                    wrapMode: Text.Wrap
                    font.pixelSize: Style.captionSize
                    font.italic: true
                    color: Style.ink
                    text: qsTr("The note is too long to edit here, kept unchanged.")
                }
                IconButton { objectName: "editNoteRemove"; text: qsTr("Remove"); icon: "trash"; x: parent.width - Style.touchMin; onClicked: { page.noteLocked = false; page.dirty = true } }
            }
            Text {
                objectName: "editNoteError"
                visible: text.length > 0
                width: parent.width
                wrapMode: Text.Wrap
                font.pixelSize: Style.captionSize
                font.bold: true
                color: Style.ink
                text: page.fieldError("note")
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
            target: scroller
            snap: 0
            showIndicator: true
        }
    }

    ChoiceDialog {
        id: labelChoice
        objectName: "editLabelChoice"
        title: qsTr("Label")
        property string target: "phones"
        property int index: 0
        function openFor(anchorItem, labels, current) {
            var c = []
            for (var i = 0; i < labels.length; ++i) c.push({ label: page.labelText(labels[i]), value: labels[i] })
            labelChoice.choices = c
            labelChoice.current = current
            labelChoice.anchor = anchorItem
            labelChoice.open()
        }
        onChosen: function (value) {
            var list = labelChoice.target === "phones" ? page.phones : (labelChoice.target === "emails" ? page.emails : page.addresses)
            if (labelChoice.index < list.length) list[labelChoice.index].label = value
            // the row shows the new label: rebuild the list the label belongs to
            if (labelChoice.target === "phones") page.phones = page.copyList(page.phones)
            else if (labelChoice.target === "emails") page.emails = page.copyList(page.emails)
            else page.addresses = page.copyList(page.addresses)
            page.dirty = true
        }
    }
    ChoiceDialog {
        id: bookChoice
        objectName: "editBookChoice"
        title: qsTr("Address book")
        onChosen: function (value) {
            page.bookId = Number(value)
            page.bookName = page.bookNameOf(page.bookId)
            page.dirty = true
        }
    }
    ConfirmDialog {
        id: leaveDialog
        objectName: "editLeaveDialog"
        title: qsTr("Leave without saving?")
        text: qsTr("Your changes to this contact are lost.")
        acceptText: qsTr("Leave")
        cancelText: qsTr("Keep editing")
        onAccepted: page.leave()
    }
}
