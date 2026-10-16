import QtQuick
import QtQuick.Controls
import ".."

// Calendar account: a CalDAV server (address, user name, password) and/or a read-only feed (https or webcal address). Test connects without
// saving; Save keeps the account and the password. After the first sync the calendars the server offers are listed with a switch each.
Item {
    id: page

    property string statusKey: ""
    property string failedSide: ""
    property string errorField: ""
    property bool connecting: false
    property var pendingForm: null
    property var found: []

    function formMap() {
        return { caldavUrl: caldavUrl.text.trim(), username: username.text, password: password.text, feedUrl: feedUrl.text.trim(), feedName: feedName.text.trim() }
    }
    function fieldError(name, text) { return page.errorField === name ? text : "" }
    function causeText(key) {
        switch (key) {
        case "connecting": return qsTr("Connecting to Wi-Fi …")
        case "ok": return qsTr("The connection works. Tap Save to keep the account.")
        case "saved": return qsTr("Saved.")
        case "auth_failed": return qsTr("The user name or password was refused.")
        case "cert_expired": return qsTr("The server's certificate has expired. Nothing was sent.")
        case "cert_untrusted": return qsTr("The server's certificate is not trusted. Nothing was sent.")
        case "cert_host_mismatch": return qsTr("The certificate is for another server name. Nothing was sent.")
        case "tls_failed": return qsTr("A secure connection could not be set up. Nothing was sent.")
        case "server_unreachable": return qsTr("The server cannot be reached.")
        case "no_network": return qsTr("There is no network connection. Turn on Wi-Fi and try again.")
        case "timeout": return qsTr("The server did not answer in time.")
        case "forbidden": return qsTr("The server refused the request.")
        case "not_found": return qsTr("The server does not know this address.")
        case "protocol_error": return qsTr("The server does not look like a calendar server.")
        case "malformed_xml": return qsTr("The server's answer could not be read.")
        case "server_error": return qsTr("The server reported a problem. Try again later.")
        case "feed_not_found": return qsTr("The feed address does not exist.")
        case "feed_refused": return qsTr("The feed host refused the request. Check the address.")
        case "not_a_calendar": return qsTr("The feed address does not deliver a calendar.")
        case "too_large": return qsTr("The answer is larger than this app accepts.")
        case "invalid_input": return qsTr("Please check the marked field.")
        case "": return ""
        default: return qsTr("The test failed (%1).").arg(key)
        }
    }
    function startTest() {
        var form = page.formMap()
        page.failedSide = ""
        page.found = []
        if (device.wifiConnected()) {
            account.testCalendar(form)
        } else {
            page.pendingForm = form
            page.connecting = true
            page.statusKey = "connecting"
            device.connectNetwork()
        }
    }

    Connections {
        target: device
        function onNetworkResult(connected) {
            if (!page.connecting) return
            page.connecting = false
            if (connected) account.testCalendar(page.pendingForm)
            else { page.statusKey = "no_network"; page.errorField = "" }
        }
    }
    Connections {
        target: account
        function onTestResult(area, key, detail) {
            if (area !== "calendar") return
            page.statusKey = key
            page.failedSide = key === "invalid_input" || key === "ok" ? "" : detail
            page.errorField = key === "invalid_input" ? detail.split(":")[0] : ""
        }
        function onCalendarsFound(list) { page.found = list }
        function onSaveResult(ok, field, key) {
            if (!page.visible) return
            if (ok) { page.errorField = ""; page.statusKey = "saved"; if (page.StackView.view) page.StackView.view.pop() }
            else { page.statusKey = "invalid_input"; page.errorField = field }
        }
    }

    Component.onCompleted: {
        var f = account.calendarForm()
        if (f.caldavUrl !== undefined) {
            caldavUrl.text = f.caldavUrl
            username.text = f.username
            feedUrl.text = f.feedUrl
            feedName.text = f.feedName
        }
    }

    TopBar {
        id: topBar
        title: qsTr("Calendar account")
        leftItems: [ IconButton { objectName: "calendarAccountBack"; text: qsTr("Back"); icon: "chevron-left"; onClicked: if (page.StackView.view) page.StackView.view.pop() } ]
    }

    Flickable {
        anchors { left: parent.left; right: parent.right; top: topBar.bottom; bottom: statusArea.top }
        contentWidth: width
        contentHeight: form.implicitHeight + 40
        boundsBehavior: Flickable.StopAtBounds
        clip: true

        Column {
            id: form
            x: 30
            y: 20
            width: parent.width - 60
            spacing: 22

            InkField { id: caldavUrl; objectName: "caldavUrl"; width: parent.width; label: qsTr("Calendar server (CalDAV address)"); placeholderText: "https://cloud.example.org"
                       field.inputMethodHints: Qt.ImhUrlCharactersOnly | Qt.ImhNoPredictiveText
                       errorText: page.fieldError("caldav_url", qsTr("Use an https address of your calendar server.")) }
            InkField { id: username; objectName: "calendarUsername"; width: parent.width; label: qsTr("User name")
                       errorText: page.fieldError("username", qsTr("Enter your user name.")) }
            InkField { id: password; objectName: "calendarPassword"; width: parent.width; label: qsTr("Password"); field.echoMode: TextInput.Password
                       errorText: page.fieldError("password", qsTr("Enter your password.")) }
            InkField { id: feedUrl; objectName: "feedUrl"; width: parent.width; label: qsTr("Calendar feed (optional, read only)"); placeholderText: "https://example.org/holidays.ics"
                       field.inputMethodHints: Qt.ImhUrlCharactersOnly | Qt.ImhNoPredictiveText
                       errorText: page.fieldError("feed_url", qsTr("Use an https or webcal address.")) }
            InkField { id: feedName; objectName: "feedName"; width: parent.width; label: qsTr("Name of the feed (optional)") }

            Text {
                visible: page.found.length > 0
                width: parent.width
                font.pixelSize: Style.bodySize
                font.bold: true
                color: Style.ink
                text: qsTr("Calendars on this server")
            }
            Repeater {
                model: page.found
                delegate: Text {
                    required property int index
                    required property var modelData
                    objectName: "foundCalendar" + index
                    width: form.width
                    elide: Text.ElideRight
                    font.pixelSize: Style.labelSize
                    color: Style.ink
                    text: modelData.name + (modelData.writable ? "" : " (" + qsTr("read only") + ")")
                }
            }

            Text {
                visible: account.calendars.length > 0
                width: parent.width
                font.pixelSize: Style.bodySize
                font.bold: true
                color: Style.ink
                text: qsTr("Show these calendars")
            }
            Repeater {
                model: account.calendars
                delegate: Item {
                    id: calRow
                    required property int index
                    required property var modelData
                    width: form.width
                    height: 96
                    Text {
                        anchors { left: parent.left; right: sw.left; rightMargin: 20; verticalCenter: parent.verticalCenter }
                        elide: Text.ElideRight
                        font.pixelSize: Style.labelSize
                        color: Style.ink
                        text: calRow.modelData.name + (calRow.modelData.writable ? "" : " (" + qsTr("read only") + ")") + (calRow.modelData.present ? "" : " (" + qsTr("not on the server") + ")")
                    }
                    InkSwitch {
                        id: sw
                        objectName: "calendarShown" + calRow.index
                        anchors { right: parent.right; verticalCenter: parent.verticalCenter }
                        checked: calRow.modelData.shown
                        onToggled: function (on) { account.setCalendarShown(calRow.modelData.id, on) }
                    }
                }
            }
        }
    }

    Column {
        id: statusArea
        anchors { left: parent.left; right: parent.right; bottom: tools.top }
        spacing: 8
        padding: 16
        Text {
            objectName: "calendarStatus"
            width: parent.width - 32
            wrapMode: Text.Wrap
            font.pixelSize: Style.bodySize
            font.bold: true
            color: Style.ink
            text: (page.failedSide === "feed" ? qsTr("Feed: ") : (page.failedSide === "caldav" ? qsTr("Calendar server: ") : "")) + page.causeText(page.statusKey)
            visible: text.length > 0
        }
        Text {
            width: parent.width - 32
            wrapMode: Text.Wrap
            font.pixelSize: Style.captionSize
            color: Style.ink
            text: qsTr("Your password is stored on this reader like the mail password. Use an app-specific password if your provider offers one.")
        }
    }

    BottomPanel {
        id: tools
        anchors.bottom: parent.bottom
        tools: [
            ToolButton { objectName: "calendarTestButton"; text: qsTr("Test"); icon: "check"; enabled: !account.busy && !page.connecting; onClicked: page.startTest() },
            ToolButton { objectName: "calendarSaveButton"; text: qsTr("Save"); icon: "save"; onClicked: account.saveCalendar(page.formMap()) }
        ]
    }
}
