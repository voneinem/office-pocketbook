#include "reader_bridge.h"

#include <QDateTime>
#include <QFileInfo>
#include <QMetaObject>
#include <QStringList>

#include "pbo/credentials_file.h"
#include "pbo/curl_transport.h"
#include "pbo/image_service.h"

namespace {

QString from_text(const std::string& name, const std::string& addr) {
    if (name.empty()) return QString::fromStdString(addr);
    if (addr.empty()) return QString::fromStdString(name);
    return QString::fromStdString(name + " <" + addr + ">");
}

// The number of leading '>' of a line (spaces between them are allowed: "> > text").
int quote_level(const QString& line, int* text_start) {
    int level = 0, i = 0;
    while (i < line.size()) {
        if (line[i] == QLatin1Char('>')) { ++level; ++i; }
        else if (line[i] == QLatin1Char(' ') && level > 0 && i + 1 < line.size() && (line[i + 1] == QLatin1Char('>'))) ++i;
        else break;
    }
    if (i < line.size() && line[i] == QLatin1Char(' ') && level > 0) ++i;   // the one blank after the markers
    *text_start = i;
    return level;
}

// "<U+FFFC>12<U+FFFC>": the number of a picture, 0 for any other line
int picture_marker(const QString& line) {
    const QChar mark(0xFFFC);
    if (line.size() < 3 || line.front() != mark || line.back() != mark) return 0;
    bool ok = false;
    const int n = line.mid(1, line.size() - 2).toInt(&ok);
    return ok && n >= 1 && n <= 1000 ? n : 0;
}

}  // namespace

ReaderBridge::ReaderBridge(Config config, QObject* parent) : QObject(parent), config_(std::move(config)) {}

ReaderBridge::~ReaderBridge() {
    if (config_.worker) config_.worker->wait_idle();   // a picture job reports back to this object
}

QString ReaderBridge::sizeText(qint64 bytes) {
    if (bytes < 1000) return QString::number(bytes < 0 ? 0 : bytes) + QStringLiteral(" B");
    const char* units[] = {"KB", "MB", "GB"};
    double v = static_cast<double>(bytes);
    int u = -1;
    while (v >= 1000.0 && u < 2) {
        v /= 1000.0;
        ++u;
    }
    return QString::number(v, 'f', 1) + QLatin1Char(' ') + QLatin1String(units[u]);
}

void ReaderBridge::close() {
    currentId_ = 0;
    loaded_ = false;
    unread_ = false;
    flagged_ = pending_ = false;
    deleteKind_.clear();
    errorKey_.clear();
    subject_.clear();
    from_.clear();
    to_.clear();
    cc_.clear();
    dateText_.clear();
    bodyState_.clear();
    bodyNote_.clear();
    blocks_.clear();
    links_.clear();
    attachments_.clear();
    imageCount_ = remotePending_ = imagesToLoad_ = 0;
    emit changed();
}

void ReaderBridge::refresh() {
    if (currentId_ != 0) open(currentId_);
}

void ReaderBridge::open(qlonglong messageId) {
    close();
    currentId_ = messageId;
    const auto view = config_.store ? config_.store->message(messageId) : std::nullopt;
    if (!view) {
        errorKey_ = QStringLiteral("not_found");
        emit changed();
        return;
    }
    const pbo::MessageHeader& h = view->header;
    loaded_ = true;
    unread_ = h.unread();
    flagged_ = (h.flags & pbo::kFlagFlagged) != 0;
    pending_ = h.local_state != "synced";
    {
        bool in_trash = false, trash_exists = false;
        for (const pbo::Folder& f : config_.store->folders()) {
            if (f.role == "trash") trash_exists = true;
            if (f.id == h.folder_id && f.role == "trash") in_trash = true;
        }
        deleteKind_ = in_trash ? QStringLiteral("permanent_in_trash") : (trash_exists ? QStringLiteral("to_trash") : QStringLiteral("permanent_no_trash"));
    }
    subject_ = QString::fromStdString(h.subject);
    from_ = from_text(h.from_name, h.from_addr);
    to_ = QString::fromStdString(h.to_text);
    cc_ = QString::fromStdString(view->cc_text);
    // the device clock's local time; the user's own zone (FR-023) replaces this with the TimeZoneBridge
    dateText_ = h.date_utc > 0 ? QDateTime::fromSecsSinceEpoch(h.date_utc).toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm")) : QString();
    bodyState_ = QString::fromStdString(h.body_state);
    bodyNote_ = QString::fromStdString(view->body_note);

    // blocks: consecutive lines of one quote level; a blank line or a change of level starts a new block; a line that is a picture marker is a picture
    const QStringList lines = QString::fromStdString(view->body_text).split(QLatin1Char('\n'));
    const std::vector<pbo::StoredImage> images = config_.store->message_images(messageId);
    imageCount_ = static_cast<int>(images.size());
    for (const pbo::StoredImage& im : images) {
        if (im.kind == "remote" && (im.state == "pending" || im.state == "failed")) ++remotePending_;
        if (im.state == "pending" || im.state == "failed") ++imagesToLoad_;
    }
    QStringList current;
    int current_level = -1;
    auto flush = [&] {
        if (current_level >= 0 && !current.isEmpty()) {
            QVariantMap b;
            b["kind"] = QStringLiteral("text");
            b["text"] = current.join(QLatin1Char('\n'));
            b["quoted"] = current_level > 0;
            b["level"] = current_level;
            blocks_.append(b);
        }
        current.clear();
        current_level = -1;
    };
    if (h.body_state == "text") {
        for (const QString& line : lines) {
            int start = 0;
            const int level = quote_level(line, &start);
            QString text = line.mid(start);
            if (text.trimmed().isEmpty()) { flush(); continue; }
            if (const int n = picture_marker(text.trimmed()); n > 0) {
                flush();
                for (const pbo::StoredImage& im : images) {
                    if (im.idx != n) continue;
                    QVariantMap b;
                    b["kind"] = QStringLiteral("image");
                    b["index"] = n;
                    b["alt"] = QString::fromStdString(im.alt);
                    b["state"] = QString::fromStdString(im.state);
                    b["sourceKind"] = QString::fromStdString(im.kind);
                    b["source"] = im.state == "loaded" ? QStringLiteral("image://mailimg/%1/%2?v=%3").arg(messageId).arg(n).arg(im.size) : QString();
                    b["widthHint"] = im.width;
                    b["heightHint"] = im.height;
                    blocks_.append(b);
                    break;
                }
                continue;   // a marker without a picture row shows nothing
            }
            text.remove(QChar(0xFFFC));   // a marker inside a line is not a picture
            if (level != current_level) flush();
            current_level = level;
            current.append(text);
        }
        flush();
    }
    if (const auto account = config_.store->mail_account()) {
        const qint64 limit = account->attachment_limit;   // the limit is set in whole MiB: "10 MiB"
        limitText_ = (limit % (1 << 20) == 0) ? QString::number(limit >> 20) + QStringLiteral(" MiB") : sizeText(limit);
    }
    for (const std::string& l : view->links) links_.append(QString::fromStdString(l));
    for (const pbo::StoredAttachment& a : view->attachments) {
        QVariantMap m;
        m["id"] = static_cast<qlonglong>(a.id);
        m["name"] = QString::fromStdString(a.name);
        m["sizeText"] = sizeText(a.size);
        m["mimeType"] = QString::fromStdString(a.mime_type);
        m["state"] = QString::fromStdString(a.state);
        m["errorKey"] = QString::fromStdString(a.error_key);
        m["section"] = QString::fromStdString(a.section);
        m["savedName"] = a.saved_path.empty() ? QString() : QFileInfo(QString::fromStdString(a.saved_path)).fileName();
        attachments_.append(m);
    }
    emit changed();
}

bool ReaderBridge::act(const QString& action, const pbo::Error& e) {
    if (currentId_ != 0) refresh();
    emit actionResult(action, e.ok(), QString::fromStdString(e.key));
    return e.ok();
}

void ReaderBridge::markRead() { act(QStringLiteral("mark_read"), config_.store->set_flags_local(currentId_, pbo::kFlagSeen, 0)); }
void ReaderBridge::markUnread() { act(QStringLiteral("mark_unread"), config_.store->set_flags_local(currentId_, 0, pbo::kFlagSeen)); }
void ReaderBridge::setFlagged(bool on) {
    act(QStringLiteral("flag"), on ? config_.store->set_flags_local(currentId_, pbo::kFlagFlagged, 0) : config_.store->set_flags_local(currentId_, 0, pbo::kFlagFlagged));
}

QVariantList ReaderBridge::moveTargets() const {
    QVariantList out;
    const auto view = config_.store && currentId_ != 0 ? config_.store->message(currentId_) : std::nullopt;
    if (!view) return out;
    for (const pbo::Folder& f : config_.store->folders()) {
        if (f.id == view->header.folder_id || f.role == "drafts") continue;
        QVariantMap m;
        m["id"] = static_cast<qlonglong>(f.id);
        m["name"] = QString::fromStdString(f.name);
        m["role"] = QString::fromStdString(f.role);
        out.append(m);
    }
    return out;
}

bool ReaderBridge::moveTo(qlonglong folderId) { return act(QStringLiteral("move"), config_.store->request_move(currentId_, folderId)); }

bool ReaderBridge::deleteMessage(bool permanentConfirmed) { return act(QStringLiteral("delete"), config_.store->request_delete(currentId_, permanentConfirmed)); }

void ReaderBridge::loadImages() {
    if (!config_.worker || !config_.store || currentId_ == 0 || imagesToLoad_ == 0 || job_running_.exchange(true)) return;
    loadingImages_ = true;
    emit loadingImagesChanged();
    const qlonglong id = currentId_;
    config_.worker->post([this, id] {
        pbo::ImageLoadReport report;
        QString key = QStringLiteral("ok");
        bool any_cid = false, any_remote = false;
        for (const pbo::StoredImage& im : config_.store->message_images(id)) {
            if (im.state != "pending" && im.state != "failed") continue;
            (im.kind == "cid" ? any_cid : any_remote) = true;
        }
        if (any_cid) {   // parts of the message: from the mail server
            const auto account = config_.store->mail_account();
            if (!account) {
                key = QStringLiteral("not_configured");
            } else {
                const pbo::Credentials creds = pbo::load_credentials(config_.data_dir).value;
                std::unique_ptr<pbo::ImapTransport> imap = config_.imap_factory ? config_.imap_factory() : pbo::make_curl_imap_transport(config_.ca_file, config_.timeouts);
                pbo::ImapEndpoint ep;
                ep.host = account->imap_host;
                ep.port = account->imap_port;
                ep.security = account->imap_security;
                ep.username = account->username;
                pbo::Error e = imap->connect(ep, creds.mail_password);
                if (e.ok()) {
                    e = pbo::load_cid_images(*imap, *config_.store, id, pbo::ImageLimits{}, &report);
                    imap->disconnect();
                }
                if (!e.ok()) key = QString::fromStdString(e.key);
            }
        }
        if (any_remote && key == QLatin1String("ok")) {   // the sender's server, over https, only because the user asked
            std::unique_ptr<pbo::HttpTransport> http = config_.http_factory ? config_.http_factory() : std::unique_ptr<pbo::HttpTransport>(new pbo::CurlTransport(config_.ca_file));
            struct NoCancel : pbo::Cancel {
                bool requested() const override { return false; }
            } cancel;
            const pbo::Error e = pbo::load_remote_images(*http, *config_.store, id, pbo::ImageLimits{}, cancel, &report);
            if (!e.ok()) key = QString::fromStdString(e.key);
        }
        const int loaded = report.loaded, failed = report.failed + report.too_large;
        QMetaObject::invokeMethod(this, [this, id, key, loaded, failed] {
            job_running_ = false;
            loadingImages_ = false;
            emit loadingImagesChanged();
            if (currentId_ == id) refresh();
            emit imagesLoaded(key == QLatin1String("ok"), key, loaded, failed);
        }, Qt::QueuedConnection);
    });
}
