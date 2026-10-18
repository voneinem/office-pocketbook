#include "sync_bridge.h"

#include <algorithm>

#include <QDateTime>
#include <QMetaObject>
#include <QTimer>
#include <utility>

#include "pbo/clock_check.h"
#include "pbo/credentials_file.h"
#include "pbo/curl_transport.h"
#include "pbo/mail_sync.h"
#include "pbo/uidvalidity_guard.h"
#include "pbo/sync.h"

namespace {

const char* outcome_text(pbo::SyncOutcome o) {
    switch (o) {
        case pbo::SyncOutcome::Success: return "success";
        case pbo::SyncOutcome::Partial: return "partial";
        case pbo::SyncOutcome::Offline: return "offline";
        case pbo::SyncOutcome::Cancelled: return "cancelled";
        default: return "failed";
    }
}

}  // namespace

SyncBridge::SyncBridge(Config config, QObject* parent)
    : QObject(parent), config_(std::move(config)), scheduler_(pbo::SyncScheduler::Options{config_.auto_sync_quiet_ms, config_.auto_sync_gap_ms}) {
    autoTimer_ = new QTimer(this);
    autoTimer_->setSingleShot(true);
    connect(autoTimer_, &QTimer::timeout, this, &SyncBridge::evaluateAutoSync);
}

SyncBridge::~SyncBridge() {
    cancel_.flag = true;
    if (config_.worker) config_.worker->wait_idle();   // the job uses members of this object
}

QString SyncBridge::lastSyncText() const {
    const pbo::SyncState s = config_.store->sync_state();
    if (!s.last_success) return QString();
    // The device clock's local time; the user's own time zone (FR-023) replaces this with the TimeZoneBridge.
    return QDateTime::fromSecsSinceEpoch(*s.last_success).toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm"));
}

QString SyncBridge::problemKey() const { return QString::fromStdString(config_.store->sync_state().problem_key); }
QString SyncBridge::problemArea() const { return QString::fromStdString(config_.store->sync_state().problem_area); }

bool SyncBridge::storageFull() const {
    const auto account = config_.store->mail_account();
    return account && account->storage_limit > 0 && config_.store->usage().database_bytes > account->storage_limit;
}

bool SyncBridge::clockWarning() const {
    const auto skew = config_.store->sync_state().clock_skew_s;
    return skew && pbo::clock_skew_is_large(*skew);
}
int SyncBridge::clockSkewMinutes() const { return pbo::clock_skew_minutes(config_.store->sync_state().clock_skew_s.value_or(0)); }
bool SyncBridge::clockAhead() const { return config_.store->sync_state().clock_skew_s.value_or(0) > 0; }

int SyncBridge::pendingCount() const { return config_.store->unsynced_count(); }

void SyncBridge::setOnline(bool online) {
    if (online == online_) return;
    online_ = online;
    emit onlineChanged();
}

void SyncBridge::cancel() { cancel_.flag = true; }

bool SyncBridge::somethingWaiting() const {
    if (!config_.store->pending_messages().empty() || !config_.store->pending_events().empty()) return true;
    for (const pbo::PendingContact& c : config_.store->pending_contacts())
        if (c.error_key.empty()) return true;   // (a change the server refused waits for the user, not for the next automatic sync)
    for (const pbo::OutboxItem& o : config_.store->outbox())
        if (o.state == "queued" || o.state == "sent_copy_pending") return true;
    return false;
}

void SyncBridge::noteChange(bool online) {
    online_ = online;
    connectAsked_ = false;
    scheduler_.note_saved(QDateTime::currentMSecsSinceEpoch());
    emit stateChanged();   // the number of waiting changes
    autoTimer_->start(std::max(1, config_.auto_sync_quiet_ms));
}

void SyncBridge::connectResult(bool connected) {
    if (!connectOutstanding_) return;
    connectOutstanding_ = false;
    online_ = connected;
    emit onlineChanged();
    autoTimer_->start(1);   // connected: the scheduler says Start; not connected: it drops (offline, nothing more to try)
}

void SyncBridge::evaluateAutoSync() {
    pbo::SyncScheduler::Inputs in;
    in.enabled = config_.store->setting("auto_sync") != "0";
    in.configured = config_.store->mail_account().has_value() || config_.store->calendar_account().has_value();
    in.running = syncing_;
    in.has_pending = somethingWaiting();
    in.online = online_;
    in.can_connect = !connectAsked_;
    const pbo::SyncScheduler::Step step = scheduler_.next(QDateTime::currentMSecsSinceEpoch(), in);
    if (step.decision == pbo::SyncScheduler::Decision::Wait) autoTimer_->start(static_cast<int>(std::max<int64_t>(1, step.wait_ms)));
    else if (step.decision == pbo::SyncScheduler::Decision::Start) syncNow();
    else if (step.decision == pbo::SyncScheduler::Decision::Connect) {
        connectAsked_ = true;
        connectOutstanding_ = true;
        emit connectRequested();
    }
    // Idle: nothing armed, or armed and waiting for the running sync (asked again when it ends)
}

bool SyncBridge::startupSync(bool online) {
    if (startup_used_) return false;
    startup_used_ = true;
    if (!online || syncing_ || (!config_.store->mail_account().has_value() && !config_.store->calendar_account().has_value())) return false;
    startup_running_ = true;
    syncNow();
    return syncing_;
}

void SyncBridge::syncNow() {
    if (syncing_.exchange(true)) return;
    scheduler_.sync_started(QDateTime::currentMSecsSinceEpoch());   // this sync reads the waiting changes now
    cancel_.flag = false;
    emit syncingChanged();
    config_.worker->post([this] {
        QString outcome;
        bool needs_zone = false;
        if (!config_.store->mail_account().has_value() && !config_.store->calendar_account().has_value()) {
            outcome = QStringLiteral("not_configured");
        } else {
            pbo::Credentials creds = pbo::load_credentials(config_.data_dir).value;
            auto transport = pbo::make_curl_imap_transport(config_.ca_file, config_.timeouts);
            pbo::CurlTransport http(config_.ca_file);
            auto smtp = pbo::make_curl_smtp_transport(config_.ca_file, config_.timeouts);
            pbo::SystemRandom random;
            pbo::SyncDeps deps{*config_.store, *transport, *config_.clock, cancel_};
            deps.smtp = smtp.get();   // the outbox and the draft mirror run before the pull
            deps.random = &random;
            if (config_.tz) {
                deps.http = &http;
                deps.tz = config_.tz;
            }
            const pbo::SyncReport report = pbo::run_sync(deps, creds);
            outcome = QString::fromLatin1(outcome_text(report.outcome));
            needs_zone = report.needs_zone;
        }
        QMetaObject::invokeMethod(this, [this, outcome, needs_zone] {
            needsZone_ = needs_zone;
            syncing_ = false;
            const bool was_startup = std::exchange(startup_running_, false);
            if (outcome != QLatin1String("offline") && outcome != QLatin1String("not_configured")) reached_server_ = true;
            emit syncingChanged();
            emit stateChanged();
            emit syncFinished(outcome);
            if (scheduler_.armed()) autoTimer_->start(1);   // a change was made while this sync ran: ask again
            if (was_startup && outcome == QLatin1String("offline") && config_.startup_retry_ms > 0)
                QTimer::singleShot(config_.startup_retry_ms, this, [this] {
                    if (!syncing_ && !reached_server_) syncNow();   // once: this sync is not a start-up sync
                });
        }, Qt::QueuedConnection);
    });
}

void SyncBridge::fetchOlder(qlonglong folderId) {
    if (syncing_.exchange(true)) return;
    cancel_.flag = false;
    emit syncingChanged();
    config_.worker->post([this, folderId] {
        QString key = QStringLiteral("ok");
        int added = 0;
        const auto account = config_.store->mail_account();
        const auto folder = config_.store->folder(folderId);
        if (!account || !folder) {
            key = QStringLiteral("not_configured");
        } else {
            pbo::Credentials creds = pbo::load_credentials(config_.data_dir).value;
            auto transport = pbo::make_curl_imap_transport(config_.ca_file, config_.timeouts);
            pbo::ImapEndpoint ep;
            ep.host = account->imap_host;
            ep.port = account->imap_port;
            ep.security = account->imap_security;
            ep.username = account->username;
            pbo::Error e = transport->connect(ep, creds.mail_password);
            if (e.ok()) {
                pbo::UidvalidityGuard guard;
                pbo::MailCounts counts;
                e = pbo::fetch_older(*transport, *config_.store, *folder, 50, cancel_, guard, &counts);
                added = counts.new_messages;
                transport->disconnect();
            }
            if (!e.ok()) key = QString::fromStdString(e.key);
        }
        QMetaObject::invokeMethod(this, [this, key, added] {
            syncing_ = false;
            emit syncingChanged();
            emit stateChanged();
            emit olderFinished(key, added);
        }, Qt::QueuedConnection);
    });
}
