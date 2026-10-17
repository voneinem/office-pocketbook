#pragma once
// A tiny TLS line client for the self-tests of the fake servers (IMAP/SMTP): connects to 127.0.0.1, verifies the
// server certificate against a CA file like a real client would, sends lines and reads CRLF-terminated lines.
// It can also start in plaintext and upgrade (STARTTLS). Test support only; the product uses libcurl.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <string>

namespace pbtest {

class TlsLineClient {
public:
    TlsLineClient() = default;
    ~TlsLineClient() { close(); }
    TlsLineClient(const TlsLineClient&) = delete;
    TlsLineClient& operator=(const TlsLineClient&) = delete;

    // Opens the TCP connection. With `implicit_tls` the TLS handshake follows at once.
    bool connect(int port, const std::string& ca_file, bool implicit_tls, const std::string& server_name = "localhost") {
        ca_file_ = ca_file;
        server_name_ = server_name;
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(static_cast<uint16_t>(port));
        if (::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) return false;
        return implicit_tls ? start_tls() : true;
    }

    // TLS handshake with certificate and host-name verification against ca_file_.
    bool start_tls() {
        ctx_ = SSL_CTX_new(TLS_client_method());
        if (!ctx_) return false;
        if (SSL_CTX_load_verify_locations(ctx_, ca_file_.c_str(), nullptr) != 1) return false;
        SSL_CTX_set_verify(ctx_, SSL_VERIFY_PEER, nullptr);
        ssl_ = SSL_new(ctx_);
        SSL_set_fd(ssl_, fd_);
        SSL_set1_host(ssl_, server_name_.c_str());
        SSL_set_tlsext_host_name(ssl_, server_name_.c_str());
        return SSL_connect(ssl_) == 1;
    }

    bool send_line(const std::string& line) { return write_all(line + "\r\n"); }
    bool write_all(const std::string& data) {
        size_t off = 0;
        while (off < data.size()) {
            int n = ssl_ ? SSL_write(ssl_, data.data() + off, static_cast<int>(data.size() - off))
                         : static_cast<int>(::send(fd_, data.data() + off, data.size() - off, MSG_NOSIGNAL));
            if (n <= 0) return false;
            off += static_cast<size_t>(n);
        }
        return true;
    }

    // Reads one line without its CRLF; false on end of stream or after `timeout_ms` without a complete line.
    bool read_line(std::string& out, int timeout_ms = 3000) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        for (;;) {
            const size_t eol = buf_.find("\r\n");
            if (eol != std::string::npos) {
                out = buf_.substr(0, eol);
                buf_.erase(0, eol + 2);
                return true;
            }
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
            if (left <= 0) return false;
            if (!ssl_ || SSL_pending(ssl_) == 0) {
                pollfd p{fd_, POLLIN, 0};
                if (::poll(&p, 1, static_cast<int>(left)) <= 0) return false;
            }
            char tmp[4096];
            int n = ssl_ ? SSL_read(ssl_, tmp, sizeof tmp) : static_cast<int>(::recv(fd_, tmp, sizeof tmp, 0));
            if (n <= 0) {
                closed_ = true;
                return false;
            }
            buf_.append(tmp, tmp + n);
        }
    }

    // Reads exactly n raw bytes (a literal).
    bool read_bytes(size_t n, std::string& out, int timeout_ms = 3000) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (buf_.size() < n) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
            if (left <= 0) return false;
            if (!ssl_ || SSL_pending(ssl_) == 0) {
                pollfd p{fd_, POLLIN, 0};
                if (::poll(&p, 1, static_cast<int>(left)) <= 0) return false;
            }
            char tmp[4096];
            int r = ssl_ ? SSL_read(ssl_, tmp, sizeof tmp) : static_cast<int>(::recv(fd_, tmp, sizeof tmp, 0));
            if (r <= 0) {
                closed_ = true;
                return false;
            }
            buf_.append(tmp, tmp + r);
        }
        out = buf_.substr(0, n);
        buf_.erase(0, n);
        return true;
    }

    // Sends "<tag> <command>" and collects every line up to and including the tagged answer.
    std::string command(const std::string& tag, const std::string& command_text, int timeout_ms = 3000) {
        send_line(tag + " " + command_text);
        std::string all, line;
        while (read_line(line, timeout_ms)) {
            all += line + "\n";
            if (line.compare(0, tag.size() + 1, tag + " ") == 0) break;
        }
        return all;
    }

    bool closed_by_peer() {
        // True when the server has closed the connection (a read returns end of stream).
        std::string line;
        read_line(line, 200);
        return closed_;
    }

    void close() {
        if (ssl_) {
            SSL_shutdown(ssl_);
            SSL_free(ssl_);
            ssl_ = nullptr;
        }
        if (ctx_) {
            SSL_CTX_free(ctx_);
            ctx_ = nullptr;
        }
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

private:
    int fd_ = -1;
    SSL_CTX* ctx_ = nullptr;
    SSL* ssl_ = nullptr;
    std::string buf_;
    std::string ca_file_;
    std::string server_name_;
    bool closed_ = false;
};

}  // namespace pbtest
