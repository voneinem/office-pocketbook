#pragma once
// Shared set-up of the mail organizing tests (US5): a store with a mail account, a fake IMAP server with INBOX, Trash and Archive, and run_sync with
// the real curl transport. UNVERIFIED against real servers (fake server).
#include <ctime>
#include <set>
#include <string>
#include <vector>

#include "fake_imap_server.h"
#include "pbo/imap_transport.h"
#include "pbo/store.h"
#include "pbo/sync.h"
#include "test_support.h"
#include "tls_test_server.h"
#include "tmp_dir.h"

namespace pbtest {

struct MailClock : pbo::Clock {
    int64_t t = 1791979200;
    int64_t now() const override { return t; }
};
struct MailNoCancel : pbo::Cancel {
    bool requested() const override { return false; }
};

struct MailEnv {
    TmpDir dir, pki_dir;
    TestPki pki{pki_dir};
    TestPki::Leaf leaf;
    MailClock clock;
    MailNoCancel cancel;
    std::unique_ptr<FakeImapServer> imap;
    std::unique_ptr<pbo::Store> store;
    int next = 1;

    // `capabilities` of the server: "IMAP4rev1 UIDPLUS LITERAL+" (+ " MOVE" ...); `with_trash`: a folder with the \Trash attribute
    explicit MailEnv(const std::string& capabilities = "IMAP4rev1 UIDPLUS LITERAL+", bool with_trash = true) {
        if (!pki.available()) throw Skip{"the openssl command is not available"};
        leaf = pki.issue("srv", "DNS:localhost,IP:127.0.0.1");
        if (!leaf.ok) throw Skip{"cannot create a test certificate"};
        FakeImapServer::Config c;
        c.cert = leaf.cert;
        c.key = leaf.key;
        imap = std::make_unique<FakeImapServer>(c);
        imap->set_capabilities(capabilities);
        imap->add_folder("INBOX");
        if (with_trash) imap->add_folder("Trash", {"\\Trash"});
        imap->add_folder("Archive", {"\\Archive"});
        store = std::move(pbo::Store::open(dir.path(), clock).value);
        pbo::MailAccount a;
        a.imap_host = "localhost";
        a.imap_port = imap->port();
        a.smtp_host = "localhost";
        a.username = "alice";
        a.address = "alice@example.org";
        store->save_mail_account(a);
    }
    // a message dated `age_days` before the clock of the test (the default date is 5 October 2026)
    static std::string mail(int n, int age_days = -1, int64_t now = 1791979200) {
        std::string date = "Mon, 05 Oct 2026 10:00:00 +0000";
        if (age_days >= 0) {
            char buf[64];
            const time_t t = static_cast<time_t>(now - age_days * 86400);
            struct tm g;
            gmtime_r(&t, &g);
            std::strftime(buf, sizeof buf, "%a, %d %b %Y %H:%M:%S +0000", &g);
            date = buf;
        }
        return "From: Sender <s" + std::to_string(n) + "@example.org>\r\nSubject: Subject " + std::to_string(n) + "\r\nDate: " + date + "\r\nMessage-ID: <m" + std::to_string(n) +
               "@example.org>\r\n\r\nBody " + std::to_string(n) + "\r\n";
    }
    uint32_t add(const std::string& folder = "INBOX", std::set<std::string> flags = {}, int age_days = 1, bool dated = false) {
        const int n = next++;
        return imap->add_message(folder, dated ? mail(n, age_days, clock.t) : mail(n), std::move(flags), clock.t - age_days * 86400 + 3600);
    }
    int64_t storage_limit_override = 0;   // bytes; 0 = the account's limit
    pbo::SyncReport sync() {
        auto transport = pbo::make_curl_imap_transport(pki.ca_cert());
        pbo::SyncDeps deps{*store, *transport, clock, cancel};
        pbo::Credentials creds;
        creds.mail_password = "pw-for-tests";
        pbo::SyncOptions options;
        options.calendar = false;
        options.storage_limit_override = storage_limit_override;
        return pbo::run_sync(deps, creds, options);
    }
    std::string q(const std::string& sql) const { return store->debug_query(sql); }
    int64_t folder_id(const std::string& name) const {
        for (const auto& f : store->folders())
            if (f.name == name) return f.id;
        return 0;
    }
    // the store id of the message with this UID in this folder
    int64_t id_of(const std::string& folder, uint32_t uid) const {
        return std::atoll(q("SELECT id FROM message WHERE folder_id = " + std::to_string(folder_id(folder)) + " AND uid = " + std::to_string(uid) + " AND stale = 0").c_str());
    }
    // the commands the server received after `mark`, as one text
    std::vector<std::string> commands_since(size_t mark) const {
        const auto all = imap->commands();
        return std::vector<std::string>(all.begin() + static_cast<std::ptrdiff_t>(std::min(mark, all.size())), all.end());
    }
    static bool any(const std::vector<std::string>& cmds, const std::string& word) {
        for (const auto& c : cmds)
            if (c.find(word) != std::string::npos) return true;
        return false;
    }
    static int count(const std::vector<std::string>& cmds, const std::string& word) {
        int n = 0;
        for (const auto& c : cmds)
            if (c.find(word) != std::string::npos) ++n;
        return n;
    }
};

}  // namespace pbtest
