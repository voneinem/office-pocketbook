import QtQuick
import QtQuick.Controls
import ".."

// Messages that are on their way: waiting for the next sync, being sent, sent but not yet filed in Sent, failed (with the reason) or of unknown
// outcome ("may have been sent"). A message of unknown outcome is NEVER sent again by itself: only the user's choice, after a confirmation,
// sends it again. Paged, never scrolled. A tap on a row offers what can be done with it.
Item {
    id: page

    property int pageIndex: 0
    readonly property int rowHeight: 150
    readonly property int rowsPerPage: Math.max(1, Math.floor((height - topBar.height - status.height - bottomBar.height) / rowHeight))
    readonly property int pageCount: Math.max(1, Math.ceil(outbox.rows.length / rowsPerPage))
    readonly property var pageRows: outbox.rows.slice(Math.min(pageIndex, pageCount - 1) * rowsPerPage, (Math.min(pageIndex, pageCount - 1) + 1) * rowsPerPage)
    property var chosen: null              // the row the action bubble belongs to
    property string confirmAction: ""
    property bool connecting: false
    property bool noNetwork: false

    function reasonText(key) {
        switch (key) {
        case "refused_recipient": return qsTr("a recipient address was refused")
        case "auth_failed": return qsTr("the user name or password was refused")
        case "cert_expired":
        case "cert_untrusted":
        case "cert_host_mismatch": return qsTr("the server's certificate is not valid")
        case "server_unreachable": return qsTr("the server cannot be reached")
        case "no_network": return qsTr("there is no network connection")
        case "timeout": return qsTr("the server did not answer in time")
        case "connection_lost": return qsTr("the connection was lost")
        case "too_large": return qsTr("the message is too large for the server")
        case "server_busy": return qsTr("the server is busy")
        case "rejected_by_server": return qsTr("the server rejected the message")
        case "interrupted": return qsTr("the app was closed while sending")
        case "sent_folder_missing": return qsTr("no Sent folder is known yet")
        case "": return ""
        default: return key
        }
    }
    function stateText(row) {
        switch (row.stateKey) {
        case "waiting": return row.reasonKey.length > 0 ? qsTr("Waiting for a connection (%1)").arg(page.reasonText(row.reasonKey)) : qsTr("Waiting to be sent")
        case "sending": return qsTr("Sending …")
        case "copy": return row.reasonKey.length > 0 ? qsTr("Sent. The copy in Sent is not filed yet (%1)").arg(page.reasonText(row.reasonKey)) : qsTr("Sent. The copy in Sent is being filed")
        case "failed": return qsTr("Not sent: %1").arg(page.reasonText(row.reasonKey))
        case "uncertain": return qsTr("May have been sent (%1)").arg(page.reasonText(row.reasonKey))
        default: return row.stateKey
        }
    }
    function choicesFor(row) {
        var c = []
        if (row.canRetry) c.push({ label: qsTr("Try again"), value: "retry" })
        if (row.canEdit) c.push({ label: qsTr("Edit"), value: "edit" })
        if (row.canSendAgain) c.push({ label: qsTr("Send again"), value: "send_again" })
        if (row.canMarkSent) c.push({ label: qsTr("It was sent"), value: "mark_sent" })
        if (row.canDelete) c.push({ label: qsTr("Delete"), value: "remove" })
        return c
    }
    function startSync() {
        page.noNetwork = false
        if (device.wifiConnected()) sync.syncNow()
        else { page.connecting = true; device.connectNetwork() }
    }
    function perform(action) {
        if (!page.chosen) return
        if (action === "retry") { outbox.retry(page.chosen.id); page.startSync() }
        else if (action === "edit") {
            var draft = outbox.edit(page.chosen.id)
            if (draft > 0 && page.StackView.view) page.StackView.view.replace(Qt.resolvedUrl("ComposePage.qml"), { draftId: draft })
        } else {
            page.confirmAction = action
            confirm.open()
        }
    }

    Component.onCompleted: outbox.refresh()
    Connections {
        target: device
        function onNetworkResult(connected) {
            if (!page.connecting) return
            page.connecting = false
            if (!connected) page.noNetwork = true
            else sync.syncNow()
        }
    }
    Connections { target: sync; function onSyncFinished(outcome) { outbox.refresh() } }

    TopBar {
        id: topBar
        title: qsTr("Outbox")
        leftItems: [ IconButton { objectName: "outboxBack"; text: qsTr("Back"); icon: "chevron-left"; onClicked: if (page.StackView.view) page.StackView.view.pop() } ]
        rightItems: [ IconButton { objectName: "outboxSync"; text: qsTr("Sync"); icon: "sync"; enabled: !sync.syncing && !page.connecting; onClicked: page.startSync() } ]
    }
    SyncStatusLine {
        id: status
        anchors { top: topBar.bottom; left: parent.left; right: parent.right }
        connecting: page.connecting
        noNetwork: page.noNetwork
    }

    Column {
        id: list
        objectName: "outboxRows"
        anchors { top: status.bottom; left: parent.left; right: parent.right }
        visible: !outbox.empty
        Repeater {
            model: page.pageRows
            delegate: Item {
                id: row
                required property int index
                required property var modelData
                width: list.width
                height: page.rowHeight
                Rectangle { anchors.fill: parent; color: Style.pressed; visible: tap.pressed }
                Text {
                    objectName: "outboxSubject" + row.index
                    x: 30
                    y: 16
                    width: parent.width - 60
                    elide: Text.ElideRight
                    font.pixelSize: Style.bodySize
                    font.bold: row.modelData.state === "failed" || row.modelData.state === "uncertain"
                    color: Style.ink
                    textFormat: Text.PlainText
                    text: row.modelData.subject.length > 0 ? row.modelData.subject : qsTr("(no subject)")
                }
                Text {
                    x: 30
                    y: 16 + Style.bodySize + 10
                    width: parent.width - 60
                    elide: Text.ElideRight
                    font.pixelSize: Style.captionSize
                    color: Style.ink
                    textFormat: Text.PlainText
                    text: qsTr("To: %1").arg(row.modelData.recipients)
                }
                Text {
                    objectName: "outboxState" + row.index
                    x: 30
                    y: 16 + Style.bodySize + Style.captionSize + 20
                    width: parent.width - 60
                    elide: Text.ElideRight
                    font.pixelSize: Style.captionSize
                    font.italic: true
                    color: Style.ink
                    text: page.stateText(row.modelData)
                }
                RowDivider { anchors.bottom: parent.bottom; x: 30; width: parent.width - 60 }
                MouseArea {
                    id: tap
                    objectName: "outboxTap" + row.index
                    anchors.fill: parent
                    enabled: page.choicesFor(row.modelData).length > 0
                    onClicked: { page.chosen = row.modelData; actions.choices = page.choicesFor(row.modelData); actions.anchor = row; actions.open() }
                }
            }
        }
    }
    Text {
        objectName: "outboxEmpty"
        visible: outbox.empty
        anchors { top: status.bottom; topMargin: 80; left: parent.left; right: parent.right; leftMargin: 30; rightMargin: 30 }
        wrapMode: Text.Wrap
        horizontalAlignment: Text.AlignHCenter
        font.pixelSize: Style.bodySize
        color: Style.ink
        text: qsTr("Nothing is waiting to be sent.")
    }

    Item {
        id: bottomBar
        anchors { bottom: parent.bottom; left: parent.left; right: parent.right }
        height: Style.bottomBarHeight
        Rectangle { y: 0; width: parent.width; height: Style.hairlineWidth; color: Style.hairline }
        ModelPager {
            anchors.centerIn: parent
            page: Math.min(page.pageIndex, page.pageCount - 1)
            pageCount: page.pageCount
            onPrevious: page.pageIndex = Math.max(0, page.pageIndex - 1)
            onNext: page.pageIndex = Math.min(page.pageCount - 1, page.pageIndex + 1)
        }
    }

    ChoiceDialog {
        id: actions
        objectName: "outboxActions"
        title: qsTr("What do you want to do?")
        onChosen: function (value) { page.perform(value) }
    }
    ConfirmDialog {
        id: confirm
        objectName: "outboxConfirm"
        title: page.confirmAction === "send_again" ? qsTr("Send again?") : (page.confirmAction === "mark_sent" ? qsTr("It was sent?") : qsTr("Delete this message?"))
        text: page.confirmAction === "send_again" ? qsTr("The message may already have been delivered. If you send it again, the recipients may get it twice.")
              : (page.confirmAction === "mark_sent" ? qsTr("The app will stop treating the message as unsent. It will not be sent again.")
                                                    : qsTr("The message and its text are deleted. It will not be sent."))
        acceptText: page.confirmAction === "send_again" ? qsTr("Send again") : (page.confirmAction === "mark_sent" ? qsTr("It was sent") : qsTr("Delete"))
        cancelText: qsTr("Cancel")
        onAccepted: {
            if (!page.chosen) return
            if (page.confirmAction === "send_again") { outbox.sendAgain(page.chosen.id); page.startSync() }
            else if (page.confirmAction === "mark_sent") outbox.markSent(page.chosen.id)
            else outbox.remove(page.chosen.id)
            sync.refreshState()
        }
    }
}
