#pragma once
// Abstract HTTP seam (T015). The production implementation (CurlTransport) verifies TLS and
// refuses non-https URLs; tests use a scripted FakeTransport. No Qt.

#include <string>
#include <utility>
#include <vector>

#include "pbo/types.h"

namespace pbo {

using Headers = std::vector<std::pair<std::string, std::string>>;

// Case-insensitive lookup; returns "" when absent.
std::string header_value(const Headers& headers, const std::string& name);
bool has_header(const Headers& headers, const std::string& name);
// Every value of a header that was sent more than once, in order (names compared without regard to case).
std::vector<std::string> header_values(const Headers& headers, const std::string& name);

struct Request {
    std::string method = "GET";
    std::string url;
    Headers headers;
    std::string body;
    // HTTP Basic credentials; the transport builds the Authorization header. Empty = none.
    std::string username;
    std::string password;
    // Methods: GET, HEAD, POST, PUT, DELETE and the WebDAV verbs PROPFIND, REPORT, OPTIONS (also with a body). A redirect keeps method and
    // body; credentials are not sent to another host or port.
    // max_response_bytes: cap for an in-memory body (8 MiB; 0 = none). When the declared or the running size exceeds it, the transfer is
    //   stopped and Response::body_too_large is set (the body is empty).
    long long max_response_bytes = 8ll << 20;
    long connect_timeout_s = 10;
    long total_timeout_s = 60;
    // File transfer (spec 002, research R-6). Methods: GET, HEAD, POST, PUT, DELETE.
    // save_body_to: a successful (2xx) body is streamed into this file instead of `Response::body`; an error
    //   status keeps its (small) body in memory and creates no file. Nothing is left behind on failure.
    // max_body_bytes: 0 = no limit; otherwise the transfer is stopped as soon as Content-Length or the running
    //   total exceeds it (Response::body_too_large) and the file is removed.
    std::string save_body_to;
    long long max_body_bytes = 0;
    // upload_file: send this file as a multipart/form-data body with one part (field `upload_field`, file name
    //   `upload_file_name`) instead of `body`.
    std::string upload_file;
    std::string upload_field = "file";
    std::string upload_file_name;
};

struct Response {
    long status = 0;  // 0 = no HTTP response (see error)
    Headers headers;
    std::string body;
    ErrorClass error = ErrorClass::None;  // transport-level classification when status == 0
    std::string error_key;                // stable cause key of the mapper (curl_session.h), "" when the status is set
    std::string error_detail;             // short, never contains credentials, addresses or message text
    bool body_too_large = false;          // the transfer was stopped at Request::max_body_bytes
    bool storage_full = false;            // writing Request::save_body_to failed because the device is full
};

class HttpTransport {
public:
    virtual ~HttpTransport() = default;
    virtual Response perform(const Request& request) = 0;
};

}  // namespace pbo
