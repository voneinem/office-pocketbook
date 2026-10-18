#include "settings_bridge.h"

#include <QVariantMap>

SettingsBridge::SettingsBridge(Config config, QObject* parent) : QObject(parent), config_(std::move(config)) { account_ = config_.store->mail_account(); }

void SettingsBridge::refresh() {
    account_ = config_.store->mail_account();
    emit changed();
}

QVariantList SettingsBridge::addressBooks() const {
    QVariantList out;
    for (const pbo::AddressBook& b : config_.store->addressbooks()) {
        if (!b.present_on_server) continue;
        QVariantMap m;
        m["id"] = static_cast<qlonglong>(b.id);
        m["name"] = QString::fromStdString(b.name);
        m["shown"] = b.shown;
        m["writable"] = b.writable;
        out.push_back(m);
    }
    return out;
}

bool SettingsBridge::setAddressBookShown(qlonglong id, bool shown) {
    const pbo::Error e = config_.store->set_addressbook_shown(id, shown);
    error_ = e.ok() ? QString() : QString::fromStdString(e.key);
    emit changed();
    return e.ok();
}

bool SettingsBridge::autoSync() const { return config_.store->setting("auto_sync") != "0"; }

void SettingsBridge::setAutoSync(bool on) {
    config_.store->set_setting("auto_sync", on ? "1" : "0");
    error_.clear();
    emit changed();
}

bool SettingsBridge::save(const pbo::MailAccount& next, const char* field) {
    const pbo::Error e = config_.store->save_mail_account(next);
    if (!e.ok()) {
        error_ = e.key == "invalid_account" ? QString::fromLatin1(field) : QStringLiteral("write_failed");
        emit changed();
        return false;
    }
    error_.clear();
    account_ = config_.store->mail_account();
    emit changed();
    return true;
}

bool SettingsBridge::setSyncDays(int days) {
    if (!account_) {
        error_ = QStringLiteral("no_account");
        emit changed();
        return false;
    }
    pbo::MailAccount a = *account_;
    a.sync_days = days;
    return save(a, "sync_days");
}

bool SettingsBridge::setAttachmentLimitMib(int mib) {
    if (!account_) {
        error_ = QStringLiteral("no_account");
        emit changed();
        return false;
    }
    pbo::MailAccount a = *account_;
    a.attachment_limit = static_cast<int64_t>(mib) << 20;
    return save(a, "attachment_limit");
}

bool SettingsBridge::setStorageLimitMib(int mib) {
    if (!account_) {
        error_ = QStringLiteral("no_account");
        emit changed();
        return false;
    }
    pbo::MailAccount a = *account_;
    a.storage_limit = static_cast<int64_t>(mib) << 20;
    return save(a, "storage_limit");
}

void SettingsBridge::setSentByServer(bool on) {
    if (!account_) return;
    pbo::MailAccount a = *account_;
    a.sent_by_server = on;
    save(a, "sent_by_server");
}

QVariantList SettingsBridge::syncDaysChoices() const { return {7, 14, 30, 60, 90, 180, 365}; }
QVariantList SettingsBridge::attachmentLimitChoices() const { return {1, 5, 10, 25, 50, 100}; }
QVariantList SettingsBridge::storageLimitChoices() const { return {50, 100, 200, 500, 1000}; }

namespace {
int64_t* id_of_role(pbo::MailAccount& a, const QString& role) {
    if (role == QLatin1String("sent")) return &a.sent_folder_id;
    if (role == QLatin1String("drafts")) return &a.drafts_folder_id;
    if (role == QLatin1String("trash")) return &a.trash_folder_id;
    return nullptr;
}
}  // namespace

QVariantList SettingsBridge::specialFolders() const {
    QVariantList out;
    if (!account_) return out;
    pbo::MailAccount a = *account_;
    for (const char* role : {"sent", "drafts", "trash"}) {
        const int64_t chosen = *id_of_role(a, QString::fromLatin1(role));
        QVariantMap m;
        m["role"] = QString::fromLatin1(role);
        m["chosen"] = false;
        m["folderId"] = 0;
        m["name"] = QString();
        std::optional<pbo::Folder> use;
        if (chosen != 0) {
            use = config_.store->folder(chosen);
            m["chosen"] = use.has_value();
        }
        if (!use)
            for (const pbo::Folder& f : config_.store->folders())
                if (f.role == role) {
                    use = f;
                    break;
                }
        if (use) {
            m["folderId"] = static_cast<qlonglong>(use->id);
            m["name"] = QString::fromStdString(use->name);
        }
        out.append(m);
    }
    return out;
}

QVariantList SettingsBridge::folderChoices() const {
    QVariantList out;
    for (const pbo::Folder& f : config_.store->folders()) {
        QVariantMap m;
        m["id"] = static_cast<qlonglong>(f.id);
        m["name"] = QString::fromStdString(f.name);
        out.append(m);
    }
    return out;
}

bool SettingsBridge::setSpecialFolder(const QString& role, qlonglong folderId) {
    if (!account_) {
        error_ = QStringLiteral("no_account");
        emit changed();
        return false;
    }
    pbo::MailAccount a = *account_;
    int64_t* slot = id_of_role(a, role);
    if (!slot) {
        error_ = QStringLiteral("bad_role");
        emit changed();
        return false;
    }
    if (folderId != 0 && !config_.store->folder(folderId)) {
        error_ = QStringLiteral("bad_folder");
        emit changed();
        return false;
    }
    *slot = folderId;
    return save(a, "folder");
}

QString SettingsBridge::language() const {
    const std::string v = config_.store->setting("ui_language");
    return v == "en" || v == "de" ? QString::fromStdString(v) : QStringLiteral("auto");
}

QVariantList SettingsBridge::languageChoices() const {
    QVariantList out;
    for (const auto& c : {std::pair<const char*, const char*>{"auto", "Automatic / Automatisch"}, {"en", "English"}, {"de", "Deutsch"}}) {
        QVariantMap m;
        m["value"] = QString::fromLatin1(c.first);
        m["label"] = QString::fromUtf8(c.second);
        out.append(m);
    }
    return out;
}

bool SettingsBridge::setLanguage(const QString& value) {
    if (value != QLatin1String("auto") && value != QLatin1String("en") && value != QLatin1String("de")) return false;
    if (!config_.store->set_setting("ui_language", value.toStdString()).ok()) {
        error_ = QStringLiteral("write_failed");
        emit changed();
        return false;
    }
    emit languageChanged();
    emit changed();
    return true;
}
