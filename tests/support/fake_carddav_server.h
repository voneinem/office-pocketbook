#pragma once
// Spec 002: a scriptable CardDAV server over TLS for the contact tests (specs/002-server-contacts/contracts/carddav-vcard-subset.md section 2).
// It speaks the subset the app uses: discovery (well-known redirect, current-user-principal, addressbook-home-set, the listing of address books with
// ctag, sync-token, privileges and supported-report-set), sync-collection, PROPFIND listings of ETags, addressbook-multiget (with or without
// honouring the address-data property list), GET, and conditional PUT and DELETE. It can inject faults per request and run a hook that edits the
// server copy between the app's read and its write. The XML it produces follows common server behaviour; it is NOT a recording of any real server
// (fixtures from real servers carry `.unverified` until a live run, Principle IX).
//
// It can stand in front of a FakeCalDavServer (set_calendar_peer): requests for calendars are handed to it, so that one address serves both.

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

#include "fake_caldav_server.h"
#include "tls_test_server.h"

namespace pbtest {

class FakeCardDavServer {
public:
    struct Config {
        std::string cert, key;
        std::string user = "alice";
        std::string password = "pw-for-tests";
        bool well_known_redirect = true;     // /.well-known/carddav -> 301 /dav/
        bool address_book_home = true;       // false: the principal names no addressbook-home-set
    };

    struct Card {
        std::string vcf, etag;
        int changed_rev = 0;
    };

    struct BookOptions {
        std::string name;
        bool writable = true;
        bool privilege_all = false;           // advertise d:all instead of the single privileges
        bool sync_collection = true;          // the REPORT is listed in supported-report-set and answered
        bool is_addressbook = true;           // false: a plain collection (not an address book)
    };

    struct Logged {
        std::string method, path, body;
        std::map<std::string, std::string> headers;   // lower-case names
    };

    struct Fault {
        enum class Kind { Status, Drop, DropAfterProcessing, Delay, MalformedXml, RedirectOtherHost } kind = Kind::Status;   // DropAfterProcessing: the request is carried out, the answer is lost
        std::string method;                   // "" = any
        std::string path_contains;            // "" = any
        std::string body_contains;            // "" = any (e.g. "addressbook-multiget" to fail only the download of the cards)
        int status = 500;
        int delay_ms = 0;
        std::string location;                 // for RedirectOtherHost
        int times = 1;                        // how often it fires (then it is spent)
    };

    explicit FakeCardDavServer(Config c) : cfg_(std::move(c)) {
        server_ = std::make_unique<TlsTestServer>(cfg_.cert, cfg_.key, [this](const std::string& req) { return handle(req); });
    }

    bool running() const { return server_ && server_->running(); }
    int port() const { return server_->port(); }
    std::string origin() const { return "https://localhost:" + std::to_string(port()); }
    std::string base_url() const { return origin() + "/dav/"; }
    std::string book_href(const std::string& slug) const { return "/dav/addressbooks/" + cfg_.user + "/" + slug + "/"; }
    std::string card_href(const std::string& slug, const std::string& name) const { return book_href(slug) + name; }

    // ---- scripting --------------------------------------------------------------------------------------------------------------
    void add_book(const std::string& slug, const BookOptions& o) {
        std::lock_guard<std::recursive_mutex> l(mu_);
        Book& b = books_[slug];
        b.opt = o;
        if (std::find(order_.begin(), order_.end(), slug) == order_.end()) order_.push_back(slug);
    }
    void remove_book(const std::string& slug) {
        std::lock_guard<std::recursive_mutex> l(mu_);
        books_.erase(slug);
        order_.erase(std::remove(order_.begin(), order_.end(), slug), order_.end());
    }
    // returns the new ETag
    std::string put_card(const std::string& slug, const std::string& name, const std::string& vcf) {
        std::lock_guard<std::recursive_mutex> l(mu_);
        Book& b = books_.at(slug);
        Card& c = b.cards[name];
        c.vcf = vcf;
        c.etag = "\"etag-" + std::to_string(++etag_counter_) + "\"";
        c.changed_rev = ++b.rev;
        return c.etag;
    }
    void remove_card(const std::string& slug, const std::string& name) {
        std::lock_guard<std::recursive_mutex> l(mu_);
        Book& b = books_.at(slug);
        if (b.cards.erase(name)) b.removed.push_back({name, ++b.rev});
    }
    bool has_card(const std::string& slug, const std::string& name) {
        std::lock_guard<std::recursive_mutex> l(mu_);
        return books_.count(slug) && books_[slug].cards.count(name);
    }
    Card card(const std::string& slug, const std::string& name) {
        std::lock_guard<std::recursive_mutex> l(mu_);
        return books_.at(slug).cards.at(name);
    }
    size_t card_count(const std::string& slug) {
        std::lock_guard<std::recursive_mutex> l(mu_);
        return books_.at(slug).cards.size();
    }
    std::vector<std::string> card_names(const std::string& slug) {
        std::lock_guard<std::recursive_mutex> l(mu_);
        std::vector<std::string> out;
        for (const auto& kv : books_.at(slug).cards) out.push_back(kv.first);
        return out;
    }
    // every sync token handed out so far becomes invalid (the server was reset): the app must resynchronise completely
    void reset_sync_tokens(const std::string& slug) {
        std::lock_guard<std::recursive_mutex> l(mu_);
        ++books_.at(slug).epoch;
    }
    // false: the server ignores the address-data property list and always sends full cards (research R-05, open item O-4)
    void set_honours_property_list(bool honours) { honours_list_ = honours; }
    // the time the server reports in its Date header (0 = the real time)
    void set_server_time(int64_t epoch) { server_time_ = epoch; }
    void inject(const Fault& f) {
        std::lock_guard<std::recursive_mutex> l(mu_);
        faults_.push_back(f);
    }
    // called with the request just before a PUT or DELETE is processed: edit the server copy "meanwhile"
    void before_write(std::function<void(FakeCardDavServer&)> hook) {
        std::lock_guard<std::recursive_mutex> l(mu_);
        before_write_ = std::move(hook);
    }
    // calendars are answered by this server (same user and password); it must outlive this object
    void set_calendar_peer(FakeCalDavServer* peer) { peer_ = peer; }

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
    struct Book {
        BookOptions opt;
        std::map<std::string, Card> cards;
        std::vector<std::pair<std::string, int>> removed;   // name, revision of the removal
        int rev = 1;
        int epoch = 1;
    };

    static std::string lower(std::string s) {
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }
    static std::string upper(std::string s) {
        for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
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
        return response(207, "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<d:multistatus xmlns:d=\"DAV:\" xmlns:card=\"urn:ietf:params:xml:ns:carddav\" "
                             "xmlns:cs=\"http://calendarserver.org/ns/\">" + inner + "</d:multistatus>");
    }
    static std::string ok_propstat(const std::string& props) { return "<d:propstat><d:prop>" + props + "</d:prop><d:status>HTTP/1.1 200 OK</d:status></d:propstat>"; }
    static std::string missing_propstat(const std::string& props) { return "<d:propstat><d:prop>" + props + "</d:prop><d:status>HTTP/1.1 404 Not Found</d:status></d:propstat>"; }

    std::string token_of(const Book& b) const { return "http://example.org/ns/sync/" + std::to_string(b.epoch) + "/" + std::to_string(b.rev); }
    std::string ctag_of(const Book& b) const { return "ctag-" + std::to_string(b.epoch) + "-" + std::to_string(b.rev); }

    std::string book_props(const Book& b, const std::string& slug) const {
        std::string p;
        p += b.opt.is_addressbook ? "<d:resourcetype><d:collection/><card:addressbook/></d:resourcetype>" : "<d:resourcetype><d:collection/></d:resourcetype>";
        p += "<d:displayname>" + xml_escape(b.opt.name.empty() ? slug : b.opt.name) + "</d:displayname>";
        if (b.opt.is_addressbook) {
            p += "<cs:getctag>" + ctag_of(b) + "</cs:getctag>";
            p += "<d:sync-token>" + token_of(b) + "</d:sync-token>";
        }
        if (b.opt.privilege_all) p += "<d:current-user-privilege-set><d:privilege><d:all/></d:privilege></d:current-user-privilege-set>";
        else if (b.opt.writable) p += "<d:current-user-privilege-set><d:privilege><d:read/></d:privilege><d:privilege><d:write-content/></d:privilege><d:privilege><d:bind/></d:privilege><d:privilege><d:unbind/></d:privilege></d:current-user-privilege-set>";
        else p += "<d:current-user-privilege-set><d:privilege><d:read/></d:privilege></d:current-user-privilege-set>";
        p += "<d:supported-report-set><d:supported-report><d:report><card:addressbook-multiget/></d:report></d:supported-report><d:supported-report><d:report><card:addressbook-query/></d:report></d:supported-report>";
        if (b.opt.sync_collection) p += "<d:supported-report><d:report><d:sync-collection/></d:report></d:supported-report>";
        p += "</d:supported-report-set>";
        return p;
    }

    // the card without the properties a client that gave a property list did not ask for: PHOTO, LOGO, SOUND, KEY (with their folded lines)
    static std::string without_heavy(const std::string& vcf) {
        std::string out;
        size_t pos = 0;
        bool dropping = false;
        while (pos < vcf.size()) {
            size_t e = vcf.find('\n', pos);
            const size_t next = e == std::string::npos ? vcf.size() : e + 1;
            const std::string line = vcf.substr(pos, next - pos);
            pos = next;
            if (!line.empty() && (line[0] == ' ' || line[0] == '\t')) {
                if (!dropping) out += line;
                continue;
            }
            const size_t cut = line.find_first_of(";:");
            std::string name = upper(line.substr(0, cut));
            const size_t dot = name.rfind('.');
            if (dot != std::string::npos) name = name.substr(dot + 1);
            dropping = name == "PHOTO" || name == "LOGO" || name == "SOUND" || name == "KEY";
            if (!dropping) out += line;
        }
        return out;
    }

    // Every answer carries a Date header: the real time, or the time set with set_server_time (for the clock tests)
    std::string handle(const std::string& raw) {
        std::string out = handle_request(raw);
        if (drop_answer_.exchange(false)) return std::string();   // the work was done, the answer never arrives
        const size_t eol = out.find("\r\n");
        if (eol != std::string::npos && out.find("\r\nDate: ") == std::string::npos) {
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
            if (!it->body_contains.empty() && r.body.find(it->body_contains) == std::string::npos) continue;
            Fault f = *it;
            if (--it->times <= 0) faults_.erase(it);
            switch (f.kind) {
                case Fault::Kind::Drop: return std::string();
                case Fault::Kind::DropAfterProcessing: drop_answer_ = true; goto proceed;
                case Fault::Kind::Delay: std::this_thread::sleep_for(std::chrono::milliseconds(f.delay_ms)); goto proceed;
                case Fault::Kind::MalformedXml: return response(207, "<?xml version=\"1.0\"?><d:multistatus xmlns:d=\"DAV:\"><d:response><d:href>/x");
                case Fault::Kind::RedirectOtherHost: return response(302, "", "Location: " + f.location + "\r\n");
                case Fault::Kind::Status: return response(f.status, "<?xml version=\"1.0\"?><d:error xmlns:d=\"DAV:\"/>");
            }
        }
    proceed:
        const std::string& p = r.path;
        // login
        {
            const std::string want = "Basic " + base64(cfg_.user + ":" + cfg_.password);
            auto it = r.headers.find("authorization");
            if (it == r.headers.end() || it->second != want) {
                ++rejected_;
                return response(401, "", "WWW-Authenticate: Basic realm=\"fake\"\r\n");
            }
        }
        const std::string home = "/dav/addressbooks/" + cfg_.user + "/";
        const std::string principal = "/dav/principals/" + cfg_.user + "/";

        // calendars belong to the peer (same address, same login)
        if (peer_) {
            const bool calendar_request = p == "/.well-known/caldav" || p.compare(0, 15, "/dav/calendars/") == 0 || p.compare(0, 7, "/feeds/") == 0 ||
                                          (r.method == "PROPFIND" && p == principal && r.body.find("calendar-home-set") != std::string::npos);
            if (calendar_request) return peer_->serve(rebuild(r));
        }

        if (r.method == "PROPFIND" || r.method == "REPORT" || r.method == "OPTIONS") {
            if (p == "/.well-known/carddav") {
                if (cfg_.well_known_redirect) return response(301, "", "Location: /dav/\r\n");
                return response(404, "");
            }
            if (r.method == "OPTIONS") return response(200, "", "DAV: 1, 2, 3, addressbook\r\nAllow: OPTIONS, PROPFIND, REPORT, PUT, DELETE, GET\r\n");
            const std::string depth = r.headers.count("depth") ? r.headers["depth"] : "infinity";
            if (r.method == "PROPFIND") {
                if (p == "/dav/" || p == "/dav") {
                    if (r.body.find("current-user-principal") != std::string::npos)
                        return multistatus("<d:response><d:href>/dav/</d:href>" + ok_propstat("<d:current-user-principal><d:href>" + principal + "</d:href></d:current-user-principal>") + "</d:response>");
                    return multistatus("<d:response><d:href>/dav/</d:href>" + ok_propstat("<d:resourcetype><d:collection/></d:resourcetype>") + "</d:response>");
                }
                if (p == principal) {
                    if (r.body.find("addressbook-home-set") != std::string::npos) {
                        if (!cfg_.address_book_home)
                            return multistatus("<d:response><d:href>" + principal + "</d:href>" + missing_propstat("<card:addressbook-home-set/>") + "</d:response>");
                        return multistatus("<d:response><d:href>" + principal + "</d:href>" + ok_propstat("<card:addressbook-home-set><d:href>" + home + "</d:href></card:addressbook-home-set>") + "</d:response>");
                    }
                    return multistatus("<d:response><d:href>" + principal + "</d:href>" + ok_propstat("<d:resourcetype><d:principal/></d:resourcetype>") + "</d:response>");
                }
                if (p == home) {
                    std::string inner = "<d:response><d:href>" + home + "</d:href>" + ok_propstat("<d:resourcetype><d:collection/></d:resourcetype>") + "</d:response>";
                    if (depth != "0")
                        for (const std::string& slug : order_) inner += "<d:response><d:href>" + book_href(slug) + "</d:href>" + ok_propstat(book_props(books_[slug], slug)) + "</d:response>";
                    return multistatus(inner);
                }
                for (auto& kv : books_) {
                    if (p != book_href(kv.first)) continue;
                    if (depth == "0") return multistatus("<d:response><d:href>" + p + "</d:href>" + ok_propstat(book_props(kv.second, kv.first)) + "</d:response>");
                    // Depth 1: the book itself and the ETag of every card
                    std::string inner = "<d:response><d:href>" + p + "</d:href>" + ok_propstat(book_props(kv.second, kv.first)) + "</d:response>";
                    for (auto& c : kv.second.cards)
                        inner += "<d:response><d:href>" + book_href(kv.first) + c.first + "</d:href>" + ok_propstat("<d:getetag>" + c.second.etag + "</d:getetag>") + "</d:response>";
                    return multistatus(inner);
                }
                return response(404, "");
            }
            // REPORT on an address book
            for (auto& kv : books_) {
                if (p != book_href(kv.first)) continue;
                Book& b = kv.second;
                if (r.body.find("sync-collection") != std::string::npos) {
                    if (!b.opt.sync_collection) return response(403, "<d:error xmlns:d=\"DAV:\"><d:supported-report/></d:error>");
                    std::smatch m;
                    std::string token;
                    static const std::regex tok("<(?:[A-Za-z0-9]+:)?sync-token>([^<]*)<");
                    if (std::regex_search(r.body, m, tok)) token = m[1];
                    int since = 0;
                    if (!token.empty()) {
                        int epoch = 0, rev = 0;
                        if (std::sscanf(token.c_str(), "http://example.org/ns/sync/%d/%d", &epoch, &rev) != 2 || epoch != b.epoch)
                            return response(409, "<?xml version=\"1.0\"?><d:error xmlns:d=\"DAV:\"><d:valid-sync-token/></d:error>");
                        since = rev;
                    }
                    std::string inner;
                    for (auto& c : b.cards)
                        if (c.second.changed_rev > since)
                            inner += "<d:response><d:href>" + book_href(kv.first) + c.first + "</d:href>" + ok_propstat("<d:getetag>" + c.second.etag + "</d:getetag>") + "</d:response>";
                    for (auto& rm : b.removed)
                        if (rm.second > since) inner += "<d:response><d:href>" + book_href(kv.first) + rm.first + "</d:href><d:status>HTTP/1.1 404 Not Found</d:status></d:response>";
                    inner += "<d:sync-token>" + token_of(b) + "</d:sync-token>";
                    return multistatus(inner);
                }
                if (r.body.find("addressbook-query") != std::string::npos) {
                    std::string inner;
                    for (auto& c : b.cards)
                        inner += "<d:response><d:href>" + book_href(kv.first) + c.first + "</d:href>" + ok_propstat("<d:getetag>" + c.second.etag + "</d:getetag>") + "</d:response>";
                    return multistatus(inner);
                }
                if (r.body.find("addressbook-multiget") != std::string::npos) {
                    static const std::regex href("<(?:[A-Za-z0-9]+:)?href>([^<]*)<");
                    const bool list_given = r.body.find("prop name=") != std::string::npos;
                    std::string inner;
                    size_t n = 0;
                    for (std::sregex_iterator it(r.body.begin(), r.body.end(), href), end; it != end; ++it) {
                        const std::string h = (*it)[1];
                        ++n;
                        const std::string name = h.substr(h.rfind('/') + 1);
                        auto c = b.cards.find(name);
                        if (c == b.cards.end()) {
                            inner += "<d:response><d:href>" + h + "</d:href><d:status>HTTP/1.1 404 Not Found</d:status></d:response>";
                        } else {
                            const std::string data = (list_given && honours_list_) ? without_heavy(c->second.vcf) : c->second.vcf;
                            inner += "<d:response><d:href>" + h + "</d:href>" + ok_propstat("<d:getetag>" + c->second.etag + "</d:getetag><card:address-data>" + xml_escape(data) + "</card:address-data>") + "</d:response>";
                        }
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
            for (auto& kv : books_) {
                const std::string prefix = book_href(kv.first);
                if (p.compare(0, prefix.size(), prefix) != 0 || p.size() == prefix.size()) continue;
                Book& b = kv.second;
                if (!b.opt.writable && !b.opt.privilege_all) return response(403, "<d:error xmlns:d=\"DAV:\"><d:need-privileges/></d:error>");
                const std::string name = p.substr(prefix.size());
                auto c = b.cards.find(name);
                const std::string if_match = r.headers.count("if-match") ? r.headers["if-match"] : "";
                const std::string if_none = r.headers.count("if-none-match") ? r.headers["if-none-match"] : "";
                if (r.method == "PUT") {
                    if (if_none == "*" && c != b.cards.end()) return response(412, "");
                    if (!if_match.empty()) {
                        if (c == b.cards.end()) return response(404, "");
                        if (c->second.etag != if_match) return response(412, "");
                    }
                    const bool created = c == b.cards.end();
                    Card& card = b.cards[name];
                    card.vcf = r.body;
                    card.etag = "\"etag-" + std::to_string(++etag_counter_) + "\"";
                    card.changed_rev = ++b.rev;
                    return response(created ? 201 : 204, "", "ETag: " + card.etag + "\r\n");
                }
                if (c == b.cards.end()) return response(404, "");
                if (!if_match.empty() && c->second.etag != if_match) return response(412, "");
                b.cards.erase(c);
                b.removed.push_back({name, ++b.rev});
                return response(204, "");
            }
            return response(404, "");
        }

        if (r.method == "GET") {
            for (auto& kv : books_) {
                const std::string prefix = book_href(kv.first);
                if (p.compare(0, prefix.size(), prefix) != 0) continue;
                auto c = kv.second.cards.find(p.substr(prefix.size()));
                if (c == kv.second.cards.end()) return response(404, "");
                return response(200, c->second.vcf, "ETag: " + c->second.etag + "\r\n", "text/vcard; charset=utf-8");
            }
        }
        return response(404, "");
    }

    // the raw request again, for the peer
    static std::string rebuild(const Logged& r) {
        std::string out = r.method + " " + r.path + " HTTP/1.1\r\n";
        for (const auto& h : r.headers) out += h.first + ": " + h.second + "\r\n";
        return out + "\r\n" + r.body;
    }

    Config cfg_;
    std::recursive_mutex mu_;
    std::map<std::string, Book> books_;
    std::vector<std::string> order_;
    std::vector<Fault> faults_;
    std::vector<Logged> log_;
    std::vector<size_t> multiget_sizes_;
    std::function<void(FakeCardDavServer&)> before_write_;
    FakeCalDavServer* peer_ = nullptr;
    std::atomic<bool> honours_list_{true};
    std::atomic<bool> drop_answer_{false};
    int etag_counter_ = 0;
    std::atomic<int64_t> server_time_{0};
    int rejected_ = 0;
    std::unique_ptr<TlsTestServer> server_;   // last: stopped first, before the state goes away
};

}  // namespace pbtest
