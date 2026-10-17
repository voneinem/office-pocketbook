#include "attachment_bridge.h"

#include <QMetaObject>

#include "pbo/attachment_service.h"
#include "pbo/credentials_file.h"

AttachmentBridge::AttachmentBridge(Config config, QObject* parent) : QObject(parent), config_(std::move(config)) {}

AttachmentBridge::~AttachmentBridge() {
    if (config_.worker) config_.worker->wait_idle();   // the job uses members of this object
}

void AttachmentBridge::download(qlonglong attachmentId) {
    qlonglong expected = 0;
    if (!busy_id_.compare_exchange_strong(expected, attachmentId)) return;
    emit busyChanged();
    config_.worker->post([this, attachmentId] {
        QString key = QStringLiteral("ok");
        const auto account = config_.store->mail_account();
        if (!account) {
            key = QStringLiteral("not_configured");
        } else {
            const pbo::Credentials creds = pbo::load_credentials(config_.data_dir).value;
            auto transport = pbo::make_curl_imap_transport(config_.ca_file, config_.timeouts);
            pbo::ImapEndpoint ep;
            ep.host = account->imap_host;
            ep.port = account->imap_port;
            ep.security = account->imap_security;
            ep.username = account->username;
            pbo::Error e = transport->connect(ep, creds.mail_password);
            if (e.ok()) {
                e = pbo::download_attachment(*transport, *config_.store, attachmentId, static_cast<uint64_t>(account->attachment_limit), config_.downloads_dir);
                transport->disconnect();
            }
            if (!e.ok()) key = QString::fromStdString(e.key);
        }
        QMetaObject::invokeMethod(this, [this, attachmentId, key] {
            busy_id_ = 0;
            emit busyChanged();
            emit finished(attachmentId, key);
        }, Qt::QueuedConnection);
    });
}
