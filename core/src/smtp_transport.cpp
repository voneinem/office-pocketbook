#include "pbo/smtp_transport.h"

#include <curl/curl.h>

#include "pbo/account_input.h"
#include "pbo/log.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace pbo {

namespace {

size_t discard(char*, size_t size, size_t nmemb, void*) { return size * nmemb; }

class CurlSmtpTransport : public SmtpTransport {
public:
    CurlSmtpTransport(std::string ca_file, TransportTimeouts timeouts) : ca_file_(std::move(ca_file)), timeouts_(timeouts) {}

    Error test_connection(const SmtpEndpoint& endpoint, const std::string& password) override {
        Error runtime = check_curl_runtime();
        if (!runtime.ok()) return runtime;
        if (!is_valid_host(endpoint.host)) return make_error(ErrorClass::Unsupported, "host_invalid", "the server name is not valid");
        if (endpoint.username.empty()) return make_error(ErrorClass::Auth, "auth_failed", "no user name");
        // The path of an SMTP URL is the name the client gives in EHLO. The machine's own host name is not sent.
        auto url = make_url(Protocol::Smtp, endpoint.security, endpoint.host, endpoint.port, "/localhost");
        if (!url.ok()) return url.error;

        CURL* curl = curl_easy_init();
        if (!curl) return make_error(ErrorClass::Protocol, "no_handle", "libcurl could not be started");
        Error e = apply_tls_policy(curl, ca_file_, Protocol::Smtp, endpoint.security);
        if (e.ok()) {
            curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, timeouts_.connect_s);
            curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, timeouts_.low_speed_s);
            curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeouts_.command_s);
            curl_easy_setopt(curl, CURLOPT_USERNAME, endpoint.username.c_str());
            curl_easy_setopt(curl, CURLOPT_PASSWORD, password.c_str());
            curl_easy_setopt(curl, CURLOPT_URL, url.value.c_str());
            // connect, TLS, EHLO and AUTH happen before the request; NOOP is the harmless request that follows, no mail is started
            curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "NOOP");
            curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discard);
            const CURLcode rc = curl_easy_perform(curl);
            if (rc != CURLE_OK) {
                long verify = 0;
                curl_easy_getinfo(curl, CURLINFO_SSL_VERIFYRESULT, &verify);
                e = map_curl_error(rc, verify, /*wifi_up=*/true);
                log_failure("smtp.connect_failed", e);
            }
        }
        curl_easy_cleanup(curl);
        return e;
    }

    SendResult send(const SmtpEndpoint& endpoint, const std::string& password, const std::string& envelope_from, const std::vector<std::string>& recipients,
                    const std::string& raw_message) override {
        SendResult result;   // CertainFailure until proven otherwise: nothing has been attempted yet
        auto fail = [&](ErrorClass cls, const char* key, const char* detail) {
            result.outcome = SendOutcome::CertainFailure;
            result.error = make_error(cls, key, detail);
            return result;
        };
        Error runtime = check_curl_runtime();
        if (!runtime.ok()) {
            result.error = runtime;
            return result;
        }
        if (!is_valid_host(endpoint.host)) return fail(ErrorClass::Unsupported, "host_invalid", "the server name is not valid");
        if (endpoint.username.empty()) return fail(ErrorClass::Auth, "auth_failed", "no user name");
        if (recipients.empty() || recipients.size() > 200) return fail(ErrorClass::Unsupported, "no_recipient", "no recipient");
        if (raw_message.empty() || raw_message.size() > (32u << 20)) return fail(ErrorClass::Unsupported, "too_large", "the message is empty or too large");
        auto bad_address = [](const std::string& a) {
            if (a.empty() || a.size() > 320) return true;
            for (unsigned char c : a)
                if (c <= 0x20 || c == '<' || c == '>' || c >= 0x7f) return true;
            return false;
        };
        if (bad_address(envelope_from)) return fail(ErrorClass::Unsupported, "from_invalid", "the sender address is not valid");
        for (const std::string& r : recipients)
            if (bad_address(r)) return fail(ErrorClass::Unsupported, "recipient_invalid", "a recipient address is not valid");
        auto url = make_url(Protocol::Smtp, endpoint.security, endpoint.host, endpoint.port, "/localhost");
        if (!url.ok()) {
            result.error = url.error;
            return result;
        }

        CURL* curl = curl_easy_init();
        if (!curl) return fail(ErrorClass::Protocol, "no_handle", "libcurl could not be started");
        Error e = apply_tls_policy(curl, ca_file_, Protocol::Smtp, endpoint.security);
        if (!e.ok()) {
            curl_easy_cleanup(curl);
            result.error = e;
            return result;
        }
        struct Upload {
            const std::string* data;
            size_t pos = 0;
            bool eof_delivered = false;   // the last byte of the message has been handed to libcurl
        } upload{&raw_message};
        auto read_cb = [](char* buffer, size_t size, size_t nitems, void* user) -> size_t {
            Upload* u = static_cast<Upload*>(user);
            const size_t room = size * nitems;
            const size_t n = std::min(room, u->data->size() - u->pos);
            std::memcpy(buffer, u->data->data() + u->pos, n);
            u->pos += n;
            // libcurl knows the size and may never ask for the end: the last byte is handed over when the position reaches the size
            if (u->pos >= u->data->size()) u->eof_delivered = true;
            return n;
        };
        curl_slist* rcpt = nullptr;
        for (const std::string& r : recipients) rcpt = curl_slist_append(rcpt, ("<" + r + ">").c_str());
        const std::string from = "<" + envelope_from + ">";

        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, timeouts_.connect_s);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, timeouts_.low_speed_s);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeouts_.command_s + static_cast<long>(raw_message.size() / 8192));
        curl_easy_setopt(curl, CURLOPT_USERNAME, endpoint.username.c_str());
        curl_easy_setopt(curl, CURLOPT_PASSWORD, password.c_str());
        curl_easy_setopt(curl, CURLOPT_URL, url.value.c_str());
        curl_easy_setopt(curl, CURLOPT_MAIL_FROM, from.c_str());
        curl_easy_setopt(curl, CURLOPT_MAIL_RCPT, rcpt);
        curl_easy_setopt(curl, CURLOPT_MAIL_RCPT_ALLOWFAILS, 0L);   // one refused recipient fails the whole message: nothing is sent to the others
        curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
        curl_easy_setopt(curl, CURLOPT_READFUNCTION, static_cast<size_t (*)(char*, size_t, size_t, void*)>(read_cb));
        curl_easy_setopt(curl, CURLOPT_READDATA, &upload);
        curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE, static_cast<curl_off_t>(raw_message.size()));
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discard);
        char errbuf[CURL_ERROR_SIZE] = {0};
        curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);

        const CURLcode rc = curl_easy_perform(curl);
        long smtp_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &smtp_code);   // the last reply code of the server
        long verify = 0;
        curl_easy_getinfo(curl, CURLINFO_SSL_VERIFYRESULT, &verify);
        curl_slist_free_all(rcpt);
        curl_easy_cleanup(curl);

        if (rc == CURLE_OK) {   // 250 after the final dot
            result.outcome = SendOutcome::Accepted;
            return result;
        }
        result.error = map_curl_error(rc, verify, /*wifi_up=*/true);
        // The server's own words about the failure (its reply code), when it gave one.
        auto by_code = [&]() {
            if (smtp_code >= 400 && smtp_code < 500) return make_error(ErrorClass::Server, "server_busy", "the server cannot take the message now");
            if (smtp_code == 552) return make_error(ErrorClass::Server, "too_large", "the server refused the size of the message");
            if (smtp_code >= 500) return make_error(ErrorClass::Server, "rejected_by_server", "the server rejected the message");
            return Error();
        };
        // libcurl's own fixed texts tell which command was answered with an error ("RCPT failed: 550"); they are read for this only and never kept
        const std::string why = errbuf;
        const bool answered_with_error = smtp_code >= 400 && (rc == CURLE_WEIRD_SERVER_REPLY || (rc == CURLE_SEND_ERROR && why.compare(0, 6, "Send f") != 0));
        if (rc == CURLE_SEND_ERROR && why.compare(0, 11, "RCPT failed") == 0) {   // the address is not named here: FR-041
            result.error = smtp_code == 552 || smtp_code == 452 ? make_error(ErrorClass::Server, "too_large", "the server refused the size of the message")
                           : smtp_code >= 400 && smtp_code < 500 ? make_error(ErrorClass::Server, "server_busy", "the server cannot take the message now")
                                                                  : make_error(ErrorClass::Server, "refused_recipient", "a recipient was refused");
        } else if (answered_with_error) {
            const Error c = by_code();
            if (!c.ok()) result.error = c;
        }
        if (!upload.eof_delivered) {
            // the last byte never left the app: nothing can have been accepted
            result.outcome = SendOutcome::CertainFailure;
        } else if (answered_with_error) {
            // the server answered the end of the data with an error: nothing was accepted
            result.outcome = SendOutcome::CertainFailure;
        } else {
            result.outcome = SendOutcome::Unknown;   // no final answer: the message may have been delivered
            if (result.error.ok() || result.error.key == "protocol_error") result.error = make_error(ErrorClass::Network, "connection_lost", "the connection was lost");
        }
        log::event("smtp.send_failed", {log::text("class", result.outcome == SendOutcome::Unknown ? "unknown" : "certain"), log::text("cause", result.error.key)});
        return result;
    }

private:
    std::string ca_file_;
    TransportTimeouts timeouts_;
};

}  // namespace

std::unique_ptr<SmtpTransport> make_curl_smtp_transport(const std::string& ca_file, const TransportTimeouts& timeouts) {
    return std::make_unique<CurlSmtpTransport>(ca_file, timeouts);
}

}  // namespace pbo
