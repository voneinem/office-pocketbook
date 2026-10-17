#pragma once
// Shared set-up of the outbox tests: a store with a mail account, a fake IMAP server with an INBOX and a Sent folder, a fake SMTP server, and
// helpers to queue a message and to run the outbox step with the real curl transports. UNVERIFIED against real servers (fake servers).

#include <map>
#include <memory>
#include <string>

#include "fake_imap_server.h"
#include "fake_smtp_server.h"
#include "pbo/compose.h"
#include "pbo/curl_session.h"
#include "pbo/draft_mirror.h"
#include "pbo/imap_transport.h"
#include "pbo/outbox.h"
#include "pbo/smtp_transport.h"
#include "pbo/store.h"
#include "test_support.h"
#include "tls_test_server.h"
#include "tmp_dir.h"

namespace pbtest {

struct OutboxClock : pbo::Clock {
    int64_t t = 1791979200;
    int64_t now() const override { return t; }
};
struct OutboxCancel : pbo::Cancel {
    bool flag = false;
    bool requested() const override { return flag; }
};

struct OutboxEnv {
    TmpDir dir, pki_dir;
    TestPki pki{pki_dir};
    TestPki::Leaf leaf;
    OutboxClock clock;
    OutboxCancel cancel;
    std::unique_ptr<FakeImapServer> imap;
    std::unique_ptr<FakeSmtpServer> smtp;
    std::unique_ptr<pbo::Store> store;
    int counter = 0;
    std::map<int64_t, std::string> ids;   // outbox id -> Message-ID, kept after the row is gone

    explicit OutboxEnv(bool sent_by_server = false) {
        if (!pki.available()) throw Skip{"the openssl command is not available"};
        leaf = pki.issue("srv", "DNS:localhost,IP:127.0.0.1");
        if (!leaf.ok) throw Skip{"cannot create a test certificate"};
        FakeImapServer::Config ic;
        ic.cert = leaf.cert;
        ic.key = leaf.key;
        imap = std::make_unique<FakeImapServer>(ic);
        imap->set_capabilities("IMAP4rev1 UIDPLUS LITERAL+");
        imap->add_folder("INBOX");
        imap->add_folder("Sent", {"\\Sent"});
        imap->add_folder("Drafts", {"\\Drafts"});
        FakeSmtpServer::Config sc;
        sc.cert = leaf.cert;
        sc.key = leaf.key;
        smtp = std::make_unique<FakeSmtpServer>(sc);
        store = std::move(pbo::Store::open(dir.path(), clock).value);
        pbo::MailAccount a;
        a.imap_host = "localhost";
        a.imap_port = imap->port();
        a.smtp_host = "localhost";
        a.smtp_port = smtp->port();
        a.username = "alice";
        a.address = "alice@example.org";
        a.sent_by_server = sent_by_server;
        store->save_mail_account(a);
        pbo::FolderListItem inbox, sent, drafts;
        inbox.raw_name = inbox.name = "INBOX";
        inbox.delimiter = "/";
        sent.raw_name = sent.name = "Sent";
        sent.delimiter = "/";
        sent.attributes = {"\\Sent"};
        drafts.raw_name = drafts.name = "Drafts";
        drafts.delimiter = "/";
        drafts.attributes = {"\\Drafts"};
        store->upsert_folders({inbox, sent, drafts});
    }

    // A draft that was sent: queued with a message built like the compose code builds it. Returns the outbox id.
    int64_t queue(const std::string& subject = "Hello", const std::string& to = "bob@example.org") {
        pbo::Draft d;
        d.message_id = "<o" + std::to_string(++counter) + "@example.org>";
        d.to_text = to;
        d.subject = subject;
        d.body = "Hello Bob.\n";
        const int64_t id = store->save_draft(d).value;
        pbo::ComposeDraft c;
        c.from_name = "Alice";
        c.from_addr = "alice@example.org";
        c.to = to;
        c.subject = subject;
        c.body = d.body;
        c.message_id = d.message_id;
        c.date_utc = clock.t;
        auto built = pbo::build_message(c);
        PB_CHECK(built.ok());
        pbo::OutboxRequest q;
        q.expected_hash = store->draft(id)->content_hash;
        q.envelope_from = built.value.envelope_from;
        q.recipients = built.value.recipients;
        q.subject_preview = subject;
        q.raw_message = built.value.raw;
        auto r = store->queue_draft(id, q);
        PB_CHECK(r.ok());
        ids[r.value] = d.message_id;
        return r.value;
    }
    std::string message_id(int64_t outbox_id) const { return ids.at(outbox_id); }
    pbo::OutboxCounts run(const std::string& smtp_password = "pw-for-tests") {
        auto smtp_transport = pbo::make_curl_smtp_transport(pki.ca_cert(), short_timeouts());
        auto imap_transport = pbo::make_curl_imap_transport(pki.ca_cert(), short_timeouts());
        pbo::UidvalidityGuard guard;
        pbo::OutboxDeps deps{*store, *smtp_transport, *imap_transport, clock, cancel, guard};
        pbo::Credentials creds;
        creds.mail_password = "pw-for-tests";
        creds.smtp_password = smtp_password;
        pbo::OutboxCounts counts;
        PB_CHECK(pbo::run_outbox(deps, creds, &counts).ok());
        return counts;
    }
    // The draft mirror step with the real IMAP transport. `random` makes the Message-IDs of conflict drafts.
    pbo::DraftMirrorCounts mirror() {
        auto imap_transport = pbo::make_curl_imap_transport(pki.ca_cert(), short_timeouts());
        pbo::UidvalidityGuard guard;
        pbo::SystemRandom random;
        pbo::DraftMirrorDeps deps{*store, *imap_transport, clock, cancel, random, guard};
        pbo::Credentials creds;
        creds.mail_password = "pw-for-tests";
        pbo::DraftMirrorCounts counts;
        PB_CHECK(pbo::run_draft_mirror(deps, creds, &counts).ok());
        return counts;
    }
    // A draft the user wrote (not sent). Returns its id.
    int64_t draft(const std::string& subject = "Draft", const std::string& body = "Some text.\n") {
        pbo::Draft d;
        d.message_id = "<dr" + std::to_string(++counter) + "@example.org>";
        d.to_text = "bob@example.org";
        d.subject = subject;
        d.body = body;
        auto r = store->save_draft(d);
        PB_CHECK(r.ok());
        return r.value;
    }
    void edit(int64_t id, const std::string& body) {
        clock.t += 5;
        auto d = *store->draft(id);
        d.body = body;
        PB_CHECK(store->save_draft(d).ok());
    }
    static pbo::TransportTimeouts short_timeouts() {
        pbo::TransportTimeouts t;
        t.connect_s = 5;
        t.low_speed_s = 3;
        t.command_s = 5;
        return t;
    }
    std::string state(int64_t id) {
        auto i = store->outbox_item(id);
        return i ? i->state : "gone";
    }
    int appends() const {
        int n = 0;
        for (const std::string& c : imap->commands())
            if (c.find("APPEND") != std::string::npos) ++n;
        return n;
    }
};

}  // namespace pbtest
