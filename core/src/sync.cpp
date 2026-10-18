#include "pbo/sync.h"

#include <algorithm>
#include <cstdlib>
#include <vector>

#include "pbo/log.h"

namespace pbo {

namespace {

SyncOutcome outcome_for(const Error& e) {
    if (e.key == "no_network" || e.key == "server_unreachable") return SyncOutcome::Offline;
    return SyncOutcome::Failed;
}

const char* state_name(SyncOutcome o) {
    switch (o) {
        case SyncOutcome::Success: return "success";
        case SyncOutcome::Partial: return "partial";
        case SyncOutcome::Offline: return "offline";
        case SyncOutcome::Cancelled: return "cancelled";
        default: return "failed";
    }
}

}  // namespace

const std::vector<std::string>& sync_step_names() {
    static const std::vector<std::string> names = {"outbox", "mail_push", "mail_pull", "calendar_push", "calendar_pull", "contacts_push", "contacts_pull", "finish"};
    return names;
}

SyncReport run_sync(SyncDeps& deps, const Credentials& credentials, const SyncOptions& options) {
    SyncReport report;
    for (const std::string& n : sync_step_names()) report.steps.push_back({n, false, {}});
    auto step = [&](const char* name) -> StepResult& {
        for (StepResult& s : report.steps)
            if (s.name == name) return s;
        return report.steps.back();
    };
    auto progress = [&](const char* name) {
        if (options.progress) options.progress({name});
    };

    // bodies read by an older version are read again once (the format number says how bodies are read)
    if (deps.store.setting("body_format") != "2") {
        deps.store.mark_bodies_for_refetch();
        deps.store.set_setting("body_format", "2");
    }
    deps.store.repair_encoded_headers();   // rows stored by an earlier version (undecoded RFC 2047 words)
    const SyncState before = deps.store.sync_state();
    SyncState state = before;
    state.outcome = "running";
    state.last_started = deps.clock.now();
    state.problem_key.clear();
    state.problem_area.clear();
    deps.store.write_sync_state(state);

    bool cancelled = false;
    bool partial = false;
    const auto account = deps.store.mail_account();
    UidvalidityGuard push_guard;   // one per sync: the Sent copy and the draft mirror write by UID
    if (options.push && options.mail && account && deps.smtp && !deps.cancel.requested()) {
        progress("outbox");
        StepResult& s = step("outbox");
        s.ran = true;
        OutboxDeps od{deps.store, *deps.smtp, deps.imap, deps.clock, deps.cancel, push_guard};
        s.error = run_outbox(od, credentials, &report.outbox_counts);   // what happens to a message is on its row; only the store can fail here
        if (!s.error.ok()) {
            report.first_problem = s.error;
            report.problem_area = "outbox";
        }
    }
    if (options.push && options.mail && account && (deps.random || !deps.store.pending_messages().empty()) && !deps.cancel.requested()) {
        progress("mail_push");
        StepResult& s = step("mail_push");
        s.ran = true;
        if (deps.random) {
            DraftMirrorDeps md{deps.store, deps.imap, deps.clock, deps.cancel, *deps.random, push_guard};
            s.error = run_draft_mirror(md, credentials, &report.mirror_counts);
            if (!s.error.ok() && report.first_problem.ok()) {
                report.first_problem = s.error;
                report.problem_area = "outbox";
            }
        }
        // the user's flags, moves and deletes; the connection problems of this step are reported by the pull that follows
        MessagePushDeps pd{deps.store, deps.imap, deps.cancel, push_guard};
        const Error pe = push_message_changes(pd, credentials, &report.message_push_counts);
        if (!pe.ok() && s.error.ok() && pe.cls != ErrorClass::Cancelled) s.error = pe;
    }
    if (options.mail && account) {
        progress("mail_pull");
        StepResult& s = step("mail_pull");
        s.ran = true;
        ImapEndpoint ep;
        ep.host = account->imap_host;
        ep.port = account->imap_port;
        ep.security = account->imap_security;
        ep.username = account->username;
        Error e;
        if (deps.cancel.requested()) {
            e = make_error(ErrorClass::Cancelled, "cancelled", "the sync was cancelled");
        } else {
            e = deps.imap.connect(ep, credentials.mail_password);
            if (e.ok()) {
                e = sync_folder_list(deps.imap, deps.store, &report.counts);
                if (e.ok()) {
                    // INBOX first, then the other folders that are switched on; one folder's problem does not stop the rest
                    std::vector<Folder> todo;
                    for (const Folder& f : deps.store.folders())
                        if (f.sync_enabled) todo.push_back(f);
                    std::stable_sort(todo.begin(), todo.end(), [](const Folder& a, const Folder& b) { return (a.role == "inbox") > (b.role == "inbox"); });
                    UidvalidityGuard guard;
                    int ok_folders = 0, failed_folders = 0;
                    for (const Folder& f : todo) {
                        Error fe = pull_folder(deps.imap, deps.store, f, account->sync_days, deps.clock, deps.cancel, guard, &report.counts);
                        if (fe.ok()) {
                            ++ok_folders;
                            continue;
                        }
                        if (fe.cls == ErrorClass::Cancelled || fe.cls == ErrorClass::Network || fe.cls == ErrorClass::Tls || fe.cls == ErrorClass::Auth ||
                            fe.cls == ErrorClass::Storage) {
                            e = fe;   // the connection or the database is the problem: the other folders would fail the same way
                            break;
                        }
                        ++failed_folders;
                        if (e.ok()) e = fe;   // the first folder problem is the one reported
                    }
                    if (e.ok() == false && e.cls != ErrorClass::Cancelled && failed_folders > 0 && ok_folders > 0 && e.cls == ErrorClass::Protocol) partial = true;
                }
                deps.imap.disconnect();
            }
        }
        s.error = e;
        if (e.cls == ErrorClass::Cancelled) {
            cancelled = true;
        } else if (!e.ok()) {
            report.first_problem = e;
            report.problem_area = "mail";
        }
    }
    // the window: when the user shortened the days of mail kept since the last sync, the mail outside the new window goes (never anything with local work).
    // A longer window needs nothing here: the pull above already fetched the added days. Only a shortening trims, so older mail the user asked for stays.
    if (options.mail && account && step("mail_pull").ran && step("mail_pull").error.ok() && !cancelled) {
        const std::string applied = deps.store.setting("window_days_applied");
        const int previous = applied.empty() ? account->sync_days : std::atoi(applied.c_str());
        if (account->sync_days < previous) {
            int removed = 0;
            const Error te = deps.store.trim_to_window(deps.clock.now() - static_cast<int64_t>(account->sync_days) * 86400, 0, &removed);
            if (te.ok() && removed > 0) log::event("store.trimmed", {log::count("deleted", removed)});
            if (te.ok()) deps.store.set_setting("window_days_applied", std::to_string(account->sync_days));
        } else if (applied != std::to_string(account->sync_days)) {
            deps.store.set_setting("window_days_applied", std::to_string(account->sync_days));
        }
    }
    // the space: above the storage limit the oldest mail outside the window goes (never anything with local work)
    if (options.mail && account && step("mail_pull").ran && !cancelled) {
        const int64_t window_start = deps.clock.now() - static_cast<int64_t>(account->sync_days) * 86400;
        const Store::Eviction ev = deps.store.evict_to_limit(options.storage_limit_override > 0 ? options.storage_limit_override : account->storage_limit, window_start);
        report.evicted = ev.deleted;
        report.storage_over = ev.over_limit;
        if (ev.deleted > 0 || ev.over_limit) log::event("store.evicted", {log::count("deleted", ev.deleted), log::count("over_limit", ev.over_limit ? 1 : 0)});
    }
    // calendar: the server calendars and the feed
    bool calendar_ok = false;
    const bool mail_ran = step("mail_pull").ran;
    const bool mail_ok = mail_ran && step("mail_pull").error.ok();
    bool push_failed = false;
    if (options.push && options.calendar && deps.http && deps.tz && !cancelled && deps.store.calendar_account() && !deps.store.pending_events().empty()) {
        progress("calendar_push");
        StepResult& s = step("calendar_push");
        s.ran = true;
        CalendarSyncDeps cd{deps.store, *deps.http, *deps.tz, deps.clock, deps.cancel};
        const Error e = deps.cancel.requested() ? make_error(ErrorClass::Cancelled, "cancelled", "the sync was cancelled")
                                                : push_events(cd, credentials.calendar_password, &report.push_counts);
        s.error = e;
        if (e.cls == ErrorClass::Cancelled) {
            cancelled = true;
        } else if (!e.ok()) {
            push_failed = true;
            if (report.first_problem.ok()) {
                report.first_problem = e;
                report.problem_area = "calendar";
            }
        }
    }
    if (options.calendar && deps.http && deps.tz && !cancelled && deps.store.calendar_account()) {
        progress("calendar_pull");
        StepResult& s = step("calendar_pull");
        s.ran = true;
        CalendarSyncDeps cd{deps.store, *deps.http, *deps.tz, deps.clock, deps.cancel};
        const Error e = deps.cancel.requested() ? make_error(ErrorClass::Cancelled, "cancelled", "the sync was cancelled")
                                                : pull_calendars(cd, credentials.calendar_password, &report.calendar_counts);
        s.error = e;
        if (e.cls == ErrorClass::Cancelled) {
            cancelled = true;
        } else if (e.key == "zone_unresolved") {
            report.needs_zone = true;   // the screen asks for the zone; the sync itself did not fail
        } else if (!e.ok()) {
            if (report.first_problem.ok()) {
                report.first_problem = e;
                report.problem_area = "calendar";
            }
        } else {
            calendar_ok = true;
        }
    }
    const bool calendar_failed = step("calendar_pull").ran && !step("calendar_pull").error.ok() && !report.needs_zone && step("calendar_pull").error.cls != ErrorClass::Cancelled;
    // contacts: the address books of the calendar server account (after the calendar, so that a contacts problem never delays mail or events)
    bool contacts_ok = false, contacts_push_failed = false;
    {
        const auto cal_account = deps.store.calendar_account();
        if (options.push && options.contacts && deps.http && !cancelled && cal_account && !cal_account->caldav_url.empty() && !deps.store.pending_contacts().empty()) {
            progress("contacts_push");
            StepResult& s = step("contacts_push");
            s.ran = true;
            ContactsSyncDeps cd{deps.store, *deps.http, deps.clock, deps.cancel};
            const Error e = deps.cancel.requested() ? make_error(ErrorClass::Cancelled, "cancelled", "the sync was cancelled")
                                                    : push_contacts(cd, credentials.calendar_password, &report.contacts_push_counts);
            s.error = e;
            if (e.cls == ErrorClass::Cancelled) {
                cancelled = true;
            } else if (!e.ok()) {
                contacts_push_failed = true;
                if (report.first_problem.ok()) {
                    report.first_problem = e;
                    report.problem_area = "contacts";
                }
            }
        }
        if (options.contacts && deps.http && !cancelled && cal_account && !cal_account->caldav_url.empty()) {
            progress("contacts_pull");
            StepResult& s = step("contacts_pull");
            s.ran = true;
            ContactsSyncDeps cd{deps.store, *deps.http, deps.clock, deps.cancel};
            const Error e = deps.cancel.requested() ? make_error(ErrorClass::Cancelled, "cancelled", "the sync was cancelled")
                                                    : pull_contacts(cd, credentials.calendar_password, &report.contacts_counts);
            s.error = e;
            if (e.cls == ErrorClass::Cancelled) {
                cancelled = true;
            } else if (!e.ok()) {
                if (report.first_problem.ok()) {
                    report.first_problem = e;
                    report.problem_area = "contacts";
                }
            } else {
                contacts_ok = true;
            }
        }
    }
    const bool contacts_failed = (step("contacts_pull").ran && !step("contacts_pull").error.ok() && step("contacts_pull").error.cls != ErrorClass::Cancelled) || contacts_push_failed;
    // something worked and something did not: partial
    if ((calendar_ok && push_failed) || (mail_ok && calendar_failed) || (calendar_ok && !mail_ok && mail_ran && step("mail_pull").error.cls != ErrorClass::Cancelled)) partial = true;
    if (contacts_failed && (mail_ok || calendar_ok || contacts_ok)) partial = true;
    if (contacts_ok && !contacts_push_failed && ((mail_ran && !mail_ok && step("mail_pull").error.cls != ErrorClass::Cancelled) || calendar_failed || push_failed)) partial = true;
    progress("finish");
    step("finish").ran = true;

    report.outcome = cancelled ? SyncOutcome::Cancelled : (report.first_problem.ok() ? SyncOutcome::Success : (partial ? SyncOutcome::Partial : outcome_for(report.first_problem)));

    state = deps.store.sync_state();
    state.last_finished = deps.clock.now();
    if (cancelled) {   // the user stopped it: the state before this run stays what the screen shows
        state.outcome = before.outcome == "running" ? "never" : before.outcome;
        state.problem_key = before.problem_key;
        state.problem_area = before.problem_area;
    } else {
        state.outcome = state_name(report.outcome);
        state.problem_key = report.first_problem.key;
        state.problem_area = report.problem_area;
        if (report.outcome == SyncOutcome::Success) state.last_success = deps.clock.now();
        state.counts_json = "{\"folders\":" + std::to_string(report.counts.folders) + ",\"new_messages\":" + std::to_string(report.counts.new_messages) + ",\"removed\":" + std::to_string(report.counts.removed) + ",\"flags_updated\":" + std::to_string(report.counts.flags_updated) +
                            ",\"sent\":" + std::to_string(report.outbox_counts.sent) + ",\"events\":" + std::to_string(report.calendar_counts.changed + report.calendar_counts.feed_events) + ",\"events_removed\":" + std::to_string(report.calendar_counts.removed) +
                            ",\"events_sent\":" + std::to_string(report.push_counts.created + report.push_counts.updated + report.push_counts.deleted) + ",\"event_conflicts\":" + std::to_string(report.push_counts.conflicts) +
                            ",\"contacts\":" + std::to_string(report.contacts_counts.changed) + ",\"contacts_removed\":" + std::to_string(report.contacts_counts.removed) +
                            ",\"contacts_sent\":" + std::to_string(report.contacts_push_counts.created + report.contacts_push_counts.updated + report.contacts_push_counts.deleted) +
                            ",\"contact_conflicts\":" + std::to_string(report.contacts_push_counts.conflicts) + "}";
    }
    deps.store.write_sync_state(state);
    log::event("sync.finished", {log::text("outcome", state_name(report.outcome)), log::count("new_messages", report.counts.new_messages),
                                 log::count("flags_sent", report.message_push_counts.flags), log::count("moved", report.message_push_counts.moved), log::count("deleted", report.message_push_counts.deleted),
                                 log::count("push_errors", report.message_push_counts.errors + report.push_counts.refused), log::count("events_sent", report.push_counts.created + report.push_counts.updated + report.push_counts.deleted),
                                 log::count("event_conflicts", report.push_counts.conflicts),
                                 log::text("problem", report.first_problem.key), log::text("area", report.problem_area), log::count("events", report.calendar_counts.changed + report.calendar_counts.feed_events),
                                 log::text("cause", report.first_problem.detail)});
    return report;
}

}  // namespace pbo
