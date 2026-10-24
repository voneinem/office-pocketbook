pragma Singleton
import QtQuick

// How many dialogs are open (DialogFrame counts itself in and out). A hand-over from another app never answers or covers a dialog: it waits until this is 0.
QtObject {
    property int open: 0
}
