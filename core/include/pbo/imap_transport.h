#pragma once
// ImapTransport (contracts/core-interfaces.md 2.1): one logical IMAP session. The production implementation drives
// libcurl; tests and the fallback client replace it. Not thread-safe: used from the sync thread only.

#include <cstdint>
#include <memory>
#include <set>
#include <string>

#include "pbo/curl_session.h"
#include "pbo/types.h"

namespace pbo {

// Receives streamed bytes. Returning false stops the transfer (the transport then reports the reason it knows).
class ByteSink {
public:
    virtual ~ByteSink() = default;
    virtual bool write(const char* data, size_t n) = 0;
};

struct ImapEndpoint {
    std::string host;
    int port = 993;
    Security security = Security::Tls;   // plaintext does not exist as a value
    std::string username;                // the password is passed per connect and never stored here
};

// Raw untagged response data of one command, exactly the bytes libcurl delivered (literals included).
struct ImapRaw {
    std::string data;
};

class ImapTransport {
public:
    virtual ~ImapTransport() = default;

    // Verifies TLS (and the enforced STARTTLS), logs in, caches CAPABILITY. Zero credentials leave the device on any
    // TLS failure. Errors: cert_*, starttls_unavailable, auth_failed, server_unreachable, no_network, timeout,
    // libcurl_too_old, protocol_error.
    virtual Error connect(const ImapEndpoint& endpoint, const std::string& password) = 0;
    virtual void disconnect() = 0;

    // One IMAP command as a custom request on the mailbox (empty = none selected), for example
    // "UID FETCH 1:* (FLAGS)". `mailbox` is the raw (modified UTF-7) name.
    virtual Result<ImapRaw> command(const std::string& mailbox, const std::string& command) = 0;

    // Streams the body of "<mailbox>;UID=<uid>[;SECTION=<s>]" into `sink`; stops as soon as more than `max_bytes`
    // arrive (Error key "too_large"; the caller discards what the sink got).
    virtual Error fetch_to(const std::string& mailbox, uint32_t uid, const std::string& section, uint64_t max_bytes,
                           ByteSink& sink) = 0;

    // APPEND: uploads `message` to `mailbox`. libcurl sends neither flags nor a date (observed with 8.18.0), so the
    // server stamps the message (research R-09) and it arrives without \Seen: callers that need flags find the
    // message by Message-ID afterwards and set them with UID STORE.
    virtual Error append(const std::string& mailbox, const std::string& message) = 0;

    virtual const std::set<std::string>& capabilities() const = 0;   // lowercase tokens
};

// Time limits of one session. The defaults are the policy of research R-12; tests shorten them.
struct TransportTimeouts {
    long connect_s = 15;
    long low_speed_s = 30;    // less than 1 byte/s for this long aborts
    long command_s = 120;     // whole command
};

// Production transport. `ca_file` is the effective CA bundle (there is no "no CA" setting).
std::unique_ptr<ImapTransport> make_curl_imap_transport(const std::string& ca_file, const TransportTimeouts& timeouts = {});

}  // namespace pbo
