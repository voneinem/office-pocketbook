#include "pbo/caldav.h"
#include "pbo/clock_check.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <functional>

#include "pbo/civil.h"
#include "pbo/xml_min.h"

namespace pbo {

namespace {

const char* kDav = "DAV:";
const char* kCal = "urn:ietf:params:xml:ns:caldav";
const char* kCs = "http://calendarserver.org/ns/";

constexpr size_t kMultigetBatch = 30;
constexpr long long kMultigetCap = 4ll << 20;

std::string escape(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '&') o += "&amp;";
        else if (c == '<') o += "&lt;";
        else if (c == '>') o += "&gt;";
        else if (c == '"') o += "&quot;";
        else o += c;
    }
    return o;
}

std::string trim(std::string s) {
    size_t b = 0;
    while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    size_t e = s.size();
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

std::string utc_stamp(int64_t epoch) {
    const LocalDateTime t = civil::from_naive_epoch(epoch);
    char b[32];
    snprintf(b, sizeof b, "%04d%02d%02dT%02d%02d%02dZ", t.year, t.month, t.day, t.hour, t.minute, t.second);
    return b;
}

bool status_ok(const std::string& status_line) {
    // "HTTP/1.1 200 OK"
    const size_t sp = status_line.find(' ');
    if (sp == std::string::npos) return false;
    return std::atoi(status_line.c_str() + sp + 1) / 100 == 2;
}

int status_code(const std::string& status_line) {
    const size_t sp = status_line.find(' ');
    return sp == std::string::npos ? 0 : std::atoi(status_line.c_str() + sp + 1);
}

// the prop element of the first propstat with a 2xx status that holds `ns:name`; nullptr if none
const XmlNode* prop_in_ok_propstat(const XmlNode& response, const char* ns, const char* name) {
    for (const XmlNode* ps : response.children_named(kDav, "propstat")) {
        const XmlNode* st = ps->child(kDav, "status");
        if (st && !status_ok(trim(st->text))) continue;
        const XmlNode* prop = ps->child(kDav, "prop");
        if (!prop) continue;
        if (const XmlNode* p = prop->child(ns, name)) return p;
    }
    return nullptr;
}

const XmlNode* ok_prop_set(const XmlNode& response) {
    for (const XmlNode* ps : response.children_named(kDav, "propstat")) {
        const XmlNode* st = ps->child(kDav, "status");
        if (st && !status_ok(trim(st->text))) continue;
        if (const XmlNode* prop = ps->child(kDav, "prop")) return prop;
    }
    return nullptr;
}

std::string path_of(const std::string& origin, const std::string& href) {
    if (href.compare(0, origin.size(), origin) == 0) return href.substr(origin.size());
    return href;
}

}  // namespace

CalDavClient::CalDavClient(HttpTransport& http, std::string base_url, std::string username, std::string password)
    : http_(http), base_url_(std::move(base_url)), user_(std::move(username)), password_(std::move(password)) {
    const size_t sep = base_url_.find("://");
    if (sep != std::string::npos) {
        const size_t slash = base_url_.find('/', sep + 3);
        origin_ = base_url_.substr(0, slash);
    }
}

std::string CalDavClient::url_for(const std::string& href) const {
    if (href.empty() || origin_.empty()) return std::string();
    if (href.compare(0, 2, "//") == 0) return std::string();   // a scheme-relative address names another host
    if (href[0] == '/') return origin_ + href;
    if (href.compare(0, origin_.size(), origin_) == 0 && (href.size() == origin_.size() || href[origin_.size()] == '/')) return href;
    if (href.find("://") != std::string::npos) return std::string();   // another host
    return origin_ + "/" + href;
}

Response CalDavClient::send(const std::string& method, const std::string& url, const std::string& body, const Headers& headers, long long cap) {
    Request r;
    r.method = method;
    r.url = url;
    r.body = body;
    r.headers = headers;
    r.username = user_;
    r.password = password_;
    r.max_response_bytes = cap;
    Response answer = http_.perform(r);
    if (answer.status != 0) {
        const std::string date = header_value(answer.headers, "Date");
        if (!date.empty())
            if (const auto t = parse_http_date(date)) server_time_ = *t;
    }
    return answer;
}

Error CalDavClient::status_error(const Response& r, bool* handled) const {
    if (handled) *handled = true;
    if (r.body_too_large) return make_error(ErrorClass::Protocol, "too_large", "the answer is larger than the limit");
    if (r.status == 0) {
        const ErrorClass cls = r.error == ErrorClass::None ? ErrorClass::Network : r.error;
        return make_error(cls, r.error_key.empty() ? "server_unreachable" : r.error_key, r.error_detail);
    }
    if (r.status == 401) return make_error(ErrorClass::Auth, "auth_failed", "the server refused the login");
    if (r.status == 403) return make_error(ErrorClass::Server, "forbidden", "the server refused the request");
    if (r.status == 404) return make_error(ErrorClass::Server, "not_found", "not found on the server");
    if (r.status >= 500 || r.status == 429 || r.status == 408) return make_error(ErrorClass::Server, "server_error", "the server reported a problem");
    if (r.status >= 200 && r.status < 300) {
        if (handled) *handled = false;
        return Error();
    }
    return make_error(ErrorClass::Protocol, "protocol_error", "unexpected answer from the server");
}

namespace {

const char* kXmlHeader = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n";

Headers dav_headers(const char* depth) { return {{"Depth", depth}, {"Content-Type", "application/xml; charset=utf-8"}}; }

Result<XmlNode> parse_multistatus(const Response& r) {
    Result<XmlNode> x = parse_xml(r.body);
    if (!x.ok()) return x;
    if (x.value.name != "multistatus" || x.value.ns != kDav) {
        x.error = make_error(ErrorClass::Protocol, "malformed_xml", "not a multistatus answer");
    }
    return x;
}

}  // namespace

Result<std::vector<CalendarInfo>> CalDavClient::discover() {
    Result<std::vector<CalendarInfo>> out;
    if (origin_.empty()) {
        out.error = make_error(ErrorClass::Protocol, "invalid_input", "no server address");
        return out;
    }
    // 1. the principal; candidates: the address as entered, and the well-known address (first when the address is a bare server)
    std::vector<std::string> starts;
    const std::string well_known = origin_ + "/.well-known/caldav";
    const bool bare = base_url_.size() <= origin_.size() + 1;
    if (bare) starts = {well_known, base_url_};
    else starts = {base_url_, well_known};

    std::string principal, home;
    Error first_error;
    for (const std::string& start : starts) {
        const Response r = send("PROPFIND", start,
                                std::string(kXmlHeader) + "<d:propfind xmlns:d=\"DAV:\"><d:prop><d:current-user-principal/></d:prop></d:propfind>", dav_headers("0"));
        if (r.status == 401 || r.status == 0 || r.body_too_large) {
            out.error = status_error(r);
            return out;
        }
        if (r.status != 207) {
            if (first_error.ok()) first_error = status_error(r);
            continue;
        }
        const Result<XmlNode> x = parse_multistatus(r);
        if (!x.ok()) {
            out.error = x.error;
            return out;
        }
        for (const XmlNode* resp : x.value.children_named(kDav, "response")) {
            if (const XmlNode* h = prop_in_ok_propstat(*resp, kDav, "current-user-principal")) {
                if (const XmlNode* href = h->child(kDav, "href")) {
                    principal = url_for(trim(href->text));
                    break;
                }
            }
        }
        if (!principal.empty()) break;
    }
    if (principal.empty()) {
        // the address may itself be the calendar home: its listing is tried below, and an error is reported if it holds no calendar
        home = base_url_;
        out.error = Error();
    }

    // 2. the calendar home
    if (!principal.empty()) {
        const Response r = send("PROPFIND", principal,
                                std::string(kXmlHeader) + "<d:propfind xmlns:d=\"DAV:\" xmlns:c=\"urn:ietf:params:xml:ns:caldav\"><d:prop><c:calendar-home-set/></d:prop></d:propfind>",
                                dav_headers("0"));
        Error e = status_error(r);
        if (r.status != 207 && e.ok()) e = make_error(ErrorClass::Protocol, "protocol_error", "unexpected answer from the server");
        if (!e.ok()) {
            out.error = e;
            return out;
        }
        const Result<XmlNode> x = parse_multistatus(r);
        if (!x.ok()) {
            out.error = x.error;
            return out;
        }
        home.clear();
        for (const XmlNode* resp : x.value.children_named(kDav, "response")) {
            if (const XmlNode* h = prop_in_ok_propstat(*resp, kCal, "calendar-home-set"))
                if (const XmlNode* href = h->child(kDav, "href")) {
                    home = url_for(trim(href->text));
                    break;
                }
        }
        if (home.empty()) {
            out.error = make_error(ErrorClass::Protocol, "protocol_error", "the server names no calendar home");
            return out;
        }
    }

    // 3. the calendars
    const char* body =
        "<d:propfind xmlns:d=\"DAV:\" xmlns:c=\"urn:ietf:params:xml:ns:caldav\" xmlns:cs=\"http://calendarserver.org/ns/\"><d:prop>"
        "<d:resourcetype/><d:displayname/><c:supported-calendar-component-set/><cs:getctag/><d:sync-token/>"
        "<d:current-user-privilege-set/><d:supported-report-set/></d:prop></d:propfind>";
    const Response r = send("PROPFIND", home, std::string(kXmlHeader) + body, dav_headers("1"));
    Error e = status_error(r);
    if (r.status != 207 && e.ok()) e = make_error(ErrorClass::Protocol, "protocol_error", "unexpected answer from the server");
    if (!e.ok()) {
        out.error = (principal.empty() && !first_error.ok()) ? first_error : e;
        return out;
    }
    const Result<XmlNode> x = parse_multistatus(r);
    if (!x.ok()) {
        out.error = x.error;
        return out;
    }
    const std::string home_path = path_of(origin_, home);
    for (const XmlNode* resp : x.value.children_named(kDav, "response")) {
        const XmlNode* href_node = resp->child(kDav, "href");
        if (!href_node) continue;
        const std::string url = url_for(trim(href_node->text));
        if (url.empty()) continue;   // another host: ignored
        const std::string path = path_of(origin_, url);
        if (path == home_path) continue;
        const XmlNode* rt = prop_in_ok_propstat(*resp, kDav, "resourcetype");
        if (!rt || !rt->child(kCal, "calendar")) continue;
        CalendarInfo c;
        c.href = path;
        c.supports_vevent = true;   // an absent component set means every component
        if (const XmlNode* set = prop_in_ok_propstat(*resp, kCal, "supported-calendar-component-set")) {
            c.supports_vevent = false;
            for (const XmlNode* comp : set->children_named(kCal, "comp"))
                if (comp->attr("name") == "VEVENT") c.supports_vevent = true;
        }
        if (!c.supports_vevent) continue;
        if (const XmlNode* n = prop_in_ok_propstat(*resp, kDav, "displayname")) c.name = trim(n->text);
        if (c.name.empty()) {
            std::string seg = path;
            while (!seg.empty() && seg.back() == '/') seg.pop_back();
            c.name = seg.substr(seg.rfind('/') + 1);
        }
        if (const XmlNode* n = prop_in_ok_propstat(*resp, kCs, "getctag")) c.ctag = trim(n->text);
        if (const XmlNode* n = prop_in_ok_propstat(*resp, kDav, "sync-token")) c.sync_token = trim(n->text);
        if (const XmlNode* privs = prop_in_ok_propstat(*resp, kDav, "current-user-privilege-set")) {
            for (const XmlNode* p : privs->children_named(kDav, "privilege"))
                if (p->child(kDav, "write-content") || p->child(kDav, "all") || p->child(kDav, "write")) c.writable = true;
        }
        if (const XmlNode* reports = prop_in_ok_propstat(*resp, kDav, "supported-report-set")) {
            for (const XmlNode* sr : reports->children_named(kDav, "supported-report"))
                if (const XmlNode* rep = sr->child(kDav, "report"))
                    if (rep->child(kDav, "sync-collection")) c.supports_sync_report = true;
        }
        out.value.push_back(std::move(c));
    }
    if (principal.empty() && out.value.empty()) {
        out.error = first_error.ok() ? make_error(ErrorClass::Protocol, "protocol_error", "no calendar principal found") : first_error;
    }
    return out;
}

namespace {

// hrefs and etags of a multistatus (a calendar-query or sync-collection answer)
void read_refs(const XmlNode& ms, const CalDavClient* /*unused*/, const std::function<std::string(const std::string&)>& to_path, const std::string& skip_path,
               ChangeSet* cs, bool with_removed) {
    for (const XmlNode* resp : ms.children_named(kDav, "response")) {
        const XmlNode* href_node = resp->child(kDav, "href");
        if (!href_node) continue;
        const std::string path = to_path(trim(href_node->text));
        if (path.empty() || path == skip_path) continue;
        const XmlNode* st = resp->child(kDav, "status");
        if (st && status_code(trim(st->text)) == 404) {
            if (with_removed) cs->removed.push_back(path);
            continue;
        }
        const XmlNode* tag = prop_in_ok_propstat(*resp, kDav, "getetag");
        if (!tag && !ok_prop_set(*resp)) continue;   // no usable property set: skipped
        cs->changed.push_back({path, tag ? trim(tag->text) : std::string()});
    }
}

}  // namespace

Result<ChangeSet> CalDavClient::changes(const CalendarInfo& cal, const std::string& stored, int64_t window_from, int64_t window_to) {
    Result<ChangeSet> out;
    const std::string cal_url = url_for(cal.href);
    if (cal_url.empty()) {
        out.error = make_error(ErrorClass::Protocol, "invalid_input", "the calendar address is not on the server");
        return out;
    }
    auto to_path = [this](const std::string& href) {
        const std::string url = url_for(href);
        return url.empty() ? std::string() : path_of(origin_, url);
    };
    const std::string cal_path = path_of(origin_, cal_url);

    out.value.new_ctag = cal.ctag;
    if (cal.supports_sync_report) {
        out.value.new_sync_token = cal.sync_token;
        if (!stored.empty()) {
            const std::string body = std::string(kXmlHeader) + "<d:sync-collection xmlns:d=\"DAV:\"><d:sync-token>" + escape(stored) +
                                     "</d:sync-token><d:sync-level>1</d:sync-level><d:prop><d:getetag/></d:prop></d:sync-collection>";
            const Response r = send("REPORT", cal_url, body, dav_headers("1"));
            const bool invalid_token = r.status == 409 || (r.status == 403 && r.body.find("valid-sync-token") != std::string::npos) ||
                                       (r.status >= 400 && r.status < 500 && r.body.find("valid-sync-token") != std::string::npos);
            if (!invalid_token) {
                Error e = status_error(r);
                if (r.status != 207 && e.ok()) e = make_error(ErrorClass::Protocol, "protocol_error", "unexpected answer from the server");
                if (!e.ok()) {
                    out.error = e;
                    return out;
                }
                const Result<XmlNode> x = parse_multistatus(r);
                if (!x.ok()) {
                    out.error = x.error;
                    return out;
                }
                bool truncated = false;
                for (const XmlNode* resp : x.value.children_named(kDav, "response"))
                    if (const XmlNode* st = resp->child(kDav, "status"))
                        if (status_code(trim(st->text)) == 507) truncated = true;   // the answer was cut short: fall back to the window
                if (!truncated) {
                    read_refs(x.value, nullptr, to_path, cal_path, &out.value, true);
                    if (const XmlNode* t = x.value.child(kDav, "sync-token")) out.value.new_sync_token = trim(t->text);
                    out.value.full = false;
                    return out;
                }
            }
            // an invalid or unusable token: a complete listing follows
        }
    } else {
        // no sync-collection: the ctag tells whether anything changed
        const Response r = send("PROPFIND", cal_url,
                                std::string(kXmlHeader) + "<d:propfind xmlns:d=\"DAV:\" xmlns:cs=\"http://calendarserver.org/ns/\"><d:prop><cs:getctag/></d:prop></d:propfind>", dav_headers("0"));
        if (r.status == 207) {
            const Result<XmlNode> x = parse_multistatus(r);
            if (x.ok())
                for (const XmlNode* resp : x.value.children_named(kDav, "response"))
                    if (const XmlNode* c = prop_in_ok_propstat(*resp, kCs, "getctag")) {
                        out.value.new_ctag = trim(c->text);
                        break;
                    }
        } else {
            const Error e = status_error(r);
            if (!e.ok()) {
                out.error = e;
                return out;
            }
        }
        if (!stored.empty() && stored == out.value.new_ctag) {
            out.value.unchanged = true;
            return out;
        }
    }

    // the complete list of the window
    const std::string body = std::string(kXmlHeader) +
        "<c:calendar-query xmlns:d=\"DAV:\" xmlns:c=\"urn:ietf:params:xml:ns:caldav\"><d:prop><d:getetag/></d:prop><c:filter><c:comp-filter name=\"VCALENDAR\">"
        "<c:comp-filter name=\"VEVENT\"><c:time-range start=\"" + utc_stamp(window_from) + "\" end=\"" + utc_stamp(window_to) + "\"/></c:comp-filter></c:comp-filter></c:filter></c:calendar-query>";
    const Response r = send("REPORT", cal_url, body, dav_headers("1"));
    Error e = status_error(r);
    if (r.status != 207 && e.ok()) e = make_error(ErrorClass::Protocol, "protocol_error", "unexpected answer from the server");
    if (!e.ok()) {
        out.error = e;
        return out;
    }
    const Result<XmlNode> x = parse_multistatus(r);
    if (!x.ok()) {
        out.error = x.error;
        return out;
    }
    read_refs(x.value, nullptr, to_path, cal_path, &out.value, false);
    out.value.full = true;
    return out;
}

Result<FetchResult> CalDavClient::fetch(const CalendarInfo& cal, const std::vector<std::string>& hrefs) {
    Result<FetchResult> out;
    const std::string cal_url = url_for(cal.href);
    if (cal_url.empty()) {
        out.error = make_error(ErrorClass::Protocol, "invalid_input", "the calendar address is not on the server");
        return out;
    }
    std::vector<std::string> paths;
    for (const std::string& h : hrefs) {
        const std::string url = url_for(h);
        if (!url.empty()) paths.push_back(path_of(origin_, url));   // a resource on another host is not asked for
    }

    // a stack: the first batch is on top
    std::vector<std::vector<std::string>> queue;
    for (size_t i = 0; i < paths.size(); i += kMultigetBatch)
        queue.emplace_back(paths.begin() + i, paths.begin() + std::min(paths.size(), i + kMultigetBatch));
    std::reverse(queue.begin(), queue.end());
    while (!queue.empty()) {
        std::vector<std::string> batch = std::move(queue.back());
        queue.pop_back();
        std::string body = std::string(kXmlHeader) +
            "<c:calendar-multiget xmlns:d=\"DAV:\" xmlns:c=\"urn:ietf:params:xml:ns:caldav\"><d:prop><d:getetag/><c:calendar-data/></d:prop>";
        for (const std::string& p : batch) body += "<d:href>" + escape(p) + "</d:href>";
        body += "</c:calendar-multiget>";
        const Response r = send("REPORT", cal_url, body, dav_headers("1"), kMultigetCap);
        if (r.body_too_large) {
            if (batch.size() > 1) {
                const size_t half = batch.size() / 2;
                queue.emplace_back(batch.begin() + half, batch.end());
                queue.emplace_back(batch.begin(), batch.begin() + half);
            } else {
                out.value.failed.push_back(batch[0]);   // one resource over 4 MiB: skipped
            }
            continue;
        }
        Error e = status_error(r);
        if (r.status != 207 && e.ok()) e = make_error(ErrorClass::Protocol, "protocol_error", "unexpected answer from the server");
        if (!e.ok()) {
            out.error = e;
            return out;
        }
        const Result<XmlNode> x = parse_multistatus(r);
        if (!x.ok()) {
            out.error = x.error;
            return out;
        }
        for (const XmlNode* resp : x.value.children_named(kDav, "response")) {
            const XmlNode* href_node = resp->child(kDav, "href");
            if (!href_node) continue;
            const std::string url = url_for(trim(href_node->text));
            if (url.empty()) continue;
            const std::string path = path_of(origin_, url);
            const XmlNode* st = resp->child(kDav, "status");
            if (st) {
                const int code = status_code(trim(st->text));
                if (code == 404) { out.value.gone.push_back(path); continue; }
                if (code >= 500) { out.value.failed.push_back(path); continue; }
            }
            const XmlNode* data = prop_in_ok_propstat(*resp, kCal, "calendar-data");
            if (!data) {
                // a propstat with an error status for this resource
                bool failed = false;
                for (const XmlNode* ps : resp->children_named(kDav, "propstat"))
                    if (const XmlNode* pst = ps->child(kDav, "status")) {
                        const int code = status_code(trim(pst->text));
                        if (code == 404) { out.value.gone.push_back(path); failed = false; break; }
                        if (code >= 500) failed = true;
                    }
                if (failed) out.value.failed.push_back(path);
                continue;
            }
            EventResource ev;
            ev.href = path;
            ev.ics = data->text;
            if (const XmlNode* tag = prop_in_ok_propstat(*resp, kDav, "getetag")) ev.etag = trim(tag->text);
            out.value.events.push_back(std::move(ev));
        }
    }
    return out;
}

namespace {

WriteResult write_outcome(const Response& r, bool creating, const CalDavClient& /*unused*/) {
    WriteResult w;
    if (r.status == 0 || r.body_too_large) {
        w.kind = WriteResult::Kind::Failed;
        const ErrorClass cls = r.error == ErrorClass::None ? ErrorClass::Network : r.error;
        w.error = make_error(cls, r.error_key.empty() ? "server_unreachable" : r.error_key, r.error_detail);
        return w;
    }
    if (r.status >= 200 && r.status < 300) {
        w.kind = creating ? WriteResult::Kind::Created : WriteResult::Kind::Updated;
        w.etag = header_value(r.headers, "ETag");
        return w;
    }
    switch (r.status) {
        case 401:
            w.kind = WriteResult::Kind::Failed;
            w.error = make_error(ErrorClass::Auth, "auth_failed", "the server refused the login");
            break;
        case 403:
        case 405:
            w.kind = WriteResult::Kind::Refused;
            w.error = make_error(ErrorClass::Server, "read_only", "the calendar does not accept changes");
            break;
        case 404:
        case 410:
            w.kind = WriteResult::Kind::NotFound;
            break;
        case 409:
        case 412:
            w.kind = WriteResult::Kind::PreconditionFailed;
            break;
        case 413:
            w.kind = WriteResult::Kind::Refused;
            w.error = make_error(ErrorClass::Server, "too_large", "the server refused the size");
            break;
        case 507:
            w.kind = WriteResult::Kind::Refused;
            w.error = make_error(ErrorClass::Server, "storage_full_server", "the server has no space left");
            break;
        default:
            w.kind = WriteResult::Kind::Failed;
            w.error = r.status >= 500 ? make_error(ErrorClass::Server, "server_error", "the server reported a problem")
                                      : make_error(ErrorClass::Protocol, "protocol_error", "unexpected answer from the server");
    }
    return w;
}

}  // namespace

WriteResult CalDavClient::put(const std::string& href, const std::string& ics, const std::string* if_match) {
    const std::string url = url_for(href);
    if (url.empty()) {
        WriteResult w;
        w.error = make_error(ErrorClass::Protocol, "invalid_input", "the address is not on the server");
        return w;
    }
    Headers h = {{"Content-Type", "text/calendar; charset=utf-8"}};
    if (if_match) h.emplace_back("If-Match", *if_match);
    else h.emplace_back("If-None-Match", "*");
    const Response r = send("PUT", url, ics, h);
    WriteResult w = write_outcome(r, if_match == nullptr, *this);
    if ((w.kind == WriteResult::Kind::Created || w.kind == WriteResult::Kind::Updated) && w.etag.empty()) {
        // the server did not name the new version: one question
        const Response p = send("PROPFIND", url, std::string(kXmlHeader) + "<d:propfind xmlns:d=\"DAV:\"><d:prop><d:getetag/></d:prop></d:propfind>", dav_headers("0"));
        if (p.status == 207) {
            const Result<XmlNode> x = parse_multistatus(p);
            if (x.ok())
                for (const XmlNode* resp : x.value.children_named(kDav, "response"))
                    if (const XmlNode* t = prop_in_ok_propstat(*resp, kDav, "getetag")) { w.etag = trim(t->text); break; }
        }
    }
    return w;
}

WriteResult CalDavClient::remove(const std::string& href, const std::string& if_match) {
    const std::string url = url_for(href);
    if (url.empty()) {
        WriteResult w;
        w.error = make_error(ErrorClass::Protocol, "invalid_input", "the address is not on the server");
        return w;
    }
    const Response r = send("DELETE", url, "", {{"If-Match", if_match}});
    WriteResult w = write_outcome(r, false, *this);
    w.etag.clear();
    return w;
}

// ---- feeds ---------------------------------------------------------------------------------------------------------------------

FeedResult fetch_feed(HttpTransport& http, const std::string& url, const std::string& etag, const std::string& last_modified) {
    FeedResult out;
    if (url.empty()) {
        out.error = make_error(ErrorClass::Protocol, "invalid_input", "no feed address");
        return out;
    }
    Request req;
    req.method = "GET";
    req.url = url;
    req.headers = {{"Accept", "text/calendar, text/plain;q=0.5, */*;q=0.1"}};
    if (!etag.empty()) req.headers.emplace_back("If-None-Match", etag);
    if (!last_modified.empty()) req.headers.emplace_back("If-Modified-Since", last_modified);
    req.max_response_bytes = 8ll << 20;
    const Response r = http.perform(req);
    if (r.body_too_large) {
        out.error = make_error(ErrorClass::Protocol, "too_large", "the feed is larger than the limit");
        return out;
    }
    if (r.status == 0) {
        const ErrorClass cls = r.error == ErrorClass::None ? ErrorClass::Network : r.error;
        out.error = make_error(cls, r.error_key.empty() ? "server_unreachable" : r.error_key, r.error_detail);
        return out;
    }
    if (r.status == 304) {
        out.state = FeedResult::State::NotModified;
        out.etag = etag;
        out.last_modified = last_modified;
        return out;
    }
    if (r.status == 401 || r.status == 403) {
        out.error = make_error(ErrorClass::Server, "feed_refused", "the feed host refused the request");
        return out;
    }
    if (r.status == 404 || r.status == 410) {
        out.error = make_error(ErrorClass::Server, "feed_not_found", "the feed address does not exist");
        return out;
    }
    if (r.status >= 500 || r.status == 429 || r.status == 408) {
        out.error = make_error(ErrorClass::Server, "server_error", "the feed host reported a problem");
        return out;
    }
    if (r.status < 200 || r.status >= 300) {
        out.error = make_error(ErrorClass::Protocol, "protocol_error", "unexpected answer from the feed host");
        return out;
    }
    size_t b = 0;
    if (r.body.compare(0, 3, "\xEF\xBB\xBF") == 0) b = 3;
    while (b < r.body.size() && std::isspace(static_cast<unsigned char>(r.body[b]))) ++b;
    std::string head = r.body.substr(b, 15);
    for (char& c : head) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (head != "BEGIN:VCALENDAR") {
        out.error = make_error(ErrorClass::Protocol, "not_a_calendar", "the address does not deliver a calendar");
        return out;
    }
    out.state = FeedResult::State::Changed;
    out.ics = r.body.substr(b);
    out.etag = header_value(r.headers, "ETag");
    out.last_modified = header_value(r.headers, "Last-Modified");
    return out;
}

}  // namespace pbo
