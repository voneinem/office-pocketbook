#pragma once
// The list of contacts, paged and searchable (specs/002-server-contacts/contracts/bridge-qml.md ContactsModel). Rows are plain maps:
//   {contactId, displayName, subtitle, bookName, pendingMark, errorKey, unreadable}
// bookName is "" while only one address book is shown; pendingMark is "" | "notSynced" | "failed" | "conflict" | "deletedOnServer".
// state: "noAccount" (no calendar server account), "noAddressbook" (the server named no address book, or none was looked for yet), "empty" (books
// without contacts), "noMatch" (a search found nothing), "ok".

#include <QObject>
#include <QString>
#include <QVariantList>

#include "pbo/store.h"

class ContactsModel : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList rows READ rows NOTIFY changed)
    Q_PROPERTY(int page READ page NOTIFY changed)
    Q_PROPERTY(int pageCount READ pageCount NOTIFY changed)
    Q_PROPERTY(int total READ total NOTIFY changed)
    Q_PROPERTY(int pageSize READ pageSize WRITE setPageSize NOTIFY changed)
    Q_PROPERTY(QString state READ state NOTIFY changed)
    Q_PROPERTY(QString query READ query NOTIFY changed)
    // A new contact needs a writable address book that is shown; createBlockedReason: "" | "no_account" | "no_addressbook" | "no_writable_book"
    // The deletions that wait for the server (newest first, at most three) and how many there are: [{contactId, displayName, bookName}]. Each can be undone
    // (ContactBridge.undoRemove) until the server confirmed it.
    Q_PROPERTY(QVariantList deletions READ deletions NOTIFY changed)
    Q_PROPERTY(int deletionCount READ deletionCount NOTIFY changed)
    Q_PROPERTY(bool canCreate READ canCreate NOTIFY changed)
    Q_PROPERTY(QString createBlockedReason READ createBlockedReason NOTIFY changed)

public:
    struct Config {
        pbo::Store* store = nullptr;
    };

    explicit ContactsModel(Config config, QObject* parent = nullptr);

    QVariantList rows() const { return rows_; }
    int page() const { return page_; }
    int pageCount() const { return pageCount_; }
    int total() const { return total_; }
    int pageSize() const { return pageSize_; }
    QVariantList deletions() const { return deletions_; }
    int deletionCount() const { return deletionCount_; }
    QString state() const { return state_; }
    QString query() const { return query_; }
    bool canCreate() const { return createReason_.isEmpty(); }
    QString createBlockedReason() const { return createReason_; }
    void setPageSize(int n);

    Q_INVOKABLE void refresh();            // after a sync, an edit or a change of the shown books: same page (clamped)
    Q_INVOKABLE void goToPage(int n);
    Q_INVOKABLE void nextPage() { goToPage(page_ + 1); }
    Q_INVOKABLE void previousPage() { goToPage(page_ - 1); }
    Q_INVOKABLE void search(const QString& text);   // runs when the field is confirmed, not on every key; starts at the first page
    Q_INVOKABLE void clearSearch();

signals:
    void changed();

private:
    void load();

    Config config_;
    int page_ = 0, pageCount_ = 1, total_ = 0, pageSize_ = 8;
    QString state_ = QStringLiteral("noAccount");
    QString query_;
    QString createReason_ = QStringLiteral("no_account");
    QVariantList rows_, deletions_;
    int deletionCount_ = 0;
};
