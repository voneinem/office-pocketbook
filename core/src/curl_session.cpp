#include "pbo/curl_session.h"

#include <cctype>

#include "pbo/log.h"

namespace pbo {

namespace {

// OpenSSL X509_V_ERR_* numbers (the core does not link OpenSSL directly).
const long kX509CertHasExpired = 10;
const long kX509HostnameMismatch = 62;
const long kX509EmailMismatch = 63;
const long kX509IpAddressMismatch = 64;

std::string lower(const std::string& s) {
    std::string out = s;
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// The one scheme that belongs to (protocol, security).
const char* scheme_for(Protocol protocol, Security security) {
    switch (protocol) {
        case Protocol::Http:
            return security == Security::Tls ? "https" : nullptr;
        case Protocol::Imap:
            return security == Security::Tls ? "imaps" : "imap";
        case Protocol::Smtp:
            return security == Security::Tls ? "smtps" : "smtp";
    }
    return nullptr;
}

Error scheme_error() { return make_error(ErrorClass::Unsupported, "scheme_not_allowed", "this kind of address is not allowed"); }

const char* class_name(ErrorClass c) {
    switch (c) {
        case ErrorClass::None: return "none";
        case ErrorClass::Network: return "network";
        case ErrorClass::Tls: return "tls";
        case ErrorClass::Auth: return "auth";
        case ErrorClass::Server: return "server";
        case ErrorClass::Protocol: return "protocol";
        case ErrorClass::Unsupported: return "unsupported";
        case ErrorClass::Storage: return "storage";
        case ErrorClass::Cancelled: return "cancelled";
    }
    return "unknown";
}

}  // namespace

Result<TlsPolicy> make_tls_policy(Protocol protocol, Security security) {
    Result<TlsPolicy> r;
    const char* scheme = scheme_for(protocol, security);
    if (!scheme) {
        r.error = make_error(ErrorClass::Unsupported, "scheme_not_allowed", "this kind of address is not allowed");
        return r;
    }
    r.value.protocols = scheme;
    if (protocol == Protocol::Http) {
        r.value.redir_protocols = "https";
        r.value.follow_redirects = true;
    } else {
        r.value.use_ssl = static_cast<long>(CURLUSESSL_ALL);  // no continuation in plaintext, no downgrade
        r.value.follow_redirects = false;
    }
    return r;
}

Error check_url_scheme(Protocol protocol, Security security, const std::string& url) {
    const char* want = scheme_for(protocol, security);
    if (!want) return scheme_error();
    const std::string prefix = std::string(want) + "://";
    if (url.size() < prefix.size()) return scheme_error();
    if (lower(url.substr(0, prefix.size())) != prefix) return scheme_error();
    return Error();
}

Result<std::string> make_url(Protocol protocol, Security security, const std::string& host, int port, const std::string& path) {
    Result<std::string> r;
    const char* scheme = scheme_for(protocol, security);
    if (!scheme) {
        r.error = scheme_error();
        return r;
    }
    bool host_ok = !host.empty();
    for (char c : host) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u <= 0x20 || u == 0x7f || c == '/' || c == '@' || c == ':' || c == '?' || c == '#' || c == '\\') host_ok = false;
    }
    if (!host_ok) {
        r.error = make_error(ErrorClass::Unsupported, "bad_host", "the server name is not valid");
        return r;
    }
    if (port < 1 || port > 65535) {
        r.error = make_error(ErrorClass::Unsupported, "bad_port", "the port is not valid");
        return r;
    }
    r.value = std::string(scheme) + "://" + host + ":" + std::to_string(port) + path;
    return r;
}

Error apply_tls_policy(CURL* curl, const std::string& ca_file, Protocol protocol, Security security) {
    if (!curl) return make_error(ErrorClass::Protocol, "no_handle", "");
    if (ca_file.empty()) return make_error(ErrorClass::Tls, "no_ca_file", "no certificate authority file");
    Result<TlsPolicy> p = make_tls_policy(protocol, security);
    if (!p.ok()) return p.error;
    const TlsPolicy& t = p.value;

    // Verification is mandatory and not configurable (constitution Principle IV).
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, t.verify_peer);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, t.verify_host);
    curl_easy_setopt(curl, CURLOPT_CAINFO, ca_file.c_str());
    curl_easy_setopt(curl, CURLOPT_SSLVERSION, t.min_tls_version);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, t.protocols.c_str());
    if (t.use_ssl) curl_easy_setopt(curl, CURLOPT_USE_SSL, static_cast<long>(CURLUSESSL_ALL));  // never TRY/CONTROL (R7)
    if (t.follow_redirects) {
        curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, t.redir_protocols.c_str());
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    } else {
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    }
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, t.connect_timeout_s);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, t.low_speed_limit);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, t.low_speed_time_s);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, t.total_timeout_s);
    return Error();
}

Error map_curl_error(CURLcode code, long ssl_verify_result, bool wifi_up) {
    switch (code) {
        case CURLE_OK:
            return Error();
        case CURLE_PEER_FAILED_VERIFICATION:
        case CURLE_SSL_CERTPROBLEM:
        case CURLE_SSL_ISSUER_ERROR:
        case CURLE_SSL_INVALIDCERTSTATUS:
            if (ssl_verify_result == kX509CertHasExpired)
                return make_error(ErrorClass::Tls, "cert_expired", "the server certificate has expired");
            if (ssl_verify_result == kX509HostnameMismatch || ssl_verify_result == kX509IpAddressMismatch ||
                ssl_verify_result == kX509EmailMismatch)
                return make_error(ErrorClass::Tls, "cert_host_mismatch", "the server certificate is for another server name");
            // libcurl checks the host name itself after the chain verified fine and then reports verify result 1 (observed with
            // 8.18.0; older code paths leave 0): with verification always on and nothing pinned, a refused certificate whose chain
            // was fine can only be the name that did not match
            if (code == CURLE_PEER_FAILED_VERIFICATION && (ssl_verify_result == 0 || ssl_verify_result == 1))
                return make_error(ErrorClass::Tls, "cert_host_mismatch", "the server certificate is for another server name");
            return make_error(ErrorClass::Tls, "cert_untrusted", "the server certificate is not trusted");
        case CURLE_SSL_CACERT_BADFILE:
            return make_error(ErrorClass::Tls, "cert_untrusted", "the certificate authority file cannot be used");
        case CURLE_SSL_CONNECT_ERROR:
        case CURLE_SSL_CIPHER:
            return make_error(ErrorClass::Tls, "tls_failed", "the secure connection could not be set up");
        case CURLE_USE_SSL_FAILED:
            return make_error(ErrorClass::Tls, "starttls_unavailable", "the server does not offer an encrypted connection");
        case CURLE_UNSUPPORTED_PROTOCOL:
            return make_error(ErrorClass::Tls, "scheme_not_allowed", "this kind of address is not allowed");
        case CURLE_LOGIN_DENIED:
            return make_error(ErrorClass::Auth, "auth_failed", "the user name or password was refused");
        case CURLE_COULDNT_RESOLVE_HOST:
        case CURLE_COULDNT_CONNECT:
        case CURLE_COULDNT_RESOLVE_PROXY:
            return wifi_up ? make_error(ErrorClass::Network, "server_unreachable", "the server cannot be reached")
                           : make_error(ErrorClass::Network, "no_network", "there is no network connection");
        case CURLE_OPERATION_TIMEDOUT:
            return make_error(ErrorClass::Network, "timeout", "the server did not answer in time");
        case CURLE_RECV_ERROR:
        case CURLE_SEND_ERROR:
        case CURLE_GOT_NOTHING:
            return make_error(ErrorClass::Network, "connection_lost", "the connection was lost");
        case CURLE_REMOTE_FILE_NOT_FOUND:
            return make_error(ErrorClass::Server, "not_found", "the item was not found on the server");
        case CURLE_FILESIZE_EXCEEDED:
            return make_error(ErrorClass::Server, "too_large", "the item is larger than the allowed size");
        case CURLE_TOO_MANY_REDIRECTS:
        case CURLE_URL_MALFORMAT:
        case CURLE_BAD_CONTENT_ENCODING:
            return make_error(ErrorClass::Server, "protocol_error", "the server answered in an unexpected way");
        default:
            return make_error(ErrorClass::Protocol, "protocol_error", "the server answered in an unexpected way");
    }
}

Error check_curl_runtime(unsigned version_num) {
    if (version_num < kMinCurlVersionNum) return {ErrorClass::Unsupported, "libcurl_too_old", "libcurl is older than 8.18.0"};
    return {};
}

Error check_curl_runtime() {
    const curl_version_info_data* info = curl_version_info(CURLVERSION_NOW);
    return check_curl_runtime(info ? info->version_num : 0u);
}

void log_failure(const char* event_code, const Error& error) {
    log::event(event_code, {log::text("class", class_name(error.cls)), log::text("cause", error.key)});
}

}  // namespace pbo
