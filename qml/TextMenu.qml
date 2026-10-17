import QtQuick

// The copy-and-paste menu of a text field (every edit field of the app, passwords included). Put it inside a TextField /
// TextInput / TextArea / TextEdit; it fills that item. A tap puts the caret there and shows the keyboard; a press-and-hold
// selects the word under the finger and opens a bubble with Cut, Copy, Paste, Delete and Select all, and two markers can be
// dragged to change the selection (the same look as the reader's own). The clipboard is the toolkit's (copy/cut/paste of the
// item). A password field never offers Cut or Copy: a secret is typed or pasted in, never copied out.
// The bubble is put on the page's stage (objectName "stage" in Main.qml), above dialogs, so no form clips it.
Item {
    id: root
    objectName: "textMenu"
    property Item target: parent
    property bool multiline: false
    readonly property bool secret: target && target.echoMode !== undefined && target.echoMode !== 0   // TextInput.Normal = 0
    readonly property bool menuShown: menu.visible
    readonly property Item bubble: menu

    anchors.fill: parent
    z: 10   // above the control's own content item, which a Control stacks after its declared children

    function hostItem() {
        var p = root
        while (p.parent) {
            if (p.objectName === "stage") return p
            p = p.parent
        }
        return p
    }
    function pointAt(x, y) { return multiline ? target.positionAt(x, y) : target.positionAt(x) }
    function hasSelection() { return target.selectionStart < target.selectionEnd }

    function items() {
        var sel = hasSelection()
        var list = []
        if (sel && !secret && !target.readOnly) list.push({ id: "cut", label: qsTr("Cut") })
        if (sel && !secret) list.push({ id: "copy", label: qsTr("Copy") })
        // (always offered: whether the clipboard holds text is only known when the paste is tried)
        if (!target.readOnly) list.push({ id: "paste", label: qsTr("Paste") })
        if (sel && !target.readOnly) list.push({ id: "remove", label: qsTr("Delete") })
        if (target.length > 0 && !(sel && target.selectionStart === 0 && target.selectionEnd === target.length))
            list.push({ id: "all", label: qsTr("Select all") })
        return list
    }
    function open(note) {
        var host = hostItem()
        menu.parent = host
        menu.items = items()
        menu.note = note || ""
        if (menu.items.length === 0) { menu.close(); return }
        var a = target.positionToRectangle(target.selectionStart)
        var b = target.positionToRectangle(target.selectionEnd)
        var pa = target.mapToItem(host, a.x, a.y)
        var pb = target.mapToItem(host, b.x, b.y + b.height)
        menu.anchorX = hasSelection() ? (pa.x + pb.x) / 2 : pa.x
        menu.selTop = pa.y
        menu.selBottom = pb.y
        menu.minY = 10
        var kb = Qt.inputMethod.keyboardRectangle
        menu.maxY = (Qt.inputMethod.visible && kb.height > 0) ? kb.y * (host.dpr || 1) : host.height
        menu.place()
        menu.visible = true
    }
    function close() { menu.close() }
    // (the item's text is only up to date a moment after a programmatic change: report the edit then)
    function edited() { if (target.textEdited) Qt.callLater(function() { target.textEdited() }) }   // TextInput only; a TextEdit/TextArea reports every change

    function chosen(id) {
        menu.close()
        if (id === "cut") { target.cut(); edited() }
        else if (id === "copy") { var end = target.selectionEnd; target.copy(); target.deselect(); target.cursorPosition = end }
        else if (id === "paste") {
            var before = target.text
            target.paste()
            // (the text is only up to date a moment later) nothing came in: the clipboard is the app's own, text copied in
            // another app is not reachable from here
            Qt.callLater(function() {
                if (target.text === before) open(qsTr("Nothing to paste. Only text copied in this app can be pasted."))
                else edited()
            })
        }
        else if (id === "remove") { target.remove(target.selectionStart, target.selectionEnd); edited() }
        else if (id === "all") { target.selectAll(); open() }
    }

    Connections {
        target: root.target
        function onActiveFocusChanged() { if (!root.target.activeFocus) menu.close() }
        function onTextChanged() { if (menu.visible && !handles.dragging) menu.close() }
    }

    MouseArea {
        anchors.fill: parent
        pressAndHoldInterval: 600
        property bool held: false
        onPressed: function(mouse) {
            held = false
            menu.close()
            root.target.forceActiveFocus()
            root.target.cursorPosition = root.pointAt(mouse.x, mouse.y)
            root.target.deselect()
        }
        onPressAndHold: function(mouse) {
            held = true
            root.target.selectWord()
            root.open()
        }
        onClicked: if (!held) Qt.inputMethod.show()
    }

    SelectionHandles {
        id: handles
        target: root.target
        multiline: root.multiline
        onDraggingChanged: if (dragging) menu.close()
        onFinished: root.open()
    }

    SelectionMenu {
        id: menu
        onChosen: function(id) { root.chosen(id) }
    }
}
