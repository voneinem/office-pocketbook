#include "pbo/dav_common.h"

#include <cctype>
#include <cstdlib>

#include "pbo/clock_check.h"

namespace pbo {

namespace dav {

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

const XmlNode* prop_in_ok_propstat(const XmlNode& response, const char* ns, const char* name) {
    for (const XmlNode* ps : response.children_named(kNsDav, "propstat")) {
        const XmlNode* st = ps->child(kNsDav, "status");
        if (st && !status_ok(trim(st->text))) continue;
        const XmlNode* prop = ps->child(kNsDav, "prop");
        if (!prop) continue;
        if (const XmlNode* p = prop->child(ns, name)) return p;
    }
    return nullptr;
}

const XmlNode* ok_prop_set(const XmlNode& response) {
    for (const XmlNode* ps : response.children_named(kNsDav, "propstat")) {
        const XmlNode* st = ps->child(kNsDav, "status");
        if (st && !status_ok(trim(st->text))) continue;
        if (const XmlNode* prop = ps->child(kNsDav, "prop")) return prop;
    }
    return nullptr;
}

std::string path_of(const std::string& origin, const std::string& href) {
    if (href.compare(0, origin.size(), origin) == 0) return href.substr(origin.size());
    return href;
}

Headers dav_headers(const char* depth) { return {{"Depth", depth}, {"Content-Type", "application/xml; charset=utf-8"}}; }

Result<XmlNode> parse_multistatus(const Response& r) {
    Result<XmlNode> x = parse_xml(r.body);
    if (!x.ok()) return x;
    if (x.value.name != "multistatus" || x.value.ns != kNsDav) {
        x.error = make_error(ErrorClass::Protocol, "malformed_xml", "not a multistatus answer");
    }
    return x;
}

}  // namespace dav

DavSession::DavSession(HttpTransport& http, std::string base_url, std::string username, std::string password)
    : http_(http), base_url_(std::move(base_url)), user_(std::move(username)), password_(std::move(password)) {
    const size_t sep = base_url_.find("://");
    if (sep != std::string::npos) {
        const size_t slash = base_url_.find('/', sep + 3);
        origin_ = base_url_.substr(0, slash);
    }
}

std::string DavSession::url_for(const std::string& href) const {
    if (href.empty() || origin_.empty()) return std::string();
    if (href.compare(0, 2, "//") == 0) return std::string();   // a scheme-relative address names another host
    if (href[0] == '/') return origin_ + href;
    if (href.compare(0, origin_.size(), origin_) == 0 && (href.size() == origin_.size() || href[origin_.size()] == '/')) return href;
    if (href.find("://") != std::string::npos) return std::string();   // another host
    return origin_ + "/" + href;
}

Response DavSession::send(const std::string& method, const std::string& url, const std::string& body, const Headers& headers, long long cap) {
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

Error DavSession::status_error(const Response& r, bool* handled) const {
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

}  // namespace pbo
