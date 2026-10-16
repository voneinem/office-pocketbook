// pbo_fake_imap: a developer tool (host only) that serves a demo mailbox from the in-process fake IMAP server over
// TLS with a throw-away certificate authority, so the app can be tried without any real account (quickstart.md
// section 3) and so `pbo-probe --imap-selftest` can run from the reader against the development machine.
//
//   pbo_fake_imap --ca-out <file> [--port N] [--listen ADDR] [--host NAME] [--messages N]
//
//   --ca-out    where the throw-away CA certificate is written (give it to the app as custom-ca.pem)
//   --port      fixed port (default: a free one)
//   --listen    bind address, default 127.0.0.1; 0.0.0.0 serves other devices on the LAN (still TLS)
//   --host      the server name in the certificate, default localhost; the name or address the client connects to
//   --messages  number of demo messages in INBOX (default 40)
//
// It prints one line "host=... port=... user=... password=... smtp=<port of the fake outgoing server>" and runs until it is stopped. UNVERIFIED against real
// servers: it proves the app against this harness, not against a provider (constitution Principle IX).
#include <signal.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <string>
#include <thread>

#include "fake_imap_server.h"
#include "fake_smtp_server.h"
#include "test_support.h"
#include "tls_test_server.h"
#include "tmp_dir.h"

PBO_FAKE_SERVER_TEST();

namespace {

volatile sig_atomic_t g_stop = 0;
void on_signal(int) { g_stop = 1; }

std::string rfc_date(int64_t epoch) {
    char buf[64];
    time_t t = static_cast<time_t>(epoch);
    tm tm{};
    gmtime_r(&t, &tm);
    std::strftime(buf, sizeof buf, "%a, %d %b %Y %H:%M:%S +0000", &tm);
    return buf;
}

const char* kSenders[][2] = {{"Anna Beispiel", "anna@example.org"}, {"Ben Muster", "ben@example.org"}, {"Carla Probe", "carla@example.org"},
                             {"Dieter Test", "dieter@example.org"}, {"Erika Demo", "erika@example.org"}};
const char* kSubjects[] = {"Planung Oktober", "Re: Planung Oktober", "Einladung zum Treffen", "Rechnung 2026-117", "Fotos vom Wochenende",
                           "Protokoll der Sitzung", "Kurze Frage zum Angebot", "Weekly summary", "Lunch on Thursday?", "Neue Version verfuegbar"};

}  // namespace

int main(int argc, char** argv) {
    std::string ca_out, listen = "127.0.0.1", host = "localhost";
    int port = 0, count = 40;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](std::string* out) {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value for %s\n", a.c_str());
                std::exit(2);
            }
            *out = argv[++i];
        };
        std::string v;
        if (a == "--ca-out") next(&ca_out);
        else if (a == "--listen") next(&listen);
        else if (a == "--host") next(&host);
        else if (a == "--port") { next(&v); port = std::atoi(v.c_str()); }
        else if (a == "--messages") { next(&v); count = std::atoi(v.c_str()); }
        else {
            std::fprintf(stderr, "usage: pbo_fake_imap --ca-out FILE [--port N] [--listen ADDR] [--host NAME] [--messages N]\n");
            return 2;
        }
    }
    if (ca_out.empty()) {
        std::fprintf(stderr, "--ca-out is required\n");
        return 2;
    }

    pbtest::TmpDir pki_dir;
    pbtest::TestPki pki(pki_dir);
    if (!pki.available()) {
        std::fprintf(stderr, "the openssl command is not available\n");
        return 1;
    }
    // the certificate names the host the client connects to (a DNS name or an IPv4 address)
    const bool is_ip = host.find_first_not_of("0123456789.") == std::string::npos;
    pbtest::TestPki::Leaf leaf = pki.issue("srv", is_ip ? "IP:" + host + ",DNS:localhost,IP:127.0.0.1" : "DNS:" + host + ",DNS:localhost,IP:127.0.0.1");
    if (!leaf.ok) {
        std::fprintf(stderr, "cannot create the certificate\n");
        return 1;
    }
    {
        std::ifstream in(pki.ca_cert(), std::ios::binary);
        std::ofstream out(ca_out, std::ios::binary | std::ios::trunc);
        out << in.rdbuf();
        if (!out) {
            std::fprintf(stderr, "cannot write %s\n", ca_out.c_str());
            return 1;
        }
    }

    pbtest::FakeImapServer::Config cfg;
    cfg.cert = leaf.cert;
    cfg.key = leaf.key;
    cfg.bind_address = listen;
    cfg.port = port;
    pbtest::FakeImapServer server(cfg);
    if (!server.running()) {
        std::fprintf(stderr, "the server could not start (port in use?)\n");
        return 1;
    }
    pbtest::FakeSmtpServer::Config scfg;   // the account form's connection test also tries the outgoing server
    scfg.cert = leaf.cert;
    scfg.key = leaf.key;
    scfg.bind_address = listen;
    pbtest::FakeSmtpServer smtp(scfg);
    server.set_capabilities("IMAP4rev1 UIDPLUS LITERAL+ CONDSTORE");
    server.add_folder("INBOX");
    server.add_folder("Sent", {"\\Sent"});
    server.add_folder("Drafts", {"\\Drafts"});
    server.add_folder("Trash", {"\\Trash"});
    const int64_t now = std::time(nullptr);
    for (int i = 1; i <= count; ++i) {
        const int64_t when = now - static_cast<int64_t>(count - i) * 14400 - 600;   // one message every 4 hours, newest 10 minutes ago
        const char** sender = const_cast<const char**>(kSenders[i % 5]);
        const std::string subject = std::string(kSubjects[i % 10]) + " (" + std::to_string(i) + ")";
        const std::string raw = std::string("From: ") + sender[0] + " <" + sender[1] + ">\r\nTo: alice@example.org\r\nSubject: " + subject +
                                "\r\nDate: " + rfc_date(when) + "\r\nMessage-ID: <demo" + std::to_string(i) + "@example.org>\r\n\r\nDemo message " +
                                std::to_string(i) + ".\r\n";
        std::set<std::string> flags;
        if (i % 4 != 0) flags.insert("\\Seen");   // every 4th message is unread
        if (i % 9 == 0) flags.insert("\\Flagged");
        server.add_message("INBOX", raw, flags, when);
    }

    ::signal(SIGINT, on_signal);
    ::signal(SIGTERM, on_signal);
    std::printf("host=%s port=%d user=%s password=%s smtp=%d\n", host.c_str(), server.port(), cfg.user.c_str(), cfg.password.c_str(), smtp.port());
    std::fflush(stdout);
    while (!g_stop) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    return 0;
}
