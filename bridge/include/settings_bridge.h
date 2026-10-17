#pragma once
// The options of the app (US6; contracts/bridge-qml.md SettingsBridge): Auto Sync, how many days of mail are kept in the window, the largest
// attachment that is downloaded, the storage the app may use, and whether the server files sent mail itself (Gmail, Outlook.com) so that the app does
// not upload a second copy. The values are validated by the core (account_input.cpp); a refused value changes nothing and names the field in errorKey.
// The time zone, the accounts and the About page have their own pages.

#include <QObject>
#include <QString>
#include <QVariantList>

#include "pbo/store.h"

class SettingsBridge : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool autoSync READ autoSync WRITE setAutoSync NOTIFY changed)
    Q_PROPERTY(bool hasMailAccount READ hasMailAccount NOTIFY changed)
    Q_PROPERTY(int syncDays READ syncDays NOTIFY changed)
    Q_PROPERTY(int attachmentLimitMib READ attachmentLimitMib NOTIFY changed)
    Q_PROPERTY(int storageLimitMib READ storageLimitMib NOTIFY changed)
    Q_PROPERTY(bool sentByServer READ sentByServer WRITE setSentByServer NOTIFY changed)
    // "" or the name of the field the last refused value belonged to: sync_days | attachment_limit | storage_limit | no_account | write_failed
    Q_PROPERTY(QString errorKey READ errorKey NOTIFY changed)
    Q_PROPERTY(QVariantList specialFolderList READ specialFolders NOTIFY changed)
    Q_PROPERTY(QVariantList syncDaysChoices READ syncDaysChoices CONSTANT)
    Q_PROPERTY(QVariantList attachmentLimitChoices READ attachmentLimitChoices CONSTANT)
    Q_PROPERTY(QVariantList storageLimitChoices READ storageLimitChoices CONSTANT)
    // the user's own certificate authority (custom-ca.pem in the data folder): "none" | "loaded" | "invalid" (the file was not used), and how many
    // certificates it contributed; verification stays on in every case
    // the language of the interface: "auto" (the reader's language) | "en" | "de"
    Q_PROPERTY(QString language READ language NOTIFY languageChanged)
    Q_PROPERTY(QVariantList languageChoices READ languageChoices CONSTANT)
    Q_PROPERTY(QString customCaStatus READ customCaStatus CONSTANT)
    Q_PROPERTY(int customCaCount READ customCaCount CONSTANT)

public:
    struct Config {
        pbo::Store* store = nullptr;
        QString custom_ca_status = QStringLiteral("none");
        int custom_ca_count = 0;
    };

    explicit SettingsBridge(Config config, QObject* parent = nullptr);

    bool autoSync() const;
    bool hasMailAccount() const { return account_.has_value(); }
    int syncDays() const { return account_ ? account_->sync_days : 30; }
    int attachmentLimitMib() const { return account_ ? static_cast<int>(account_->attachment_limit >> 20) : 10; }
    int storageLimitMib() const { return account_ ? static_cast<int>(account_->storage_limit >> 20) : 200; }
    bool sentByServer() const { return account_ && account_->sent_by_server; }
    QString errorKey() const { return error_; }
    QString language() const;
    QVariantList languageChoices() const;   // [{value, label}]; the labels are in their own language and are not translated
    Q_INVOKABLE bool setLanguage(const QString& value);   // false for a language that is not offered
    QString customCaStatus() const { return config_.custom_ca_status; }
    int customCaCount() const { return config_.custom_ca_count; }
    QVariantList syncDaysChoices() const;
    QVariantList attachmentLimitChoices() const;
    QVariantList storageLimitChoices() const;

    void setAutoSync(bool on);
    void setSentByServer(bool on);
    Q_INVOKABLE bool setSyncDays(int days);
    Q_INVOKABLE bool setAttachmentLimitMib(int mib);
    Q_INVOKABLE bool setStorageLimitMib(int mib);
    // The folders the app files sent mail, drafts and deleted mail in: [{role: "sent" | "drafts" | "trash", folderId, name, chosen}]. `name` is "" when no
    // such folder is known; `chosen` is true when the user picked it (else the server's special-use attribute or the folder name decided).
    Q_INVOKABLE QVariantList specialFolders() const;
    Q_INVOKABLE QVariantList folderChoices() const;   // every folder of the account: [{id, name}]
    // folderId 0 = automatic. Errors (errorKey): bad_role, bad_folder, no_account
    Q_INVOKABLE bool setSpecialFolder(const QString& role, qlonglong folderId);
    Q_INVOKABLE void refresh();

signals:
    void changed();
    void languageChanged();

private:
    bool save(const pbo::MailAccount& changed, const char* field);

    Config config_;
    std::optional<pbo::MailAccount> account_;
    QString error_;
};
