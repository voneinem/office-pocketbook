#include "account_bridge.h"

#include <QMetaObject>

#include "pbo/account_input.h"
#include "pbo/calendar_account_input.h"
#include "pbo/curl_transport.h"
#include "pbo/connection_test.h"
#include "pbo/credentials_file.h"
#include "pbo/log.h"
#include "pbo/provider_presets.h"

namespace {

pbo::MailAccountInput input_from(const QVariantMap& f) {
    pbo::MailAccountInput in;
    pbo::MailAccount& a = in.account;
    a.imap_host = f.value("imapHost").toString().trimmed().toStdString();
    a.imap_port = f.value("imapPort", 993).toInt();
    a.imap_security = f.value("imapSecurity").toString() == "starttls" ? pbo::Security::StartTls : pbo::Security::Tls;
    a.smtp_host = f.value("smtpHost").toString().trimmed().toStdString();
    a.smtp_port = f.value("smtpPort", 465).toInt();
    a.smtp_security = f.value("smtpSecurity").toString() == "starttls" ? pbo::Security::StartTls : pbo::Security::Tls;
    a.username = f.value("username").toString().toStdString();
    a.address = f.value("address").toString().trimmed().toStdString();
    a.display_name = f.value("displayName").toString().trimmed().toStdString();
    in.imap_password = f.value("password").toString().toStdString();
    in.smtp_password = f.value("smtpPassword").toString().toStdString();
    return in;
}

pbo::CalendarAccountInput calendar_input_from(const QVariantMap& f) {
    pbo::CalendarAccountInput in;
    in.caldav_url = f.value("caldavUrl").toString().toStdString();
    in.username = f.value("username").toString().toStdString();
    in.password = f.value("password").toString().toStdString();
    in.feed_url = f.value("feedUrl").toString().toStdString();
    in.feed_name = f.value("feedName").toString().toStdString();
    return in;
}

}  // namespace

AccountBridge::AccountBridge(Config config, QObject* parent) : QObject(parent), config_(std::move(config)) {}

AccountBridge::~AccountBridge() {
    if (config_.worker) config_.worker->wait_idle();   // a running test must not outlive the object it reports to
}

bool AccountBridge::mailConfigured() const { return config_.store && config_.store->mail_account().has_value(); }

QString AccountBridge::protectionState() const {
    switch (pbo::credentials_protection(config_.data_dir)) {
        case pbo::ProtectionStatus::Enforced: return QStringLiteral("enforced");
        case pbo::ProtectionStatus::NotEnforced: return QStringLiteral("not_enforced");
        default: return QStringLiteral("unknown");
    }
}

QVariantMap AccountBridge::mailForm() const {
    QVariantMap m;
    if (!config_.store) return m;
    const auto a = config_.store->mail_account();
    if (!a) return m;
    m["imapHost"] = QString::fromStdString(a->imap_host);
    m["imapPort"] = a->imap_port;
    m["imapSecurity"] = a->imap_security == pbo::Security::StartTls ? "starttls" : "tls";
    m["smtpHost"] = QString::fromStdString(a->smtp_host);
    m["smtpPort"] = a->smtp_port;
    m["smtpSecurity"] = a->smtp_security == pbo::Security::StartTls ? "starttls" : "tls";
    m["username"] = QString::fromStdString(a->username);
    m["address"] = QString::fromStdString(a->address);
    m["displayName"] = QString::fromStdString(a->display_name);
    return m;
}

void AccountBridge::testMail(const QVariantMap& form) {
    const pbo::MailAccountInput in = input_from(form);
    const auto errors = pbo::validate_mail_account(in);
    if (!errors.empty()) {
        emit testResult("mail", "invalid_input", QString::fromStdString(errors.front().field + ":" + errors.front().key));
        return;
    }
    if (busy_.exchange(true)) return;   // one test at a time
    emit busyChanged();
    const std::string ca = config_.ca_file;
    const pbo::TransportTimeouts timeouts = config_.timeouts;
    config_.worker->post([this, in, ca, timeouts] {
        auto imap = pbo::make_curl_imap_transport(ca, timeouts);
        auto smtp = pbo::make_curl_smtp_transport(ca, timeouts);
        const auto r = pbo::test_mail_connection(*imap, *smtp, in);
        const QString key = r.ok() ? QStringLiteral("ok") : QString::fromStdString(r.error.key);
        const QString side = QString::fromStdString(r.value.failed_side);   // "imap" or "smtp"
        QMetaObject::invokeMethod(this, [this, key, side] {
            busy_ = false;
            emit busyChanged();
            emit testResult("mail", key, side);
        }, Qt::QueuedConnection);
    });
}

void AccountBridge::saveMail(const QVariantMap& form) {
    const pbo::MailAccountInput in = input_from(form);
    const auto errors = pbo::validate_mail_account(in);
    if (!errors.empty()) {
        emit saveResult(false, QString::fromStdString(errors.front().field), QString::fromStdString(errors.front().key));
        return;
    }
    const bool was_configured = mailConfigured();
    // the first save applies the provider preset (E1); a later save keeps what is not on the form (the user's choices in Settings)
    pbo::Error e = config_.store->save_mail_account(pbo::merge_for_save(in.account, config_.store->mail_account()));
    if (e.ok()) {
        pbo::Credentials creds = pbo::load_credentials(config_.data_dir).value;   // keeps the calendar password
        creds.mail_password = in.imap_password;
        creds.smtp_password = in.smtp_password;
        e = pbo::save_credentials(config_.data_dir, creds);
    }
    if (!e.ok()) {
        emit saveResult(false, QString(), QString::fromStdString(e.key));
        return;
    }
    emit saveResult(true, QString(), QString());
    emit protectionChanged();
    if (!was_configured) emit mailConfiguredChanged();
}

// ---------------------------------------------------------------------------------------------- calendar

bool AccountBridge::calendarConfigured() const { return config_.store && config_.store->calendar_account().has_value(); }

QVariantList AccountBridge::calendars() const {
    QVariantList out;
    if (!config_.store) return out;
    for (const pbo::Calendar& c : config_.store->calendars()) {
        QVariantMap m;
        m["id"] = static_cast<qlonglong>(c.id);
        m["name"] = QString::fromStdString(c.name);
        m["kind"] = QString::fromStdString(c.kind);
        m["shown"] = c.shown;
        m["writable"] = c.writable;
        m["present"] = c.present_on_server;
        out.push_back(m);
    }
    return out;
}

QVariantMap AccountBridge::calendarForm() const {
    QVariantMap m;
    if (!config_.store) return m;
    const auto a = config_.store->calendar_account();
    if (!a) return m;
    m["caldavUrl"] = QString::fromStdString(a->caldav_url);
    m["username"] = QString::fromStdString(a->username);
    m["feedUrl"] = QString::fromStdString(a->feed_url);
    m["feedName"] = QString::fromStdString(a->feed_name);
    return m;
}

void AccountBridge::setCalendarShown(qlonglong id, bool shown) {
    if (config_.store && config_.store->set_calendar_shown(id, shown).ok()) emit calendarsChanged();
}

void AccountBridge::testCalendar(const QVariantMap& form) {
    const pbo::CalendarAccountInput in = pbo::normalize_calendar_account(calendar_input_from(form));
    const auto errors = pbo::validate_calendar_account(in);
    if (!errors.empty()) {
        emit testResult("calendar", "invalid_input", QString::fromStdString(errors.front().field + ":" + errors.front().key));
        return;
    }
    if (busy_.exchange(true)) return;
    emit busyChanged();
    const std::string ca = config_.ca_file;
    config_.worker->post([this, in, ca] {
        pbo::CurlTransport http(ca);
        const auto r = pbo::test_calendar_connection(http, in);
        const QString key = r.ok() ? QStringLiteral("ok") : QString::fromStdString(r.error.key);
        const QString side = QString::fromStdString(r.value.failed_side);   // "caldav" or "feed"
        QVariantList found;
        for (const pbo::CalendarInfo& c : r.value.calendars) {
            QVariantMap m;
            m["name"] = QString::fromStdString(c.name);
            m["writable"] = c.writable;
            found.push_back(m);
        }
        QMetaObject::invokeMethod(this, [this, key, side, found] {
            busy_ = false;
            emit busyChanged();
            emit calendarsFound(found);
            emit testResult("calendar", key, side);
        }, Qt::QueuedConnection);
    });
}

void AccountBridge::saveCalendar(const QVariantMap& form) {
    const pbo::CalendarAccountInput in = pbo::normalize_calendar_account(calendar_input_from(form));
    const auto errors = pbo::validate_calendar_account(in);
    if (!errors.empty()) {
        emit saveResult(false, QString::fromStdString(errors.front().field), QString::fromStdString(errors.front().key));
        return;
    }
    const bool was_configured = calendarConfigured();
    pbo::CalendarAccount a;
    a.caldav_url = in.caldav_url;
    a.username = in.username;
    a.feed_url = in.feed_url;
    a.feed_name = in.feed_name;
    pbo::Error e = config_.store->save_calendar_account(a);
    if (e.ok()) {
        pbo::Credentials creds = pbo::load_credentials(config_.data_dir).value;   // keeps the mail passwords
        creds.calendar_password = in.caldav_url.empty() ? std::string() : in.password;
        e = pbo::save_credentials(config_.data_dir, creds);
    }
    if (!e.ok()) {
        emit saveResult(false, QString(), QString::fromStdString(e.key));
        return;
    }
    emit saveResult(true, QString(), QString());
    emit protectionChanged();
    emit calendarsChanged();
    if (!was_configured) emit calendarConfiguredChanged();
}
