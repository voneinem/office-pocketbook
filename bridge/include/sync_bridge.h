#pragma once
// Sync control and global sync state (contracts/bridge-qml.md SyncBridge). run_sync runs on the worker thread; this
// object only starts it, cancels it and reports the result on the UI thread.

#include <QObject>
#include <QString>
#include <QTimer>
#include <atomic>
#include <string>

#include "pbo/imap_transport.h"
#include "pbo/store.h"
#include "pbo/sync_scheduler.h"
#include "pbo/tz.h"
#include "pbo/types.h"
#include "worker.h"

class SyncBridge : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool syncing READ syncing NOTIFY syncingChanged)
    Q_PROPERTY(QString lastSyncText READ lastSyncText NOTIFY stateChanged)
    // "" = no problem, else the cause key of the first problem of the last sync (auth_failed, cert_expired, ...)
    Q_PROPERTY(QString problemKey READ problemKey NOTIFY stateChanged)
    // Whether the reader has a network connection (set by the page that watches the device) and how many local changes and unsent
    // messages wait for the next sync (FR-033: shown on every page)
    Q_PROPERTY(bool online READ online NOTIFY onlineChanged)
    Q_PROPERTY(int pendingCount READ pendingCount NOTIFY stateChanged)
    // The last sync could not pull the calendar because the user's time zone is not known yet
    Q_PROPERTY(bool needsZone READ needsZone NOTIFY stateChanged)
    // The reader's clock differs from the time a server reported during the last sync by more than ten minutes (wrong date or time on the reader)
    // The mail on the reader is above the storage limit and nothing more may be removed (everything left is inside the window or has local work)
    Q_PROPERTY(bool storageFull READ storageFull NOTIFY stateChanged)
    Q_PROPERTY(bool clockWarning READ clockWarning NOTIFY stateChanged)
    Q_PROPERTY(int clockSkewMinutes READ clockSkewMinutes NOTIFY stateChanged)   // how far off, in minutes
    Q_PROPERTY(bool clockAhead READ clockAhead NOTIFY stateChanged)               // true: the reader's clock is ahead of the server's

public:
    struct Config {
        pbo::Store* store = nullptr;
        std::string data_dir;
        std::string ca_file;
        pbo::TransportTimeouts timeouts;
        Worker* worker = nullptr;
        pbo::Clock* clock = nullptr;
        const pbo::TzDatabase* tz = nullptr;   // the calendar step needs it; without it only mail is synchronised
        // How long after a start-up sync that ended offline the one retry waits (Wi-Fi shows "connected" before the network is
        // usable); 0 = no retry
        int startup_retry_ms = 15000;
        // Auto Sync after a change the user made on the reader (flags, move, delete, event save ...): the sync starts this long after the latest change
        // (changes close together share one) and not sooner than the gap after the latest sync (core/sync_scheduler.h)
        int auto_sync_quiet_ms = 4000;
        int auto_sync_gap_ms = 15000;
    };

    explicit SyncBridge(Config config, QObject* parent = nullptr);
    ~SyncBridge() override;

    bool syncing() const { return syncing_; }
    QString lastSyncText() const;
    QString problemKey() const;
    bool online() const { return online_; }
    int pendingCount() const;
    bool needsZone() const { return needsZone_; }
    bool storageFull() const;
    bool clockWarning() const;
    int clockSkewMinutes() const;
    bool clockAhead() const;

    Q_INVOKABLE void syncNow();   // ignored while a sync is running
    Q_INVOKABLE void cancel();
    // The sync at the start of the app (FR-030): starts one if there is an account and `online` (Wi-Fi is connected); it never
    // asks the device to connect. Only the first call in the life of this object can start anything. Returns whether it did.
    // If that sync ends offline (the network is not usable yet), it is tried once more after Config::startup_retry_ms, unless some
    // other sync reached the server in the meantime.
    Q_INVOKABLE bool startupSync(bool online);
    Q_INVOKABLE void setOnline(bool online);
    // The user changed something that has to reach the server (a flag, a move, a delete, an event ...). When Auto Sync is on (setting auto_sync, on by
    // default) and something is waiting, a sync starts a few seconds later. If the reader is offline, connectRequested() asks the owner of the device to
    // switch Wi-Fi on (once per change); connectResult(connected) is the answer. When the connection cannot be made the change waits for the next sync
    // (the status line shows the number of waiting changes). The sync at the start of the app never asks for a connection.
    Q_INVOKABLE void noteChange(bool online);
    Q_INVOKABLE void connectResult(bool connected);
    // Reads the state again after a local change (the pending count)
    Q_INVOKABLE void refreshState() { emit stateChanged(); }
    // Older mail of a folder on demand (the next 50 messages below the ones the app has). Ignored while a sync or another fetch
    // runs. The caller connects Wi-Fi first, like for a sync.
    Q_INVOKABLE void fetchOlder(qlonglong folderId);

signals:
    // outcome: "success" | "partial" | "failed" | "offline" | "cancelled" | "not_configured"
    void syncFinished(const QString& outcome);
    void syncingChanged();
    void onlineChanged();
    // key: "ok" or the cause key of the problem; added: how many messages came
    void olderFinished(const QString& key, int added);
    void stateChanged();
    void connectRequested();

private:
    struct CancelFlag : pbo::Cancel {
        std::atomic<bool> flag{false};
        bool requested() const override { return flag; }
    };

    void evaluateAutoSync();
    bool somethingWaiting() const;

    Config config_;
    pbo::SyncScheduler scheduler_;
    QTimer* autoTimer_ = nullptr;
    std::atomic<bool> syncing_{false};
    bool connectAsked_ = false;      // the device was asked to connect for the change(s) now armed
    bool connectOutstanding_ = false;
    bool startup_used_ = false;
    bool startup_running_ = false;   // the sync that runs now is the one of the app start
    bool reached_server_ = false;    // some sync since the start got an answer from the server (or failed for another reason)
    bool online_ = false;
    bool needsZone_ = false;
    CancelFlag cancel_;
};
