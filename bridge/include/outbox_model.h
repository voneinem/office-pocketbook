#pragma once
// The outbox list (contracts/bridge-qml.md OutboxModel). Rows: {id, state, stateKey, reasonKey, subject, recipients, attempts, waiting, canRetry,
// canEdit, canSendAgain, canMarkSent, canDelete}. `state` is the store's state; `stateKey` the word the page shows:
// "waiting" (queued), "sending", "copy" (sent, the Sent copy is still to be filed), "failed", "uncertain" (may have been sent).

#include <QObject>
#include <QString>
#include <QVariantList>

#include "pbo/store.h"

class OutboxModel : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList rows READ rows NOTIFY changed)
    Q_PROPERTY(int total READ total NOTIFY changed)
    Q_PROPERTY(bool empty READ empty NOTIFY changed)
    // messages that need the user: failed and uncertain
    Q_PROPERTY(int attentionCount READ attentionCount NOTIFY changed)
    // messages that will go out with the next sync
    Q_PROPERTY(int waitingCount READ waitingCount NOTIFY changed)

public:
    struct Config {
        pbo::Store* store = nullptr;
    };

    explicit OutboxModel(Config config, QObject* parent = nullptr);

    QVariantList rows() const { return rows_; }
    int total() const { return rows_.size(); }
    bool empty() const { return rows_.isEmpty(); }
    int attentionCount() const { return attention_; }
    int waitingCount() const { return waiting_; }

    Q_INVOKABLE void refresh();
    Q_INVOKABLE bool retry(qlonglong id);         // failed -> queued
    Q_INVOKABLE qlonglong edit(qlonglong id);     // failed -> back to a draft; returns the draft's id (0 = not possible)
    Q_INVOKABLE bool remove(qlonglong id);        // failed | uncertain: the message is deleted (the page asked for confirmation)
    Q_INVOKABLE bool sendAgain(qlonglong id);     // uncertain -> queued (the page asked for confirmation)
    Q_INVOKABLE bool markSent(qlonglong id);      // uncertain -> "it was sent" (the page asked for confirmation)

signals:
    void changed();
    void actionResult(const QString& action, bool ok, const QString& key);

private:
    bool done(const QString& action, const pbo::Error& e);

    Config config_;
    QVariantList rows_;
    int attention_ = 0, waiting_ = 0;
};

// The drafts the user can continue (contracts/bridge-qml.md: the Drafts folder view). Rows: {id, subject, to, updatedText, kind, pendingOnServer}.
class DraftsModel : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList rows READ rows NOTIFY changed)
    Q_PROPERTY(int total READ total NOTIFY changed)
    Q_PROPERTY(bool empty READ empty NOTIFY changed)

public:
    struct Config {
        pbo::Store* store = nullptr;
    };
    explicit DraftsModel(Config config, QObject* parent = nullptr);
    QVariantList rows() const { return rows_; }
    int total() const { return rows_.size(); }
    bool empty() const { return rows_.isEmpty(); }

    Q_INVOKABLE void refresh();
    Q_INVOKABLE bool remove(qlonglong id);   // the user deleted the draft (the page asked for confirmation)

signals:
    void changed();

private:
    Config config_;
    QVariantList rows_;
};
