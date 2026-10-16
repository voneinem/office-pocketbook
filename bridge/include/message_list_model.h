#pragma once
// A paged list of message headers for one folder (contracts/bridge-qml.md MessageListModel). The list is read from
// the store on the calling (UI) thread, one page at a time; a page is small, so this never blocks for long.
// Rows are plain maps: id, sender, address, subject, dateText, unread, flagged, hasAttachments, pendingMark, errorKey.

#include <QObject>
#include <QString>
#include <QVariantList>

#include <vector>

#include "pbo/store.h"

class MessageListModel : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList rows READ rows NOTIFY changed)
    Q_PROPERTY(int page READ page NOTIFY changed)
    Q_PROPERTY(int pageCount READ pageCount NOTIFY changed)
    Q_PROPERTY(int total READ total NOTIFY changed)
    Q_PROPERTY(int pageSize READ pageSize WRITE setPageSize NOTIFY changed)
    Q_PROPERTY(bool empty READ empty NOTIFY changed)
    Q_PROPERTY(QString errorKey READ errorKey NOTIFY changed)
    // the open folder: id (0 = none), its full name, its role (inbox, sent, ...) and whether older mail waits on the server
    Q_PROPERTY(qlonglong folderId READ folderId NOTIFY changed)
    Q_PROPERTY(QString folderName READ folderName NOTIFY changed)
    Q_PROPERTY(QString folderRole READ folderRole NOTIFY changed)
    Q_PROPERTY(bool olderOnServer READ olderOnServer NOTIFY changed)
    // the order of the list: "date" (newest first, the default) | "dateAsc" | "sender" | "subject"
    Q_PROPERTY(QString sort READ sort WRITE setSort NOTIFY changed)
    // search mode: the rows are the results of search() over every folder; `queryText` is what was searched for
    Q_PROPERTY(bool searching READ searching NOTIFY changed)
    Q_PROPERTY(QString queryText READ queryText NOTIFY changed)

public:
    struct Config {
        pbo::Store* store = nullptr;
    };

    explicit MessageListModel(Config config, QObject* parent = nullptr);

    QVariantList rows() const { return rows_; }
    int page() const { return page_; }
    int pageCount() const { return pageCount_; }
    int total() const { return total_; }
    int pageSize() const { return pageSize_; }
    bool empty() const { return total_ == 0; }
    QString errorKey() const { return errorKey_; }
    qlonglong folderId() const { return folder_; }
    QString folderName() const { return folderName_; }
    QString folderRole() const { return folderRole_; }
    bool olderOnServer() const { return olderOnServer_; }
    void setPageSize(int n);
    QString sort() const { return sort_; }
    void setSort(const QString& key);   // an unknown key is ignored; a new order starts at the first page
    bool searching() const { return searching_; }
    QString queryText() const { return query_; }

    Q_INVOKABLE void openInbox();
    Q_INVOKABLE void open(qlonglong folderId);
    // Search over sender, recipients, subject and downloaded text of every folder, offline. An empty query leaves the search mode.
    Q_INVOKABLE void search(const QString& query);
    Q_INVOKABLE void goToPage(int n);   // clamped to the existing pages
    Q_INVOKABLE void nextPage() { goToPage(page_ + 1); }
    Q_INVOKABLE void previousPage() { goToPage(page_ - 1); }
    Q_INVOKABLE void refresh();         // after a sync: same folder, same page (clamped)

signals:
    void changed();

private:
    void load();
    void load_search();
    static QVariantMap row_of(const pbo::MessageHeader& m);

    Config config_;
    qlonglong folder_ = 0;
    int page_ = 0, pageCount_ = 1, total_ = 0, pageSize_ = 12;
    QVariantList rows_;
    QString errorKey_, folderName_, folderRole_, sort_ = QStringLiteral("date"), query_;
    bool searching_ = false;
    std::vector<pbo::MessageHeader> results_;
    bool olderOnServer_ = false;
};
