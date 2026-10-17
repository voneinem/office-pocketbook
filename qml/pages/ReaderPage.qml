import QtQuick
import QtQuick.Controls
import ".."

// Reading one message, page by page (e-ink: no kinetic scrolling, no animation). Everything on the page is text in one
// line height, so a page turn moves by whole lines and the viewport shows whole lines only: no line is ever cut.
// Quoted text is marked by a bar and an indent. Links are shown as text and never opened.
Item {
    id: page

    property var messageId: 0
    readonly property int lineHeight: 44            // px; every text on this page has this line height
    readonly property int margin: 30
    readonly property int quoteIndent: 28

    function bodyNoticeText() {
        if (reader.bodyState === "headers") return qsTr("The text of this message has not been downloaded yet. Sync with Wi-Fi on.")
        if (reader.bodyState === "unreadable") return qsTr("This message cannot be shown as text.")
        if (reader.bodyState === "too_large") return qsTr("This message is too large to show.")
        if (reader.bodyNote === "no_readable_text") return qsTr("This message has no text, only attachments.")
        if (reader.bodyNote === "truncated") return qsTr("The message continues on the server; only the first part is shown.")
        if (reader.bodyNote === "charset_unsupported") return qsTr("The character set of this message is not known; some letters may be wrong.")
        return ""
    }

    // ---- attachments: tap to download; Wi-Fi is switched on first; the status shows under the row ----
    property var statuses: ({})          // attachment id -> "connecting" | "downloading" | an error key; "" = none
    property var connectingId: 0
    function setStatus(id, key) {
        var s = {}
        for (var k in statuses) s[k] = statuses[k]
        s[id] = key
        statuses = s
    }
    function failureText(key) {
        switch (key) {
        case "no_network": return qsTr("No network connection. Turn on Wi-Fi and try again.")
        case "too_large": return qsTr("Larger than the limit of %1. Not downloaded.").arg(reader.attachmentLimitText)
        case "refused":
        case "not_found": return qsTr("The server no longer has this attachment.")
        case "auth_failed": return qsTr("The password was refused.")
        case "cert_expired":
        case "cert_untrusted":
        case "cert_host_mismatch": return qsTr("The server's certificate is not valid. Nothing was sent.")
        case "server_unreachable":
        case "timeout":
        case "connection_lost": return qsTr("The download did not finish. Try again.")
        case "storage_full": return qsTr("There is not enough space on the reader.")
        case "write_failed": return qsTr("The file could not be saved.")
        default: return qsTr("The download failed (%1).").arg(key)
        }
    }
    function attachmentStatusText(a) {
        var k = statuses[a.id]
        if (k === "connecting") return qsTr("Connecting to Wi-Fi …")
        if (k === "downloading") return qsTr("Downloading …")
        if (k && k.length > 0) return failureText(k)
        if (a.state === "downloaded") return a.savedName.length > 0 ? qsTr("Saved in Downloads: %1").arg(a.savedName) : qsTr("Downloaded")
        if (a.errorKey.length > 0) return failureText(a.errorKey)
        return ""
    }
    function startDownload(id) {
        setStatus(id, "downloading")
        attachments.download(id)
    }
    function requestDownload(a) {
        if (a.state === "downloaded" || attachments.busy || page.connectingId !== 0) return
        if (device.wifiConnected()) {
            startDownload(a.id)
        } else {
            page.connectingId = a.id
            setStatus(a.id, "connecting")
            device.connectNetwork()
        }
    }
    Connections {
        target: device
        function onNetworkResult(connected) {
            if (page.connectingId === 0) return
            var id = page.connectingId
            page.connectingId = 0
            if (connected) page.startDownload(id)
            else page.setStatus(id, "no_network")
        }
    }
    Connections {
        target: attachments
        function onFinished(id, key) {
            page.setStatus(id, key === "ok" ? "" : key)
            reader.refresh()
        }
    }

    // ---- pictures of an HTML mail: parts of the message show at once; pictures on the sender's server only after a tap (it tells the sender you read the mail) ----
    property bool connectingImages: false
    property string imageNote: ""         // the result of the last tap
    function imageOfferText() {
        if (reader.remotePending > 0)
            return qsTr("This message has %n picture(s) on the sender's server. Loading them tells the sender that you opened the message.", "", reader.remotePending)
        return qsTr("Some pictures of this message are not loaded yet.")
    }
    function startLoadingImages() {
        page.imageNote = ""
        if (device.wifiConnected()) { reader.loadImages() }
        else { page.connectingImages = true; device.connectNetwork() }
    }
    function imagesLoadedText(ok, key, loaded, failed) {
        if (!ok) return key === "no_network" ? qsTr("No network connection. Turn on Wi-Fi and try again.") : qsTr("The pictures could not be loaded (%1).").arg(key)
        if (failed > 0) return qsTr("%n picture(s) could not be loaded.", "", failed)
        return ""
    }
    function pictureNote(state, sourceKind) {
        switch (state) {
        case "pending": return sourceKind === "remote" ? qsTr("Picture on the sender's server, not loaded") : qsTr("Picture not loaded yet")
        case "failed": return qsTr("The picture could not be loaded")
        case "too_large": return qsTr("The picture is too large")
        case "unavailable": return qsTr("The picture is not part of the message")
        default: return qsTr("The picture cannot be shown")
        }
    }
    Connections {
        target: device
        function onNetworkResult(connected) {
            if (!page.connectingImages) return
            page.connectingImages = false
            if (connected) reader.loadImages()
            else page.imageNote = qsTr("No network connection. Turn on Wi-Fi and try again.")
        }
    }
    Connections {
        target: reader
        function onImagesLoaded(ok, key, loaded, failed) { page.imageNote = page.imagesLoadedText(ok, key, loaded, failed) }
    }

    // ---- links are long, mostly tracking: the list shows the start and the end; a tap shows the whole address (it is text and is never opened) ----
    property var openLinks: ({})
    function linkLabel(url) {
        var s = url.replace(/^https?:\/\//i, "").replace(/^www\./i, "")
        if (s.length <= 46) return s
        var slash = s.indexOf("/")
        var host = slash < 0 ? s : s.substring(0, slash)
        if (host.length >= 40) return host.substring(0, 40) + "…"
        return s.substring(0, 34) + "…" + s.substring(s.length - 8)
    }
    function toggleLink(i) {
        var o = {}
        for (var k in openLinks) o[k] = openLinks[k]
        o[i] = !o[i]
        openLinks = o
    }

    // Reply, reply to all or forward: the form is prepared from this message; nothing is saved until the user changes something
    function composeFrom(kind) {
        compose.start(kind, page.messageId)
        if (page.StackView.view) page.StackView.view.push(Qt.resolvedUrl("ComposePage.qml"), { prepared: true })
    }

    // ---- what the user does with the message: read / unread, star, move, delete ----
    function actionChoices() {
        var c = [ { label: reader.unread ? qsTr("Mark as read") : qsTr("Mark as unread"), value: "toggle_read" },
                  { label: qsTr("Move to …"), value: "move" } ]
        return c
    }
    function perform(value) {
        if (value === "toggle_read") { if (reader.unread) reader.markRead(); else reader.markUnread() }
        else if (value === "toggle_star") reader.setFlagged(!reader.flagged)
        else if (value === "move") {
            var t = reader.moveTargets(), c = []
            for (var i = 0; i < t.length; ++i) c.push({ label: t[i].name, value: String(t[i].id) })
            moveChoice.choices = c
            moveChoice.open()
        } else if (value === "delete") deleteDialog.open()
    }
    // every change the user makes to a message reaches the server at the next sync - which starts by itself a few seconds later when Wi-Fi is on
    Connections {
        target: reader
        function onActionResult(action, ok, key) { if (ok) sync.noteChange(device.wifiConnected()) }
    }
    function leaveAfter() { if (page.StackView.view) page.StackView.view.pop() }

    Component.onCompleted: {
        reader.open(page.messageId)
        if (reader.loaded && reader.unread) reader.markRead()   // opening a message reads it; the server learns at the next sync
    }
    Component.onDestruction: reader.close()

    TopBar {
        id: topBar
        title: qsTr("Message")
        leftItems: [
            IconButton { objectName: "readerBack"; text: qsTr("Back"); icon: "chevron-left"; onClicked: if (page.StackView.view) page.StackView.view.pop() },
            IconButton { objectName: "replyButton"; text: qsTr("Reply"); icon: "undo"; onClicked: page.composeFrom("reply") },
            IconButton { objectName: "replyAllButton"; text: qsTr("Reply to all"); icon: "reply-all"; onClicked: page.composeFrom("reply_all") },
            IconButton { objectName: "forwardButton"; text: qsTr("Forward"); icon: "redo"; onClicked: page.composeFrom("forward") }
        ]
        rightItems: [
            IconButton { objectName: "starButton"; text: reader.flagged ? qsTr("Remove the star") : qsTr("Star"); icon: reader.flagged ? "star-filled" : "star"; onClicked: page.perform("toggle_star") },
            IconButton { objectName: "deleteButton"; text: qsTr("Delete"); icon: "trash"; onClicked: page.perform("delete") },
            IconButton { id: moreButton; objectName: "readerActions"; text: qsTr("More"); icon: "menu"; onClicked: { actionChoice.choices = page.actionChoices(); actionChoice.anchor = moreButton; actionChoice.open() } }
        ]
    }

    // the sync state is visible on every page (FR-033): offline, waiting changes, time of the last sync
    SyncStatusLine { id: status; objectName: "readerStatus"; anchors { top: topBar.bottom; left: parent.left; right: parent.right } }

    // the area for the text; the Flickable inside shows a whole number of lines
    Item {
        id: area
        anchors { top: status.bottom; left: parent.left; right: parent.right; bottom: bottomBar.top }
        clip: true

        Flickable {
            id: view
            objectName: "readerView"
            width: parent.width
            height: Math.max(page.lineHeight, Math.floor(area.height / page.lineHeight) * page.lineHeight)
            contentWidth: width
            contentHeight: content.height
            interactive: false              // pages are turned with the buttons
            boundsBehavior: Flickable.StopAtBounds
            clip: true

            Column {
                id: content
                x: page.margin
                width: view.width - 2 * page.margin

                // one line of text in the page's line height
                component Line: Text {
                    width: content.width
                    wrapMode: Text.Wrap
                    textFormat: Text.PlainText      // mail text is never read as markup
                    font.pixelSize: Style.bodySize
                    lineHeight: page.lineHeight
                    lineHeightMode: Text.FixedHeight
                    color: Style.ink
                }
                component Gap: Item { width: 1; height: page.lineHeight }

                Line { objectName: "readerSubject"; text: reader.subject; font.bold: true; visible: reader.loaded }
                Line { objectName: "readerFrom"; text: qsTr("From: %1").arg(reader.from); visible: reader.loaded }
                Line { objectName: "readerTo"; text: qsTr("To: %1").arg(reader.to); visible: reader.loaded && reader.to.length > 0 }
                Line { objectName: "readerCc"; text: qsTr("Cc: %1").arg(reader.cc); visible: reader.loaded && reader.cc.length > 0 }
                // the date line also says how the message is marked now: a change made here shows at once, before the server knows it
                Row {
                    objectName: "readerMarks"
                    visible: reader.loaded && reader.dateText.length > 0
                    spacing: 12
                    Icon { objectName: "readerStarIcon"; visible: reader.flagged; source: "star-filled"; size: 32; anchors.verticalCenter: parent.verticalCenter }
                    Line {
                        objectName: "readerDate"
                        width: content.width - (reader.flagged ? 44 : 0)
                        text: reader.dateText + (reader.flagged ? "  ·  " + qsTr("starred") : "") + (reader.unread ? "  ·  " + qsTr("unread") : "")
                              + (reader.pendingMark ? "  ·  " + qsTr("not synced") : "")
                    }
                }
                Item {
                    width: content.width
                    height: page.lineHeight
                    Rectangle { anchors.verticalCenter: parent.verticalCenter; width: parent.width; height: Style.ruleWidth; color: Style.ink }
                }

                Line { text: qsTr("Attachments"); font.bold: true; visible: reader.attachments.length > 0 }
                Repeater {
                    model: reader.attachments
                    delegate: Column {
                        id: attItem
                        required property int index
                        required property var modelData
                        width: content.width
                        Item {
                        width: parent.width
                        height: page.lineHeight
                        Rectangle { anchors.fill: parent; color: Style.pressed; visible: attTap.pressed }
                        MouseArea { id: attTap; objectName: "attachmentTap" + attItem.index; anchors.fill: parent; onClicked: page.requestDownload(attItem.modelData) }
                        Text {
                            objectName: "attachmentName" + attItem.index
                            x: 0
                            width: parent.width - sizeText.width - 20
                            height: page.lineHeight
                            verticalAlignment: Text.AlignVCenter
                            elide: Text.ElideMiddle
                            font.pixelSize: Style.bodySize
                            color: Style.ink
                            textFormat: Text.PlainText
                            text: attItem.modelData.name.length > 0 ? attItem.modelData.name : qsTr("(no name)")
                        }
                        Text {
                            id: sizeText
                            objectName: "attachmentSize" + attItem.index
                            anchors.right: parent.right
                            height: page.lineHeight
                            verticalAlignment: Text.AlignVCenter
                            font.pixelSize: Style.captionSize
                            color: Style.ink
                            text: attItem.modelData.sizeText
                        }
                        }
                        Line {
                            objectName: "attachmentStatus" + attItem.index
                            font.pixelSize: Style.captionSize
                            font.italic: true
                            text: page.attachmentStatusText(attItem.modelData)
                            visible: text.length > 0
                        }
                    }
                }
                Gap { visible: reader.attachments.length > 0 }

                Line { objectName: "readerNotice"; text: page.bodyNoticeText(); font.italic: true; visible: text.length > 0 }
                Line { objectName: "readerError"; text: qsTr("This message is not available."); visible: reader.errorKey.length > 0 }

                // the offer to load the pictures (two lines, so that the page turns stay on whole lines)
                Item {
                    objectName: "imageOffer"
                    visible: reader.imagesToLoad > 0 || reader.loadingImages || page.connectingImages || page.imageNote.length > 0
                    width: content.width
                    height: visible ? page.lineHeight * 3 : 0
                    Line {
                        objectName: "imageOfferText"
                        width: parent.width
                        height: page.lineHeight * 2
                        font.pixelSize: Style.captionSize
                        elide: Text.ElideRight
                        maximumLineCount: 2
                        text: reader.loadingImages || page.connectingImages ? qsTr("Loading pictures …") : (page.imageNote.length > 0 ? page.imageNote : page.imageOfferText())
                    }
                    Item {
                        y: page.lineHeight * 2
                        width: parent.width
                        height: page.lineHeight
                        visible: reader.imagesToLoad > 0 && !reader.loadingImages && !page.connectingImages
                        Icon { id: offerIcon; source: "download"; size: 32; anchors.verticalCenter: parent.verticalCenter }
                        Text {
                            objectName: "loadImagesLabel"
                            x: offerIcon.width + 12
                            anchors.verticalCenter: parent.verticalCenter
                            font.pixelSize: Style.bodySize
                            font.bold: true
                            font.underline: true
                            color: Style.ink
                            text: qsTr("Load pictures")
                        }
                        MouseArea { objectName: "loadImages"; anchors.fill: parent; onClicked: page.startLoadingImages() }
                    }
                }

                Repeater {
                    model: reader.blocks
                    delegate: Item {
                        id: blockItem
                        required property int index
                        required property var modelData
                        readonly property bool isImage: modelData.kind === "image"
                        readonly property real maxPicture: Math.max(page.lineHeight * 4, view.height - page.lineHeight)
                        width: content.width
                        clip: isImage
                        // text: the text and one empty line after it; picture: whole lines too (the picture, or the two lines of its note, and one empty line)
                        height: isImage ? pictureHeight + page.lineHeight : txt.height + page.lineHeight
                        // a portrait-sized picture with the lines of its row beside it (the bridge decides): the picture column, then the text
                        readonly property bool hasBeside: isImage && modelData.beside.length > 0
                        readonly property real colWidth: hasBeside ? Math.min(modelData.widthHint, content.width * 0.4) : 0
                        readonly property bool pictureReady: pic.visible && pic.status === Image.Ready
                        // the height the picture, or its frame while it is not shown, takes
                        readonly property real framedHeight: pictureReady ? Math.min(pic.paintedHeight, maxPicture)
                                                           : (hasBeside ? Math.min(modelData.heightHint * colWidth / Math.max(1, modelData.widthHint), maxPicture) : 0)
                        readonly property real pictureHeight: {
                            if (!isImage) return 0
                            if (!hasBeside && !pictureReady) return page.lineHeight * 2
                            return Math.ceil(Math.max(framedHeight, hasBeside ? besideTxt.height : 0) / page.lineHeight) * page.lineHeight
                        }
                        Rectangle {
                            objectName: "blockMark" + blockItem.index
                            visible: !blockItem.isImage && blockItem.modelData.quoted
                            x: 0
                            y: 0
                            width: 4
                            height: txt.height
                            color: Style.ink
                        }
                        Line {
                            id: txt
                            objectName: "block" + blockItem.index
                            visible: !blockItem.isImage
                            property bool quoted: !blockItem.isImage && blockItem.modelData.quoted
                            x: !blockItem.isImage && blockItem.modelData.quoted ? page.quoteIndent * Math.min(blockItem.modelData.level, 4) : 0
                            width: content.width - x
                            text: blockItem.isImage ? "" : blockItem.modelData.text
                        }
                        Image {
                            id: pic
                            objectName: "picture" + blockItem.index
                            visible: blockItem.isImage && blockItem.modelData.source.length > 0 && status !== Image.Error
                            x: 0
                            y: 0
                            // never larger than the picture itself (a small logo stays small), never wider than the text or taller than a page
                            // (the size the sender gave the picture applies when it is smaller: a picture of twice the size is shown at the size meant)
                            width: Math.min(content.width, implicitWidth, blockItem.hasBeside ? blockItem.colWidth : (blockItem.isImage && blockItem.modelData.widthHint > 0 ? blockItem.modelData.widthHint : 1e9))
                            height: implicitWidth > 0 ? Math.min(blockItem.maxPicture, width * implicitHeight / implicitWidth) : 0
                            source: blockItem.isImage ? blockItem.modelData.source : ""
                            sourceSize: Qt.size(content.width, blockItem.maxPicture)
                            fillMode: Image.PreserveAspectFit
                            horizontalAlignment: Image.AlignLeft
                            verticalAlignment: Image.AlignTop
                            asynchronous: true
                            cache: false
                        }
                        Line {
                            id: besideTxt
                            objectName: "beside" + blockItem.index
                            visible: blockItem.hasBeside
                            x: blockItem.colWidth + 16
                            width: content.width - x
                            text: blockItem.hasBeside ? blockItem.modelData.beside : ""
                        }
                        // not shown: a frame with the alt text and why (beside a row of text: only the frame, the page's offer says why)
                        Rectangle {
                            objectName: "pictureNote" + blockItem.index
                            visible: blockItem.isImage && !(pic.visible && pic.status === Image.Ready) && !(pic.visible && pic.status === Image.Loading)
                            width: blockItem.hasBeside ? blockItem.colWidth : content.width
                            height: blockItem.hasBeside ? blockItem.framedHeight : page.lineHeight * 2
                            color: Style.paper
                            border.width: Style.hairlineWidth
                            border.color: Style.hairline
                            Text {
                                visible: !blockItem.hasBeside
                                x: 12
                                width: parent.width - 24
                                height: page.lineHeight
                                verticalAlignment: Text.AlignVCenter
                                elide: Text.ElideRight
                                font.pixelSize: Style.bodySize
                                color: Style.ink
                                textFormat: Text.PlainText
                                text: blockItem.isImage && blockItem.modelData.alt.length > 0 ? "[" + blockItem.modelData.alt + "]" : ""
                            }
                            Text {
                                visible: !blockItem.hasBeside
                                x: 12
                                y: page.lineHeight
                                width: parent.width - 24
                                height: page.lineHeight
                                verticalAlignment: Text.AlignVCenter
                                elide: Text.ElideRight
                                font.pixelSize: Style.captionSize
                                font.italic: true
                                color: Style.ink
                                text: blockItem.isImage ? (pic.visible && pic.status === Image.Error ? qsTr("The picture cannot be shown") : page.pictureNote(blockItem.modelData.state, blockItem.modelData.sourceKind)) : ""
                            }
                        }
                    }
                }

                Line { text: qsTr("Links"); font.bold: true; visible: reader.links.length > 0 }
                Repeater {
                    model: reader.links
                    delegate: Item {
                        id: linkItem
                        required property int index
                        required property var modelData
                        width: content.width
                        height: linkLine.height
                        Line {
                            id: linkLine
                            objectName: "linkText" + linkItem.index
                            font.pixelSize: Style.captionSize
                            text: "[" + (linkItem.index + 1) + "] " + (page.openLinks[linkItem.index] ? linkItem.modelData : page.linkLabel(linkItem.modelData))
                        }
                        MouseArea { objectName: "linkTap" + linkItem.index; anchors.fill: parent; onClicked: page.toggleLink(linkItem.index) }
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

    ChoiceDialog {
        id: actionChoice
        objectName: "readerActionChoice"
        title: qsTr("What do you want to do?")
        onChosen: function (value) { page.perform(value) }
    }
    ChoiceDialog {
        id: moveChoice
        objectName: "readerMoveChoice"
        title: qsTr("Move to")
        onChosen: function (value) { if (reader.moveTo(Number(value))) page.leaveAfter() }
    }
    ConfirmDialog {
        id: deleteDialog
        objectName: "readerDeleteDialog"
        title: reader.deleteKind === "to_trash" ? qsTr("Move to the Trash?") : qsTr("Delete for good?")
        text: reader.deleteKind === "to_trash" ? qsTr("The message goes to the Trash folder. You can get it back from there.")
              : (reader.deleteKind === "permanent_in_trash" ? qsTr("The message is deleted from the server. This cannot be undone.")
                                                            : qsTr("There is no Trash folder. The message is deleted from the server. This cannot be undone."))
        acceptText: reader.deleteKind === "to_trash" ? qsTr("Move to Trash") : qsTr("Delete for good")
        cancelText: qsTr("Cancel")
        onAccepted: { if (reader.deleteMessage(reader.deleteKind !== "to_trash")) page.leaveAfter() }
    }
}
