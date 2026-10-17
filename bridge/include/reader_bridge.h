#pragma once
// One message for reading (contracts/bridge-qml.md ReaderBridge). Reads the stored message on the UI thread (one row and
// its attachments: small), splits the text into blocks so quoted text can be styled, and formats sizes and dates.

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

#include <atomic>
#include <functional>
#include <memory>
#include <string>

#include "pbo/http_transport.h"
#include "pbo/imap_transport.h"
#include "pbo/store.h"
#include "worker.h"

class ReaderBridge : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool loaded READ loaded NOTIFY changed)
    // "" or a cause key: not_found
    Q_PROPERTY(QString errorKey READ errorKey NOTIFY changed)
    Q_PROPERTY(QString subject READ subject NOTIFY changed)
    Q_PROPERTY(QString from READ from NOTIFY changed)
    Q_PROPERTY(QString to READ to NOTIFY changed)
    Q_PROPERTY(QString cc READ cc NOTIFY changed)
    Q_PROPERTY(QString dateText READ dateText NOTIFY changed)
    // "text" | "unreadable" | "headers" (the body has not been fetched yet) | "too_large"
    Q_PROPERTY(QString bodyState READ bodyState NOTIFY changed)
    // "" | "truncated" | "charset_unsupported" | "no_readable_text" | "html_converted"
    Q_PROPERTY(QString bodyNote READ bodyNote NOTIFY changed)
    // In reading order: [{kind: "text", text, quoted, level}] paragraphs (quoted ones come from lines starting with ">") and
    // [{kind: "image", index, alt, state, sourceKind, source, widthHint, heightHint}] pictures of an HTML mail. `state` is pending | loaded | failed |
    // too_large | unavailable; `source` is the image://mailimg/ address once the picture is loaded, else "".
    Q_PROPERTY(QVariantList blocks READ blocks NOTIFY changed)
    // ["https://...", ...]: the targets of the [n] markers; shown as text, never opened
    Q_PROPERTY(QVariantList links READ links NOTIFY changed)
    // [{id, name, sizeText, mimeType, state, errorKey, section, savedName}]
    Q_PROPERTY(QVariantList attachments READ attachments NOTIFY changed)
    Q_PROPERTY(bool unread READ unread NOTIFY changed)
    Q_PROPERTY(bool flagged READ flagged NOTIFY changed)
    // a change of this message waits for the next sync
    Q_PROPERTY(bool pendingMark READ pendingMark NOTIFY changed)
    // what Delete does here: "to_trash" (moves to the Trash) | "permanent_in_trash" (the message is in the Trash) | "permanent_no_trash" (no Trash folder)
    Q_PROPERTY(QString deleteKind READ deleteKind NOTIFY changed)
    // the account's attachment limit as text ("10 MiB"), for the message shown when a download is refused for its size
    // pictures: how many the message has, how many are on the sender's server and not loaded (loading them tells the sender that the mail was read),
    // how many a tap on "Load pictures" would ask for (those, and parts of the message that were not loaded yet)
    Q_PROPERTY(int imageCount READ imageCount NOTIFY changed)
    Q_PROPERTY(int remotePending READ remotePending NOTIFY changed)
    Q_PROPERTY(int imagesToLoad READ imagesToLoad NOTIFY changed)
    Q_PROPERTY(bool loadingImages READ loadingImages NOTIFY loadingImagesChanged)
    Q_PROPERTY(QString attachmentLimitText READ attachmentLimitText NOTIFY changed)

public:
    struct Config {
        pbo::Store* store = nullptr;
        // for loading pictures (only on the user's tap; without a worker nothing is ever loaded)
        Worker* worker = nullptr;
        std::string data_dir;   // where the credentials file is
        std::string ca_file;
        pbo::TransportTimeouts timeouts;
        // test seams: the transports to use instead of the curl ones
        std::function<std::unique_ptr<pbo::HttpTransport>()> http_factory;
        std::function<std::unique_ptr<pbo::ImapTransport>()> imap_factory;
    };

    explicit ReaderBridge(Config config, QObject* parent = nullptr);
    ~ReaderBridge() override;

    bool loaded() const { return loaded_; }
    QString errorKey() const { return errorKey_; }
    QString subject() const { return subject_; }
    QString from() const { return from_; }
    QString to() const { return to_; }
    QString cc() const { return cc_; }
    QString dateText() const { return dateText_; }
    QString bodyState() const { return bodyState_; }
    QString bodyNote() const { return bodyNote_; }
    QVariantList blocks() const { return blocks_; }
    QVariantList links() const { return links_; }
    QVariantList attachments() const { return attachments_; }
    bool unread() const { return unread_; }
    bool flagged() const { return flagged_; }
    bool pendingMark() const { return pending_; }
    QString deleteKind() const { return deleteKind_; }
    QString attachmentLimitText() const { return limitText_; }
    int imageCount() const { return imageCount_; }
    int remotePending() const { return remotePending_; }
    int imagesToLoad() const { return imagesToLoad_; }
    bool loadingImages() const { return loadingImages_; }

    Q_INVOKABLE void open(qlonglong messageId);
    Q_INVOKABLE void close();
    // Reloads the open message (after a download changed an attachment row).
    Q_INVOKABLE void refresh();

    // What the user does with the open message (US5). Each answers with actionResult(action, ok, key) and refreshes the properties.
    Q_INVOKABLE void markRead();
    Q_INVOKABLE void markUnread();
    Q_INVOKABLE void setFlagged(bool on);
    // The folders the message can be moved to: [{id, name}] (not the one it is in, not Drafts).
    Q_INVOKABLE QVariantList moveTargets() const;
    Q_INVOKABLE bool moveTo(qlonglong folderId);
    // Delete. A delete outside the Trash moves the message to the Trash; in the Trash, or without a Trash folder, it is permanent and needs
    // `permanentConfirmed` (a second question on the page). Returns false with actionResult key "confirm_required" / "no_trash" when it was not given.
    Q_INVOKABLE bool deleteMessage(bool permanentConfirmed);

    // Loads the pictures of the open message that are not loaded yet: the parts of the message from the server and the pictures on the sender's server,
    // each by one plain GET over TLS (no credentials, no cookies, no referrer). Only ever called by the user's tap. Answers with
    // imagesLoaded(ok, key, loaded, failed); the blocks then show what arrived. Wi-Fi must be on (the page connects first).
    Q_INVOKABLE void loadImages();

    // "123 B", "2.5 KB", "1.3 MB" (decimal units, one decimal)
    static QString sizeText(qint64 bytes);

signals:
    void changed();
    void actionResult(const QString& action, bool ok, const QString& key);
    void loadingImagesChanged();
    void imagesLoaded(bool ok, const QString& key, int loaded, int failed);

private:
    bool act(const QString& action, const pbo::Error& e);

    Config config_;
    bool loaded_ = false, unread_ = false, flagged_ = false, pending_ = false;
    int imageCount_ = 0, remotePending_ = 0, imagesToLoad_ = 0;
    bool loadingImages_ = false;
    std::atomic<bool> job_running_{false};
    QString deleteKind_;
    qlonglong currentId_ = 0;
    QString limitText_;
    QString errorKey_, subject_, from_, to_, cc_, dateText_, bodyState_, bodyNote_;
    QVariantList blocks_, links_, attachments_;
};
