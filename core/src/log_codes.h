#pragma once
// The event codes the application may log (constitution Principle IV, contracts/core-interfaces.md §9).
// A code is a short dotted identifier ("area.what"); it never carries data. Adding a code here is a reviewed
// change: tests/unit/test_log.cpp fails when a source file logs a code that is not listed, and when a listed
// code is no longer used.

#include <iterator>
#include <string>

namespace pbo {
namespace log {

inline constexpr const char* kAllowedCodes[] = {
    "about.bad_blocks",
    "about.opened",
    "about.text_unavailable",
    "attachment.failed",
    "body.fetched",
    "attachment.write_failed",
    "ca.selected",
    "contacts.conflict",
    "contacts.pull",
    "contacts.push",
    "credentials.forgotten",
    "credentials.saved",
    "credentials.unreadable",
    "credentials.write_failed",
    "font.emoji_missing",
    "font.ui",
    "imap.connect_failed",
    "imap.unparsed",
    "network.connect",
    "rotate.begin",
    "rotate.done",
    "rotate.geometry_failed",
    "store.create_failed",
    "store.migration_failed",
    "smtp.connect_failed",
    "smtp.send_failed",
    "connection.tested",
    "store.rebuild_started",
    "store.evicted",
    "store.trimmed",
    "image.loaded",
    "image.undecodable",
    "store.recovered",
    "store.write_failed",
    "sync.finished",
    "tz.unavailable",
};

inline bool is_allowed_code(const std::string& code) {
    for (const char* c : kAllowedCodes)
        if (code == c) return true;
    return false;
}

}  // namespace log
}  // namespace pbo
