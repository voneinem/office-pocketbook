#include "pbo/imap_transport.h"

#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <cstring>

#include "pbo/account_input.h"
#include "pbo/imap_parse.h"
#include "pbo/log.h"

namespace pbo {

namespace {

// Appends what libcurl delivers, with a hard cap (a hostile server must not fill the memory).
struct Collector {
    std::string data;
    bool overflow = false;
};

size_t collect(char* ptr, size_t size, size_t nmemb, void* user) {
    auto* c = static_cast<Collector*>(user);
    const size_t n = size * nmemb;
    if (c->data.size() + n > kMaxImapInput) {
        c->overflow = true;
        return 0;   // aborts the transfer
    }
    c->data.append(ptr, n);
    return n;
}

// Streams the literal of one "* n FETCH (UID u BODY[s] {N}\r\n<N bytes>)" answer into a sink. The text before the literal is
// kept (4 KiB at most) only to find the "{N}" marker; a literal larger than the limit is refused before a byte is taken;
// whatever follows the N bytes is ignored.
struct LiteralStream {
    ByteSink* sink = nullptr;
    uint64_t max_bytes = 0;
    std::string head;            // the answer up to the end of the "{N}" line
    bool in_literal = false;
    uint64_t expected = 0, received = 0;
    bool too_large = false, sink_refused = false, no_literal = false;
};

size_t to_literal(char* ptr, size_t size, size_t nmemb, void* user) {
    auto* s = static_cast<LiteralStream*>(user);
    size_t n = size * nmemb;
    const size_t total = n;
    if (!s->in_literal) {
        if (s->head.size() + n > 4096) {   // no literal marker in the first 4 KiB: this is not a part
            s->no_literal = true;
            return 0;
        }
        const size_t before = s->head.size();
        s->head.append(ptr, n);
        // the marker ends the first line that ends with "}"
        size_t eol = s->head.find('\n');
        if (eol == std::string::npos) return total;
        size_t end = eol;
        if (end > 0 && s->head[end - 1] == '\r') --end;
        if (end == 0 || s->head[end - 1] != '}') {   // the first line has no literal: NIL or something else
            s->no_literal = true;
            return 0;
        }
        const size_t open = s->head.rfind('{', end - 1);
        if (open == std::string::npos) { s->no_literal = true; return 0; }
        uint64_t len = 0;
        bool digits = false;
        for (size_t k = open + 1; k + 1 < end; ++k) {
            if (s->head[k] < '0' || s->head[k] > '9') { if (s->head[k] == '+' && k + 2 == end) break; s->no_literal = true; return 0; }
            len = len * 10 + static_cast<uint64_t>(s->head[k] - '0');
            digits = true;
            if (len > (1ull << 40)) break;
        }
        if (!digits) { s->no_literal = true; return 0; }
        if (len > s->max_bytes) { s->too_large = true; return 0; }
        s->in_literal = true;
        s->expected = len;
        // bytes after the line end belong to the literal
        ptr += (eol + 1) - before;
        n = s->head.size() - (eol + 1);
    }
    const uint64_t room = s->expected > s->received ? s->expected - s->received : 0;
    const size_t take = static_cast<size_t>(std::min<uint64_t>(n, room));
    if (take > 0) {
        if (!s->sink->write(ptr, take)) {
            s->sink_refused = true;
            return 0;
        }
        s->received += take;
    }
    return total;   // the rest (the closing parenthesis) is ignored
}

struct Upload {
    const std::string* data = nullptr;
    size_t pos = 0;
};

size_t from_string(char* buf, size_t size, size_t nmemb, void* user) {
    auto* u = static_cast<Upload*>(user);
    const size_t want = size * nmemb;
    const size_t left = u->data->size() - u->pos;
    const size_t n = std::min(want, left);
    std::memcpy(buf, u->data->data() + u->pos, n);
    u->pos += n;
    return n;
}

class CurlImapTransport : public ImapTransport {
public:
    CurlImapTransport(std::string ca_file, TransportTimeouts timeouts) : ca_file_(std::move(ca_file)), timeouts_(timeouts) {}
    ~CurlImapTransport() override { disconnect(); }

    Error connect(const ImapEndpoint& endpoint, const std::string& password) override {
        disconnect();
        Error runtime = check_curl_runtime();
        if (!runtime.ok()) return runtime;
        if (!is_valid_host(endpoint.host)) return make_error(ErrorClass::Unsupported, "host_invalid", "the server name is not valid");
        if (endpoint.username.empty()) return make_error(ErrorClass::Auth, "auth_failed", "no user name");
        auto url = make_url(Protocol::Imap, endpoint.security, endpoint.host, endpoint.port, "/");
        if (!url.ok()) return url.error;

        curl_ = curl_easy_init();
        if (!curl_) return make_error(ErrorClass::Protocol, "no_handle", "libcurl could not be started");
        Error policy = apply_tls_policy(curl_, ca_file_, Protocol::Imap, endpoint.security);
        if (!policy.ok()) {
            disconnect();
            return policy;
        }
        curl_easy_setopt(curl_, CURLOPT_CONNECTTIMEOUT, timeouts_.connect_s);
        curl_easy_setopt(curl_, CURLOPT_LOW_SPEED_TIME, timeouts_.low_speed_s);
        curl_easy_setopt(curl_, CURLOPT_TIMEOUT, timeouts_.command_s);
        curl_easy_setopt(curl_, CURLOPT_USERNAME, endpoint.username.c_str());
        curl_easy_setopt(curl_, CURLOPT_PASSWORD, password.c_str());
        endpoint_ = endpoint;
        base_url_ = url.value;   // "<scheme>://host:port/"

        // The first request makes libcurl connect, verify the certificate, upgrade with STARTTLS if configured and log
        // in. CAPABILITY is a harmless command that needs no mailbox.
        connected_ = false;
        Result<ImapRaw> r = run_command(base_url_, "CAPABILITY");
        if (!r.ok()) {
            Error e = r.error;
            disconnect();
            log_failure("imap.connect_failed", e);
            return e;
        }
        caps_ = parse_capability(r.value);
        if (!caps_.count("imap4rev1") && !caps_.count("imap4")) {
            // a server that does not say it speaks IMAP4rev1 is not one this app can work with
            disconnect();
            return make_error(ErrorClass::Protocol, "protocol_error", "the server did not announce IMAP4rev1");
        }
        connected_ = true;
        return {};
    }

    void disconnect() override {
        if (curl_) {
            curl_easy_cleanup(curl_);
            curl_ = nullptr;
        }
        connected_ = false;
        caps_.clear();
    }

    Result<ImapRaw> command(const std::string& mailbox, const std::string& command) override {
        Result<ImapRaw> r;
        if (!curl_) {
            r.error = make_error(ErrorClass::Protocol, "not_connected", "no connection");
            return r;
        }
        if (command.find('\r') != std::string::npos || command.find('\n') != std::string::npos) {
            r.error = make_error(ErrorClass::Protocol, "bad_command", "a command must be one line");   // no command injection
            return r;
        }
        r = run_command(url_for(mailbox), command);
        // libcurl reports a failed SELECT of the mailbox as "login denied"; after a successful connect that means
        // the mailbox cannot be opened, not that the password is wrong.
        if (!r.ok() && r.error.key == "auth_failed" && connected_) r.error = make_error(ErrorClass::Server, "mailbox_unavailable", "the folder cannot be opened");
        return r;
    }

    Error fetch_to(const std::string& mailbox, uint32_t uid, const std::string& section, uint64_t max_bytes, ByteSink& sink) override {
        if (!curl_) return make_error(ErrorClass::Protocol, "not_connected", "no connection");
        for (char c : section)
            if (!std::isdigit(static_cast<unsigned char>(c)) && c != '.') return make_error(ErrorClass::Protocol, "bad_section", "not a body section");
        if (mailbox.empty()) return make_error(ErrorClass::Protocol, "bad_command", "no folder");
        // BODY.PEEK: reading a part must not mark the message as read. (libcurl's own URL form for parts sends BODY[...],
        // which real servers answer by setting \Seen.)
        const std::string command = "UID FETCH " + std::to_string(uid) + " (BODY.PEEK[" + section + "])";
        reset_request();
        LiteralStream stream;
        stream.sink = &sink;
        stream.max_bytes = max_bytes;
        curl_easy_setopt(curl_, CURLOPT_URL, url_for(mailbox).c_str());
        curl_easy_setopt(curl_, CURLOPT_CUSTOMREQUEST, command.c_str());
        curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, to_literal);
        curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &stream);
        const CURLcode rc = curl_easy_perform(curl_);
        if (stream.too_large) return make_error(ErrorClass::Server, "too_large", "the item is larger than the allowed size");
        if (stream.sink_refused) return make_error(ErrorClass::Storage, "write_failed", "the data could not be stored");
        if (stream.no_literal) return make_error(ErrorClass::Server, "not_found", "the part was not found on the server");
        if (rc != CURLE_OK) {
            Error e = mapped(rc);
            if (e.key == "auth_failed" && connected_) e = make_error(ErrorClass::Server, "mailbox_unavailable", "the folder cannot be opened");
            return e;
        }
        if (!stream.in_literal) return make_error(ErrorClass::Server, "not_found", "the part was not found on the server");
        if (stream.received != stream.expected) return make_error(ErrorClass::Network, "connection_lost", "the connection was lost");
        return {};
    }

    Error append(const std::string& mailbox, const std::string& message) override {
        if (!curl_) return make_error(ErrorClass::Protocol, "not_connected", "no connection");
        reset_request();
        Upload up;
        up.data = &message;
        Collector sink;
        curl_easy_setopt(curl_, CURLOPT_URL, url_for(mailbox).c_str());
        curl_easy_setopt(curl_, CURLOPT_UPLOAD, 1L);
        curl_easy_setopt(curl_, CURLOPT_INFILESIZE_LARGE, static_cast<curl_off_t>(message.size()));
        curl_easy_setopt(curl_, CURLOPT_READFUNCTION, from_string);
        curl_easy_setopt(curl_, CURLOPT_READDATA, &up);
        curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, collect);
        curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &sink);
        const CURLcode rc = curl_easy_perform(curl_);
        curl_easy_setopt(curl_, CURLOPT_UPLOAD, 0L);
        if (rc == CURLE_OK) return {};
        Error e = mapped(rc);
        if (e.key == "auth_failed" && connected_) e = make_error(ErrorClass::Server, "mailbox_unavailable", "the folder cannot be opened");
        return e;
    }

    const std::set<std::string>& capabilities() const override { return caps_; }

private:
    std::string url_for(const std::string& mailbox) {
        if (mailbox.empty()) return base_url_;
        char* esc = curl_easy_escape(curl_, mailbox.c_str(), static_cast<int>(mailbox.size()));
        std::string url = base_url_ + (esc ? esc : "");
        curl_free(esc);
        return url;
    }

    void reset_request() {
        curl_easy_setopt(curl_, CURLOPT_CUSTOMREQUEST, nullptr);
        curl_easy_setopt(curl_, CURLOPT_UPLOAD, 0L);
        curl_easy_setopt(curl_, CURLOPT_NOBODY, 0L);
    }

    Error mapped(CURLcode rc) {
        long verify = 0;
        curl_easy_getinfo(curl_, CURLINFO_SSL_VERIFYRESULT, &verify);
        return map_curl_error(rc, verify, /*wifi_up=*/true);
    }

    Result<ImapRaw> run_command(const std::string& url, const std::string& command) {
        Result<ImapRaw> r;
        reset_request();
        Collector out;
        curl_easy_setopt(curl_, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl_, CURLOPT_CUSTOMREQUEST, command.c_str());
        curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, collect);
        curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &out);
        const CURLcode rc = curl_easy_perform(curl_);
        if (out.overflow) {
            r.error = make_error(ErrorClass::Server, "too_large", "the answer is larger than the allowed size");
            return r;
        }
        if (rc != CURLE_OK) {
            r.error = mapped(rc);
            return r;
        }
        r.value.data = std::move(out.data);
        return r;
    }

    std::string ca_file_;
    TransportTimeouts timeouts_;
    CURL* curl_ = nullptr;
    ImapEndpoint endpoint_;
    std::string base_url_;
    std::set<std::string> caps_;
    bool connected_ = false;
};

}  // namespace

std::unique_ptr<ImapTransport> make_curl_imap_transport(const std::string& ca_file, const TransportTimeouts& timeouts) {
    return std::make_unique<CurlImapTransport>(ca_file, timeouts);
}

}  // namespace pbo
