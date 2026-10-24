#pragma once
// A text handed to PB Office by another app becomes a draft (spec 003, contracts/core-interfaces.md). The draft is saved BEFORE any screen is asked to show it
// (Principle III), one draft per hand-over; nothing is merged, replaced or sent, and the bridge never starts a sync. The arguments come from a request to the
// running app (deliver) or from a pending entry that the launcher wrote (checkInbox); the entry is removed only after the draft is saved. Nothing a hand-over
// carries is ever logged or put into a signal other than the draft that holds it.

#include <QObject>
#include <QString>
#include <deque>
#include <string>
#include <vector>

#include "pbo/handoff.h"
#include "pbo/handoff_inbox.h"
#include "pbo/store.h"
#include "pbo/types.h"

class HandoffBridge : public QObject {
    Q_OBJECT
    Q_PROPERTY(int waiting READ waiting NOTIFY waitingChanged)    // drafts made by hand-overs that no screen has shown yet

public:
    struct Config {
        pbo::Store* store = nullptr;
        pbo::Clock* clock = nullptr;
        pbo::Random* random = nullptr;
        std::string handoff_dir;     // system/pboffice/handoff; "" = no pending entries
    };

    explicit HandoffBridge(Config config, QObject* parent = nullptr);

    int waiting() const { return static_cast<int>(unshown_.size()); }

    // Thread-safe: copies the arguments and queues the work to the bridge's thread (the request listener of the running app calls it from another thread).
    // `source` is only for the log: "listener" or "file".
    void deliver(std::vector<std::string> args, const char* source);

    Q_INVOKABLE void checkInbox();     // start, window to the front, wake signal: read the pending entries
    Q_INVOKABLE void accountReady();   // a mail account was saved: continue a held hand-over
    Q_INVOKABLE void setupLeft();      // the account set-up page was left: a held hand-over is discarded, with the notice
    Q_INVOKABLE void setupActive(bool active);   // the account set-up page is (not) the current page
    Q_INVOKABLE void shown();          // a page showed the newest waiting draft: nothing waits to be shown any more

signals:
    void show(qlonglong draftId, int moreWaiting);   // open the compose page for this draft; `moreWaiting` older hand-over drafts are in the Drafts list
    void notice(const QString& key, int n);          // a key of research R-08; never a value of the hand-over
    void waitingChanged();

private:
    // False when the hand-over could not be saved and its entry must stay for the next look.
    bool process(const std::vector<std::string>& args, const char* source, const std::string& entry_id);
    // Saves the draft of one parsed hand-over, then shows it and tells what was left out. False when it could not be saved.
    bool make_draft(const pbo::Handoff& h, const std::string& address, const std::string& entry_id);
    void consume(const std::string& entry_id);
    void tell(const char* key, int n = 0) { emit notice(QString::fromLatin1(key), n); }

    Config config_;
    pbo::HandoffInbox inbox_;
    std::deque<qlonglong> unshown_;
    std::vector<pbo::Handoff> held_;   // hand-overs that wait in memory (never on disk) while the account set-up page is open and no mail account exists
    bool setup_active_ = false;
};
