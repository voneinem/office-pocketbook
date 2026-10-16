#pragma once
// SmtpTransport (contracts/core-interfaces.md 2.2): one logical SMTP submission session. The production implementation drives
// libcurl; tests replace it. Not thread-safe: used from the sync thread only.
//
// `send` follows the outcome rule of contracts/imap-smtp-subset.md section 5: Accepted only on the server's 250 after the final dot;
// CertainFailure only when it is provable that nothing was accepted; everything else is Unknown.

#include <memory>
#include <string>
#include <vector>

#include "pbo/curl_session.h"
#include "pbo/imap_transport.h"
#include "pbo/types.h"

namespace pbo {

struct SmtpEndpoint {
    std::string host;
    int port = 465;
    Security security = Security::Tls;   // plaintext does not exist as a value
    std::string username;                // the password is passed per call and never stored here
};

enum class SendOutcome {
    Accepted,          // the server answered 250 after DATA: the message is out
    CertainFailure,    // nothing was accepted: refused recipient, auth/TLS/connect failure, or failure before the last body byte was handed over
    Unknown            // failure AFTER the last body byte was handed to the transport: may or may not have been delivered
};

struct SendResult {
    SendOutcome outcome = SendOutcome::CertainFailure;
    Error error;
};

class SmtpTransport {
public:
    virtual ~SmtpTransport() = default;

    // Connects, verifies TLS (and the enforced STARTTLS), says EHLO and authenticates, then ends the session without a mail
    // transaction. Zero credentials leave the device on any TLS failure. Errors: cert_*, starttls_unavailable, auth_failed,
    // server_unreachable, no_network, timeout, libcurl_too_old, protocol_error, host_invalid.
    virtual Error test_connection(const SmtpEndpoint& endpoint, const std::string& password) = 0;

    // Sends `raw_message` (complete RFC 5322 bytes). `Bcc` recipients are only in `recipients`. MUST return Unknown rather than
    // CertainFailure whenever the outcome is not provable.
    virtual SendResult send(const SmtpEndpoint& endpoint, const std::string& password, const std::string& envelope_from,
                            const std::vector<std::string>& recipients, const std::string& raw_message) = 0;
};

// Production transport. `ca_file` is the effective CA bundle (there is no "no CA" setting).
std::unique_ptr<SmtpTransport> make_curl_smtp_transport(const std::string& ca_file, const TransportTimeouts& timeouts = {});

}  // namespace pbo
