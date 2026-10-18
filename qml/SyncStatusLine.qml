import QtQuick

// The one-line sync state under the top bar (FR-033): what is happening now (connecting, syncing, a failure), else when the last
// sync was, with "Offline" while the reader has no network and the number of changes that wait for the next sync. The owner
// page says what it is doing itself through `connecting`, `noNetwork` and `note`.
Item {
    id: root
    property bool connecting: false
    property bool noNetwork: false
    property string note: ""          // a message of the owner page, shown instead of the sync state (for example a failed "older mail")
    readonly property alias text: line.text
    height: 56

    function causeText(key) {
        switch (key) {
        case "auth_failed": return qsTr("the user name or password was refused")
        case "cert_expired": return qsTr("the server's certificate has expired")
        case "cert_untrusted": return qsTr("the server's certificate is not trusted")
        case "cert_host_mismatch": return qsTr("the certificate is for another server name")
        case "server_unreachable": return qsTr("the server cannot be reached")
        case "no_network": return qsTr("there is no network connection")
        case "timeout": return qsTr("the server did not answer in time")
        case "uidvalidity_changed": return qsTr("the folder was reset on the server")
        case "protocol_error": return qsTr("the server's answer could not be understood")
        case "forbidden": return qsTr("the server refused the request")
        case "server_error": return qsTr("the server reported a problem")
        case "malformed_xml": return qsTr("the calendar server's answer could not be read")
        case "feed_not_found": return qsTr("the calendar feed address does not exist")
        case "feed_refused": return qsTr("the calendar feed host refused the request")
        case "not_a_calendar": return qsTr("the calendar feed does not deliver a calendar")
        case "too_large": return qsTr("the answer is larger than this app accepts")
        default: return key
        }
    }

    function tail() {
        var parts = []
        if (!sync.online) parts.push(qsTr("Offline"))
        if (sync.pendingCount > 0) parts.push(qsTr("%n change(s) waiting", "", sync.pendingCount))
        if (sync.clockWarning) parts.push(qsTr("Check the clock"))
        return parts.length > 0 ? "  ·  " + parts.join("  ·  ") : ""
    }

    // the connection state follows the device: asked now, then every 10 seconds, and after a connect attempt
    function poll() { sync.setOnline(device.wifiConnected()) }
    Component.onCompleted: poll()
    Timer { interval: 10000; repeat: true; running: root.visible; onTriggered: root.poll() }
    Connections { target: device; function onNetworkResult(connected) { root.poll() } }

    Text {
        id: line
        objectName: "statusLine"
        x: 30
        width: parent.width - 60
        anchors.verticalCenter: parent.verticalCenter
        elide: Text.ElideRight
        font.pixelSize: Style.captionSize
        color: Style.ink
        text: root.connecting ? qsTr("Connecting to Wi-Fi …")
              : root.noNetwork ? qsTr("No network connection. Turn on Wi-Fi and try again.")
              : sync.syncing ? qsTr("Syncing …")
              : root.note.length > 0 ? root.note + root.tail()
              : sync.problemKey.length > 0 ? (sync.problemArea === "contacts" ? qsTr("The last sync of the contacts failed: %1.") : qsTr("The last sync failed: %1.")).arg(root.causeText(sync.problemKey)) + root.tail()
              : (sync.lastSyncText.length > 0 ? qsTr("Synced %1").arg(sync.lastSyncText) : qsTr("Not synced yet")) + root.tail()
    }
    RowDivider { anchors.bottom: parent.bottom; x: 30; width: parent.width - 60 }
}
