# Contract: bridge and QML for the hand-over

**Feature**: [spec.md](../spec.md) | **Plan**: [plan.md](../plan.md) | **Core interfaces**: [core-interfaces.md](core-interfaces.md)

One new object (`handoff`, `HandoffBridge`), a navigator component placed by `Main.qml`, small changes in `ComposePage.qml`, `AccountSetupPage.qml` and `DialogFrame.qml`, no new page.

## `handoff` (context property, only when the storage opened)

See [core-interfaces.md](core-interfaces.md). QML uses: signals `show(draftId, moreWaiting)` and `notice(key, n)`, the methods
`accountReady()`, `setupLeft()`, `shown()`, `checkInbox()`.

## `qml/HandoffNavigator.qml` (new) and `qml/DialogState.qml` (new singleton)

The navigation lives in a component of its own (not in `Main.qml`) so that the QML flow test can drive it over a plain `StackView`; `Main.qml` only places it:
`HandoffNavigator { nav: nav }` inside the stage, before the sync `Connections`.

```text
HandoffNavigator (Item, fills its parent; properties nav, heldShow, lastMore, notices)
  Connections { target: handoff
    onShow(draftId, more):  a dialog open (DialogState.open > 0) -> heldShow = {draftId, more} (the newest wins), else showHandoff(draftId, more)
    onNotice(key, n):       addNotice(key, n) }
  Connections { target: DialogState   onOpenChanged: when it is 0 and heldShow is set -> showHandoff(...) }
  Connections { target: account       onMailConfiguredChanged: if account.mailConfigured -> handoff.accountReady() }
  showHandoff(draftId, more): lastMore = more; compose.leave() (the open message is saved NOW: the compose object is reused for the new draft);
                              pop every page named "composePage" (StackView.Immediate); push pages/ComposePage.qml {draftId} (Immediate);
                              handoff.shown(); if more > 0 -> addNotice("more_waiting", more)
  notices: one ConfirmDialog (showCancel false, objectName "handoffNotice", title "Text from another app") at a time, the next after the user accepts
```

- `DialogState.open` counts the open dialogs: `DialogFrame` adds one in `onVisibleChanged` and removes it again (also in `Component.onDestruction`), so every dialog of the
  app counts. A hand-over never answers or covers a dialog.
- The account set-up page (`AccountSetupPage.qml`, `objectName: "accountSetupPage"`) calls `handoff.setupActive(true)` when it is created and, when it is destroyed,
  `handoff.setupActive(false)` and `handoff.setupLeft()`. The pages are pushed by URL, so the stack destroys them (an object pushed ready-made is not destroyed by the stack).
- E-ink: no transition and no animation; the page change is the existing `push`/`pop`.

## `qml/pages/ComposePage.qml` (changed)

- `objectName: "composePage"` on the page root (so that `Main.qml` can find an open one).
- Focus: the first **empty** field of recipient, subject, body gets the focus (today only the recipient is checked); for an ordinary new message this is unchanged
  because all are empty and the recipient is first. The `StackView` gives the focus to the page it has just pushed, which takes it from a field focused while the page
  was created (measured in the QML test): the focus is set again one event-loop turn after the page became current (`StackView.onActivated`), once.
- Nothing else: a hand-over message is opened like any draft (`draftId`).

## Strings

New English source strings in `HandoffNavigator.qml` (the notice texts of research R-08; the plural of "more waiting" is two plain strings, one and n) with German translations in `qml/strings_de.tsv`;
`scripts/extract-strings.py` and `check-ui-style.sh` pass. The German texts are data (Principle X).

## UI tests (`tst_handoff_flow`)

| Case | Expected |
|------|----------|
| hand-over on the home page | ComposePage with the text as body, recipient and subject as given, focus in the first empty field; Back returns to the home page |
| hand-over while the message list is open | the same; Back returns to the list (same folder, same page of the list) |
| hand-over while another message is being typed | the earlier text is a draft, unchanged; the new page holds only the new text |
| hand-over while a dialog is open | the dialog is not answered; the editor appears when it closes |
| hand-over on the account set-up page | the editor appears after the mail account is saved (also when the page goes just before the signal); leaving the set-up without an account shows `no_account` |
| refused hand-over | no page change; the matching notice |
| two hand-overs | the newest is shown; both are drafts; with a dialog open the notice "1 more hand-over waits in your drafts." follows the editor (without one, the second replaces the first editor at once and the first is in the Drafts list) |
