#pragma once
// An in-process fake SMTP submission server for the contract and integration tests (contracts/imap-smtp-subset.md).
//
// UNVERIFIED against real servers: it imitates the SMTP subset the app uses (EHLO, STARTTLS or implicit TLS, AUTH PLAIN and
// LOGIN, MAIL FROM, RCPT TO, DATA, RSET, NOOP, QUIT); it proves the app against the harness, not against Gmail, Outlook.com or
// Postfix (constitution Principle IX). Every test that includes this header must start with PBO_FAKE_SERVER_TEST().
//
// It listens on 127.0.0.1 (random port) with a real TLS handshake, so the production transport (libcurl) runs end to end.
// Every received command is logged without credentials. Failure hooks: reply after N ms, drop the connection after the 354
// reply, in the middle of the body, after the final "." before the 250 reply, or after the reply, refuse STARTTLS, refuse
// chosen recipients. Accepted messages are recorded with their envelope and counted per Message-ID.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace pbtest {

class FakeSmtpServer {
public:
    enum class Mode { ImplicitTls, StartTls };

    struct Config {
        std::string cert;
        std::string key;
        Mode mode = Mode::ImplicitTls;
        std::string user = "alice";
        std::string password = "pw-for-tests";
        std::string bind_address = "127.0.0.1";
        int port = 0;
        size_t max_size = 10u << 20;   // announced with SIZE; a bigger message is answered 552
    };

    struct Accepted {
        std::string envelope_from;
        std::vector<std::string> recipients;
        std::string data;   // dot-unstuffed, exactly the bytes the client sent
    };

    // ---- failure hooks (set before or between connections) ----
    std::atomic<int> reply_delay_ms{0};          // every reply is delayed
    std::atomic<bool> refuse_starttls{false};    // STARTTLS is not advertised and answered 502
    std::atomic<bool> drop_after_354{false};     // the connection closes right after "354"
    std::atomic<int> drop_in_body_after{-1};     // the connection closes after this many body bytes arrived
    std::atomic<bool> drop_before_final_reply{false};   // the message is received completely (and counted as accepted), then the connection closes without a reply
    std::atomic<bool> drop_after_reply{false};   // the final "250" is sent, then the connection closes
    std::atomic<bool> require_auth{true};        // MAIL FROM before AUTH is answered 530
    std::atomic<int> reject_data_code{0};        // the complete message is answered with this code (for example 554 or 451) and NOT counted as accepted

    explicit FakeSmtpServer(Config config) : config_(std::move(config)) {
        ::signal(SIGPIPE, SIG_IGN);
        ctx_ = SSL_CTX_new(TLS_server_method());
        if (!ctx_ || SSL_CTX_use_certificate_chain_file(ctx_, config_.cert.c_str()) != 1 ||
            SSL_CTX_use_PrivateKey_file(ctx_, config_.key.c_str(), SSL_FILETYPE_PEM) != 1)
            return;
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        int one = 1;
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        if (::inet_pton(AF_INET, config_.bind_address.c_str(), &addr.sin_addr) != 1) return;
        addr.sin_port = htons(static_cast<uint16_t>(config_.port));
        if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 || ::listen(listen_fd_, 16) != 0) return;
        socklen_t len = sizeof addr;
        ::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len);
        port_ = ntohs(addr.sin_port);
        running_ = true;
        accept_thread_ = std::thread([this] { accept_loop(); });
    }
    ~FakeSmtpServer() {
        stop_ = true;
        if (accept_thread_.joinable()) accept_thread_.join();
        for (auto& t : conn_threads_) t.join();
        if (listen_fd_ >= 0) ::close(listen_fd_);
        if (ctx_) SSL_CTX_free(ctx_);
    }
    FakeSmtpServer(const FakeSmtpServer&) = delete;
    FakeSmtpServer& operator=(const FakeSmtpServer&) = delete;

    bool running() const { return running_; }
    int port() const { return port_; }

    // ---- scripting ----
    void refuse_recipient(const std::string& address) {
        std::lock_guard<std::mutex> l(mu_);
        refused_.insert(lower(address));
    }

    // ---- observation ----
    std::vector<std::string> commands() const {   // every received command line, credentials replaced
        std::lock_guard<std::mutex> l(mu_);
        return commands_;
    }
    std::vector<Accepted> accepted() const {
        std::lock_guard<std::mutex> l(mu_);
        return accepted_;
    }
    // How many times a message with this Message-ID was accepted (a duplicate send shows as 2).
    int accepted_count(const std::string& message_id) const {
        std::lock_guard<std::mutex> l(mu_);
        int n = 0;
        for (const Accepted& a : accepted_)
            if (header_value(a.data, "message-id") == message_id) ++n;
        return n;
    }
    int connections() const { return connections_; }
    bool credentials_seen() const { return credentials_seen_; }             // any AUTH exchange carrying the user name or password
    bool plaintext_credentials_seen() const { return plaintext_credentials_seen_; }   // ... before the channel was encrypted

private:
    struct Conn {
        int fd = -1;
        SSL* ssl = nullptr;
        std::string rbuf;
        bool tls = false;
        bool dead = false;
        bool authenticated = false;
        bool greeted = false;
        std::string from;
        bool have_from = false;
        std::vector<std::string> rcpts;
    };

    static std::string lower(std::string s) {
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }
    static std::string upper(std::string s) {
        for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return s;
    }
    static std::string header_value(const std::string& data, const std::string& name_lower) {
        size_t pos = 0;
        while (pos < data.size()) {
            size_t eol = data.find("\r\n", pos);
            if (eol == std::string::npos) eol = data.size();
            if (eol == pos) break;   // end of the header block
            const std::string line = data.substr(pos, eol - pos);
            const size_t colon = line.find(':');
            if (colon != std::string::npos && lower(line.substr(0, colon)) == name_lower) {
                size_t v = colon + 1;
                while (v < line.size() && line[v] == ' ') ++v;
                return line.substr(v);
            }
            pos = eol + 2;
        }
        return {};
    }
    static std::string base64_decode(const std::string& in) {
        static const std::string chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        unsigned val = 0;
        int bits = -8;
        for (char c : in) {
            const size_t p = chars.find(c);
            if (p == std::string::npos) continue;
            val = ((val << 6) + static_cast<unsigned>(p)) & 0xFFFFFFu;
            bits += 6;
            if (bits >= 0) {
                out += static_cast<char>((val >> bits) & 0xFF);
                bits -= 8;
            }
        }
        return out;
    }

    void accept_loop() {
        while (!stop_) {
            pollfd p{listen_fd_, POLLIN, 0};
            if (::poll(&p, 1, 50) <= 0) continue;
            int fd = ::accept(listen_fd_, nullptr, nullptr);
            if (fd < 0) continue;
            ++connections_;
            conn_threads_.emplace_back([this, fd] { serve(fd); });
        }
    }

    bool fill(Conn& c) {
        for (;;) {
            if (stop_) return false;
            if (!(c.ssl && SSL_pending(c.ssl) > 0)) {
                pollfd p{c.fd, POLLIN, 0};
                const int r = ::poll(&p, 1, 100);
                if (r < 0) return false;
                if (r == 0) continue;
            }
            break;
        }
        char tmp[4096];
        int n = c.ssl ? SSL_read(c.ssl, tmp, sizeof tmp) : static_cast<int>(::recv(c.fd, tmp, sizeof tmp, 0));
        if (n <= 0) {
            if (c.ssl) {
                const int e = SSL_get_error(c.ssl, n);
                if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) return true;
            }
            return false;
        }
        c.rbuf.append(tmp, tmp + n);
        return true;
    }

    bool read_line(Conn& c, std::string& out) {
        for (;;) {
            const size_t eol = c.rbuf.find("\r\n");
            if (eol != std::string::npos) {
                out = c.rbuf.substr(0, eol);
                c.rbuf.erase(0, eol + 2);
                return true;
            }
            if (!fill(c)) return false;
        }
    }

    bool write(Conn& c, const std::string& data) {
        if (c.dead) return false;
        if (reply_delay_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(reply_delay_ms));
        size_t off = 0;
        while (off < data.size()) {
            int n = c.ssl ? SSL_write(c.ssl, data.data() + off, static_cast<int>(data.size() - off))
                          : static_cast<int>(::send(c.fd, data.data() + off, data.size() - off, MSG_NOSIGNAL));
            if (n <= 0) {
                c.dead = true;
                return false;
            }
            off += static_cast<size_t>(n);
        }
        return true;
    }

    bool start_tls(Conn& c) {
        c.ssl = SSL_new(ctx_);
        SSL_set_fd(c.ssl, c.fd);
        if (SSL_accept(c.ssl) != 1) return false;
        c.tls = true;
        return true;
    }

    void log_command(const std::string& line) {
        std::lock_guard<std::mutex> l(mu_);
        commands_.push_back(line);
    }

    void serve(int fd) {
        Conn c;
        c.fd = fd;
        timeval tv{0, 100000};
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        bool ok = true;
        if (config_.mode == Mode::ImplicitTls) ok = start_tls(c);
        if (ok) {
            ok = write(c, "220 localhost fake ESMTP ready\r\n");
            std::string line;
            while (ok && !c.dead && read_line(c, line))
                if (!handle(c, line)) break;
        }
        if (c.ssl) {
            SSL_shutdown(c.ssl);
            SSL_free(c.ssl);
        }
        ::close(fd);
    }

    static std::string address_of(const std::string& arg) {   // "FROM:<a@b> SIZE=1" -> "a@b"
        const size_t lt = arg.find('<'), gt = arg.find('>');
        if (lt == std::string::npos || gt == std::string::npos || gt < lt) return {};
        return arg.substr(lt + 1, gt - lt - 1);
    }

    bool authenticate(Conn& c, const std::string& user, const std::string& pass) {
        const bool good = user == config_.user && pass == config_.password;
        credentials_seen_ = true;
        if (!c.tls) plaintext_credentials_seen_ = true;
        if (good) c.authenticated = true;
        return good;
    }

    bool handle(Conn& c, const std::string& line) {
        const std::string up = upper(line);
        const std::string verb = up.substr(0, up.find(' '));
        if (verb == "AUTH") {
            // credentials never go into the log
            log_command(line.size() > 4 ? "AUTH " + line.substr(5, line.find(' ', 5) == std::string::npos ? std::string::npos : line.find(' ', 5) - 5) + " <redacted>" : "AUTH");
        } else {
            log_command(line);
        }

        if (verb == "EHLO" || verb == "HELO") {
            c.greeted = true;
            std::string out = "250-localhost\r\n250-SIZE " + std::to_string(config_.max_size) + "\r\n250-8BITMIME\r\n";
            if (config_.mode == Mode::StartTls && !c.tls && !refuse_starttls) out += "250-STARTTLS\r\n";
            if (c.tls) out += "250-AUTH PLAIN LOGIN\r\n";   // like a real server: authentication is offered once the channel is encrypted
            out += "250 ENHANCEDSTATUSCODES\r\n";
            return write(c, out);
        }
        if (verb == "STARTTLS") {
            if (config_.mode != Mode::StartTls || c.tls || refuse_starttls) return write(c, "502 5.5.1 STARTTLS not available\r\n");
            if (!write(c, "220 2.0.0 Ready to start TLS\r\n")) return false;
            if (!start_tls(c)) return false;
            c.greeted = false;   // the client must say EHLO again
            c.rbuf.clear();
            return true;
        }
        if (verb == "AUTH") {
            std::vector<std::string> w = split_words(line);
            if (w.size() < 2) return write(c, "501 5.5.4 syntax error\r\n");
            const std::string mech = upper(w[1]);
            if (mech == "PLAIN") {
                std::string resp;
                if (w.size() >= 3) {
                    resp = w[2];
                } else {
                    if (!write(c, "334 \r\n") || !read_line(c, resp)) return false;
                }
                const std::string d = base64_decode(resp);   // authzid NUL user NUL pass
                const size_t p1 = d.find('\0');
                const size_t p2 = p1 == std::string::npos ? std::string::npos : d.find('\0', p1 + 1);
                if (p2 == std::string::npos) return write(c, "501 5.5.2 cannot decode\r\n");
                return write(c, authenticate(c, d.substr(p1 + 1, p2 - p1 - 1), d.substr(p2 + 1)) ? "235 2.7.0 Authentication successful\r\n"
                                                                                                    : "535 5.7.8 Authentication credentials invalid\r\n");
            }
            if (mech == "LOGIN") {
                std::string u, p;
                if (!write(c, "334 VXNlcm5hbWU6\r\n") || !read_line(c, u) || !write(c, "334 UGFzc3dvcmQ6\r\n") || !read_line(c, p)) return false;
                return write(c, authenticate(c, base64_decode(u), base64_decode(p)) ? "235 2.7.0 Authentication successful\r\n"
                                                                                       : "535 5.7.8 Authentication credentials invalid\r\n");
            }
            return write(c, "504 5.5.4 unrecognized authentication type\r\n");
        }
        if (verb == "MAIL") {
            if (require_auth && !c.authenticated) return write(c, "530 5.7.0 Authentication required\r\n");
            c.from = address_of(line);
            c.have_from = true;
            c.rcpts.clear();
            return write(c, "250 2.1.0 Ok\r\n");
        }
        if (verb == "RCPT") {
            if (!c.have_from) return write(c, "503 5.5.1 need MAIL first\r\n");
            const std::string to = address_of(line);
            bool refused;
            {
                std::lock_guard<std::mutex> l(mu_);
                refused = refused_.count(lower(to)) != 0;
            }
            if (to.empty() || refused) return write(c, "550 5.1.1 <" + to + "> recipient rejected\r\n");
            c.rcpts.push_back(to);
            return write(c, "250 2.1.5 Ok\r\n");
        }
        if (verb == "DATA") return handle_data(c);
        if (verb == "RSET") {
            c.have_from = false;
            c.rcpts.clear();
            return write(c, "250 2.0.0 Ok\r\n");
        }
        if (verb == "NOOP") return write(c, "250 2.0.0 Ok\r\n");
        if (verb == "QUIT") {
            write(c, "221 2.0.0 Bye\r\n");
            return false;
        }
        return write(c, "500 5.5.1 command not recognized\r\n");
    }

    static std::vector<std::string> split_words(const std::string& s) {
        std::vector<std::string> w;
        size_t i = 0;
        while (i < s.size()) {
            while (i < s.size() && s[i] == ' ') ++i;
            const size_t j = s.find(' ', i);
            if (i < s.size()) w.push_back(s.substr(i, j == std::string::npos ? std::string::npos : j - i));
            if (j == std::string::npos) break;
            i = j;
        }
        return w;
    }

    bool handle_data(Conn& c) {
        if (!c.have_from || c.rcpts.empty()) return write(c, "503 5.5.1 need MAIL and RCPT first\r\n");
        if (!write(c, "354 End data with <CR><LF>.<CR><LF>\r\n")) return false;
        if (drop_after_354) return false;
        std::string data;   // everything up to "\r\n.\r\n", dot-unstuffed
        for (;;) {
            const size_t end = c.rbuf.find("\r\n.\r\n");
            if (end != std::string::npos) {
                data = c.rbuf.substr(0, end + 2);
                c.rbuf.erase(0, end + 5);
                break;
            }
            if (c.rbuf.compare(0, 3, ".\r\n") == 0) {   // an empty message body
                c.rbuf.erase(0, 3);
                break;
            }
            if (drop_in_body_after >= 0 && static_cast<int>(c.rbuf.size()) >= drop_in_body_after) return false;
            if (!fill(c)) return false;
        }
        // dot-unstuffing: a line that starts with ".." loses one dot
        std::string clean;
        bool at_line_start = true;
        for (size_t i = 0; i < data.size(); ++i) {
            if (at_line_start && data[i] == '.' && i + 1 < data.size() && data[i + 1] == '.') {
                at_line_start = false;   // the stuffed dot is dropped; the next dot belongs to the text
                continue;
            }
            clean += data[i];
            at_line_start = data[i] == '\n';
        }
        if (reject_data_code > 0) {
            c.have_from = false;
            c.rcpts.clear();
            return write(c, std::to_string(reject_data_code.load()) + " 5.7.1 message rejected\r\n");
        }
        if (clean.size() > config_.max_size) {
            c.have_from = false;
            c.rcpts.clear();
            return write(c, "552 5.3.4 message too large\r\n");
        }
        {
            std::lock_guard<std::mutex> l(mu_);
            accepted_.push_back({c.from, c.rcpts, clean});
        }
        c.have_from = false;
        c.rcpts.clear();
        if (drop_before_final_reply) return false;
        const bool ok = write(c, "250 2.0.0 Ok: queued\r\n");
        if (drop_after_reply) return false;
        return ok;
    }

    Config config_;
    SSL_CTX* ctx_ = nullptr;
    int listen_fd_ = -1;
    int port_ = 0;
    bool running_ = false;
    std::atomic<bool> stop_{false};
    std::thread accept_thread_;
    std::vector<std::thread> conn_threads_;
    mutable std::mutex mu_;
    std::vector<std::string> commands_;
    std::vector<Accepted> accepted_;
    std::set<std::string> refused_;
    std::atomic<int> connections_{0};
    std::atomic<bool> credentials_seen_{false};
    std::atomic<bool> plaintext_credentials_seen_{false};
};

}  // namespace pbtest
