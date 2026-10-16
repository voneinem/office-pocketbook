#include "message_list_model.h"

#include <QDateTime>
#include <algorithm>
#include <QVariantMap>

namespace {

// Today: the time; this year: day and month; older: with the year. (Zone: see SyncBridge::lastSyncText.)
QString date_text(qint64 epoch) {
    const QDateTime when = QDateTime::fromSecsSinceEpoch(epoch).toLocalTime();
    const QDateTime now = QDateTime::currentDateTime();
    if (when.date() == now.date()) return when.toString(QStringLiteral("HH:mm"));
    if (when.date().year() == now.date().year()) return when.toString(QStringLiteral("dd.MM."));
    return when.toString(QStringLiteral("dd.MM.yyyy"));
}

}  // namespace

MessageListModel::MessageListModel(Config config, QObject* parent) : QObject(parent), config_(std::move(config)) {}

void MessageListModel::setPageSize(int n) {
    n = qBound(1, n, 100);
    if (n == pageSize_) return;
    pageSize_ = n;
    if (folder_ != 0) load();
    else emit changed();
}

void MessageListModel::openInbox() {
    folder_ = 0;
    for (const pbo::Folder& f : config_.store->folders())
        if (f.role == "inbox") {
            folder_ = f.id;
            break;
        }
    page_ = 0;
    load();
}

void MessageListModel::open(qlonglong folderId) {
    folder_ = folderId;
    page_ = 0;
    load();
}

void MessageListModel::goToPage(int n) {
    page_ = qBound(0, n, pageCount_ - 1);
    load();
}

void MessageListModel::setSort(const QString& key) {
    static const QStringList known = {QStringLiteral("date"), QStringLiteral("dateAsc"), QStringLiteral("sender"), QStringLiteral("subject")};
    if (!known.contains(key) || key == sort_) return;
    sort_ = key;
    page_ = 0;
    if (folder_ != 0) load();
    else emit changed();
}

void MessageListModel::search(const QString& query) {
    query_ = query.trimmed();
    searching_ = !query_.isEmpty();
    page_ = 0;
    load();
}

void MessageListModel::refresh() {
    if (searching_) {
        load();
        return;
    }
    if (folder_ == 0) {
        openInbox();   // the folder list may have appeared with the first sync
        return;
    }
    load();
}

QVariantMap MessageListModel::row_of(const pbo::MessageHeader& m) {
    QVariantMap row;
    row["id"] = static_cast<qlonglong>(m.id);
    row["sender"] = QString::fromStdString(m.from_name.empty() ? m.from_addr : m.from_name);
    row["address"] = QString::fromStdString(m.from_addr);
    row["subject"] = QString::fromStdString(m.subject);
    row["dateText"] = date_text(m.date_utc);
    row["unread"] = m.unread();
    row["flagged"] = (m.flags & pbo::kFlagFlagged) != 0;
    row["hasAttachments"] = m.has_attachments;
    row["pendingMark"] = m.local_state != "synced";
    row["errorKey"] = QString::fromStdString(m.local_error);   // for example "gone_on_server": the message was removed there, local work is kept
    return row;
}

void MessageListModel::load_search() {
    rows_.clear();
    results_ = config_.store->search(query_.toStdString(), 200);
    total_ = static_cast<int>(results_.size());
    pageCount_ = std::max(1, (total_ + pageSize_ - 1) / pageSize_);
    page_ = qBound(0, page_, pageCount_ - 1);
    const int from = page_ * pageSize_;
    for (int i = from; i < total_ && i < from + pageSize_; ++i) rows_.append(row_of(results_[static_cast<size_t>(i)]));
    emit changed();
}

void MessageListModel::load() {
    if (searching_) {
        load_search();
        return;
    }
    rows_.clear();
    errorKey_.clear();
    folderName_.clear();
    folderRole_.clear();
    olderOnServer_ = false;
    if (const auto f = folder_ != 0 ? config_.store->folder(folder_) : std::nullopt) {
        folderName_ = QString::fromStdString(f->name);
        folderRole_ = QString::fromStdString(f->role);
        olderOnServer_ = f->older_on_server;
    }
    if (folder_ == 0) {
        total_ = 0;
        pageCount_ = 1;
        page_ = 0;
        emit changed();
        return;
    }
    const pbo::SortKey key = sort_ == QLatin1String("dateAsc") ? pbo::SortKey::DateAsc
                             : sort_ == QLatin1String("sender") ? pbo::SortKey::SenderAsc
                             : sort_ == QLatin1String("subject") ? pbo::SortKey::SubjectAsc : pbo::SortKey::DateDesc;
    const pbo::Page<pbo::MessageHeader> p = config_.store->list_messages(folder_, key, page_, pageSize_);
    page_ = p.page;
    pageCount_ = p.page_count;
    total_ = p.total;
    for (const pbo::MessageHeader& m : p.items) rows_.append(row_of(m));
    emit changed();
}
