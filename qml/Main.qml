import QtQuick
import QtQuick.Controls
import QtQuick.Window

// The single window. The system rotates (device.orientation), Qt resizes the window; this stage only
// compensates the panel's devicePixelRatio so that sizes authored in panel pixels are 1:1.
// No page sets a font family: every text inherits the application font, which carries the emoji fallback
// (app/main.cpp).
ApplicationWindow {
    id: win
    visible: true
    visibility: Window.FullScreen
    color: "#ffffff"

    // True once a page is shown (checked by the UI self-test: a blank screen must never go unnoticed).
    readonly property bool pageLoaded: nav.depth > 0

    Item {
        id: stage
        objectName: "stage"   // where the text fields' copy-and-paste bubble is put (TextMenu)
        readonly property real dpr: Screen.devicePixelRatio
        width: win.width * dpr
        height: win.height * dpr
        anchors.centerIn: parent
        scale: 1 / dpr

        StackView {
            id: nav
            anchors.fill: parent
            // initialItem with a binding silently showed nothing (measured on the desktop and the reader):
            // push the first page explicitly: the start page once an account exists, else the account setup (first start).
            Component.onCompleted: {
                push(Qt.resolvedUrl(startupError.length > 0 ? "pages/StorageErrorPage.qml" : (account.mailConfigured || account.calendarConfigured ? "pages/HomePage.qml" : "pages/AccountSetupPage.qml")))
                // FR-030: a sync at the start of the app when Wi-Fi is already connected (it never switches Wi-Fi on by itself;
                // the sync button does). The list shows "Syncing …" and fills in when it is done.
                if (startupError.length === 0) sync.startupSync(device.wifiConnected())
            }
            // E-ink: no transitions.
            pushEnter: Transition {}
            pushExit: Transition {}
            popEnter: Transition {}
            popExit: Transition {}
            replaceEnter: Transition {}
            replaceExit: Transition {}
        }

        // A change the user made on the reader asks for a sync (SyncBridge.noteChange); when the reader is offline the bridge asks for Wi-Fi first
        Connections {
            target: typeof sync !== "undefined" ? sync : null     // there is no sync object when the storage could not be opened
            function onConnectRequested() { device.connectNetwork() }
        }
        // ... also a change to a contact (create, change, delete, retry ...): here and not on the page, which may be gone when the signal arrives
        Connections {
            target: typeof contact !== "undefined" ? contact : null
            function onLocalChange() { if (typeof sync !== "undefined") sync.noteChange(device.wifiConnected()) }
        }
        Connections {
            target: device
            function onNetworkResult(connected) { if (typeof sync !== "undefined") sync.connectResult(connected) }
        }

        Connections {
            target: device
            function onBackRequested() {
                // a page with an open popup (the editor's "/" menu) takes Back for itself (spec 006 FR-009)
                var page = nav.currentItem
                if (page && page.backHandled && page.backHandled()) return
                if (nav.depth > 1) {
                    Qt.inputMethod.hide()
                    nav.pop()
                }
            }
        }
    }
}
