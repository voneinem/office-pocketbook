#pragma once
// Writing a message (contracts/bridge-qml.md ComposeBridge). The page edits the fields; the bridge saves the draft by itself - 2 seconds after the last
// change, when the page is left and when the app is suspended - so that a draft is never lost (FR-012, SC-006). Nothing is created for a page that was
// opened and left without a change. Send builds the message once and moves the draft into the outbox in one step; sending itself is the
// sync's job (the page starts a sync when the reader is online, and tells the user that the message waits otherwise).

#include <QObject>
#include <QString>
#include <QTimer>

#include "pbo/store.h"
#include "pbo/types.h"

class ComposeBridge : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString to READ to WRITE setTo NOTIFY fieldsChanged)
    Q_PROPERTY(QString cc READ cc WRITE setCc NOTIFY fieldsChanged)
    Q_PROPERTY(QString bcc READ bcc WRITE setBcc NOTIFY fieldsChanged)
    Q_PROPERTY(QString subject READ subject WRITE setSubject NOTIFY fieldsChanged)
    Q_PROPERTY(QString body READ body WRITE setBody NOTIFY fieldsChanged)
    // "" (nothing yet) | "pending" (a change waits to be saved) | "saved" | "error"
    Q_PROPERTY(QString saveState READ saveState NOTIFY saveStateChanged)
    Q_PROPERTY(qlonglong draftId READ draftId NOTIFY saveStateChanged)
    Q_PROPERTY(QString kind READ kind NOTIFY fieldsChanged)
    Q_PROPERTY(bool forwardNote READ forwardNote NOTIFY fieldsChanged)   // a forwarded message carries its text, not its attachments
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)

public:
    struct Config {
        pbo::Store* store = nullptr;
        pbo::Clock* clock = nullptr;
        pbo::Random* random = nullptr;
        int autosave_ms = 2000;
    };

    explicit ComposeBridge(Config config, QObject* parent = nullptr);

    QString to() const { return to_; }
    QString cc() const { return cc_; }
    QString bcc() const { return bcc_; }
    QString subject() const { return subject_; }
    QString body() const { return body_; }
    QString saveState() const { return saveState_; }
    qlonglong draftId() const { return draftId_; }
    QString kind() const { return kind_; }
    bool forwardNote() const { return kind_ == QLatin1String("forward"); }
    bool busy() const { return false; }

    void setTo(const QString& v) { set(to_, v); }
    void setCc(const QString& v) { set(cc_, v); }
    void setBcc(const QString& v) { set(bcc_, v); }
    void setSubject(const QString& v) { set(subject_, v); }
    void setBody(const QString& v) { set(body_, v); }

    // Address suggestions for a recipient field: `text` is the whole field; only the part after the last comma counts (at least 2 characters) and
    // addresses already in the field are not offered again. Entries are "Name <address>" or "address". Cached and sent mail, offline.
    Q_INVOKABLE QStringList suggest(const QString& text) const;
    // The field with the part being typed replaced by the chosen entry and ", " after it.
    Q_INVOKABLE QString applySuggestion(const QString& text, const QString& suggestion) const;

    // kind: "new" | "reply" | "reply_all" | "forward"; messageId for the last three. The fields are prepared (recipients, subject, quoted text); no
    // draft exists until the user changes something.
    Q_INVOKABLE void start(const QString& kind, qlonglong messageId = 0);
    // Opens a saved draft. False if there is none (or it is in the outbox).
    Q_INVOKABLE bool edit(qlonglong draftId);
    Q_INVOKABLE void autosave();         // now, if something changed (page leave, suspend)
    // Builds the message and puts it into the outbox. Answers with sendResult(ok, key, field): key "no_account", "no_recipient",
    // "recipient_invalid" (field "to" | "cc" | "bcc"), "from_invalid", "too_large", or a store problem.
    Q_INVOKABLE void send();
    Q_INVOKABLE void discard();          // the draft is deleted (the page asked for confirmation)
    Q_INVOKABLE void leave();            // the page closes: a pending change is saved first
    Q_INVOKABLE void reset();

signals:
    void fieldsChanged();
    void saveStateChanged();
    void busyChanged();
    void sendResult(bool ok, const QString& key, const QString& field);
    void queued(qlonglong outboxId);

private:
    void set(QString& field, const QString& value);
    bool save();                         // the draft as it is now; sets the state
    std::string own_address() const;

    Config config_;
    QTimer timer_;
    qlonglong draftId_ = 0;
    QString kind_ = QStringLiteral("new");
    QString to_, cc_, bcc_, subject_, body_;
    std::string messageId_, inReplyTo_, refs_;
    qlonglong replyTo_ = 0;
    bool dirty_ = false;
    QString saveState_;
};
