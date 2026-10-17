#pragma once
// Downloading one attachment on request (contracts/bridge-qml.md ReaderBridge.downloadAttachment, split out): connects on the
// worker thread, streams the part into its file (core/attachment_service.h) and reports the result key on the UI thread.
// Wi-Fi is switched on by the page before it calls download().

#include <QObject>
#include <QString>
#include <atomic>
#include <string>

#include "pbo/imap_transport.h"
#include "pbo/store.h"
#include "worker.h"

class AttachmentBridge : public QObject {
    Q_OBJECT
    Q_PROPERTY(qlonglong busyId READ busyId NOTIFY busyChanged)   // the attachment being downloaded, 0 = none
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)

public:
    struct Config {
        pbo::Store* store = nullptr;
        std::string data_dir;
        std::string ca_file;
        std::string downloads_dir;   // where downloaded attachments are saved (the reader's Downloads folder)
        pbo::TransportTimeouts timeouts;
        Worker* worker = nullptr;
    };

    explicit AttachmentBridge(Config config, QObject* parent = nullptr);
    ~AttachmentBridge() override;

    qlonglong busyId() const { return busy_id_; }
    bool busy() const { return busy_id_ != 0; }

    // One download at a time; a second call while busy is ignored.
    Q_INVOKABLE void download(qlonglong attachmentId);

signals:
    // key: "ok" or a cause key: too_large, not_found, auth_failed, cert_*, server_unreachable, timeout, connection_lost,
    // write_failed, storage_full, not_configured, ...
    void finished(qlonglong attachmentId, const QString& key);
    void busyChanged();

private:
    Config config_;
    std::atomic<qlonglong> busy_id_{0};
};
