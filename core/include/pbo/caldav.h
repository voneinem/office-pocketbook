#pragma once
// CalDAV client over the HTTP seam (contracts/caldav-ical-subset.md section 1) and the read-only feed download. Basic authentication
// over https only (the transport refuses anything else); user names and passwords are never part of a URL; a server answer that names a
// resource on another host is ignored; every XML body goes through xml_min. Server behaviour is UNVERIFIED until a live run.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "pbo/http_transport.h"
#include "pbo/types.h"

namespace pbo {

struct CalendarInfo {
    std::string href;                  // path on the server, e.g. "/dav/calendars/alice/work/"
    std::string name;
    std::string ctag, sync_token;      // as the server reported them at discovery
    bool writable = false;             // privileges include write-content or all
    bool supports_vevent = false;
    bool supports_sync_report = false;
};

struct EventRef {
    std::string href, etag;
};

struct ChangeSet {
    std::vector<EventRef> changed;     // new or changed resources (all resources of the window when `full`)
    std::vector<std::string> removed;  // hrefs the server reports as gone (only when !full)
    std::string new_sync_token, new_ctag;
    bool full = false;                 // the list is complete: the caller removes what is not in it
    bool unchanged = false;            // ctag equal to the stored one: nothing to do
};

struct EventResource {
    std::string href, etag, ics;
};

struct FetchResult {
    std::vector<EventResource> events;
    std::vector<std::string> gone;     // 404 inside the answer
    std::vector<std::string> failed;   // a per-resource error (5xx) or a resource that was too large: retried at the next sync
};

struct WriteResult {
    enum class Kind { Created, Updated, PreconditionFailed, NotFound, Refused, Failed } kind = Kind::Failed;
    std::string etag;                  // the new ETag when known ("" otherwise: the next sync learns it)
    Error error;                       // set for Refused (key "read_only", "too_large", "storage_full_server") and Failed (network, auth, server)
};

class CalDavClient {
public:
    // base_url: what the user entered (a server address, a /dav/ address, or a calendar home). Must be https.
    CalDavClient(HttpTransport& http, std::string base_url, std::string username, std::string password);

    // current-user-principal -> calendar-home-set -> calendars; only calendars that hold VEVENT
    Result<std::vector<CalendarInfo>> discover();

    // What changed in a calendar. `stored` is the sync token (calendars with sync-collection) or the ctag. window_from/to: epoch
    // seconds of the time range used when the complete list is needed.
    Result<ChangeSet> changes(const CalendarInfo& cal, const std::string& stored, int64_t window_from, int64_t window_to);

    // calendar-multiget in batches of 30 resources (at most 4 MiB per answer; a batch that is too big is split)
    Result<FetchResult> fetch(const CalendarInfo& cal, const std::vector<std::string>& hrefs);

    // The time the server reported (HTTP Date header) with its latest answer; used to notice a wrong clock on the reader (clock_check.h)
    std::optional<int64_t> last_server_time() const { return server_time_; }

    // Conditional writes (Principle III): if_match null = create (If-None-Match: *), else If-Match: <etag>
    WriteResult put(const std::string& href, const std::string& ics, const std::string* if_match);
    WriteResult remove(const std::string& href, const std::string& if_match);

private:
    Response send(const std::string& method, const std::string& path_or_url, const std::string& body, const Headers& headers, long long cap = 8ll << 20);
    std::string url_for(const std::string& href) const;   // "" if the href names another host
    Error status_error(const Response& r, bool* handled = nullptr) const;

    HttpTransport& http_;
    std::optional<int64_t> server_time_;
    std::string base_url_, origin_, user_, password_;
};

struct FeedResult {
    enum class State { Changed, NotModified, Failed } state = State::Failed;
    std::string ics, etag, last_modified;
    Error error;
};

// Download of an https calendar feed (webcal:// must already be rewritten). 8 MiB at most. No credentials are sent.
FeedResult fetch_feed(HttpTransport& http, const std::string& url, const std::string& etag, const std::string& last_modified);

}  // namespace pbo
