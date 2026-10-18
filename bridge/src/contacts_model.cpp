#include "contacts_model.h"

#include <QVariantMap>

namespace {

QString pending_mark(const pbo::ContactListItem& it) {
    if (it.sync_state == "conflict") return QStringLiteral("conflict");
    if (it.sync_state == "server_deleted") return QStringLiteral("deletedOnServer");
    if (it.sync_state == "pending_create" || it.sync_state == "pending_update" || it.sync_state == "pending_delete")
        return it.error_key.empty() ? QStringLiteral("notSynced") : QStringLiteral("failed");
    return QString();
}

}  // namespace

ContactsModel::ContactsModel(Config config, QObject* parent) : QObject(parent), config_(std::move(config)) {}

void ContactsModel::setPageSize(int n) {
    n = qBound(2, n, 100);
    if (n == pageSize_) return;
    pageSize_ = n;
    load();
}

void ContactsModel::refresh() { load(); }

void ContactsModel::goToPage(int n) {
    page_ = qBound(0, n, pageCount_ - 1);
    load();
}

void ContactsModel::search(const QString& text) {
    query_ = text.trimmed();
    page_ = 0;
    load();
}

void ContactsModel::clearSearch() {
    query_.clear();
    page_ = 0;
    load();
}

void ContactsModel::load() {
    rows_.clear();
    deletions_.clear();
    deletionCount_ = 0;
    total_ = 0;
    if (!config_.store) {
        createReason_ = QStringLiteral("no_account");
        state_ = QStringLiteral("noAccount");
        pageCount_ = 1;
        page_ = 0;
        emit changed();
        return;
    }
    for (const pbo::ContactListItem& it : config_.store->deleted_contacts(3)) {
        QVariantMap d;
        d["contactId"] = static_cast<qlonglong>(it.id);
        d["displayName"] = QString::fromStdString(it.display_name);
        d["bookName"] = QString::fromStdString(it.addressbook_name);
        deletions_.push_back(d);
    }
    deletionCount_ = static_cast<int>(config_.store->deleted_contacts(1000).size());
    const auto account = config_.store->calendar_account();
    const std::vector<pbo::AddressBook> books = config_.store->addressbooks();
    bool writable_shown = false;
    for (const pbo::AddressBook& b : books)
        if (b.writable && b.shown && b.present_on_server) writable_shown = true;
    createReason_ = (!account || account->caldav_url.empty()) ? QStringLiteral("no_account")
                    : books.empty() ? QStringLiteral("no_addressbook") : (writable_shown ? QString() : QStringLiteral("no_writable_book"));
    if (!account || account->caldav_url.empty()) {
        state_ = QStringLiteral("noAccount");
    } else if (books.empty()) {
        state_ = QStringLiteral("noAddressbook");
    } else {
        int shown = 0;
        for (const pbo::AddressBook& b : books)
            if (b.shown) ++shown;
        const std::string query = query_.toStdString();
        total_ = config_.store->contacts_count(query);
        pageCount_ = qMax(1, (total_ + pageSize_ - 1) / pageSize_);
        page_ = qBound(0, page_, pageCount_ - 1);
        for (const pbo::ContactListItem& it : config_.store->contacts_page(query, page_ * pageSize_, pageSize_)) {
            QVariantMap r;
            r["contactId"] = static_cast<qlonglong>(it.id);
            r["displayName"] = QString::fromStdString(it.display_name);
            r["subtitle"] = QString::fromStdString(it.subtitle);
            r["bookName"] = shown > 1 ? QString::fromStdString(it.addressbook_name) : QString();
            r["pendingMark"] = pending_mark(it);
            r["errorKey"] = QString::fromStdString(it.error_key);
            r["unreadable"] = it.unreadable;
            rows_.push_back(r);
        }
        state_ = total_ > 0 ? QStringLiteral("ok") : (query_.isEmpty() ? QStringLiteral("empty") : QStringLiteral("noMatch"));
    }
    if (state_ != QLatin1String("ok")) {
        pageCount_ = 1;
        page_ = 0;
    }
    emit changed();
}
