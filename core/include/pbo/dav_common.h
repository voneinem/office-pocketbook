#pragma once
// The generic WebDAV plumbing shared by the CalDAV client (caldav.h) and the CardDAV client (carddav.h), spec 002 research R-03: the origin and
// host rule, one request with the credentials handed to the transport, the HTTP status mapping and the multistatus helpers. Security rules kept
// in one place: an address on another host is never followed, user names and passwords are never part of a URL, every XML body goes through
// xml_min. Server behaviour is UNVERIFIED until a live run.

#include <cstdint>
#include <optional>
#include <string>

#include "pbo/http_transport.h"
#include "pbo/types.h"
#include "pbo/xml_min.h"

namespace pbo {

namespace dav {

inline constexpr const char* kNsDav = "DAV:";
inline constexpr const char* kNsCal = "urn:ietf:params:xml:ns:caldav";
inline constexpr const char* kNsCard = "urn:ietf:params:xml:ns:carddav";
inline constexpr const char* kNsCs = "http://calendarserver.org/ns/";
inline constexpr const char* kXmlHeader = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n";

std::string escape(const std::string& s);       // & < > " for XML text and attribute values
std::string trim(std::string s);
bool status_ok(const std::string& status_line);  // "HTTP/1.1 200 OK" -> true for 2xx
int status_code(const std::string& status_line); // 0 when the line is not a status line
// The prop element of the first propstat with a 2xx status that holds `ns:name`; nullptr if none.
const XmlNode* prop_in_ok_propstat(const XmlNode& response, const char* ns, const char* name);
// The prop element of the first propstat with a 2xx status; nullptr if none.
const XmlNode* ok_prop_set(const XmlNode& response);
// `href` without the origin (a path), or `href` itself when it does not start with it.
std::string path_of(const std::string& origin, const std::string& href);
Headers dav_headers(const char* depth);          // Depth + Content-Type: application/xml
// The root must be d:multistatus; error key "malformed_xml" otherwise.
Result<XmlNode> parse_multistatus(const Response& r);

}  // namespace dav

// One server account: the base address as entered, the user and the password. Not copyable state: it only holds the strings and the server time.
class DavSession {
public:
    DavSession(HttpTransport& http, std::string base_url, std::string username, std::string password);

    const std::string& base_url() const { return base_url_; }
    const std::string& origin() const { return origin_; }   // "https://host[:port]", "" when the base address has no scheme

    // The full address for a path or address from the server; "" when it names another host (or a scheme-relative address).
    std::string url_for(const std::string& href) const;

    // One request. The credentials go to the transport (Request::username/password), never into the URL. `cap` limits the answer in memory.
    // The HTTP Date header of an answer is remembered (last_server_time).
    Response send(const std::string& method, const std::string& url, const std::string& body, const Headers& headers, long long cap = 8ll << 20);

    // Maps a transport failure or an HTTP status to the stable keys auth_failed, forbidden, not_found, server_error, protocol_error, too_large and
    // the transport's own keys. A 2xx status gives an empty Error and *handled = false (the caller reads the answer).
    Error status_error(const Response& r, bool* handled = nullptr) const;

    std::optional<int64_t> last_server_time() const { return server_time_; }

private:
    HttpTransport& http_;
    std::optional<int64_t> server_time_;
    std::string base_url_, origin_, user_, password_;
};

}  // namespace pbo
