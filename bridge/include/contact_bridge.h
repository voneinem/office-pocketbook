#pragma once
// One contact for the contact page and its form (specs/002-server-contacts/contracts/bridge-qml.md ContactBridge). Lists: phones and emails are [{label, value}], addresses [{label, text}] (the text holds the lines of the address).
// readOnlyReason: "" | "unreadable" | "gone" (the address book is gone on the server) | "book" (the address book is read-only) | "conflict" | "deleting".

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

#include "pbo/store.h"

class ContactBridge : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool loaded READ loaded NOTIFY changed)
    Q_PROPERTY(qlonglong contactId READ contactId NOTIFY changed)
    Q_PROPERTY(QString displayName READ displayName NOTIFY changed)
    Q_PROPERTY(QString organization READ organization NOTIFY changed)
    Q_PROPERTY(QVariantList phones READ phones NOTIFY changed)
    Q_PROPERTY(QVariantList emails READ emails NOTIFY changed)
    Q_PROPERTY(QVariantList addresses READ addresses NOTIFY changed)
    Q_PROPERTY(QString note READ note NOTIFY changed)
    Q_PROPERTY(QString bookName READ bookName NOTIFY changed)
    Q_PROPERTY(bool readOnly READ readOnly NOTIFY changed)
    Q_PROPERTY(QString readOnlyReason READ readOnlyReason NOTIFY changed)
    Q_PROPERTY(bool pending READ pending NOTIFY changed)             // a change waits for the server (also a failed one)
    Q_PROPERTY(QString failedReason READ failedReason NOTIFY changed) // the server's refusal, "" when none
    Q_PROPERTY(bool conflict READ conflict NOTIFY changed)
    Q_PROPERTY(bool deleting READ deleting NOTIFY changed)
    Q_PROPERTY(bool deletedOnServer READ deletedOnServer NOTIFY changed)
    Q_PROPERTY(bool unreadable READ unreadable NOTIFY changed)
    // ---- the form (a new contact, or the open contact changed) ----
    Q_PROPERTY(bool editing READ editing NOTIFY changed)
    // The starting point of the form: {contactId, bookId, given, family, org, note, noteLocked, phones, emails, addresses}; the entries are
    // {label, value, origIndex, locked} (addresses {label, street, city, region, postcode, country, origIndex, locked})
    Q_PROPERTY(QVariantMap edit READ edit NOTIFY changed)
    Q_PROPERTY(QVariantList writableBooks READ writableBooks NOTIFY changed)   // [{id, name}]: writable, shown address books
    Q_PROPERTY(QString createBlockedReason READ createBlockedReason NOTIFY changed)
    // Why the open contact cannot be changed ("" when it can): unreadable | book_gone | read_only | in_conflict | delete_pending
    Q_PROPERTY(QString editBlockedReason READ editBlockedReason NOTIFY changed)
    Q_PROPERTY(QVariantList validationErrors READ validationErrors NOTIFY changed)   // [{field, key}] in the order of the form
    Q_PROPERTY(QString saveError READ saveError NOTIFY changed)   // "" or the key of a refusal that is not about a field (read_only, not_found, ...)

public:
    struct Config {
        pbo::Store* store = nullptr;
        pbo::Clock* clock = nullptr;     // the form needs these two; without them nothing can be saved
        pbo::Random* random = nullptr;
    };

    explicit ContactBridge(Config config, QObject* parent = nullptr);

    bool loaded() const { return loaded_; }
    qlonglong contactId() const { return id_; }
    QString displayName() const { return name_; }
    QString organization() const { return org_; }
    QVariantList phones() const { return phones_; }
    QVariantList emails() const { return emails_; }
    QVariantList addresses() const { return addresses_; }
    QString note() const { return note_; }
    QString bookName() const { return book_; }
    bool readOnly() const { return !reason_.isEmpty(); }
    QString readOnlyReason() const { return reason_; }
    bool pending() const { return pending_; }
    QString failedReason() const { return failed_; }
    bool conflict() const { return conflict_; }
    bool deleting() const { return deleting_; }
    bool deletedOnServer() const { return deletedOnServer_; }
    bool unreadable() const { return unreadable_; }
    bool editing() const { return editing_; }
    QVariantMap edit() const { return edit_; }
    QVariantList writableBooks() const { return books_; }
    QString createBlockedReason() const { return blocked_; }
    QString editBlockedReason() const { return editBlocked_; }
    QVariantList validationErrors() const { return errors_; }
    QString saveError() const { return saveError_; }

    Q_INVOKABLE void open(qlonglong contactId);   // an unknown id opens nothing
    Q_INVOKABLE void close();

    // A new contact: fills writableBooks and edit (the last used book preselected). False, with createBlockedReason, when no book can take it.
    Q_INVOKABLE bool newContact();
    // The form for the open contact: fills edit (every entry with its origIndex, an entry or a note that is too long to edit as `locked`). False, with
    // editBlockedReason, when the contact cannot be changed.
    Q_INVOKABLE bool loadEdit();
    // Checks and saves the form (the shape of `edit`, with bookId; contactId > 0 changes that contact). False with validationErrors or saveError; true
    // emits saved(id), and localChange() when something has to reach the server (a form without a difference writes nothing).
    Q_INVOKABLE bool save(const QVariantMap& form);
    // The decision about the conflict of the open contact: "mine" | "server" | "both"; the open contact is read again
    Q_INVOKABLE bool resolve(const QString& choice);
    // Delete the open contact (the page asked the user before; the bridge never asks): it leaves the list and the next sync deletes it on the server. False,
    // with saveError (read_only, in_conflict, ...), when it cannot be deleted. The page closes: ContactsModel.deletions lists it for an undo.
    Q_INVOKABLE bool remove();
    // Until the server confirmed it: the deleted contact `contactId` comes back as it was
    Q_INVOKABLE bool undoRemove(qlonglong contactId);
    // The open contact that the server removed is sent again as a new card / removed for good (the page asked the user)
    Q_INVOKABLE bool recreate();
    Q_INVOKABLE bool discardServerDeleted();
    // The failed contact open now is sent again at the next sync / given up (a new one is removed).
    Q_INVOKABLE bool retry();
    Q_INVOKABLE bool discardChange();

signals:
    void changed();
    void saved(qlonglong contactId);
    // something the server has to learn happened: the sync control starts an Auto Sync shortly
    void localChange();

private:
    Config config_;
    bool editing_ = false;
    QVariantMap edit_;
    QVariantList books_, errors_;
    QString blocked_, editBlocked_, saveError_;
    bool loaded_ = false, pending_ = false, conflict_ = false, deleting_ = false, deletedOnServer_ = false, unreadable_ = false;
    qlonglong id_ = 0;
    QString name_, org_, note_, book_, reason_, failed_;
    QVariantList phones_, emails_, addresses_;
};
