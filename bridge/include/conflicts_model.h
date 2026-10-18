#pragma once
// Things that need the user's decision.
//  - Events (US4): a conflict (the event was changed here and on the server, both versions are kept) or an event that was deleted on the server after
//    it was changed here. Rows: {kind: "event", eventId, state ("conflict" | "server_deleted"), deleting, calendarName, mineTitle, mineWhen, serverTitle,
//    serverWhen, title, line1, line2} - the "when" texts are "YYYY-MM-DD HH:MM" (or the date for an all-day event) in the event's own zone.
//  - Messages (US5): a change that could not be applied (the message is gone or moved on the server, or the server refused). Rows: {kind: "message",
//    messageId, errorKey ("gone_on_server" | "moved_on_server" | "refused" | "no_trash"), title (the subject), line1 (the sender), line2 (what happened)}.
//  - Contacts (spec 002 US3): a conflict or a contact the server removed after it was changed here. Rows: {kind: "contact", contactId, state ("conflict" |
//    "server_deleted"), deleting, bookName, mineTitle, serverTitle, title}.
// The page asks before KeepServer's loss of the local version and before a discard; this model does what it is told.

#include <QObject>
#include <QString>
#include <QVariantList>

#include "pbo/store.h"
#include "pbo/tz.h"
#include "pbo/types.h"

class ConflictsModel : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList rows READ rows NOTIFY changed)
    Q_PROPERTY(int total READ total NOTIFY changed)
    Q_PROPERTY(bool empty READ empty NOTIFY changed)

public:
    struct Config {
        pbo::Store* store = nullptr;
        const pbo::TzDatabase* tz = nullptr;
        pbo::Clock* clock = nullptr;
        pbo::Random* random = nullptr;
    };

    explicit ConflictsModel(Config config, QObject* parent = nullptr);

    QVariantList rows() const { return rows_; }
    int total() const { return rows_.size(); }
    bool empty() const { return rows_.isEmpty(); }

    Q_INVOKABLE void refresh();
    // choice: "mine" | "server" | "both"
    Q_INVOKABLE bool resolve(qlonglong eventId, const QString& choice);
    Q_INVOKABLE bool recreate(qlonglong eventId);   // deleted on the server -> send it again as a new event
    Q_INVOKABLE bool discard(qlonglong eventId);    // deleted on the server -> drop the kept version (the page asked)
    Q_INVOKABLE bool resolveContact(qlonglong contactId, const QString& choice);   // choice as for resolve()
    Q_INVOKABLE bool recreateContact(qlonglong contactId);
    Q_INVOKABLE bool discardContact(qlonglong contactId);
    Q_INVOKABLE bool acknowledgeMessage(qlonglong messageId);   // the user saw it: a message that is gone is removed, a refused change is dropped

signals:
    void changed();
    void actionResult(const QString& action, bool ok, const QString& key);

private:
    bool done(const QString& action, const pbo::Error& e);

    Config config_;
    QVariantList rows_;
};
