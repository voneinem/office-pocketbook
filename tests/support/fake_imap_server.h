#pragma once
// An in-process fake IMAP server for the contract and integration tests (contracts/imap-smtp-subset.md section 4).
//
// UNVERIFIED against real servers: this imitates the IMAP subset the app uses; it proves the app against the
// harness, not against Dovecot, Gmail or Outlook.com (constitution Principle IX). Every test that includes this
// header must start with PBO_FAKE_SERVER_TEST().
//
// It listens on 127.0.0.1 (random port) with a real TLS handshake, so the production transport (libcurl) runs end
// to end: implicit TLS (imaps) or STARTTLS after a plaintext greeting. The mailbox state is scripted (folders, UIDs,
// flags, UIDVALIDITY, modseq), every received command is logged without the password, and failure hooks drop the
// connection after N commands or N bytes, delay replies, refuse STARTTLS or disable plaintext logins.

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
#include <ctime>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace pbtest {

class FakeImapServer {
public:
    enum class Mode { ImplicitTls, StartTls };

    struct Config {
        std::string cert;
        std::string key;
        Mode mode = Mode::ImplicitTls;
        std::string user = "alice";
        std::string password = "pw-for-tests";
        std::string bind_address = "127.0.0.1";   // "0.0.0.0" serves a device on the LAN (the demo tool, T051)
        int port = 0;                              // 0 = a free port
    };

    struct Message {
        uint32_t uid = 0;
        std::set<std::string> flags;
        uint64_t modseq = 1;
        std::string raw;
        int64_t internal_date = 0;
    };

    struct Folder {
        std::string name;
        std::vector<std::string> attributes;
        uint32_t uidvalidity = 1;
        uint32_t uidnext = 1;
        uint64_t highestmodseq = 1;
        std::vector<Message> messages;
    };

    // ---- failure hooks (set before or between connections) ----
    std::atomic<int> drop_after_commands{-1};   // after N commands the next one is received but never answered; then the connection closes
    std::atomic<int> drop_after_bytes{-1};      // the connection closes after this many bytes were written
    std::atomic<int> delay_reply_ms{0};         // every reply is delayed
    std::atomic<bool> refuse_starttls{false};   // STARTTLS is not advertised and answered NO
    std::atomic<bool> login_disabled{false};    // LOGINDISABLED even after TLS
    std::atomic<int64_t> clock_offset_s{0};     // server clock = real clock + this (for the clock-skew tests)
    std::atomic<bool> corrupt_bodystructure{false};   // BODYSTRUCTURE answers are garbled (the app must fall back)
    std::atomic<bool> drop_after_append{false};       // an APPEND is carried out (the message is stored) but never answered: the connection closes
    std::atomic<bool> refuse_append{false};           // every APPEND is answered NO
    std::atomic<bool> refuse_copy{false};             // every UID COPY / UID MOVE is answered NO
    std::atomic<int> bare_expunges{0};                // how many times a bare EXPUNGE (without UID) arrived: the app must never send one

    explicit FakeImapServer(Config config) : config_(std::move(config)) {
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
    ~FakeImapServer() {
        stop_ = true;
        if (accept_thread_.joinable()) accept_thread_.join();
        for (auto& t : conn_threads_) t.join();
        if (listen_fd_ >= 0) ::close(listen_fd_);
        if (ctx_) SSL_CTX_free(ctx_);
    }
    FakeImapServer(const FakeImapServer&) = delete;
    FakeImapServer& operator=(const FakeImapServer&) = delete;

    bool running() const { return running_; }
    int port() const { return port_; }

    // ---- scripting ----
    void set_capabilities(const std::string& caps) {
        std::lock_guard<std::mutex> l(mu_);
        capabilities_ = caps;
    }
    Folder& add_folder(const std::string& name, std::vector<std::string> attributes = {}) {
        std::lock_guard<std::mutex> l(mu_);
        Folder f;
        f.name = name;
        f.attributes = std::move(attributes);
        folders_.push_back(std::move(f));
        return folders_.back();
    }
    uint32_t add_message(const std::string& folder, const std::string& raw, std::set<std::string> flags = {},
                         int64_t internal_date = 0) {
        std::lock_guard<std::mutex> l(mu_);
        return add_message_locked(find(folder), raw, std::move(flags), internal_date ? internal_date : now_locked());
    }
    // As after a reset of the mailbox on the server: no messages, numbering starts again, a new UIDVALIDITY.
    void reset_folder(const std::string& folder, uint32_t new_uidvalidity) {
        std::lock_guard<std::mutex> l(mu_);
        Folder& f = find(folder);
        f.messages.clear();
        f.uidnext = 1;
        f.uidvalidity = new_uidvalidity;
        ++f.highestmodseq;
    }
    void set_uidvalidity(const std::string& folder, uint32_t v) {
        std::lock_guard<std::mutex> l(mu_);
        find(folder).uidvalidity = v;
    }
    void expunge(const std::string& folder, uint32_t uid) {
        std::lock_guard<std::mutex> l(mu_);
        Folder& f = find(folder);
        f.messages.erase(std::remove_if(f.messages.begin(), f.messages.end(), [&](const Message& m) { return m.uid == uid; }), f.messages.end());
        ++f.highestmodseq;
    }
    void set_flags(const std::string& folder, uint32_t uid, std::set<std::string> flags) {
        std::lock_guard<std::mutex> l(mu_);
        Folder& f = find(folder);
        for (Message& m : f.messages)
            if (m.uid == uid) {
                m.flags = std::move(flags);
                m.modseq = ++f.highestmodseq;
            }
    }

    // ---- observation ----
    std::vector<std::string> commands() const {  // every received command line, passwords replaced
        std::lock_guard<std::mutex> l(mu_);
        return commands_;
    }
    int connections() const { return connections_; }
    bool credentials_seen() const { return credentials_seen_; }
    bool plaintext_credentials_seen() const { return plaintext_credentials_seen_; }
    int message_count(const std::string& folder) const {
        std::lock_guard<std::mutex> l(mu_);
        for (const Folder& f : folders_)
            if (f.name == folder) return static_cast<int>(f.messages.size());
        return -1;
    }
    int64_t internal_date(const std::string& folder, uint32_t uid) const {
        std::lock_guard<std::mutex> l(mu_);
        for (const Folder& f : folders_)
            if (f.name == folder)
                for (const Message& m : f.messages)
                    if (m.uid == uid) return m.internal_date;
        return 0;
    }
    // the messages of a folder: uid and raw bytes
    std::vector<std::pair<uint32_t, std::string>> messages(const std::string& folder) const {
        std::lock_guard<std::mutex> l(mu_);
        std::vector<std::pair<uint32_t, std::string>> out;
        for (const Folder& f : folders_)
            if (f.name == folder)
                for (const Message& m : f.messages) out.emplace_back(m.uid, m.raw);
        return out;
    }
    std::set<std::string> flags_of(const std::string& folder, uint32_t uid) const {
        std::lock_guard<std::mutex> l(mu_);
        for (const Folder& f : folders_)
            if (f.name == folder)
                for (const Message& m : f.messages)
                    if (m.uid == uid) return m.flags;
        return {};
    }

private:
    // ------------------------------------------------------------------ connection plumbing
    struct Conn {
        int fd = -1;
        SSL* ssl = nullptr;
        std::string rbuf;
        long written = 0;
        bool tls = false;
        bool dead = false;
        std::string selected;
        bool authenticated = false;
        int commands = 0;
    };

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

    bool io_wait(Conn& c, bool for_read) {
        for (;;) {
            if (stop_) return false;
            if (for_read && c.ssl && SSL_pending(c.ssl) > 0) return true;
            pollfd p{c.fd, static_cast<short>(for_read ? POLLIN : POLLOUT), 0};
            const int r = ::poll(&p, 1, 100);
            if (r > 0) return true;
            if (r < 0) return false;
        }
    }

    // Reads more bytes into rbuf; false when the peer closed or the server stops.
    bool fill(Conn& c) {
        if (!io_wait(c, true)) return false;
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

    bool read_bytes(Conn& c, size_t n, std::string& out) {
        while (c.rbuf.size() < n)
            if (!fill(c)) return false;
        out = c.rbuf.substr(0, n);
        c.rbuf.erase(0, n);
        return true;
    }

    // Writes with the byte limit of the drop_after_bytes hook.
    bool write(Conn& c, const std::string& data) {
        if (c.dead) return false;
        size_t len = data.size();
        const int limit = drop_after_bytes;
        bool cut = false;
        if (limit >= 0 && c.written + static_cast<long>(len) > limit) {
            len = static_cast<size_t>(std::max<long>(0, limit - c.written));
            cut = true;
        }
        size_t off = 0;
        while (off < len) {
            int n = c.ssl ? SSL_write(c.ssl, data.data() + off, static_cast<int>(len - off))
                          : static_cast<int>(::send(c.fd, data.data() + off, len - off, MSG_NOSIGNAL));
            if (n <= 0) {
                c.dead = true;
                return false;
            }
            off += static_cast<size_t>(n);
            c.written += n;
        }
        if (cut) {
            c.dead = true;
            return false;
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

    // ------------------------------------------------------------------ the protocol
    void serve(int fd) {
        Conn c;
        c.fd = fd;
        timeval tv{0, 100000};
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        bool ok = true;
        if (config_.mode == Mode::ImplicitTls) ok = start_tls(c);
        if (ok) {
            ok = write(c, "* OK " + (config_.mode == Mode::ImplicitTls ? "[CAPABILITY " + capability_text(c) + "] " : std::string()) +
                              "IMAP4rev1 fake server ready\r\n");
            std::string line;
            while (ok && !c.dead && read_line(c, line)) {
                ++c.commands;
                if (drop_after_commands >= 0 && c.commands > drop_after_commands) break;  // received, never answered
                if (!handle(c, line)) break;
            }
        }
        if (c.ssl) {
            SSL_shutdown(c.ssl);
            SSL_free(c.ssl);
        }
        ::close(fd);
    }

    std::string capability_text(const Conn& c) const {
        std::string caps;
        {
            std::lock_guard<std::mutex> l(mu_);
            caps = capabilities_;
        }
        if (c.tls && !login_disabled) caps += " AUTH=PLAIN";   // like a real server: the SASL mechanism is offered once the channel is encrypted
        if (config_.mode == Mode::StartTls && !c.tls && !refuse_starttls) caps += " STARTTLS";
        if ((config_.mode == Mode::StartTls && !c.tls) || login_disabled) caps += " LOGINDISABLED";
        return caps;
    }

    static std::string upper(std::string s) {
        for (char& ch : s) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        return s;
    }

    // Splits "word word (a b) \"quoted x\" {n}" style arguments on blanks outside (), [] and quotes.
    static std::vector<std::string> split_args(const std::string& s) {
        std::vector<std::string> out;
        std::string cur;
        int depth = 0;
        bool quoted = false;
        for (size_t i = 0; i < s.size(); ++i) {
            const char ch = s[i];
            if (quoted) {
                cur += ch;
                if (ch == '\\' && i + 1 < s.size()) cur += s[++i];
                else if (ch == '"') quoted = false;
                continue;
            }
            if (ch == '"') { quoted = true; cur += ch; continue; }
            if (ch == '(' || ch == '[') ++depth;
            if (ch == ')' || ch == ']') --depth;
            if (ch == ' ' && depth == 0) {
                if (!cur.empty()) out.push_back(cur), cur.clear();
                continue;
            }
            cur += ch;
        }
        if (!cur.empty()) out.push_back(cur);
        return out;
    }

    static std::string unquote(std::string s) {
        if (s.size() >= 2 && s.front() == '"' && s.back() == '"') {
            std::string o;
            for (size_t i = 1; i + 1 < s.size(); ++i) {
                if (s[i] == '\\' && i + 2 < s.size() + 0) ++i;
                o += s[i];
            }
            return o;
        }
        return s;
    }

    static bool uid_in_set(uint32_t uid, const std::string& set, uint32_t max_uid) {
        std::stringstream ss(set);
        std::string part;
        while (std::getline(ss, part, ',')) {
            const size_t colon = part.find(':');
            auto num = [&](const std::string& t) -> uint32_t { return t == "*" ? max_uid : static_cast<uint32_t>(std::strtoul(t.c_str(), nullptr, 10)); };
            if (colon == std::string::npos) {
                if (uid == num(part)) return true;
            } else {
                uint32_t a = num(part.substr(0, colon)), b = num(part.substr(colon + 1));
                if (a > b) std::swap(a, b);
                if (uid >= a && uid <= b) return true;
            }
        }
        return false;
    }

    int64_t now_locked() const { return static_cast<int64_t>(std::time(nullptr)) + clock_offset_s; }

    static std::string imap_date(int64_t t) {
        std::time_t tt = static_cast<std::time_t>(t);
        std::tm tm{};
        ::gmtime_r(&tt, &tm);
        char buf[64];
        std::strftime(buf, sizeof buf, "%d-%b-%Y %H:%M:%S +0000", &tm);
        return buf;
    }

    static int64_t parse_search_date(const std::string& s) {  // dd-Mon-yyyy, midnight UTC
        std::tm tm{};
        const std::string t = unquote(s);
        if (!::strptime(t.c_str(), "%d-%b-%Y", &tm)) return 0;
        return static_cast<int64_t>(::timegm(&tm));
    }

    Folder& find(const std::string& name) {
        for (Folder& f : folders_)
            if (f.name == name || (upper(f.name) == "INBOX" && upper(name) == "INBOX")) return f;
        static Folder none;
        return none;
    }

    uint32_t add_message_locked(Folder& f, const std::string& raw, std::set<std::string> flags, int64_t date) {
        Message m;
        m.uid = f.uidnext++;
        m.flags = std::move(flags);
        m.modseq = ++f.highestmodseq;
        m.raw = raw;
        m.internal_date = date;
        f.messages.push_back(std::move(m));
        return f.messages.back().uid;
    }

    static std::string flag_list(const std::set<std::string>& flags) {
        std::string s = "(";
        bool first = true;
        for (const auto& f : flags) {
            if (!first) s += " ";
            s += f;
            first = false;
        }
        return s + ")";
    }

    static std::set<std::string> parse_flags(const std::string& group) {
        std::set<std::string> out;
        std::string g = group;
        if (!g.empty() && g.front() == '(') g = g.substr(1);
        if (!g.empty() && g.back() == ')') g.pop_back();
        std::stringstream ss(g);
        std::string t;
        while (ss >> t) out.insert(t);
        return out;
    }

    // The header block of a raw message including the blank line.
    static std::string header_of(const std::string& raw) {
        const size_t p = raw.find("\r\n\r\n");
        return p == std::string::npos ? raw + "\r\n" : raw.substr(0, p + 4);
    }
    static std::string text_of(const std::string& raw) {
        const size_t p = raw.find("\r\n\r\n");
        return p == std::string::npos ? std::string() : raw.substr(p + 4);
    }
    static std::string header_fields(const std::string& raw, const std::vector<std::string>& wanted) {
        const std::string head = header_of(raw);
        std::string out, line;
        std::stringstream ss(head);
        bool keep = false;
        while (std::getline(ss, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) break;
            if (line[0] == ' ' || line[0] == '\t') {
                if (keep) out += line + "\r\n";
                continue;
            }
            const std::string name = upper(line.substr(0, line.find(':')));
            keep = std::find(wanted.begin(), wanted.end(), name) != wanted.end();
            if (keep) out += line + "\r\n";
        }
        return out + "\r\n";
    }

    // ---- a small MIME reader of its own (independent of the app's parser, so tests do not share its bugs) ----
    struct MNode {
        std::string type = "text", subtype = "plain", disposition, id, cte = "7BIT", body;
        std::vector<std::pair<std::string, std::string>> params, disp_params;   // names as written, values unquoted
        std::vector<MNode> kids;
    };

    static std::string mtrim(const std::string& v) {
        size_t b = 0, e = v.size();
        while (b < e && (v[b] == ' ' || v[b] == '\t' || v[b] == '\r' || v[b] == '\n')) ++b;
        while (e > b && (v[e - 1] == ' ' || v[e - 1] == '\t' || v[e - 1] == '\r' || v[e - 1] == '\n')) --e;
        return v.substr(b, e - b);
    }

    // "value; name=\"x\"; name2=y" -> value, pairs
    static void split_params(const std::string& v, std::string* main, std::vector<std::pair<std::string, std::string>>* out) {
        size_t i = 0;
        auto read_until_semicolon = [&]() {
            std::string t;
            bool q = false;
            while (i < v.size() && (q || v[i] != ';')) {
                if (v[i] == '"') q = !q;
                t += v[i++];
            }
            if (i < v.size()) ++i;
            return mtrim(t);
        };
        *main = read_until_semicolon();
        while (i < v.size()) {
            const std::string item = read_until_semicolon();
            const size_t eq = item.find('=');
            if (eq == std::string::npos) continue;
            std::string name = mtrim(item.substr(0, eq)), val = mtrim(item.substr(eq + 1));
            if (val.size() >= 2 && val.front() == '"' && val.back() == '"') {
                std::string u;
                for (size_t k = 1; k + 1 < val.size(); ++k) {
                    if (val[k] == '\\' && k + 2 < val.size()) ++k;
                    u += val[k];
                }
                val = u;
            }
            out->emplace_back(upper(name), val);
        }
    }

    static MNode parse_mime(const std::string& raw) {
        MNode n;
        std::string head = header_of(raw);
        n.body = text_of(raw);
        // unfold the header block
        std::vector<std::pair<std::string, std::string>> hs;
        std::stringstream ss(head);
        std::string line;
        while (std::getline(ss, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) break;
            if ((line[0] == ' ' || line[0] == '\t') && !hs.empty()) { hs.back().second += " " + mtrim(line); continue; }
            const size_t c = line.find(':');
            if (c != std::string::npos) hs.emplace_back(upper(line.substr(0, c)), mtrim(line.substr(c + 1)));
        }
        auto get = [&](const char* name) {
            for (auto& h : hs)
                if (h.first == name) return h.second;
            return std::string();
        };
        std::string ct;
        split_params(get("CONTENT-TYPE"), &ct, &n.params);
        if (!ct.empty() && ct.find('/') != std::string::npos) {
            n.type = ct.substr(0, ct.find('/'));
            n.subtype = ct.substr(ct.find('/') + 1);
        }
        for (char& c : n.type) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        for (char& c : n.subtype) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        std::string disp;
        split_params(get("CONTENT-DISPOSITION"), &disp, &n.disp_params);
        n.disposition = upper(disp);
        n.id = get("CONTENT-ID");
        const std::string cte = get("CONTENT-TRANSFER-ENCODING");
        if (!cte.empty()) n.cte = upper(cte);
        if (n.type == "multipart") {
            std::string boundary;
            for (auto& p : n.params)
                if (p.first == "BOUNDARY") boundary = p.second;
            if (!boundary.empty()) {
                const std::string delim = "--" + boundary;
                std::vector<std::string> parts;
                std::string cur;
                bool in_part = false;
                std::stringstream bs(n.body);
                std::string l;
                while (std::getline(bs, l)) {
                    std::string t = l;
                    if (!t.empty() && t.back() == '\r') t.pop_back();
                    if (t == delim || t == delim + "--") {
                        if (in_part) {
                            if (cur.size() >= 2 && cur.compare(cur.size() - 2, 2, "\r\n") == 0) cur.resize(cur.size() - 2);
                            parts.push_back(cur);
                        }
                        cur.clear();
                        in_part = t == delim;
                        if (t == delim + "--") break;
                        continue;
                    }
                    if (in_part) cur += l + "\n";
                }
                if (in_part && !cur.empty()) parts.push_back(cur);
                for (std::string& pt : parts) {
                    // restore CRLF line ends that getline split off
                    std::string fixed;
                    for (size_t k = 0; k < pt.size(); ++k) {
                        if (pt[k] == '\n' && (k == 0 || pt[k - 1] != '\r')) fixed += "\r\n";
                        else fixed += pt[k];
                    }
                    if (fixed.size() >= 2 && fixed.compare(fixed.size() - 2, 2, "\r\n") == 0) fixed.resize(fixed.size() - 2);
                    n.kids.push_back(parse_mime(fixed));
                }
            }
        }
        return n;
    }

    static std::string imap_string(const std::string& v) {
        bool plain = true;
        for (unsigned char c : v)
            if (c < 0x20 || c >= 0x7F || c == '\r' || c == '\n') plain = false;
        if (!plain) return "{" + std::to_string(v.size()) + "}\r\n" + v;
        std::string q = "\"";
        for (char c : v) {
            if (c == '"' || c == '\\') q += '\\';
            q += c;
        }
        return q + "\"";
    }

    static std::string params_list(const std::vector<std::pair<std::string, std::string>>& ps) {
        if (ps.empty()) return "NIL";
        std::string out = "(";
        for (size_t i = 0; i < ps.size(); ++i) out += (i ? " " : "") + imap_string(ps[i].first) + " " + imap_string(ps[i].second);
        return out + ")";
    }

    static std::string render_structure(const MNode& n) {
        if (n.type == "multipart") {
            std::string out = "(";
            for (const MNode& k : n.kids) out += render_structure(k);
            std::vector<std::pair<std::string, std::string>> ps;
            for (auto& p : n.params) ps.push_back(p);
            return out + " " + imap_string(upper(n.subtype)) + " " + params_list(ps) + " NIL NIL NIL)";
        }
        size_t lines = 0;
        for (char c : n.body)
            if (c == '\n') ++lines;
        std::string out = "(" + imap_string(upper(n.type)) + " " + imap_string(upper(n.subtype)) + " " + params_list(n.params) + " " +
                          (n.id.empty() ? std::string("NIL") : imap_string(n.id)) + " NIL " + imap_string(n.cte) + " " + std::to_string(n.body.size());
        if (n.type == "text") out += " " + std::to_string(lines);
        if (n.type == "message" && n.subtype == "rfc822")
            out += " (NIL \"inner\" NIL NIL NIL NIL NIL NIL NIL NIL) (\"TEXT\" \"PLAIN\" NIL NIL NIL \"7BIT\" 10 1) " + std::to_string(lines);
        out += " NIL ";
        out += n.disposition.empty() ? std::string("NIL") : "(" + imap_string(n.disposition) + " " + params_list(n.disp_params) + ")";
        return out + " NIL NIL)";
    }

    // the body of the numeric section ("1", "2.1") in its transfer encoding; "" if there is no such part
    static std::string numeric_section(const std::string& raw, const std::string& section, bool* found = nullptr) {
        if (found) *found = false;
        MNode root = parse_mime(raw);
        const MNode* cur = &root;
        std::stringstream ss(section);
        std::string tok;
        bool first = true;
        while (std::getline(ss, tok, '.')) {
            const size_t idx = static_cast<size_t>(std::atoi(tok.c_str()));
            if (cur->type != "multipart") {
                if (first && idx == 1) { first = false; continue; }   // a single-part message: section 1 is the body itself
                return std::string();
            }
            first = false;
            if (idx < 1 || idx > cur->kids.size()) return std::string();
            cur = &cur->kids[idx - 1];
        }
        if (found) *found = true;
        return cur->body;
    }

    std::string section_data(const std::string& raw, const std::string& section) const {
        const std::string up = upper(section);
        if (up.empty()) return raw;
        if (up == "HEADER") return header_of(raw);
        if (up == "TEXT") return text_of(raw);
        if (up.compare(0, 14, "HEADER.FIELDS ") == 0) {
            const size_t a = section.find('('), b = section.rfind(')');
            std::vector<std::string> wanted;
            std::stringstream ss(upper(section.substr(a + 1, b - a - 1)));
            std::string t;
            while (ss >> t) wanted.push_back(t);
            return header_fields(raw, wanted);
        }
        if (std::isdigit(static_cast<unsigned char>(up[0]))) return numeric_section(raw, section);
        return text_of(raw);
    }

    bool tagged(Conn& c, const std::string& tag, const std::string& status, const std::string& text) {
        const int d = delay_reply_ms;
        if (d > 0) std::this_thread::sleep_for(std::chrono::milliseconds(d));
        return write(c, tag + " " + status + " " + text + "\r\n");
    }

    bool handle(Conn& c, const std::string& line) {
        const size_t sp = line.find(' ');
        const std::string tag = line.substr(0, sp);
        const std::string rest = sp == std::string::npos ? std::string() : line.substr(sp + 1);
        std::vector<std::string> a = split_args(rest);
        if (a.empty()) return tagged(c, tag, "BAD", "empty command");
        std::string cmd = upper(a[0]);

        // log the command without secrets
        {
            std::string logged = tag + " " + cmd;
            if (cmd == "LOGIN") {
                credentials_seen_ = true;
                if (!c.tls) plaintext_credentials_seen_ = true;
                logged += " <user> <password>";
            } else if (cmd == "AUTHENTICATE") {
                credentials_seen_ = true;
                if (!c.tls) plaintext_credentials_seen_ = true;
                logged += " <redacted>";
            } else {
                logged = tag + " " + rest;
            }
            std::lock_guard<std::mutex> l(mu_);
            commands_.push_back(logged);
        }

        if (cmd == "CAPABILITY") {
            write(c, "* CAPABILITY " + capability_text(c) + "\r\n");
            return tagged(c, tag, "OK", "CAPABILITY completed");
        }
        if (cmd == "NOOP") return tagged(c, tag, "OK", "NOOP completed");
        if (cmd == "LOGOUT") {
            write(c, "* BYE logging out\r\n");
            tagged(c, tag, "OK", "LOGOUT completed");
            return false;
        }
        if (cmd == "STARTTLS") {
            if (config_.mode != Mode::StartTls || c.tls || refuse_starttls) return tagged(c, tag, "NO", "STARTTLS not available");
            if (!tagged(c, tag, "OK", "Begin TLS negotiation now")) return false;
            return start_tls(c);
        }
        if (cmd == "LOGIN") {
            if ((config_.mode == Mode::StartTls && !c.tls) || login_disabled) return tagged(c, tag, "NO", "[PRIVACYREQUIRED] LOGIN is disabled");
            if (a.size() >= 3 && unquote(a[1]) == config_.user && unquote(a[2]) == config_.password) {
                c.authenticated = true;
                return tagged(c, tag, "OK", "LOGIN completed");
            }
            return tagged(c, tag, "NO", "[AUTHENTICATIONFAILED] invalid credentials");
        }
        if (cmd == "AUTHENTICATE") return do_authenticate(c, tag, a);
        if (!c.authenticated) return tagged(c, tag, "NO", "login first");

        if (cmd == "LIST") return do_list(c, tag);
        if (cmd == "STATUS") return do_status(c, tag, a);
        if (cmd == "SELECT" || cmd == "EXAMINE") return do_select(c, tag, a);
        if (cmd == "APPEND") return do_append(c, tag, rest);
        if (cmd == "UID" && a.size() >= 2) {
            const std::string sub = upper(a[1]);
            if (c.selected.empty()) return tagged(c, tag, "NO", "no mailbox selected");
            if (sub == "SEARCH") return do_search(c, tag, a);
            if (sub == "FETCH") return do_fetch(c, tag, a);
            if (sub == "STORE") return do_store(c, tag, a);
            if (sub == "EXPUNGE") return do_uid_expunge(c, tag, a);
            if (sub == "COPY") return do_uid_copy(c, tag, a, false);
            if (sub == "MOVE") return do_uid_copy(c, tag, a, true);
        }
        if (cmd == "EXPUNGE") {   // a bare EXPUNGE removes every \Deleted message of the mailbox: the app must never send it
            ++bare_expunges;
            if (c.selected.empty()) return tagged(c, tag, "NO", "no mailbox selected");
            std::string out;
            {
                std::lock_guard<std::mutex> l(mu_);
                Folder& f = find(c.selected);
                int seq = 0;
                for (auto it = f.messages.begin(); it != f.messages.end();) {
                    ++seq;
                    if (it->flags.count("\\Deleted")) {
                        out += "* " + std::to_string(seq) + " EXPUNGE\r\n";
                        it = f.messages.erase(it);
                        --seq;
                    } else {
                        ++it;
                    }
                }
            }
            if (!out.empty()) write(c, out);
            return tagged(c, tag, "OK", "EXPUNGE completed");
        }
        return tagged(c, tag, "BAD", "command not supported by the fake server");
    }

    static std::string b64decode(const std::string& in) {
        std::string out;
        uint32_t acc = 0;
        int bits = 0;
        for (char ch : in) {
            int v;
            if (ch >= 'A' && ch <= 'Z') v = ch - 'A';
            else if (ch >= 'a' && ch <= 'z') v = ch - 'a' + 26;
            else if (ch >= '0' && ch <= '9') v = ch - '0' + 52;
            else if (ch == '+') v = 62;
            else if (ch == '/') v = 63;
            else continue;
            acc = (acc << 6) | static_cast<uint32_t>(v);
            bits += 6;
            if (bits >= 8) {
                bits -= 8;
                out += static_cast<char>((acc >> bits) & 0xFF);
            }
        }
        return out;
    }

    // AUTHENTICATE PLAIN [initial-response] (RFC 4616): authzid NUL user NUL password, only over an encrypted channel
    bool do_authenticate(Conn& c, const std::string& tag, const std::vector<std::string>& a) {
        if (a.size() < 2 || upper(a[1]) != "PLAIN") return tagged(c, tag, "NO", "unsupported mechanism");
        if ((config_.mode == Mode::StartTls && !c.tls) || login_disabled) return tagged(c, tag, "NO", "[PRIVACYREQUIRED] authentication requires TLS");
        std::string b64;
        if (a.size() >= 3) {
            b64 = a[2];
        } else {
            if (!write(c, "+ \r\n") || !read_line(c, b64)) return false;
        }
        const std::string raw = b64decode(b64);
        const size_t p1 = raw.find('\0');
        const size_t p2 = p1 == std::string::npos ? std::string::npos : raw.find('\0', p1 + 1);
        if (p2 == std::string::npos) return tagged(c, tag, "NO", "[AUTHENTICATIONFAILED] malformed");
        if (raw.substr(p1 + 1, p2 - p1 - 1) == config_.user && raw.substr(p2 + 1) == config_.password) {
            c.authenticated = true;
            return tagged(c, tag, "OK", "Logged in");
        }
        return tagged(c, tag, "NO", "[AUTHENTICATIONFAILED] invalid credentials");
    }

    bool do_list(Conn& c, const std::string& tag) {
        std::vector<Folder> copy;
        {
            std::lock_guard<std::mutex> l(mu_);
            copy = folders_;
        }
        std::string out;
        for (const Folder& f : copy) {
            std::string attrs;
            for (size_t i = 0; i < f.attributes.size(); ++i) attrs += (i ? " " : "") + f.attributes[i];
            out += "* LIST (" + attrs + ") \"/\" \"" + f.name + "\"\r\n";
        }
        write(c, out);
        return tagged(c, tag, "OK", "LIST completed");
    }

    bool do_status(Conn& c, const std::string& tag, const std::vector<std::string>& a) {
        if (a.size() < 3) return tagged(c, tag, "BAD", "STATUS needs a mailbox and items");
        const std::string name = unquote(a[1]);
        std::string out;
        {
            std::lock_guard<std::mutex> l(mu_);
            const Folder& f = find(name);
            if (f.name.empty()) return tagged(c, tag, "NO", "[NONEXISTENT] no such mailbox");
            int unseen = 0;
            for (const Message& m : f.messages) unseen += m.flags.count("\\Seen") ? 0 : 1;
            std::string items = "(";
            std::stringstream ss(a[2].substr(1, a[2].size() - 2));
            std::string it;
            bool first = true;
            while (ss >> it) {
                std::string val;
                const std::string U = upper(it);
                if (U == "MESSAGES") val = std::to_string(f.messages.size());
                else if (U == "UIDNEXT") val = std::to_string(f.uidnext);
                else if (U == "UIDVALIDITY") val = std::to_string(f.uidvalidity);
                else if (U == "UNSEEN") val = std::to_string(unseen);
                else if (U == "HIGHESTMODSEQ") val = std::to_string(f.highestmodseq);
                else continue;
                items += (first ? "" : " ") + U + " " + val;
                first = false;
            }
            items += ")";
            out = "* STATUS \"" + f.name + "\" " + items + "\r\n";
        }
        write(c, out);
        return tagged(c, tag, "OK", "STATUS completed");
    }

    bool do_select(Conn& c, const std::string& tag, const std::vector<std::string>& a) {
        if (a.size() < 2) return tagged(c, tag, "BAD", "SELECT needs a mailbox");
        const std::string name = unquote(a[1]);
        std::string out;
        {
            std::lock_guard<std::mutex> l(mu_);
            const Folder& f = find(name);
            if (f.name.empty()) return tagged(c, tag, "NO", "[NONEXISTENT] no such mailbox");
            c.selected = f.name;
            out = "* " + std::to_string(f.messages.size()) + " EXISTS\r\n* 0 RECENT\r\n* FLAGS (\\Answered \\Flagged \\Deleted \\Seen \\Draft)\r\n";
            out += "* OK [UIDVALIDITY " + std::to_string(f.uidvalidity) + "] UIDs valid\r\n";
            out += "* OK [UIDNEXT " + std::to_string(f.uidnext) + "] Predicted next UID\r\n";
            if (capabilities_.find("CONDSTORE") != std::string::npos)
                out += "* OK [HIGHESTMODSEQ " + std::to_string(f.highestmodseq) + "] Highest\r\n";
        }
        write(c, out);
        return tagged(c, tag, "OK", "[READ-WRITE] SELECT completed");
    }

    bool do_append(Conn& c, const std::string& tag, const std::string& rest) {
        const size_t brace = rest.rfind('{');
        if (brace == std::string::npos) return tagged(c, tag, "BAD", "APPEND needs a literal");
        const bool plus = rest.find('+', brace) != std::string::npos;
        const size_t n = static_cast<size_t>(std::strtoul(rest.c_str() + brace + 1, nullptr, 10));
        std::vector<std::string> a = split_args(rest.substr(0, brace));
        if (a.size() < 2) return tagged(c, tag, "BAD", "APPEND needs a mailbox");
        const std::string name = unquote(a[1]);
        std::set<std::string> flags;
        int64_t date = 0;
        for (size_t i = 2; i < a.size(); ++i) {
            if (!a[i].empty() && a[i][0] == '(') flags = parse_flags(a[i]);
            else if (!a[i].empty() && a[i][0] == '"') date = parse_search_date(a[i]);
        }
        bool exists;
        {
            std::lock_guard<std::mutex> l(mu_);
            exists = !find(name).name.empty();
        }
        if (!exists) return tagged(c, tag, "NO", "[TRYCREATE] no such mailbox");
        if (refuse_append) return tagged(c, tag, "NO", "[OVERQUOTA] mailbox is full");
        if (!plus && !write(c, "+ Ready for literal data\r\n")) return false;
        std::string data, tail;
        if (!read_bytes(c, n, data) || !read_line(c, tail)) return false;
        uint32_t uid, uv;
        {
            std::lock_guard<std::mutex> l(mu_);
            Folder& f = find(name);
            uid = add_message_locked(f, data, flags, date ? date : now_locked());  // the server stamps it when the client sent no date
            uv = f.uidvalidity;
        }
        if (drop_after_append) return false;   // stored, but the client never learns it
        return tagged(c, tag, "OK", "[APPENDUID " + std::to_string(uv) + " " + std::to_string(uid) + "] APPEND completed");
    }

    // UID COPY / UID MOVE <set> <mailbox> (RFC 3501, 4315, 6851). COPYUID is answered when the server advertises UIDPLUS (as real ones do).
    // The command is refused (and counted) when `refuse_copy` is set. A mailbox that does not exist: NO [TRYCREATE].
    bool do_uid_copy(Conn& c, const std::string& tag, const std::vector<std::string>& a, bool move) {
        if (a.size() < 4) return tagged(c, tag, "BAD", "COPY needs a set and a mailbox");
        if (refuse_copy) return tagged(c, tag, "NO", "[OVERQUOTA] cannot copy");
        const std::string dest = unquote(a[3]);
        std::string out, src_uids, dst_uids;
        uint32_t src_uv = 0, dst_uv = 0;
        {
            std::lock_guard<std::mutex> l(mu_);
            if (find(dest).name.empty()) return tagged(c, tag, "NO", "[TRYCREATE] no such mailbox");
            Folder& f = find(c.selected);
            Folder& d = find(dest);
            src_uv = f.uidvalidity;
            dst_uv = d.uidvalidity;
            const uint32_t max_uid = f.uidnext ? f.uidnext - 1 : 0;
            std::vector<Message> moved;
            for (const Message& m : f.messages)
                if (uid_in_set(m.uid, a[2], max_uid)) moved.push_back(m);
            for (const Message& m : moved) {
                const uint32_t nu = add_message_locked(d, m.raw, m.flags, m.internal_date);
                src_uids += (src_uids.empty() ? "" : ",") + std::to_string(m.uid);
                dst_uids += (dst_uids.empty() ? "" : ",") + std::to_string(nu);
            }
            if (move) {
                int seq = 0;
                for (auto it = f.messages.begin(); it != f.messages.end();) {
                    ++seq;
                    if (uid_in_set(it->uid, a[2], max_uid)) {
                        out += "* " + std::to_string(seq) + " EXPUNGE\r\n";
                        it = f.messages.erase(it);
                        --seq;
                    } else {
                        ++it;
                    }
                }
                ++f.highestmodseq;
            }
        }
        if (!out.empty()) write(c, out);
        std::string code;
        if (!src_uids.empty() && capabilities_.find("UIDPLUS") != std::string::npos)
            code = "[COPYUID " + std::to_string(dst_uv) + " " + src_uids + " " + dst_uids + "] ";
        (void)src_uv;
        return tagged(c, tag, "OK", code + (move ? "MOVE completed" : "COPY completed"));
    }

    // UID EXPUNGE <set>: only the messages of the set that carry \Deleted (RFC 4315)
    bool do_uid_expunge(Conn& c, const std::string& tag, const std::vector<std::string>& a) {
        if (a.size() < 3) return tagged(c, tag, "BAD", "UID EXPUNGE needs a set");
        std::string out;
        {
            std::lock_guard<std::mutex> l(mu_);
            Folder& f = find(c.selected);
            const uint32_t max_uid = f.uidnext ? f.uidnext - 1 : 0;
            int seq = 0;
            for (auto it = f.messages.begin(); it != f.messages.end();) {
                ++seq;
                if (uid_in_set(it->uid, a[2], max_uid) && it->flags.count("\\Deleted")) {
                    out += "* " + std::to_string(seq) + " EXPUNGE\r\n";
                    it = f.messages.erase(it);
                    --seq;
                } else {
                    ++it;
                }
            }
        }
        if (!out.empty()) write(c, out);
        return tagged(c, tag, "OK", "EXPUNGE completed");
    }

    bool do_search(Conn& c, const std::string& tag, const std::vector<std::string>& a) {
        std::string out = "* SEARCH";
        {
            std::lock_guard<std::mutex> l(mu_);
            const Folder& f = find(c.selected);
            const uint32_t max_uid = f.uidnext ? f.uidnext - 1 : 0;
            for (const Message& m : f.messages) {
                bool match = true;
                for (size_t i = 2; i < a.size(); ++i) {
                    const std::string k = upper(a[i]);
                    if (k == "ALL") continue;
                    if (k == "UNSEEN") match = match && !m.flags.count("\\Seen");
                    else if (k == "SEEN") match = match && m.flags.count("\\Seen");
                    else if (k == "SINCE" && i + 1 < a.size()) match = match && m.internal_date >= parse_search_date(a[++i]);
                    else if (k == "UID" && i + 1 < a.size()) match = match && uid_in_set(m.uid, a[++i], max_uid);
                    else if (k == "HEADER" && i + 2 < a.size()) {
                        std::string field = upper(unquote(a[++i]));
                        std::string value = unquote(a[++i]);
                        const std::string hdr = header_fields(m.raw, {field});
                        match = match && hdr.find(value) != std::string::npos;
                    }
                }
                if (match) out += " " + std::to_string(m.uid);
            }
        }
        write(c, out + "\r\n");
        return tagged(c, tag, "OK", "SEARCH completed");
    }

    bool do_fetch(Conn& c, const std::string& tag, const std::vector<std::string>& a) {
        if (a.size() < 4) return tagged(c, tag, "BAD", "FETCH needs a set and items");
        const std::string set = a[2];
        // libcurl's URL fetch sends a bare item ("BODY[]"), the app's custom requests send a parenthesized list
        std::vector<std::string> items = a[3][0] == '(' ? split_args(a[3].substr(1, a[3].size() - 2)) : std::vector<std::string>{a[3]};
        uint64_t changed_since = 0;
        bool want_modseq = false;
        if (a.size() >= 5) {  // (CHANGEDSINCE n)
            const std::string mod = upper(a[4]);
            const size_t p = mod.find("CHANGEDSINCE");
            if (p != std::string::npos) {
                changed_since = std::strtoull(mod.c_str() + p + 12, nullptr, 10);
                want_modseq = true;
            }
        }
        std::vector<Message> msgs;
        std::vector<int> seqs;
        {
            std::lock_guard<std::mutex> l(mu_);
            const Folder& f = find(c.selected);
            const uint32_t max_uid = f.uidnext ? f.uidnext - 1 : 0;
            int seq = 0;
            for (const Message& m : f.messages) {
                ++seq;
                if (!uid_in_set(m.uid, set, max_uid)) continue;
                if (want_modseq && m.modseq <= changed_since) continue;
                msgs.push_back(m);
                seqs.push_back(seq);
            }
        }
        std::vector<uint32_t> mark_seen;
        for (size_t k = 0; k < msgs.size(); ++k) {
            const Message& m = msgs[k];
            std::string out = "* " + std::to_string(seqs[k]) + " FETCH (UID " + std::to_string(m.uid);
            for (const std::string& it : items) {
                const std::string U = upper(it);
                if (U == "UID") continue;
                if (U == "FLAGS") out += " FLAGS " + flag_list(m.flags);
                else if (U == "INTERNALDATE") out += " INTERNALDATE \"" + imap_date(m.internal_date) + "\"";
                else if (U == "RFC822.SIZE") out += " RFC822.SIZE " + std::to_string(m.raw.size());
                else if (U == "MODSEQ") out += " MODSEQ (" + std::to_string(m.modseq) + ")";
                else if (U == "BODYSTRUCTURE" || U == "BODY") out += " BODYSTRUCTURE " + (corrupt_bodystructure ? std::string("(((((") : render_structure(parse_mime(m.raw)));
                else if (U.compare(0, 5, "BODY[") == 0 || U.compare(0, 10, "BODY.PEEK[") == 0) {
                    if (U.compare(0, 5, "BODY[") == 0 && U.compare(0, 11, "BODY[HEADER") != 0) mark_seen.push_back(m.uid);   // without PEEK the message counts as read
                    const size_t lb = it.find('['), rb = it.rfind(']');
                    const std::string section = it.substr(lb + 1, rb - lb - 1);
                    bool exists = true;
                    if (!section.empty() && std::isdigit(static_cast<unsigned char>(section[0]))) numeric_section(m.raw, section, &exists);
                    if (!exists) {   // a part that is not there: NIL, like a real server
                        out += " BODY[" + section + "] NIL";
                        continue;
                    }
                    std::string data = section_data(m.raw, section);
                    std::string key = "BODY[" + section + "]";
                    const size_t lt = it.find('<', rb);
                    if (lt != std::string::npos) {  // <origin.length>
                        const size_t origin = std::strtoul(it.c_str() + lt + 1, nullptr, 10);
                        const size_t dot = it.find('.', lt);
                        const size_t len = dot == std::string::npos ? data.size() : std::strtoul(it.c_str() + dot + 1, nullptr, 10);
                        data = origin < data.size() ? data.substr(origin, len) : std::string();
                        key += "<" + std::to_string(origin) + ">";
                    }
                    out += " " + key + " {" + std::to_string(data.size()) + "}\r\n" + data;
                    // the rest of the item list continues after the literal
                    write(c, out);
                    out.clear();
                }
            }
            if (!write(c, out + ")\r\n")) return false;
        }
        if (!mark_seen.empty()) {
            std::lock_guard<std::mutex> l(mu_);
            Folder& f = find(c.selected);
            for (Message& mm : f.messages)
                if (std::find(mark_seen.begin(), mark_seen.end(), mm.uid) != mark_seen.end() && !mm.flags.count("\\Seen")) {
                    mm.flags.insert("\\Seen");
                    mm.modseq = ++f.highestmodseq;
                }
        }
        return tagged(c, tag, "OK", "FETCH completed");
    }

    bool do_store(Conn& c, const std::string& tag, const std::vector<std::string>& a) {
        if (a.size() < 5) return tagged(c, tag, "BAD", "STORE needs a set, an action and flags");
        const std::string set = a[2];
        const std::string action = upper(a[3]);
        const bool silent = action.find(".SILENT") != std::string::npos;
        const bool add = action[0] == '+', remove = action[0] == '-';
        const std::set<std::string> flags = parse_flags(a[4]);
        std::string out;
        {
            std::lock_guard<std::mutex> l(mu_);
            Folder& f = find(c.selected);
            const uint32_t max_uid = f.uidnext ? f.uidnext - 1 : 0;
            int seq = 0;
            for (Message& m : f.messages) {
                ++seq;
                if (!uid_in_set(m.uid, set, max_uid)) continue;
                if (add) m.flags.insert(flags.begin(), flags.end());
                else if (remove) for (const auto& fl : flags) m.flags.erase(fl);
                else m.flags = flags;
                m.modseq = ++f.highestmodseq;
                if (!silent) out += "* " + std::to_string(seq) + " FETCH (UID " + std::to_string(m.uid) + " FLAGS " + flag_list(m.flags) + ")\r\n";
            }
        }
        if (!out.empty()) write(c, out);
        return tagged(c, tag, "OK", "STORE completed");
    }

    // ------------------------------------------------------------------ state
    Config config_;
    SSL_CTX* ctx_ = nullptr;
    int listen_fd_ = -1;
    int port_ = 0;
    bool running_ = false;
    std::atomic<bool> stop_{false};
    std::thread accept_thread_;
    std::vector<std::thread> conn_threads_;
    mutable std::mutex mu_;
    std::vector<Folder> folders_;
    std::string capabilities_ = "IMAP4rev1 UIDPLUS LITERAL+";
    std::vector<std::string> commands_;
    std::atomic<int> connections_{0};
    std::atomic<bool> credentials_seen_{false};
    std::atomic<bool> plaintext_credentials_seen_{false};
};

}  // namespace pbtest
