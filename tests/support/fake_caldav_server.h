#pragma once
// T098: a scriptable CalDAV server over TLS for the calendar tests (contracts/caldav-ical-subset.md section 2). It speaks the subset the app uses:
// discovery (well-known redirect, current-user-principal, calendar-home-set, calendar listing with ctag, sync-token, privileges and
// supported-report-set), sync-collection, calendar-query with a time range, calendar-multiget, conditional PUT and DELETE, and plain
// feed downloads with ETag / Last-Modified. It can inject faults per request and run a hook that edits the server copy between the
// app's read and its write. The XML it produces follows common server behaviour; it is NOT a recording of any real server
// (fixtures from real servers carry `.unverified` until a live run, Principle IX).

#include <algorithm>
#include <atomic>
#include <ctime>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "tls_test_server.h"

namespace pbtest {

class FakeCalDavServer {
public:
    struct Config {
        std::string cert, key;
        std::string user = "alice";
        std::string password = "pw-for-tests";
        bool well_known_redirect = true;     // /.well-known/caldav -> 301 /dav/
    };

    struct Event {
        std::string ics, etag;
        int64_t first_utc = 0;                // for calendar-query: the instance range of the event
        int64_t last_utc = INT64_MAX;
        int changed_rev = 0;
    };

    struct CalendarOptions {
        std::string name;
        bool writable = true;
        bool privilege_all = false;           // advertise d:all instead of the single privileges
        bool vevent = true;                   // supported-calendar-component-set holds VEVENT (false: only VTODO)
        bool sync_collection = true;          // the REPORT is listed in supported-report-set and answered
        bool is_calendar = true;              // false: a plain collection (not a calendar)
    };

    struct Logged {
        std::string method, path, body;
        std::map<std::string, std::string> headers;   // lower-case names
    };

    struct Fault {
        enum class Kind { Status, Drop, Delay, MalformedXml, RedirectOtherHost } kind = Kind::Status;
        std::string method;                   // "" = any
        std::string path_contains;            // "" = any
        int status = 500;
        int delay_ms = 0;
        std::string location;                 // for RedirectOtherHost
        int times = 1;                        // how often it fires (then it is spent)
    };

    explicit FakeCalDavServer(Config c) : cfg_(std::move(c)) {
        server_ = std::make_unique<TlsTestServer>(cfg_.cert, cfg_.key, [this](const std::string& req) { return handle(req); });
    }

    bool running() const { return server_ && server_->running(); }
    int port() const { return server_->port(); }
    std::string origin() const { return "https://localhost:" + std::to_string(port()); }
    std::string base_url() const { return origin() + "/dav/"; }
    std::string calendar_href(const std::string& slug) const { return "/dav/calendars/" + cfg_.user + "/" + slug + "/"; }
    std::string event_href(const std::string& slug, const std::string& name) const { return calendar_href(slug) + name; }

    // ---- scripting --------------------------------------------------------------------------------------------------------------
    void add_calendar(const std::string& slug, const CalendarOptions& o) {
        std::lock_guard<std::recursive_mutex> l(mu_);
        Cal& c = cals_[slug];
        c.opt = o;
        if (std::find(order_.begin(), order_.end(), slug) == order_.end()) order_.push_back(slug);
    }
    void remove_calendar(const std::string& slug) {
        std::lock_guard<std::recursive_mutex> l(mu_);
        cals_.erase(slug);
        order_.erase(std::remove(order_.begin(), order_.end(), slug), order_.end());
    }
    // returns the new ETag
    std::string put_event(const std::string& slug, const std::string& name, const std::string& ics, int64_t first_utc = 0, int64_t last_utc = INT64_MAX) {
        std::lock_guard<std::recursive_mutex> l(mu_);
        Cal& c = cals_.at(slug);
        Event& e = c.events[name];
        e.ics = ics;
        e.first_utc = first_utc;
        e.last_utc = last_utc;
        e.etag = "\"etag-" + std::to_string(++etag_counter_) + "\"";
        e.changed_rev = ++c.rev;
        return e.etag;
    }
    void remove_event(const std::string& slug, const std::string& name) {
        std::lock_guard<std::recursive_mutex> l(mu_);
        Cal& c = cals_.at(slug);
        if (c.events.erase(name)) c.removed.push_back({name, ++c.rev});
    }
    bool has_event(const std::string& slug, const std::string& name) {
        std::lock_guard<std::recursive_mutex> l(mu_);
        return cals_.count(slug) && cals_[slug].events.count(name);
    }
    Event event(const std::string& slug, const std::string& name) {
        std::lock_guard<std::recursive_mutex> l(mu_);
        return cals_.at(slug).events.at(name);
    }
    size_t event_count(const std::string& slug) {
        std::lock_guard<std::recursive_mutex> l(mu_);
        return cals_.at(slug).events.size();
    }
    // every sync token handed out so far becomes invalid (the server was reset): the app must resynchronise completely
    void reset_sync_tokens(const std::string& slug) {
        std::lock_guard<std::recursive_mutex> l(mu_);
        ++cals_.at(slug).epoch;
    }
    void set_feed(const std::string& path, const std::string& ics) {
        std::lock_guard<std::recursive_mutex> l(mu_);
        Feed& f = feeds_[path];
        f.ics = ics;
        f.etag = "\"feed-" + std::to_string(++etag_counter_) + "\"";
        f.last_modified = "Wed, 14 Oct 2026 08:00:00 GMT";
    }
    // the time the server reports in its Date header (0 = the real time)
    void set_server_time(int64_t epoch) { server_time_ = epoch; }
    void inject(const Fault& f) {
        std::lock_guard<std::recursive_mutex> l(mu_);
        faults_.push_back(f);
    }
    // called with the request just before a PUT or DELETE is processed: edit the server copy "meanwhile"
    void before_write(std::function<void(FakeCalDavServer&)> hook) {
        std::lock_guard<std::recursive_mutex> l(mu_);
        before_write_ = std::move(hook);
    }

    // The answer to one raw HTTP request (what the TLS front end would send). Used by the fake CardDAV server, which can stand in front of this
    // one so that calendars and address books answer on the same address, as on a real server.
    std::string serve(const std::string& raw) { return handle(raw); }

    // ---- observation ------------------------------------------------------------------------------------------------------------
    std::vector<Logged> log() {
        std::lock_guard<std::recursive_mutex> l(mu_);
        return log_;
    }
    int count(const std::string& method, const std::string& path_contains = "") {
        int n = 0;
        for (const Logged& r : log())
            if (r.method == method && r.path.find(path_contains) != std::string::npos) ++n;
        return n;
    }
    std::vector<size_t> multiget_batch_sizes() {
        std::lock_guard<std::recursive_mutex> l(mu_);
        return multiget_sizes_;
    }
    int rejected_logins() {
        std::lock_guard<std::recursive_mutex> l(mu_);
        return rejected_;
    }
    bool saw_credentials_in_url() {
        for (const Logged& r : log())
            if (r.path.find('@') != std::string::npos) return true;
        return false;
    }

private:
    struct Cal {
        CalendarOptions opt;
        std::map<std::string, Event> events;
        std::vector<std::pair<std::string, int>> removed;   // name, revision of the removal
        int rev = 1;
        int epoch = 1;
    };
    struct Feed {
        std::string ics, etag, last_modified;
    };

    static std::string lower(std::string s) {
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }
    static std::string xml_escape(const std::string& s) {
        std::string o;
        for (char c : s) {
            if (c == '&') o += "&amp;";
            else if (c == '<') o += "&lt;";
            else if (c == '>') o += "&gt;";
            else o += c;
        }
        return o;
    }
    static std::string base64(const std::string& in) {
        static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        size_t i = 0;
        for (; i + 2 < in.size(); i += 3) {
            const unsigned v = (unsigned char)in[i] << 16 | (unsigned char)in[i + 1] << 8 | (unsigned char)in[i + 2];
            out += t[v >> 18]; out += t[(v >> 12) & 63]; out += t[(v >> 6) & 63]; out += t[v & 63];
        }
        if (i + 1 == in.size()) {
            const unsigned v = (unsigned char)in[i] << 16;
            out += t[v >> 18]; out += t[(v >> 12) & 63]; out += "==";
        } else if (i + 2 == in.size()) {
            const unsigned v = (unsigned char)in[i] << 16 | (unsigned char)in[i + 1] << 8;
            out += t[v >> 18]; out += t[(v >> 12) & 63]; out += t[(v >> 6) & 63]; out += '=';
        }
        return out;
    }

    static std::string response(int status, const std::string& body, const std::string& extra = "", const char* ctype = "application/xml; charset=utf-8") {
        return "HTTP/1.1 " + std::to_string(status) + " X\r\nContent-Type: " + ctype + "\r\nContent-Length: " + std::to_string(body.size()) +
               "\r\nConnection: close\r\n" + extra + "\r\n" + body;
    }
    static std::string multistatus(const std::string& inner) {
        return response(207, "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<d:multistatus xmlns:d=\"DAV:\" xmlns:c=\"urn:ietf:params:xml:ns:caldav\" "
                             "xmlns:cs=\"http://calendarserver.org/ns/\">" + inner + "</d:multistatus>");
    }
    static std::string ok_propstat(const std::string& props) { return "<d:propstat><d:prop>" + props + "</d:prop><d:status>HTTP/1.1 200 OK</d:status></d:propstat>"; }

    std::string token_of(const Cal& c) const { return "http://example.org/ns/sync/" + std::to_string(c.epoch) + "/" + std::to_string(c.rev); }
    std::string ctag_of(const Cal& c) const { return "ctag-" + std::to_string(c.epoch) + "-" + std::to_string(c.rev); }

    std::string calendar_props(const Cal& c, const std::string& slug) const {
        std::string p;
        p += c.opt.is_calendar ? "<d:resourcetype><d:collection/><c:calendar/></d:resourcetype>" : "<d:resourcetype><d:collection/></d:resourcetype>";
        p += "<d:displayname>" + xml_escape(c.opt.name.empty() ? slug : c.opt.name) + "</d:displayname>";
        if (c.opt.is_calendar) {
            p += std::string("<c:supported-calendar-component-set><c:comp name=\"") + (c.opt.vevent ? "VEVENT" : "VTODO") + "\"/></c:supported-calendar-component-set>";
            p += "<cs:getctag>" + ctag_of(c) + "</cs:getctag>";
            p += "<d:sync-token>" + token_of(c) + "</d:sync-token>";
        }
        if (c.opt.privilege_all) p += "<d:current-user-privilege-set><d:privilege><d:all/></d:privilege></d:current-user-privilege-set>";
        else if (c.opt.writable) p += "<d:current-user-privilege-set><d:privilege><d:read/></d:privilege><d:privilege><d:write-content/></d:privilege><d:privilege><d:bind/></d:privilege><d:privilege><d:unbind/></d:privilege></d:current-user-privilege-set>";
        else p += "<d:current-user-privilege-set><d:privilege><d:read/></d:privilege></d:current-user-privilege-set>";
        p += "<d:supported-report-set><d:supported-report><d:report><c:calendar-multiget/></d:report></d:supported-report><d:supported-report><d:report><c:calendar-query/></d:report></d:supported-report>";
        if (c.opt.sync_collection) p += "<d:supported-report><d:report><d:sync-collection/></d:report></d:supported-report>";
        p += "</d:supported-report-set>";
        return p;
    }

    static int64_t parse_utc(const std::string& s) {   // 20260101T000000Z
        if (s.size() < 15) return 0;
        const int y = std::stoi(s.substr(0, 4)), m = std::stoi(s.substr(4, 2)), d = std::stoi(s.substr(6, 2));
        const int hh = std::stoi(s.substr(9, 2)), mm = std::stoi(s.substr(11, 2)), ss = std::stoi(s.substr(13, 2));
        // days from civil
        int64_t yy = y - (m <= 2);
        const int64_t era = (yy >= 0 ? yy : yy - 399) / 400;
        const int64_t yoe = yy - era * 400;
        const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
        const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
        return (era * 146097 + doe - 719468) * 86400 + hh * 3600 + mm * 60 + ss;
    }

    // Every answer carries a Date header: the real time, or the time set with set_server_time (for the clock tests)
    std::string handle(const std::string& raw) {
        std::string out = handle_request(raw);
        const size_t eol = out.find("\r\n");
        if (eol != std::string::npos) {
            const int64_t t = server_time_ != 0 ? server_time_.load() : static_cast<int64_t>(std::time(nullptr));
            char buf[64];
            const time_t tt = static_cast<time_t>(t);
            struct tm g;
            gmtime_r(&tt, &g);
            std::strftime(buf, sizeof buf, "%a, %d %b %Y %H:%M:%S GMT", &g);
            out.insert(eol + 2, std::string("Date: ") + buf + "\r\n");
        }
        return out;
    }
    std::string handle_request(const std::string& raw) {
        Logged r;
        const size_t eol = raw.find("\r\n");
        std::istringstream first(raw.substr(0, eol));
        std::string version;
        first >> r.method >> r.path >> version;
        size_t pos = eol + 2;
        while (pos < raw.size()) {
            const size_t e = raw.find("\r\n", pos);
            if (e == std::string::npos || e == pos) { pos = e == std::string::npos ? raw.size() : e + 2; break; }
            const std::string line = raw.substr(pos, e - pos);
            const size_t colon = line.find(':');
            if (colon != std::string::npos) {
                size_t b = colon + 1;
                while (b < line.size() && line[b] == ' ') ++b;
                r.headers[lower(line.substr(0, colon))] = line.substr(b);
            }
            pos = e + 2;
        }
        r.body = raw.substr(std::min(pos, raw.size()));

        std::lock_guard<std::recursive_mutex> l(mu_);
        log_.push_back(r);

        // faults first (a dropped or delayed answer must not depend on the login)
        for (auto it = faults_.begin(); it != faults_.end(); ++it) {
            if (!it->method.empty() && it->method != r.method) continue;
            if (!it->path_contains.empty() && r.path.find(it->path_contains) == std::string::npos) continue;
            Fault f = *it;
            if (--it->times <= 0) faults_.erase(it);
            switch (f.kind) {
                case Fault::Kind::Drop: return std::string();
                case Fault::Kind::Delay: std::this_thread::sleep_for(std::chrono::milliseconds(f.delay_ms)); goto proceed;
                case Fault::Kind::MalformedXml: return response(207, "<?xml version=\"1.0\"?><d:multistatus xmlns:d=\"DAV:\"><d:response><d:href>/x");
                case Fault::Kind::RedirectOtherHost: return response(302, "", "Location: " + f.location + "\r\n");
                case Fault::Kind::Status: return response(f.status, "<?xml version=\"1.0\"?><d:error xmlns:d=\"DAV:\"/>");
            }
        }
    proceed:
        const std::string& p = r.path;
        // feeds are plain downloads (public: no login is asked for)
        if (r.method == "GET" && feeds_.count(p)) {
            const Feed& f = feeds_[p];
            auto inm = r.headers.find("if-none-match");
            auto ims = r.headers.find("if-modified-since");
            if ((inm != r.headers.end() && inm->second == f.etag) || (inm == r.headers.end() && ims != r.headers.end() && ims->second == f.last_modified))
                return response(304, "", "ETag: " + f.etag + "\r\n");
            return response(200, f.ics, "ETag: " + f.etag + "\r\nLast-Modified: " + f.last_modified + "\r\n", "text/calendar; charset=utf-8");
        }

        if (r.method == "GET" && p.compare(0, 7, "/feeds/") == 0) return response(404, "");   // an unknown feed
        // login
        {
            const std::string want = "Basic " + base64(cfg_.user + ":" + cfg_.password);
            auto it = r.headers.find("authorization");
            if (it == r.headers.end() || it->second != want) {
                ++rejected_;
                return response(401, "", "WWW-Authenticate: Basic realm=\"fake\"\r\n");
            }
        }
        const std::string home = "/dav/calendars/" + cfg_.user + "/";
        const std::string principal = "/dav/principals/" + cfg_.user + "/";

        if (r.method == "PROPFIND" || r.method == "REPORT" || r.method == "OPTIONS") {
            if (p == "/.well-known/caldav") {
                if (cfg_.well_known_redirect) return response(301, "", "Location: /dav/\r\n");
                return response(404, "");
            }
            if (r.method == "OPTIONS") return response(200, "", "DAV: 1, 2, 3, calendar-access\r\nAllow: OPTIONS, PROPFIND, REPORT, PUT, DELETE, GET\r\n");
            const std::string depth = r.headers.count("depth") ? r.headers["depth"] : "infinity";
            if (r.method == "PROPFIND") {
                if (p == "/dav/" || p == "/dav") {
                    if (r.body.find("current-user-principal") != std::string::npos)
                        return multistatus("<d:response><d:href>/dav/</d:href>" + ok_propstat("<d:current-user-principal><d:href>" + principal + "</d:href></d:current-user-principal>") + "</d:response>");
                    return multistatus("<d:response><d:href>/dav/</d:href>" + ok_propstat("<d:resourcetype><d:collection/></d:resourcetype>") + "</d:response>");
                }
                if (p == principal) {
                    if (r.body.find("calendar-home-set") != std::string::npos)
                        return multistatus("<d:response><d:href>" + principal + "</d:href>" + ok_propstat("<c:calendar-home-set><d:href>" + home + "</d:href></c:calendar-home-set>") + "</d:response>");
                    return multistatus("<d:response><d:href>" + principal + "</d:href>" + ok_propstat("<d:resourcetype><d:principal/></d:resourcetype>") + "</d:response>");
                }
                if (p == home) {
                    std::string inner = "<d:response><d:href>" + home + "</d:href>" + ok_propstat("<d:resourcetype><d:collection/></d:resourcetype>") + "</d:response>";
                    if (depth != "0")
                        for (const std::string& slug : order_) inner += "<d:response><d:href>" + calendar_href(slug) + "</d:href>" + ok_propstat(calendar_props(cals_[slug], slug)) + "</d:response>";
                    return multistatus(inner);
                }
                for (auto& kv : cals_)
                    if (p == calendar_href(kv.first)) {
                        std::string inner = "<d:response><d:href>" + p + "</d:href>" + ok_propstat(calendar_props(kv.second, kv.first)) + "</d:response>";
                        return multistatus(inner);
                    }
                return response(404, "");
            }
            // REPORT on a calendar
            for (auto& kv : cals_) {
                if (p != calendar_href(kv.first)) continue;
                Cal& c = kv.second;
                if (r.body.find("sync-collection") != std::string::npos) {
                    if (!c.opt.sync_collection) return response(403, "<d:error xmlns:d=\"DAV:\"><d:supported-report/></d:error>");
                    std::smatch m;
                    std::string token;
                    static const std::regex tok("<(?:[A-Za-z0-9]+:)?sync-token>([^<]*)<");
                    if (std::regex_search(r.body, m, tok)) token = m[1];
                    int since = 0;
                    if (!token.empty()) {
                        int epoch = 0, rev = 0;
                        if (std::sscanf(token.c_str(), "http://example.org/ns/sync/%d/%d", &epoch, &rev) != 2 || epoch != c.epoch)
                            return response(409, "<?xml version=\"1.0\"?><d:error xmlns:d=\"DAV:\"><d:valid-sync-token/></d:error>");
                        since = rev;
                    }
                    std::string inner;
                    for (auto& ev : c.events)
                        if (ev.second.changed_rev > since)
                            inner += "<d:response><d:href>" + calendar_href(kv.first) + ev.first + "</d:href>" + ok_propstat("<d:getetag>" + ev.second.etag + "</d:getetag>") + "</d:response>";
                    for (auto& rm : c.removed)
                        if (rm.second > since) inner += "<d:response><d:href>" + calendar_href(kv.first) + rm.first + "</d:href><d:status>HTTP/1.1 404 Not Found</d:status></d:response>";
                    inner += "<d:sync-token>" + token_of(c) + "</d:sync-token>";
                    return multistatus(inner);
                }
                if (r.body.find("calendar-query") != std::string::npos) {
                    int64_t from = INT64_MIN, to = INT64_MAX;
                    std::smatch m;
                    static const std::regex range("time-range[^>]*start=\"([0-9TZ]+)\"[^>]*end=\"([0-9TZ]+)\"");
                    if (std::regex_search(r.body, m, range)) { from = parse_utc(m[1]); to = parse_utc(m[2]); }
                    std::string inner;
                    for (auto& ev : c.events)
                        if (ev.second.last_utc >= from && ev.second.first_utc < to)
                            inner += "<d:response><d:href>" + calendar_href(kv.first) + ev.first + "</d:href>" + ok_propstat("<d:getetag>" + ev.second.etag + "</d:getetag>") + "</d:response>";
                    return multistatus(inner);
                }
                if (r.body.find("calendar-multiget") != std::string::npos) {
                    static const std::regex href("<(?:[A-Za-z0-9]+:)?href>([^<]*)<");
                    std::string inner;
                    size_t n = 0;
                    for (std::sregex_iterator it(r.body.begin(), r.body.end(), href), end; it != end; ++it) {
                        const std::string h = (*it)[1];
                        ++n;
                        const std::string name = h.substr(h.rfind('/') + 1);
                        auto ev = c.events.find(name);
                        if (ev == c.events.end()) inner += "<d:response><d:href>" + h + "</d:href><d:status>HTTP/1.1 404 Not Found</d:status></d:response>";
                        else inner += "<d:response><d:href>" + h + "</d:href>" + ok_propstat("<d:getetag>" + ev->second.etag + "</d:getetag><c:calendar-data>" + xml_escape(ev->second.ics) + "</c:calendar-data>") + "</d:response>";
                    }
                    multiget_sizes_.push_back(n);
                    return multistatus(inner);
                }
                return response(400, "");
            }
            return response(404, "");
        }

        if (r.method == "PUT" || r.method == "DELETE") {
            if (before_write_) {
                auto hook = before_write_;
                hook(*this);
            }
            for (auto& kv : cals_) {
                const std::string prefix = calendar_href(kv.first);
                if (p.compare(0, prefix.size(), prefix) != 0 || p.size() == prefix.size()) continue;
                Cal& c = kv.second;
                if (!c.opt.writable && !c.opt.privilege_all) return response(403, "<d:error xmlns:d=\"DAV:\"><d:need-privileges/></d:error>");
                const std::string name = p.substr(prefix.size());
                auto ev = c.events.find(name);
                const std::string if_match = r.headers.count("if-match") ? r.headers["if-match"] : "";
                const std::string if_none = r.headers.count("if-none-match") ? r.headers["if-none-match"] : "";
                if (r.method == "PUT") {
                    if (if_none == "*" && ev != c.events.end()) return response(412, "");
                    if (!if_match.empty()) {
                        if (ev == c.events.end()) return response(404, "");
                        if (ev->second.etag != if_match) return response(412, "");
                    }
                    const bool created = ev == c.events.end();
                    Event& e = c.events[name];
                    e.ics = r.body;
                    e.first_utc = 0;
                    e.last_utc = INT64_MAX;
                    e.etag = "\"etag-" + std::to_string(++etag_counter_) + "\"";
                    e.changed_rev = ++c.rev;
                    return response(created ? 201 : 204, "", "ETag: " + e.etag + "\r\n");
                }
                if (ev == c.events.end()) return response(404, "");
                if (!if_match.empty() && ev->second.etag != if_match) return response(412, "");
                c.events.erase(ev);
                c.removed.push_back({name, ++c.rev});
                return response(204, "");
            }
            return response(404, "");
        }

        if (r.method == "GET") {
            for (auto& kv : cals_) {
                const std::string prefix = calendar_href(kv.first);
                if (p.compare(0, prefix.size(), prefix) != 0) continue;
                auto ev = kv.second.events.find(p.substr(prefix.size()));
                if (ev == kv.second.events.end()) return response(404, "");
                return response(200, ev->second.ics, "ETag: " + ev->second.etag + "\r\n", "text/calendar; charset=utf-8");
            }
        }
        return response(404, "");
    }

    Config cfg_;
    std::recursive_mutex mu_;
    std::map<std::string, Cal> cals_;
    std::vector<std::string> order_;
    std::map<std::string, Feed> feeds_;
    std::vector<Fault> faults_;
    std::vector<Logged> log_;
    std::vector<size_t> multiget_sizes_;
    std::function<void(FakeCalDavServer&)> before_write_;
    int etag_counter_ = 0;
    std::atomic<int64_t> server_time_{0};
    int rejected_ = 0;
    std::unique_ptr<TlsTestServer> server_;   // last: stopped first, before the state goes away
};

}  // namespace pbtest
