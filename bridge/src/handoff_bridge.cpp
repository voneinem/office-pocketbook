#include "handoff_bridge.h"

#include <QMetaObject>

#include "pbo/compose.h"
#include "pbo/log.h"

namespace {

// Characters of valid UTF-8 (every byte that is not a continuation byte).
long characters(const std::string& s) {
    long n = 0;
    for (unsigned char c : s)
        if ((c & 0xc0) != 0x80) ++n;
    return n;
}

const char* refusal_reason(pbo::HandoffRefusal r) {
    switch (r) {
        case pbo::HandoffRefusal::TooLong: return "too_long";
        case pbo::HandoffRefusal::Incomplete: return "incomplete";
        default: return "malformed";
    }
}

}  // namespace

HandoffBridge::HandoffBridge(Config config, QObject* parent)
    : QObject(parent), config_(std::move(config)), inbox_(config_.handoff_dir, *config_.clock) {}

void HandoffBridge::deliver(std::vector<std::string> args, const char* source) {
    QMetaObject::invokeMethod(
        this, [this, args = std::move(args), src = std::string(source)]() { process(args, src.c_str(), std::string()); }, Qt::QueuedConnection);
}

void HandoffBridge::checkInbox() {
    if (config_.handoff_dir.empty()) return;
    int expired = 0;
    const std::vector<pbo::PendingHandoff> ready = inbox_.take_ready(&expired);
    if (expired > 0) pbo::log::event("handoff.expired", {pbo::log::count("count", expired)});
    for (const pbo::PendingHandoff& p : ready) {
        if (p.refused) {
            pbo::log::event("handoff.refused", {pbo::log::text("reason", "malformed")});
            tell("malformed");
            consume(p.id);
            continue;
        }
        std::vector<std::string> args;
        args.reserve(p.args.size() + 1);
        args.push_back(pbo::kHandoffMarker);   // the launcher keeps the arguments after the marker
        for (const std::string& a : p.args) args.push_back(a);
        process(args, "file", p.id);
    }
}

void HandoffBridge::consume(const std::string& entry_id) {
    if (!entry_id.empty()) inbox_.remove(entry_id);
}

bool HandoffBridge::process(const std::vector<std::string>& args, const char* source, const std::string& entry_id) {
    const pbo::HandoffResult r = pbo::parse_handoff(args);
    if (!r.is_handoff) {   // an ordinary start
        consume(entry_id);
        return true;
    }
    if (r.refusal != pbo::HandoffRefusal::None) {
        pbo::log::event("handoff.refused", {pbo::log::text("reason", refusal_reason(r.refusal))});
        tell(refusal_reason(r.refusal));
        consume(entry_id);
        return true;
    }
    const pbo::Handoff& h = r.handoff;
    if (h.empty()) {   // nothing to make a message of (FR-014): no message, no notice
        consume(entry_id);
        return true;
    }
    const int mask = (h.text.empty() ? 0 : 1) | (h.subject.empty() ? 0 : 2) | (h.to.empty() ? 0 : 4);
    pbo::log::event("handoff.received", {pbo::log::count("text_chars", characters(h.text)), pbo::log::count("fields", mask), pbo::log::text("source", source)});

    const auto account = config_.store ? config_.store->mail_account() : std::nullopt;
    if (!account) {
        // A draft keeps the account's address in its Message-ID, so a mail account is needed (FR-008). While the account set-up page is open the hand-over
        // waits in memory (never on disk: the entry goes) and continues when the account is saved; anywhere else the user is told at once.
        if (setup_active_ && held_.size() < 8) held_.push_back(h);
        else tell("no_account");
        consume(entry_id);
        return true;
    }
    return make_draft(h, account->address, entry_id);
}

bool HandoffBridge::make_draft(const pbo::Handoff& h, const std::string& address, const std::string& entry_id) {
    pbo::Draft d;
    d.kind = "new";
    d.message_id = pbo::make_message_id(*config_.random, address);
    d.to_text = h.to;
    d.subject = h.subject;
    d.body = h.text;
    const auto saved = config_.store->save_draft(d);
    if (!saved.ok()) return false;   // the entry stays: the next look tries again
    pbo::log::event("handoff.drafted", {pbo::log::count("count", 1)});
    consume(entry_id);

    const int more = static_cast<int>(unshown_.size());
    unshown_.push_back(static_cast<qlonglong>(saved.value));
    emit waitingChanged();
    emit show(static_cast<qlonglong>(saved.value), more);
    for (pbo::HandoffNotice n : h.notices)   // what was left out, told after the editor is open
        tell(n == pbo::HandoffNotice::SubjectTooLong ? "subject_too_long" : "recipient_ignored");
    return true;
}

void HandoffBridge::accountReady() {
    if (held_.empty()) return;
    const auto account = config_.store ? config_.store->mail_account() : std::nullopt;
    if (!account) return;
    std::vector<pbo::Handoff> held;
    held.swap(held_);
    for (size_t i = 0; i < held.size(); ++i)
        if (!make_draft(held[i], account->address, std::string())) {   // could not be saved: keep this one and the rest for the next time
            held_.assign(held.begin() + static_cast<long>(i), held.end());
            return;
        }
}

void HandoffBridge::setupActive(bool active) { setup_active_ = active; }

void HandoffBridge::setupLeft() {
    if (held_.empty()) return;
    if (config_.store && config_.store->mail_account()) {   // the account was saved just before the page went: the text is not lost
        accountReady();
        return;
    }
    const int count = static_cast<int>(held_.size());
    held_.clear();
    tell("no_account", count);
}

void HandoffBridge::shown() {
    if (unshown_.empty()) return;
    unshown_.clear();
    emit waitingChanged();
}
