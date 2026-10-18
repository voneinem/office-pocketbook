#pragma once
// run_sync: the ordered steps of one synchronisation (research R-18). Only mail_pull has behaviour in the
// walking skeleton; the other steps are listed so the report and the tests already have their final shape.
//
// Guarantees: a run that is interrupted at any point and run again ends in the same state as an uninterrupted run;
// a failing step is recorded and later independent steps still run; local data stays intact.

#include <functional>
#include <string>
#include <vector>

#include "pbo/calendar_sync.h"
#include "pbo/contacts_sync.h"
#include "pbo/credentials_file.h"
#include "pbo/draft_mirror.h"
#include "pbo/outbox.h"
#include "pbo/smtp_transport.h"
#include "pbo/imap_transport.h"
#include "pbo/mail_push.h"
#include "pbo/mail_sync.h"
#include "pbo/store.h"
#include "pbo/types.h"

namespace pbo {

struct SyncDeps {
    Store& store;
    ImapTransport& imap;
    Clock& clock;
    Cancel& cancel;
    // The calendar step needs these two; without them it is skipped.
    HttpTransport* http = nullptr;
    const TzDatabase* tz = nullptr;
    // The outbox step needs `smtp`, the draft mirror needs `random` (Message-IDs of conflict drafts); without them the steps are skipped.
    SmtpTransport* smtp = nullptr;
    Random* random = nullptr;
};

enum class SyncOutcome { Success, Partial, Failed, Offline, Cancelled };

struct SyncProgress {
    std::string step;   // "outbox", "mail_push", "mail_pull", "calendar_push", "calendar_pull", "contacts_push", "contacts_pull", "finish"
};

struct SyncOptions {
    bool mail = true;
    bool calendar = true;
    bool contacts = true;
    int64_t storage_limit_override = 0;   // bytes; 0 = the account's limit (a seam for tests: the account's limit cannot be below 20 MiB)
    bool push = true;       // false: only pull (what the user changed on the reader is left where it is)
    std::function<void(const SyncProgress&)> progress;
};

struct StepResult {
    std::string name;
    bool ran = false;        // false = nothing to do for this step (not configured, not implemented yet)
    Error error;
};

struct SyncReport {
    SyncOutcome outcome = SyncOutcome::Success;
    std::vector<StepResult> steps;
    MailCounts counts;
    CalendarCounts calendar_counts;
    PushCounts push_counts;
    ContactsCounts contacts_counts;
    ContactsPushCounts contacts_push_counts;
    OutboxCounts outbox_counts;
    DraftMirrorCounts mirror_counts;
    MessagePushCounts message_push_counts;
    int evicted = 0;            // old messages removed to stay below the storage limit
    bool storage_over = false;  // above the limit although everything that may go has gone: the protected mail alone is too big
    bool needs_zone = false;    // the calendar step waits for the user's choice of the time zone (not a problem)
    Error first_problem;
    std::string problem_area;   // "", "mail", "calendar", "outbox", "contacts"
};

// The step names in execution order.
const std::vector<std::string>& sync_step_names();

SyncReport run_sync(SyncDeps& deps, const Credentials& credentials, const SyncOptions& options = {});

}  // namespace pbo
