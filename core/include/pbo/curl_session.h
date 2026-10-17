#pragma once
// The shared TLS and endpoint policy of every libcurl transport (IMAP, SMTP, HTTPS), research R-12.
//
// Constitution Principle IV: certificate and host-name verification are always on and cannot be turned off;
// only imaps://, smtps:// and https:// endpoints, or the clear-text mail schemes with STARTTLS enforced
// (CURLUSESSL_ALL), are accepted; there is no plaintext fallback. curl_session.cpp is the ONLY source file
// allowed to name the clear-text mail schemes (scripts/check-constitution.sh rule R7).

#include <curl/curl.h>

#include <optional>
#include <string>

#include "pbo/types.h"

namespace pbo {

enum class Protocol { Http, Imap, Smtp };
// Tls: implicit TLS from the first byte (imaps, smtps, https). StartTls: plaintext greeting, then an enforced upgrade.
enum class Security { Tls, StartTls };

struct TlsPolicy {
    long verify_peer = 1;
    long verify_host = 2;
    long min_tls_version = CURL_SSLVERSION_TLSv1_2;
    std::optional<long> use_ssl;   // CURLUSESSL_ALL for IMAP/SMTP; unset for HTTP
    std::string protocols;         // exactly one scheme
    std::string redir_protocols;   // HTTP only
    bool follow_redirects = false;
    long connect_timeout_s = 15;
    long low_speed_limit = 1;      // bytes per second ...
    long low_speed_time_s = 30;    // ... for this long
    long total_timeout_s = 120;
};

// Http requires Security::Tls (there is no STARTTLS for it).
Result<TlsPolicy> make_tls_policy(Protocol protocol, Security security);

// Accepts only the scheme that belongs to this transport and security mode (case-insensitive). Every other
// scheme, including a plaintext one without STARTTLS, is refused before any connection: Unsupported,
// key "scheme_not_allowed".
Error check_url_scheme(Protocol protocol, Security security, const std::string& url);

// Builds "<scheme>://<host>:<port><path>". The host must be non-empty and free of blanks, control characters
// and the characters / @ ; the port 1..65535.
Result<std::string> make_url(Protocol protocol, Security security, const std::string& host, int port,
                             const std::string& path);

// Sets the policy on a handle: verification, the CA file, the TLS minimum, the allowed schemes, STARTTLS
// enforcement and the timeouts. `ca_file` must not be empty: there is no "no CA" setting.
Error apply_tls_policy(CURL* curl, const std::string& ca_file, Protocol protocol, Security security);

// Maps a libcurl result to the stable cause keys. `ssl_verify_result` is CURLINFO_SSL_VERIFYRESULT (an OpenSSL
// X509_V_ERR_* number); `wifi_up` tells "no network" from "server unreachable". Takes no error-buffer text on
// purpose: the buffer contains host names, so nothing from it can reach Error::detail.
Error map_curl_error(CURLcode code, long ssl_verify_result, bool wifi_up);

// The oldest libcurl the mail transports run on (research R-01, known limitation 5): 8.18.0. Older versions (8.12.0 on
// the reader, 8.16.0 in the SDK) drop the contents of IMAP literals, so the app carries 8.18.0 statically on the
// reader. `check_curl_runtime()` looks at the library in use; the overload takes a CURLVERSION number (0xMMmmpp) for
// tests. An unknown version fails closed.
inline constexpr unsigned kMinCurlVersionNum = 0x081200;
Error check_curl_runtime(unsigned version_num);
Error check_curl_runtime();   // curl_version_info(CURLVERSION_NOW)->version_num

// Logs one event with the error class and cause key only (never Error::detail).
void log_failure(const char* event_code, const Error& error);

}  // namespace pbo
