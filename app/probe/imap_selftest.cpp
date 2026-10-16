#include "imap_selftest.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "pbo/imap_parse.h"
#include "pbo/imap_transport.h"

namespace {

struct Report {
    int failed = 0;
    void pass(const char* name) { std::printf("PASS %s\n", name); }
    void fail(const char* name, const std::string& key) {
        std::printf("FAIL %s %s\n", name, key.empty() ? "unspecified" : key.c_str());
        ++failed;
    }
    void skip(const char* name, const char* reason) { std::printf("SKIP %s %s\n", name, reason); }
    void info(const char* name, const std::string& text) { std::printf("INFO %s %s\n", name, text.c_str()); }
    void check(const char* name, bool ok, const std::string& key = std::string()) {
        if (ok) pass(name);
        else fail(name, key);
    }
};

struct CountSink : pbo::ByteSink {
    size_t bytes = 0;
    bool write(const char*, size_t n) override {   // the data is counted and dropped: never kept, never printed
        bytes += n;
        return true;
    }
};

std::string env_or(const char* name, const char* fallback) {
    const char* v = std::getenv(name);
    return v && *v ? v : fallback;
}

}  // namespace

int run_imap_selftest(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: pbo-probe --imap-selftest <host> <port> <ca.pem> [--bad-login] [--append <mailbox>]\n");
        return 2;
    }
    const std::string host = argv[0], ca = argv[2];
    const int port = std::atoi(argv[1]);
    bool bad_login = false;
    std::string append_mailbox;
    for (int i = 3; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--bad-login")) bad_login = true;
        else if (!std::strcmp(argv[i], "--append") && i + 1 < argc) append_mailbox = argv[++i];
    }
    const std::string user = env_or("PBO_SELFTEST_USER", "alice");
    const std::string password = env_or("PBO_SELFTEST_PASSWORD", "pw-for-tests");

    pbo::ImapEndpoint ep;
    ep.host = host;
    ep.port = port;
    ep.security = pbo::Security::Tls;
    ep.username = user;

    Report r;
    auto transport = pbo::make_curl_imap_transport(ca);
    const pbo::Error conn = transport->connect(ep, password);
    r.check("connect", conn.ok(), conn.key);
    if (!conn.ok()) return 1;   // nothing else can run

    r.check("capability", transport->capabilities().count("imap4rev1") == 1, "no_imap4rev1");

    {
        auto list = transport->command("", "LIST \"\" \"*\"");
        bool has_inbox = false;
        if (list.ok())
            for (const auto& item : pbo::parse_list(list.value).value) {
                std::string n = item.raw_name;
                for (char& c : n) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                if (n == "INBOX") has_inbox = true;
            }
        r.check("list", list.ok() && has_inbox, list.ok() ? "no_inbox" : list.error.key);
    }
    {
        auto st = transport->command("", "STATUS \"INBOX\" (UIDNEXT UIDVALIDITY MESSAGES)");
        const auto parsed = st.ok() ? pbo::parse_status(st.value) : pbo::Parsed<pbo::StatusInfo>();
        r.check("status", st.ok() && parsed.ok && parsed.value.has_uidvalidity && parsed.value.has_uidnext, st.ok() ? "unparsable" : st.error.key);
    }

    std::vector<uint32_t> uids;
    {
        auto se = transport->command("INBOX", "UID SEARCH ALL");
        if (se.ok()) uids = pbo::parse_search(se.value).value;
        r.check("search", se.ok(), se.error.key);
    }

    if (uids.empty()) {
        for (const char* n : {"fetch_header_literal", "fetch_missing_paren", "session_in_sync", "fetch_body_url", "fetch_body_limit"}) r.skip(n, "inbox_is_empty");
    } else {
        const uint32_t uid = uids.front();
        auto f = transport->command("INBOX", "UID FETCH " + std::to_string(uid) + " (UID FLAGS BODY.PEEK[HEADER.FIELDS (SUBJECT)])");
        pbo::Parsed<std::vector<pbo::FetchItem>> items;
        if (f.ok()) items = pbo::parse_fetch(f.value);
        const bool got = f.ok() && items.value.size() == 1 && items.value[0].uid == uid && !items.value[0].header_bytes.empty();
        r.check("fetch_header_literal", got, f.ok() ? "item_missing" : f.error.key);
        // libcurl may or may not deliver the ")" that closes the item after a literal; either way the item is understood
        r.check("fetch_missing_paren", got, "item_missing");
        if (f.ok()) {
            const std::string& d = f.value.data;
            const size_t lit = d.rfind('}');
            r.info("closing_parenthesis", lit != std::string::npos && d.find(')', lit) != std::string::npos ? "delivered" : "not_delivered");
        }

        auto again = transport->command("INBOX", "UID SEARCH ALL");
        r.check("session_in_sync", again.ok() && pbo::parse_search(again.value).value == uids, again.ok() ? "answer_differs" : again.error.key);

        // characterization of the libcurl behavior behind research R-01 (informational, never a failure)
        if (uids.size() >= 2) {
            auto multi = transport->command("INBOX", "UID FETCH " + std::to_string(uids[0]) + "," + std::to_string(uids[1]) +
                                                         " (UID BODY.PEEK[HEADER.FIELDS (SUBJECT)])");
            const size_t n = multi.ok() ? pbo::parse_fetch(multi.value).value.size() : 0;
            auto after = transport->command("INBOX", "UID SEARCH ALL");
            const bool in_sync = after.ok() && pbo::parse_search(after.value).value == uids;
            r.info("multi_literal_fetch", "items=" + std::to_string(n) + "/2 session_in_sync=" + (in_sync ? "yes" : "no"));
            if (!in_sync) {   // resynchronize with a fresh connection before the remaining checks
                transport->disconnect();
                const pbo::Error again_conn = transport->connect(ep, password);
                if (!again_conn.ok()) {
                    r.fail("reconnect", again_conn.key);
                    return 1;
                }
            }
        } else {
            r.info("multi_literal_fetch", "skipped_fewer_than_two_messages");
        }

        CountSink whole;
        const pbo::Error body = transport->fetch_to("INBOX", uid, "", 4u << 20, whole);
        r.check("fetch_body_url", body.ok() && whole.bytes > 0, body.ok() ? "empty" : body.key);

        CountSink part;
        const pbo::Error limited = transport->fetch_to("INBOX", uid, "", 16, part);
        r.check("fetch_body_limit", limited.key == "too_large" && part.bytes <= 16, limited.ok() ? "no_limit_error" : limited.key);
    }

    if (bad_login) {
        auto other = pbo::make_curl_imap_transport(ca);
        const pbo::Error e = other->connect(ep, password + "-wrong");
        r.check("wrong_password", e.key == "auth_failed", e.key);
    } else {
        r.skip("wrong_password", "flag_not_given");
    }

    if (!append_mailbox.empty()) {
        const std::string id = "<pbo-selftest-" + std::to_string(static_cast<long>(std::rand())) + "@example.org>";
        const std::string msg = "From: " + user + "@example.org\r\nSubject: pbo self-test\r\nMessage-ID: " + id + "\r\n\r\nself-test message\r\n";
        const pbo::Error e = transport->append(append_mailbox, msg);
        bool found = false;
        if (e.ok()) {
            auto s = transport->command(append_mailbox, "UID SEARCH HEADER Message-ID \"" + id + "\"");
            found = s.ok() && !pbo::parse_search(s.value).value.empty();
        }
        r.check("append", e.ok() && found, e.ok() ? "not_found_afterwards" : e.key);
    } else {
        r.skip("append", "flag_not_given");
    }

    transport->disconnect();
    return r.failed == 0 ? 0 : 1;
}
