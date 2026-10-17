#pragma once
// Account setup and tests (contracts/bridge-qml.md AccountBridge). Thin: validation and rules live in office_core
// (account_input.h, store, credentials_file). The connection test runs on the worker thread and answers with
// testResult; saving is a short local write and answers at once with saveResult.
//
// Form maps use these keys: imapHost, imapPort, imapSecurity ("tls" | "starttls"), smtpHost, smtpPort, smtpSecurity,
// username, password, smtpPassword (optional), address, displayName.

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <atomic>
#include <string>

#include "pbo/imap_transport.h"
#include "pbo/store.h"
#include "worker.h"

class AccountBridge : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool mailConfigured READ mailConfigured NOTIFY mailConfiguredChanged)
    // "enforced" | "not_enforced" | "unknown": whether the storage keeps the password file private, checked at run time
    Q_PROPERTY(QString protectionState READ protectionState NOTIFY protectionChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(bool calendarConfigured READ calendarConfigured NOTIFY calendarConfiguredChanged)
    // the calendars known after a sync: {id, name, kind ("caldav" | "feed"), shown, writable, present}
    Q_PROPERTY(QVariantList calendars READ calendars NOTIFY calendarsChanged)

public:
    struct Config {
        pbo::Store* store = nullptr;
        std::string data_dir;
        std::string ca_file;                 // the effective CA bundle (never empty)
        pbo::TransportTimeouts timeouts;
        Worker* worker = nullptr;
    };

    explicit AccountBridge(Config config, QObject* parent = nullptr);
    ~AccountBridge() override;

    bool mailConfigured() const;
    QString protectionState() const;
    bool busy() const { return busy_; }

    // Connects, verifies the certificate, logs in, and disconnects. Saves nothing.
    Q_INVOKABLE void testMail(const QVariantMap& form);
    // Validates, then stores the account and the password (credentials file). Nothing changes on a rejected value.
    Q_INVOKABLE void saveMail(const QVariantMap& form);
    // The stored account as a form map, without passwords (empty map when none).
    Q_INVOKABLE QVariantMap mailForm() const;

    // Calendar tab. Form keys: caldavUrl, username, password, feedUrl, feedName (a webcal:// feed address is accepted and becomes https://).
    bool calendarConfigured() const;
    QVariantList calendars() const;
    Q_INVOKABLE void testCalendar(const QVariantMap& form);   // answers with testResult("calendar", key, side) and calendarsFound
    Q_INVOKABLE void saveCalendar(const QVariantMap& form);   // answers with saveResult; nothing changes on a rejected value
    Q_INVOKABLE QVariantMap calendarForm() const;             // the stored account without the password
    Q_INVOKABLE void setCalendarShown(qlonglong id, bool shown);

signals:
    // area "mail"; key "ok" or the cause key (auth_failed, cert_expired, server_unreachable, ...) or "invalid_input"
    // with detailKey "<field>:<key>" naming the first rejected field.
    void testResult(const QString& area, const QString& key, const QString& detailKey);
    void saveResult(bool ok, const QString& field, const QString& key);
    void mailConfiguredChanged();
    void calendarConfiguredChanged();
    void calendarsChanged();
    // after a successful calendar test: what the server offers, {name, writable}
    void calendarsFound(const QVariantList& calendars);
    void protectionChanged();
    void busyChanged();

private:
    Config config_;
    std::atomic<bool> busy_{false};
};
