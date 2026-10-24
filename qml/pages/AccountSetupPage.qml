import QtQuick
import QtQuick.Controls
import ".."

// Mail account setup (the walking skeleton's minimal tab): server, port, encryption, user name, password, address.
// Test connects and logs in without saving; Save keeps the account and the password. Nothing is sent anywhere but to
// the server entered here, and only over a verified encrypted connection.
Item {
    id: page
    objectName: "accountSetupPage"

    // "" = nothing tested yet; "ok" or a cause key (auth_failed, cert_expired, ...) after a test; "invalid_input"
    // when a field was rejected (errorField then names it); "saved" after a successful save.
    property string statusKey: ""
    property string errorField: ""
    property bool saved: false
    property string security: "tls"      // "tls" | "starttls"

    function effectiveSmtpHost() {
        var smtp = smtpHost.text.trim()
        if (smtp.length > 0) return smtp
        var imap = imapHost.text.trim()
        return imap.indexOf("imap.") === 0 ? "smtp." + imap.substring(5) : imap
    }

    function formMap() {
        var imapPortValue = parseInt(imapPort.text)
        var smtpPortValue = smtpPort.text.length > 0 ? parseInt(smtpPort.text) : 465
        return {
            imapHost: imapHost.text.trim(),
            imapPort: isNaN(imapPortValue) ? 0 : imapPortValue,
            imapSecurity: page.security,
            smtpHost: effectiveSmtpHost(),
            smtpPort: isNaN(smtpPortValue) ? 0 : smtpPortValue,
            smtpSecurity: "tls",
            username: username.text,
            password: password.text,
            address: address.text.trim(),
            displayName: displayName.text.trim()
        }
    }

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
        case "starttls_unavailable": return qsTr("The server does not offer an encrypted connection. Nothing was sent.")
        case "server_unreachable": return qsTr("The server cannot be reached.")
        case "no_network": return qsTr("There is no network connection. Turn on Wi-Fi and try again.")
        case "timeout": return qsTr("The server did not answer in time.")
        case "libcurl_too_old": return qsTr("This reader's network library is too old for mail.")
        case "invalid_input": return qsTr("Please check the marked field.")
        case "": return ""
        default: return qsTr("The test failed (%1).").arg(key)
        }
    }

    // The connection test switches Wi-Fi on first, like the sync.
    property bool connecting: false
    property string failedSide: ""   // "imap" or "smtp": which server the last test failure belongs to
    property var pendingForm: null
    function startTest() {
        var form = page.formMap()
        page.failedSide = ""
        if (device.wifiConnected()) {
            account.testMail(form)
        } else {
            page.pendingForm = form
            page.connecting = true
            page.statusKey = "connecting"
            device.connectNetwork()
        }
    }

    function fieldError(name, text) { return page.errorField === name ? text : "" }

    function protectionText(state) {
        switch (state) {
        case "enforced": return qsTr("Your password is kept in a file that only this app can read.")
        case "not_enforced": return qsTr("This reader's storage cannot keep the password file private: anyone who can read the storage can read your password. Use an app-specific password.")
        default: return qsTr("The password is stored as plain text in a file on this reader. Whether the storage keeps it private is checked when you save. Use an app-specific password.")
        }
    }

    Connections {
        target: device
        function onNetworkResult(connected) {
            if (!page.connecting) return
            page.connecting = false
            if (connected) account.testMail(page.pendingForm)
            else {
                page.statusKey = "no_network"
                page.errorField = ""
            }
        }
    }

    Connections {
        target: account
        function onTestResult(area, key, detail) {
            page.statusKey = key
            page.failedSide = key === "invalid_input" || key === "ok" ? "" : detail
            page.errorField = key === "invalid_input" ? detail.split(":")[0] : ""
        }
        function onSaveResult(ok, field, key) {
            if (ok) {
                page.errorField = ""
                page.statusKey = "saved"
                page.saved = true
                if (page.StackView.view) page.StackView.view.replace(Qt.resolvedUrl("MessageListPage.qml"), { autoSync: true })
            } else {
                page.statusKey = "invalid_input"
                page.errorField = field
            }
        }
    }

    // A text handed over by another app while no mail account exists waits here (in memory only) and becomes a draft once the account is saved; when this
    // page goes without an account the user is told that the text was not kept (spec 003 FR-008).
    Component.onDestruction: {
        if (typeof handoff === "undefined") return
        handoff.setupActive(false)
        handoff.setupLeft()
    }

    Component.onCompleted: {
        if (typeof handoff !== "undefined") handoff.setupActive(true)
        var f = account.mailForm()
        if (f.imapHost !== undefined) {
            imapHost.text = f.imapHost
            imapPort.text = String(f.imapPort)
            page.security = f.imapSecurity
            smtpHost.text = f.smtpHost
            smtpPort.text = String(f.smtpPort)
            username.text = f.username
            address.text = f.address
            displayName.text = f.displayName
        }
    }

    TopBar {
        id: topBar
        title: qsTr("Mail account")
        leftItems: [ IconButton { text: qsTr("Back"); icon: "chevron-left"; visible: page.StackView.view && page.StackView.view.depth > 1; onClicked: page.StackView.view.pop() } ]
    }

    Flickable {
        objectName: "accountScroller"
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

            InkField { id: imapHost; objectName: "imapHost"; width: parent.width; label: qsTr("Mail server (IMAP)"); placeholderText: "imap.example.org"
                       errorText: page.fieldError("imap_host", qsTr("This is not a valid server name.")) }
            InkField { id: imapPort; objectName: "imapPort"; width: parent.width; label: qsTr("Port"); text: "993"
                       field.inputMethodHints: Qt.ImhDigitsOnly
                       errorText: page.fieldError("imap_port", qsTr("Use a port from 1 to 65535.")) + page.fieldError("imap_security", qsTr("STARTTLS is only accepted on port 143.")) }
            InkRow {
                objectName: "securityRow"
                text: qsTr("Encryption")
                value: page.security === "tls" ? qsTr("Encrypted from the start (TLS)") : qsTr("Upgrade with STARTTLS")
                onClicked: {
                    page.security = page.security === "tls" ? "starttls" : "tls"
                    if (page.security === "starttls" && imapPort.text === "993") imapPort.text = "143"
                    else if (page.security === "tls" && imapPort.text === "143") imapPort.text = "993"
                }
            }
            InkField { id: username; objectName: "username"; width: parent.width; label: qsTr("User name")
                       errorText: page.fieldError("username", qsTr("Enter your user name.")) }
            InkField { id: password; objectName: "password"; width: parent.width; label: qsTr("Password"); field.echoMode: TextInput.Password
                       errorText: page.fieldError("imap_password", qsTr("Enter your password.")) }
            InkField { id: address; objectName: "address"; width: parent.width; label: qsTr("Your e-mail address"); placeholderText: "name@example.org"
                       field.inputMethodHints: Qt.ImhEmailCharactersOnly | Qt.ImhNoPredictiveText
                       errorText: page.fieldError("address", qsTr("This is not a valid address.")) }
            InkField { id: displayName; objectName: "displayName"; width: parent.width; label: qsTr("Your name (optional)") }
            InkField { id: smtpHost; objectName: "smtpHost"; width: parent.width; label: qsTr("Outgoing server (SMTP, optional)"); placeholderText: "smtp.example.org"
                       errorText: page.fieldError("smtp_host", qsTr("This is not a valid server name.")) }
            InkField { id: smtpPort; objectName: "smtpPort"; width: parent.width; label: qsTr("Outgoing port"); text: "465"
                       field.inputMethodHints: Qt.ImhDigitsOnly
                       errorText: page.fieldError("smtp_port", qsTr("Use a port from 1 to 65535.")) }
            InkRow {
                objectName: "calendarAccountRow"
                text: qsTr("Calendar account")
                value: account.calendarConfigured ? qsTr("set up") : qsTr("not set up")
                chevron: true
                onClicked: if (page.StackView.view) page.StackView.view.push(Qt.resolvedUrl("CalendarAccountPage.qml"))
            }
        }
    }

    Column {
        id: statusArea
        anchors { left: parent.left; right: parent.right; bottom: tools.top }
        spacing: 8
        padding: 16
        Text {
            objectName: "statusText"
            width: parent.width - 32
            wrapMode: Text.Wrap
            font.pixelSize: Style.bodySize
            font.bold: true
            color: Style.ink
            text: (page.failedSide === "smtp" ? qsTr("Outgoing server: ") : (page.failedSide === "imap" ? qsTr("Incoming server: ") : "")) + page.causeText(page.statusKey)
            visible: text.length > 0
        }
        Text {
            objectName: "protectionText"
            width: parent.width - 32
            wrapMode: Text.Wrap
            font.pixelSize: Style.captionSize
            color: Style.ink
            text: page.protectionText(account.protectionState)
        }
    }

    BottomPanel {
        id: tools
        anchors.bottom: parent.bottom
        tools: [
            ToolButton { objectName: "testButton"; text: qsTr("Test"); icon: "check"; enabled: !account.busy && !page.connecting; onClicked: page.startTest() },
            ToolButton { objectName: "saveButton"; text: qsTr("Save"); icon: "save"; onClicked: account.saveMail(page.formMap()) }
        ]
    }
}
