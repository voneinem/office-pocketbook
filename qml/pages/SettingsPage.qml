import QtQuick
import QtQuick.Controls
import ".."

// The options of the app and the way to the accounts, the time zone and About. Changes are saved at once; a value the app does not accept says so.
// The window of mail (days) is used by the next sync; mail outside it stays on the reader until the space is needed.
Item {
    id: page

    function push(name) { if (page.StackView.view) page.StackView.view.push(Qt.resolvedUrl(name)) }
    function mibText(v) { return v >= 1000 ? qsTr("%1 GB").arg(v / 1000) : qsTr("%1 MB").arg(v) }
    function openChoice(kind) {
        var c = [], cur = "", list = []
        if (kind === "days") { list = settings.syncDaysChoices; cur = String(settings.syncDays); for (var i = 0; i < list.length; ++i) c.push({ label: qsTr("%n day(s)", "", list[i]), value: String(list[i]) }) }
        else if (kind === "attachment") { list = settings.attachmentLimitChoices; cur = String(settings.attachmentLimitMib); for (var j = 0; j < list.length; ++j) c.push({ label: page.mibText(list[j]), value: String(list[j]) }) }
        else if (kind === "language") { list = settings.languageChoices; cur = settings.language; for (var m = 0; m < list.length; ++m) c.push({ label: list[m].label, value: list[m].value }) }
        else { list = settings.storageLimitChoices; cur = String(settings.storageLimitMib); for (var k = 0; k < list.length; ++k) c.push({ label: page.mibText(list[k]), value: String(list[k]) }) }
        choice.kind = kind
        choice.choices = c
        choice.current = cur
        choice.open()
    }
    function missingFolder(f) {
        for (var i = 0; i < f.length; ++i) if (f[i].name.length === 0) return true
        return false
    }
    function openFolderChoice(role) {
        var c = [ { label: qsTr("Automatic"), value: "0" } ], all = settings.folderChoices()
        for (var i = 0; i < all.length; ++i) c.push({ label: all[i].name, value: String(all[i].id) })
        choice.kind = "folder:" + role
        choice.choices = c
        choice.current = "0"
        choice.open()
    }
    function chosen(value) {
        if (choice.kind.indexOf("folder:") === 0) { settings.setSpecialFolder(choice.kind.substring(7), Number(value)); return }
        if (choice.kind === "language") { settings.setLanguage(value); return }
        var n = Number(value)
        if (choice.kind === "days") settings.setSyncDays(n)
        else if (choice.kind === "attachment") settings.setAttachmentLimitMib(n)
        else settings.setStorageLimitMib(n)
    }
    function errorText() {
        switch (settings.errorKey) {
        case "": return ""
        case "sync_days": return qsTr("The number of days must be between 1 and 365.")
        case "attachment_limit": return qsTr("The attachment limit must be between 1 and 100 MB.")
        case "storage_limit": return qsTr("The storage limit must be at least 20 MB.")
        case "no_account": return qsTr("Set up the mail account first.")
        case "bad_folder": return qsTr("That folder does not exist on this account.")
        default: return qsTr("The setting could not be saved.")
        }
    }

    Component.onCompleted: settings.refresh()

    TopBar {
        id: topBar
        title: qsTr("Settings")
        leftItems: [ IconButton { objectName: "settingsBack"; text: qsTr("Back"); icon: "chevron-left"; onClicked: if (page.StackView.view) page.StackView.view.pop() } ]
    }

    // The page is taller than the screen and dragging is off (kinetic scrolling smears on e-ink), so the rows below the
    // first screen are reached with the page buttons of the bar below, as on the contact and licence pages.
    Flickable {
        id: view
        objectName: "settingsView"
        anchors { left: parent.left; right: parent.right; top: topBar.bottom; bottom: bottomBar.top }
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
            component SwitchRow: Item {
                id: sr
                property string label
                property string note
                property alias checked: sw.checked
                property alias switchName: sw.objectName
                signal toggled(bool on)
                width: parent.width
                height: Math.max(110, labelText.implicitHeight + noteText.implicitHeight + 40)
                Text {
                    id: labelText
                    x: 30
                    y: 20
                    width: parent.width - 60 - sw.width - 20
                    wrapMode: Text.Wrap
                    font.pixelSize: Style.bodySize
                    color: Style.ink
                    text: sr.label
                }
                Text {
                    id: noteText
                    x: 30
                    anchors.top: labelText.bottom
                    width: parent.width - 60 - sw.width - 20
                    wrapMode: Text.Wrap
                    visible: text.length > 0
                    font.pixelSize: Style.captionSize
                    color: Style.ink
                    text: sr.note
                }
                InkSwitch {
                    id: sw
                    anchors { right: parent.right; rightMargin: 30; verticalCenter: parent.verticalCenter }
                    onToggled: function (on) { sr.toggled(on) }
                }
                RowDivider { anchors.bottom: parent.bottom; x: 30; width: parent.width - 60 }
            }

            Head { text: qsTr("Language") }
            InkRow {
                objectName: "settingsLanguage"
                text: qsTr("Language of the app")
                value: settings.language === "de" ? "Deutsch" : (settings.language === "en" ? "English" : qsTr("Automatic"))
                chevron: true
                onClicked: page.openChoice("language")
            }
            Head { text: qsTr("Accounts") }
            InkRow { objectName: "settingsMailAccount"; text: qsTr("Mail account"); chevron: true; onClicked: page.push("AccountSetupPage.qml") }
            InkRow { objectName: "settingsCalendarAccount"; text: qsTr("Calendar account"); chevron: true; onClicked: page.push("CalendarAccountPage.qml") }
            InkRow { objectName: "settingsTimeZone"; text: qsTr("Time zone"); value: timezone.zoneName.length > 0 ? timezone.zoneName : qsTr("not chosen"); chevron: true; onClicked: page.push("TimeZonePage.qml") }

            Head { text: qsTr("Syncing") }
            SwitchRow {
                switchName: "settingsAutoSync"
                label: qsTr("Sync after my changes")
                note: qsTr("A few seconds after you read, star, move, delete, write or change an event, the reader connects to Wi-Fi and syncs. Off: changes wait for the Sync button.")
                checked: settings.autoSync
                onToggled: function (on) { settings.autoSync = on }
            }
            InkRow { objectName: "settingsDays"; text: qsTr("Mail kept on the reader"); value: qsTr("%n day(s)", "", settings.syncDays); chevron: true; enabled: settings.hasMailAccount; onClicked: page.openChoice("days") }
            Text {
                objectName: "settingsDaysNote"
                x: 30
                width: parent.width - 60
                visible: settings.hasMailAccount
                wrapMode: Text.Wrap
                font.pixelSize: Style.captionSize
                color: Style.ink
                text: qsTr("Fewer days: the next sync removes the older mail from this reader (it stays on the server). Mail with a change that has not synced, and drafts, are never removed.")
            }
            InkRow { objectName: "settingsAttachment"; text: qsTr("Largest attachment"); value: page.mibText(settings.attachmentLimitMib); chevron: true; enabled: settings.hasMailAccount; onClicked: page.openChoice("attachment") }
            InkRow { objectName: "settingsStorage"; text: qsTr("Space for mail"); value: page.mibText(settings.storageLimitMib); chevron: true; enabled: settings.hasMailAccount; onClicked: page.openChoice("storage") }
            SwitchRow {
                switchName: "settingsSentByServer"
                label: qsTr("The server keeps sent mail itself")
                note: qsTr("On for Gmail and Outlook.com: they file every sent message in Sent. Then the app does not upload a second copy.")
                checked: settings.sentByServer
                enabled: settings.hasMailAccount
                onToggled: function (on) { settings.sentByServer = on }
            }
            Head { objectName: "settingsBooksHead"; text: qsTr("Address books"); visible: settings.addressBooks.length > 0 }
            Repeater {
                model: settings.addressBooks
                delegate: SwitchRow {
                    required property int index
                    required property var modelData
                    switchName: "settingsBook" + index
                    label: modelData.name
                    note: modelData.writable ? "" : qsTr("Read-only")
                    checked: modelData.shown
                    onToggled: function (on) { settings.setAddressBookShown(modelData.id, on) }
                }
            }
            Text {
                x: 30
                width: parent.width - 60
                visible: settings.addressBooks.length > 0
                wrapMode: Text.Wrap
                font.pixelSize: Style.captionSize
                color: Style.ink
                text: qsTr("A hidden address book is not shown or downloaded. Nothing is deleted: its contacts and any change waiting for the server stay.")
            }
            Head { text: qsTr("Folders for sent mail, drafts and deleted mail"); visible: settings.hasMailAccount }
            Repeater {
                model: settings.hasMailAccount ? settings.specialFolderList : []
                delegate: InkRow {
                    required property int index
                    required property var modelData
                    objectName: "settingsFolder" + index
                    text: modelData.role === "sent" ? qsTr("Sent mail") : (modelData.role === "drafts" ? qsTr("Drafts") : qsTr("Deleted mail (Trash)"))
                    value: modelData.name.length > 0 ? modelData.name : qsTr("not found")
                    chevron: true
                    onClicked: page.openFolderChoice(modelData.role)
                }
            }
            Text {
                objectName: "settingsFolderHint"
                x: 30
                width: parent.width - 60
                visible: settings.hasMailAccount && page.missingFolder(settings.specialFolderList)
                wrapMode: Text.Wrap
                font.pixelSize: Style.captionSize
                color: Style.ink
                text: qsTr("A folder that is not found: choose one, or sync once so that the folders are known. Without a Trash folder deleting is permanent.")
            }
            Text {
                objectName: "settingsError"
                x: 30
                width: parent.width - 60
                visible: text.length > 0
                wrapMode: Text.Wrap
                topPadding: 12
                font.pixelSize: Style.bodySize
                font.bold: true
                color: Style.ink
                text: page.errorText()
            }

            Text {
                objectName: "settingsStorageFull"
                x: 30
                width: parent.width - 60
                visible: sync.storageFull
                wrapMode: Text.Wrap
                topPadding: 12
                font.pixelSize: Style.bodySize
                font.bold: true
                color: Style.ink
                text: qsTr("The mail on this reader is more than the space allows, and nothing older can be removed. Raise the space for mail, or shorten the days kept and sync.")
            }
            Text {
                objectName: "settingsClock"
                x: 30
                width: parent.width - 60
                visible: sync.clockWarning
                wrapMode: Text.Wrap
                topPadding: 12
                font.pixelSize: Style.bodySize
                font.bold: true
                color: Style.ink
                text: sync.clockAhead ? qsTr("The clock of this reader is about %n minute(s) ahead of the server's. Set the date and time in the reader's settings; events and times depend on it.", "", sync.clockSkewMinutes)
                                      : qsTr("The clock of this reader is about %n minute(s) behind the server's. Set the date and time in the reader's settings; events and times depend on it.", "", sync.clockSkewMinutes)
            }

            Head { text: qsTr("Security") }
            Text {
                objectName: "settingsCustomCa"
                x: 30
                width: parent.width - 60
                wrapMode: Text.Wrap
                font.pixelSize: Style.captionSize
                color: Style.ink
                text: settings.customCaStatus === "loaded" ? qsTr("Your own certificate authority is loaded (%n certificate(s)). Every connection is still checked; a certificate that cannot be verified is refused.", "", settings.customCaCount)
                    : settings.customCaStatus === "invalid" ? qsTr("The file custom-ca.pem could not be used and is ignored. Every connection is checked against the usual authorities; there is no way to skip the check.")
                    : qsTr("Every connection is checked against the usual authorities. For a server with your own authority, put its certificate as custom-ca.pem in the app's data folder (see the README).")
            }
            Head { text: qsTr("About this app") }
            InkRow { objectName: "settingsAbout"; text: qsTr("About, licenses"); chevron: true; onClicked: page.push("AboutPage.qml") }
        }
    }

    Item {
        id: bottomBar
        anchors { bottom: parent.bottom; left: parent.left; right: parent.right }
        height: Style.bottomBarHeight
        Rectangle { y: 0; width: parent.width; height: Style.hairlineWidth; color: Style.hairline }
        PageButtons {
            id: pager
            anchors.centerIn: parent
            target: view
            showIndicator: true
        }
    }

    ChoiceDialog {
        id: choice
        objectName: "settingsChoice"
        property string kind: "days"
        title: qsTr("Choose a value")
        onChosen: function (value) { page.chosen(value) }
    }
}
