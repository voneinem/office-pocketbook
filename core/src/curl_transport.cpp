#include "pbo/curl_transport.h"

#include <curl/curl.h>

#include "pbo/curl_session.h"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <mutex>

namespace pbo {

namespace {

std::once_flag g_curl_init;

bool iequals(const std::string& a, const char* b) {
    size_t i = 0;
    for (; i < a.size() && b[i]; ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    return i == a.size() && b[i] == '\0';
}

// State of one transfer shared by the header and the body callbacks.
struct Transfer {
    std::string* body = nullptr;
    Headers* headers = nullptr;
    // file sink (Request::save_body_to)
    std::string save_to;
    long long max_bytes = 0;
    long long max_memory = 0;   // cap for an in-memory body, 0 = none
    FILE* file = nullptr;
    long status = 0;       // status of the response block being received (redirects start new blocks)
    long long total = 0;   // body bytes of a 2xx answer received so far
    bool too_large = false;
    bool storage_full = false;

    bool to_file() const { return !save_to.empty() && status >= 200 && status < 300; }
};

size_t write_body(char* data, size_t size, size_t nmemb, void* userdata) {
    auto* t = static_cast<Transfer*>(userdata);
    const size_t n = size * nmemb;
    if (!t->to_file()) {
        if (t->max_memory > 0 && static_cast<long long>(t->body->size() + n) > t->max_memory) {
            t->too_large = true;
            return 0;  // aborts the transfer
        }
        t->body->append(data, n);
        return n;
    }
    t->total += static_cast<long long>(n);
    if (t->max_bytes > 0 && t->total > t->max_bytes) {
        t->too_large = true;
        return 0;  // aborts the transfer
    }
    if (!t->file) {
        t->file = std::fopen(t->save_to.c_str(), "wb");
        if (!t->file) {
            t->storage_full = errno == ENOSPC || errno == EDQUOT || errno == EFBIG;
            return 0;
        }
    }
    if (n > 0 && std::fwrite(data, 1, n, t->file) != n) {
        t->storage_full = errno == ENOSPC || errno == EDQUOT || errno == EFBIG;
        return 0;
    }
    return n;
}

size_t write_header(char* data, size_t size, size_t nmemb, void* userdata) {
    auto* t = static_cast<Transfer*>(userdata);
    Headers* headers = t->headers;
    const size_t total = size * nmemb;
    std::string line(data, total);
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
    if (line.compare(0, 5, "HTTP/") == 0) {  // a new response block (redirects): start over
        headers->clear();
        const size_t space = line.find(' ');
        t->status = space == std::string::npos ? 0 : std::atol(line.c_str() + space + 1);
        t->total = 0;
    } else {
        const size_t colon = line.find(':');
        if (colon != std::string::npos) {
            std::string name = line.substr(0, colon);
            std::string value = line.substr(colon + 1);
            const size_t b = value.find_first_not_of(" \t");
            value = b == std::string::npos ? std::string() : value.substr(b);
            // a declared length over the cap stops the download before the body
            if (t->to_file() && t->max_bytes > 0 && iequals(name, "Content-Length") && std::atoll(value.c_str()) > t->max_bytes) {
                t->too_large = true;
                return 0;
            }
            if (!t->to_file() && t->max_memory > 0 && iequals(name, "Content-Length") && std::atoll(value.c_str()) > t->max_memory) {
                t->too_large = true;
                return 0;
            }
            headers->emplace_back(std::move(name), std::move(value));
        }
    }
    return total;
}

}  // namespace

CurlTransport::CurlTransport(std::string ca_file) : ca_file_(std::move(ca_file)) {
    std::call_once(g_curl_init, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

Response CurlTransport::perform(const Request& request) {
    Response out;
    if (!check_url_scheme(Protocol::Http, Security::Tls, request.url).ok()) {
        out.error = ErrorClass::Tls;
        out.error_key = "scheme_not_allowed";
        out.error_detail = "refusing a non-https URL";
        return out;
    }

    if (!request.upload_file.empty()) {  // refuse before anything is sent
        struct stat st;
        if (::stat(request.upload_file.c_str(), &st) != 0 || !S_ISREG(st.st_mode) || ::access(request.upload_file.c_str(), R_OK) != 0) {
            out.error = ErrorClass::Server;
            out.error_detail = "the upload file cannot be read";
            return out;
        }
    }

    CURL* curl = curl_easy_init();
    if (!curl) {
        out.error = ErrorClass::Network;
        out.error_detail = "curl init failed";
        return out;
    }

    struct curl_slist* header_list = nullptr;
    for (const auto& h : request.headers) header_list = curl_slist_append(header_list, (h.first + ": " + h.second).c_str());
    header_list = curl_slist_append(header_list, "Expect:");  // no 100-continue round trip

    curl_easy_setopt(curl, CURLOPT_URL, request.url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");

    // TLS: verification is mandatory and not configurable; the whole policy lives in curl_session.
    if (!apply_tls_policy(curl, ca_file_, Protocol::Http, Security::Tls).ok()) {
        out.error = ErrorClass::Tls;
        out.error_key = "no_ca_file";
        out.error_detail = "no certificate authority file";
        curl_slist_free_all(header_list);
        curl_easy_cleanup(curl);
        return out;
    }
    // The timeouts of the request replace the session defaults for this call.
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, request.connect_timeout_s);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, request.total_timeout_s);

    if (!request.username.empty() || !request.password.empty()) {
        curl_easy_setopt(curl, CURLOPT_HTTPAUTH, static_cast<long>(CURLAUTH_BASIC));
        curl_easy_setopt(curl, CURLOPT_USERNAME, request.username.c_str());
        curl_easy_setopt(curl, CURLOPT_PASSWORD, request.password.c_str());
    }

    curl_mime* mime = nullptr;
    if (!request.upload_file.empty()) {  // multipart/form-data with one part: the file
        mime = curl_mime_init(curl);
        curl_mimepart* part = curl_mime_addpart(mime);
        curl_mime_name(part, request.upload_field.c_str());
        curl_mime_filedata(part, request.upload_file.c_str());
        curl_mime_filename(part, request.upload_file_name.c_str());  // the name the server should see, not the path
        curl_mime_type(part, "application/octet-stream");
        curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
    } else if (request.method == "GET") {
        curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    } else if (request.method == "HEAD") {
        curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
    } else {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, request.method.c_str());
        if (!request.body.empty() || request.method == "POST" || request.method == "PUT") {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request.body.data());
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(request.body.size()));
            // a redirect (also 301/302/303) keeps method and body: a PROPFIND to /.well-known/caldav must stay a PROPFIND
            curl_easy_setopt(curl, CURLOPT_POSTREDIR, static_cast<long>(CURL_REDIR_POST_ALL));
        }
    }

    std::string body;
    Headers headers;
    Transfer transfer;
    transfer.body = &body;
    transfer.headers = &headers;
    if (request.method != "HEAD") {
        transfer.save_to = request.save_body_to;
        transfer.max_bytes = request.max_body_bytes;
        transfer.max_memory = request.max_response_bytes;
    }
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_body);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &transfer);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, write_header);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &transfer);

    char errbuf[CURL_ERROR_SIZE] = {0};
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);

    const CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    if (rc == CURLE_OK && !transfer.save_to.empty() && transfer.to_file() && !transfer.file && request.method != "HEAD") {
        transfer.file = std::fopen(transfer.save_to.c_str(), "wb");  // an empty 2xx body is an empty file
        if (!transfer.file) transfer.storage_full = errno == ENOSPC || errno == EDQUOT || errno == EFBIG;
    }
    if (transfer.file) {
        if (std::fclose(transfer.file) != 0) transfer.storage_full = true;  // the last write failed (disk full)
        transfer.file = nullptr;
    }
    const bool file_failed = transfer.too_large || transfer.storage_full || rc != CURLE_OK;
    if (!transfer.save_to.empty() && (file_failed || !(status >= 200 && status < 300))) std::remove(transfer.save_to.c_str());
    if (transfer.too_large) {
        out.status = status;
        out.headers = std::move(headers);
        out.body_too_large = true;
    } else if (transfer.storage_full) {
        out.status = status;
        out.storage_full = true;
        out.error_detail = "not enough storage space";
    } else if (rc == CURLE_OK) {
        out.status = status;
        out.headers = std::move(headers);
        out.body = std::move(body);
    } else {
        long verify_result = 0;
        curl_easy_getinfo(curl, CURLINFO_SSL_VERIFYRESULT, &verify_result);
        // Only the fixed text of the mapper: curl's own error buffer can contain the host name (FR-041).
        (void)errbuf;
        const Error mapped = map_curl_error(rc, verify_result, true);
        out.error = mapped.cls;
        out.error_key = mapped.key;
        out.error_detail = mapped.detail;
    }
    curl_slist_free_all(header_list);
    curl_easy_cleanup(curl);
    if (mime) curl_mime_free(mime);
    return out;
}

}  // namespace pbo
