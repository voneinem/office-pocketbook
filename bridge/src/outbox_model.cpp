#include "outbox_model.h"

#include <QDateTime>
#include <QVariantMap>

#include "calendar_text.h"

OutboxModel::OutboxModel(Config config, QObject* parent) : QObject(parent), config_(std::move(config)) {}

void OutboxModel::refresh() {
    rows_.clear();
    attention_ = waiting_ = 0;
    if (config_.store) {
        for (const pbo::OutboxItem& o : config_.store->outbox()) {
            QVariantMap r;
            r["id"] = static_cast<qlonglong>(o.id);
            r["state"] = QString::fromStdString(o.state);
            const std::string& s = o.state;
            r["stateKey"] = QString::fromStdString(s == "queued" ? "waiting" : s == "sending" ? "sending" : s == "sent_copy_pending" ? "copy" : s);
            r["reasonKey"] = QString::fromStdString(o.error_key);
            r["subject"] = QString::fromStdString(o.subject_preview);
            QStringList to;
            for (const std::string& a : o.recipients) to << QString::fromStdString(a);
            r["recipients"] = to.join(QStringLiteral(", "));
            r["attempts"] = o.attempts;
            r["waiting"] = s == "queued";
            r["canRetry"] = s == "failed";
            r["canEdit"] = s == "failed" && o.draft_id != 0;
            r["canSendAgain"] = s == "uncertain";
            r["canMarkSent"] = s == "uncertain";
            r["canDelete"] = s == "failed" || s == "uncertain";
            if (s == "failed" || s == "uncertain") ++attention_;
            if (s == "queued") ++waiting_;
            rows_.push_back(r);
        }
    }
    emit changed();
}

bool OutboxModel::done(const QString& action, const pbo::Error& e) {
    refresh();
    emit actionResult(action, e.ok(), QString::fromStdString(e.key));
    return e.ok();
}

bool OutboxModel::retry(qlonglong id) { return done(QStringLiteral("retry"), config_.store->retry_outbox(id)); }
bool OutboxModel::remove(qlonglong id) { return done(QStringLiteral("remove"), config_.store->delete_outbox(id)); }
bool OutboxModel::sendAgain(qlonglong id) { return done(QStringLiteral("send_again"), config_.store->send_again(id)); }
bool OutboxModel::markSent(qlonglong id) { return done(QStringLiteral("mark_sent"), config_.store->mark_was_sent(id)); }

qlonglong OutboxModel::edit(qlonglong id) {
    const auto item = config_.store->outbox_item(id);
    const qlonglong draft = item ? item->draft_id : 0;
    const bool ok = done(QStringLiteral("edit"), config_.store->edit_outbox(id));
    return ok ? draft : 0;
}

// ---------------------------------------------------------------------------------------------- drafts

DraftsModel::DraftsModel(Config config, QObject* parent) : QObject(parent), config_(std::move(config)) {}

void DraftsModel::refresh() {
    rows_.clear();
    if (config_.store) {
        for (const pbo::Draft& d : config_.store->drafts()) {
            QVariantMap r;
            r["id"] = static_cast<qlonglong>(d.id);
            r["subject"] = QString::fromStdString(d.subject);
            r["to"] = QString::fromStdString(d.to_text);
            r["updatedText"] = caltext::short_stamp(d.updated);
            r["kind"] = QString::fromStdString(d.kind);
            r["pendingOnServer"] = d.server_state == "none" || d.server_state == "pending";
            rows_.push_back(r);
        }
    }
    emit changed();
}

bool DraftsModel::remove(qlonglong id) {
    const bool ok = config_.store && config_.store->delete_draft(id).ok();
    refresh();
    return ok;
}
