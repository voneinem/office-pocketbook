// T066: downloading an attachment on demand: decoded into the Downloads folder under its real (made safe) file name, never
// over an existing file, size limit, no partial file, no "read" mark, resumable by simply trying again.
// UNVERIFIED against real servers (fake server).
#include <sys/stat.h>
#include <unistd.h>

#include <dirent.h>

#include <algorithm>
#include <fstream>
#include <sstream>
#include <string>

#include "fake_imap_server.h"
#include "fixtures.h"
#include "pbo/attachment_service.h"
#include "pbo/imap_transport.h"
#include "pbo/store.h"
#include "pbo/sync.h"
#include "test_support.h"
#include "tls_test_server.h"
#include "tmp_dir.h"

using pbtest::FakeImapServer;

PBO_FAKE_SERVER_TEST();

namespace {

constexpr int64_t kNow = 1791979200;

struct FixedClock : pbo::Clock {
    int64_t now() const override { return kNow; }
};
struct NeverCancel : pbo::Cancel {
    bool requested() const override { return false; }
};

const std::string kPdf = "%PDF-1.4 fake pdf content for tests only\n%PDF-1.4 fake pdf content for tests only\n%PDF-1.4 fake pdf content for tests only\n";

struct Env {
    pbtest::TmpDir dir, pki_dir, dl_dir;
    pbtest::TestPki pki{pki_dir};
    pbtest::TestPki::Leaf leaf;
    FixedClock clock;
    NeverCancel cancel;
    std::unique_ptr<FakeImapServer> server;
    std::unique_ptr<pbo::Store> store;
    std::unique_ptr<pbo::ImapTransport> imap;
    Env() {
        if (!pki.available()) throw pbtest::Skip{"the openssl command is not available"};
        leaf = pki.issue("srv", "DNS:localhost,IP:127.0.0.1");
        if (!leaf.ok) throw pbtest::Skip{"cannot create a test certificate"};
        FakeImapServer::Config c;
        c.cert = leaf.cert;
        c.key = leaf.key;
        server = std::make_unique<FakeImapServer>(c);
        server->set_capabilities("IMAP4rev1 UIDPLUS LITERAL+");
        server->add_folder("INBOX");
        store = std::move(pbo::Store::open(dir.path(), clock).value);
        pbo::MailAccount a;
        a.imap_host = "localhost";
        a.imap_port = server->port();
        a.smtp_host = "localhost";
        a.username = "alice";
        a.address = "alice@example.org";
        store->save_mail_account(a);
    }
    uint32_t add(const std::string& raw) { return server->add_message("INBOX", raw, {}, kNow - 3600); }
    void sync() {
        auto transport = pbo::make_curl_imap_transport(pki.ca_cert());
        pbo::SyncDeps deps{*store, *transport, clock, cancel};
        pbo::Credentials creds;
        creds.mail_password = "pw-for-tests";
        PB_CHECK(pbo::run_sync(deps, creds).first_problem.ok());
    }
    void connect() {
        imap = pbo::make_curl_imap_transport(pki.ca_cert());
        pbo::ImapEndpoint ep;
        ep.host = "localhost";
        ep.port = server->port();
        ep.username = "alice";
        PB_CHECK(imap->connect(ep, "pw-for-tests").ok());
    }
    int64_t attachment_id(const std::string& section) const {
        return std::stoll(store->debug_query("SELECT id FROM attachment WHERE section = '" + section + "' ORDER BY id LIMIT 1"));
    }
    std::string downloads() const { return dl_dir.path(); }
    pbo::Error download(int64_t id, uint64_t limit = 10u << 20) { return pbo::download_attachment(*imap, *store, id, limit, downloads()); }
    std::string saved(int64_t id) const { return store->attachment(id)->attachment.saved_path; }
    std::vector<std::string> files_in(const std::string& d) const {
        std::vector<std::string> out;
        if (DIR* h = opendir(d.c_str())) {
            while (dirent* e = readdir(h)) {
                const std::string n = e->d_name;
                if (n != "." && n != "..") out.push_back(n);
            }
            closedir(h);
        }
        std::sort(out.begin(), out.end());
        return out;
    }
};

std::string read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
bool exists(const std::string& path) {
    struct stat st;
    return ::stat(path.c_str(), &st) == 0;
}
size_t count_fetches(const FakeImapServer& s) {
    size_t n = 0;
    for (const std::string& c : s.commands())
        if (c.find("BODY.PEEK[2]") != std::string::npos) ++n;
    return n;
}

}  // namespace

PB_TEST(an_attachment_is_saved_decoded_in_the_downloads_folder_under_its_real_name) {
    Env env;
    env.add(pbtest::fixture("mail/with_attachments.eml"));
    env.sync();
    env.connect();
    const int64_t pdf = env.attachment_id("2"), png = env.attachment_id("3");
    PB_CHECK(env.download(pdf).ok());
    PB_CHECK_EQ(env.saved(pdf), env.downloads() + "/report.pdf");
    PB_CHECK_EQ(read_file(env.downloads() + "/report.pdf"), kPdf);              // the decoded content, not base64
    PB_CHECK(env.download(png).ok());
    PB_CHECK_EQ(env.saved(png), env.downloads() + "/gr\xC3\xBC\xC3\x9F" "e.png");   // the RFC 2231 name, with its umlaut and sharp s
    PB_CHECK_EQ(env.files_in(env.downloads()).size(), static_cast<size_t>(2));  // nothing else: no part file, no second copy
    PB_CHECK(env.files_in(env.dir.path() + "/attachments").empty());
    const auto ref = env.store->attachment(pdf);
    PB_CHECK_EQ(ref->attachment.state, std::string("downloaded"));
    PB_CHECK_EQ(ref->attachment.error_key, std::string(""));
    PB_CHECK(env.server->flags_of("INBOX", 1).count("\\Seen") == 0);           // downloading is not reading
}

PB_TEST(a_second_download_fetches_nothing_and_a_deleted_file_is_fetched_again_never_over_another_file) {
    Env env;
    env.add(pbtest::fixture("mail/with_attachments.eml"));
    env.sync();
    env.connect();
    const int64_t id = env.attachment_id("2");
    PB_CHECK(env.download(id).ok());
    PB_CHECK_EQ(count_fetches(*env.server), static_cast<size_t>(1));
    PB_CHECK(env.download(id).ok());
    PB_CHECK_EQ(count_fetches(*env.server), static_cast<size_t>(1));
    ::unlink(env.saved(id).c_str());                                           // the user deleted it
    PB_CHECK(env.download(id).ok());
    PB_CHECK_EQ(count_fetches(*env.server), static_cast<size_t>(2));
    PB_CHECK_EQ(read_file(env.saved(id)), kPdf);
}

PB_TEST(an_existing_file_is_never_overwritten_the_new_one_gets_a_number) {
    Env env;
    env.add(pbtest::fixture("mail/with_attachments.eml"));
    env.sync();
    env.connect();
    {
        std::ofstream(env.downloads() + "/report.pdf") << "the user's own file";
        std::ofstream(env.downloads() + "/report (1).pdf") << "another one";
    }
    const int64_t id = env.attachment_id("2");
    PB_CHECK(env.download(id).ok());
    PB_CHECK_EQ(env.saved(id), env.downloads() + "/report (2).pdf");
    PB_CHECK_EQ(read_file(env.downloads() + "/report.pdf"), std::string("the user's own file"));
    PB_CHECK_EQ(read_file(env.downloads() + "/report (1).pdf"), std::string("another one"));
    PB_CHECK_EQ(read_file(env.saved(id)), kPdf);
}

PB_TEST(file_names_from_the_network_are_made_safe) {
    Env env;
    const std::string raw =
        "From: a@example.org\r\nSubject: Names\r\nContent-Type: multipart/mixed; boundary=\"B\"\r\n\r\n--B\r\nContent-Type: text/plain\r\n\r\nText\r\n"
        "--B\r\nContent-Type: application/octet-stream; name=\"x\"\r\nContent-Disposition: attachment; filename=\"../../etc/evil.sh\"\r\nContent-Transfer-Encoding: 8bit\r\n\r\nA\r\n"
        "--B\r\nContent-Type: application/octet-stream\r\nContent-Disposition: attachment; filename=\"a:b*c?.txt\"\r\nContent-Transfer-Encoding: 8bit\r\n\r\nB\r\n"
        "--B\r\nContent-Type: application/pdf\r\nContent-Disposition: attachment\r\nContent-Transfer-Encoding: 8bit\r\n\r\nC\r\n"
        "--B\r\nContent-Type: image/png\r\nContent-Disposition: attachment\r\nContent-Transfer-Encoding: 8bit\r\n\r\nD\r\n"
        "--B\r\nContent-Type: application/x-weird\r\nContent-Disposition: attachment\r\nContent-Transfer-Encoding: 8bit\r\n\r\nE\r\n--B--\r\n";
    env.add(raw);
    env.sync();
    env.connect();
    for (const char* section : {"2", "3", "4", "5", "6"}) PB_CHECK(env.download(env.attachment_id(section)).ok());
    const auto names = env.files_in(env.downloads());
    PB_CHECK((names == std::vector<std::string>{"a_b_c_.txt", "attachment", "attachment.pdf", "attachment.png", "evil.sh"}));
    PB_CHECK(!exists(env.dir.path() + "/../../etc/evil.sh"));
}

PB_TEST(an_announced_size_over_the_limit_is_refused_without_fetching_anything) {
    Env env;
    env.add(pbtest::fixture("mail/with_attachments.eml"));
    env.sync();
    env.connect();
    const int64_t id = env.attachment_id("2");
    const auto e = env.download(id, 100);
    PB_CHECK_EQ(e.key, std::string("too_large"));
    PB_CHECK_EQ(count_fetches(*env.server), static_cast<size_t>(0));
    PB_CHECK(env.files_in(env.downloads()).empty());
    const auto ref = env.store->attachment(id);
    PB_CHECK_EQ(ref->attachment.state, std::string("remote"));
    PB_CHECK_EQ(ref->attachment.error_key, std::string("too_large"));
    PB_CHECK(env.download(id).ok());                                           // with the default limit the next try works and clears the error
    PB_CHECK_EQ(env.store->attachment(id)->attachment.error_key, std::string(""));
}

PB_TEST(a_part_that_turns_out_larger_than_announced_is_aborted_and_leaves_no_file) {
    Env env;
    env.add(pbtest::fixture("mail/with_attachments.eml"));
    env.sync();
    env.connect();
    const int64_t id = env.attachment_id("2");
    PB_CHECK(env.store->debug_exec("UPDATE attachment SET size = 10 WHERE id = " + std::to_string(id)).empty());   // the structure under-reported
    PB_CHECK_EQ(env.download(id, 100).key, std::string("too_large"));
    PB_CHECK(env.files_in(env.downloads()).empty());                           // not even a hidden part file
    PB_CHECK_EQ(env.store->attachment(id)->attachment.state, std::string("remote"));
    PB_CHECK_EQ(env.store->attachment(id)->attachment.error_key, std::string("too_large"));
}

PB_TEST(quoted_printable_and_unencoded_parts_are_decoded_or_kept_as_they_are) {
    Env env;
    const std::string raw =
        "From: a@example.org\r\nSubject: Files\r\nContent-Type: multipart/mixed; boundary=\"B\"\r\n\r\n--B\r\nContent-Type: text/plain\r\n\r\nText\r\n"
        "--B\r\nContent-Type: text/plain; name=\"qp.txt\"\r\nContent-Disposition: attachment; filename=\"qp.txt\"\r\nContent-Transfer-Encoding: quoted-printable\r\n\r\nGr=FC=DFe =\r\nund mehr=0D=0A\r\n"
        "--B\r\nContent-Type: application/octet-stream; name=\"raw.bin\"\r\nContent-Disposition: attachment; filename=\"raw.bin\"\r\nContent-Transfer-Encoding: 8bit\r\n\r\nplain bytes\r\n"
        "--B--\r\n";
    env.add(raw);
    env.sync();
    env.connect();
    const int64_t qp = env.attachment_id("2"), bin = env.attachment_id("3");
    PB_CHECK(env.download(qp, 1u << 20).ok());
    PB_CHECK(env.download(bin, 1u << 20).ok());
    PB_CHECK_EQ(read_file(env.downloads() + "/qp.txt"), std::string("Gr\xFC\xDF" "e und mehr\r\n"));
    PB_CHECK_EQ(read_file(env.downloads() + "/raw.bin"), std::string("plain bytes"));
}

PB_TEST(base64_split_over_many_network_chunks_decodes_correctly) {
    Env env;
    std::string payload;
    for (int i = 0; i < 300000; ++i) payload += static_cast<char>((i * 7 + (i >> 5)) & 0xFF);
    // base64 by hand: the test must not depend on the code under test
    static const char* kB64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string enc, line;
    for (size_t i = 0; i < payload.size(); i += 3) {
        const uint32_t v = (static_cast<uint8_t>(payload[i]) << 16) | (i + 1 < payload.size() ? static_cast<uint8_t>(payload[i + 1]) << 8 : 0) | (i + 2 < payload.size() ? static_cast<uint8_t>(payload[i + 2]) : 0);
        line += kB64[(v >> 18) & 63];
        line += kB64[(v >> 12) & 63];
        line += i + 1 < payload.size() ? kB64[(v >> 6) & 63] : '=';
        line += i + 2 < payload.size() ? kB64[v & 63] : '=';
        if (line.size() >= 76) { enc += line + "\r\n"; line.clear(); }
    }
    if (!line.empty()) enc += line + "\r\n";
    env.add("From: a@example.org\r\nSubject: Big\r\nContent-Type: multipart/mixed; boundary=\"B\"\r\n\r\n--B\r\nContent-Type: text/plain\r\n\r\nText\r\n--B\r\nContent-Type: application/octet-stream; name=\"d.bin\"\r\n"
            "Content-Disposition: attachment; filename=\"d.bin\"\r\nContent-Transfer-Encoding: base64\r\n\r\n" + enc + "--B--\r\n");
    env.sync();
    env.connect();
    const int64_t id = env.attachment_id("2");
    PB_CHECK(env.download(id).ok());
    PB_CHECK(read_file(env.downloads() + "/d.bin") == payload);
}

PB_TEST(a_dropped_connection_leaves_no_file_and_the_next_try_works) {
    Env env;
    env.add(pbtest::fixture("mail/with_attachments.eml"));
    env.sync();
    env.connect();
    const int64_t id = env.attachment_id("2");
    env.server->drop_after_bytes = static_cast<int>(env.server->connections() > 0 ? 400 : 400);
    const auto e = env.download(id);
    PB_CHECK(!e.ok());
    PB_CHECK(env.files_in(env.downloads()).empty());
    PB_CHECK_EQ(env.store->attachment(id)->attachment.state, std::string("remote"));
    PB_CHECK_EQ(env.store->attachment(id)->attachment.error_key, std::string(""));    // a network problem is not recorded as a refusal
    env.server->drop_after_bytes = -1;
    env.connect();
    PB_CHECK(env.download(id).ok());
    PB_CHECK_EQ(read_file(env.saved(id)), kPdf);
}

PB_TEST(a_part_the_server_no_longer_has_is_recorded_as_refused) {
    Env env;
    const uint32_t uid = env.add(pbtest::fixture("mail/with_attachments.eml"));
    env.sync();
    env.connect();
    const int64_t id = env.attachment_id("2");
    env.server->expunge("INBOX", uid);                                       // deleted on the server after the sync
    const auto e = env.download(id);
    PB_CHECK_EQ(e.key, std::string("not_found"));
    PB_CHECK_EQ(env.store->attachment(id)->attachment.error_key, std::string("refused"));
    PB_CHECK(env.files_in(env.downloads()).empty());
}

PB_TEST(an_unknown_attachment_id_is_not_found) {
    Env env;
    env.connect();
    PB_CHECK_EQ(env.download(4242).key, std::string("not_found"));
}

PB_TEST(a_missing_downloads_folder_is_created_and_an_unusable_one_is_an_error_that_changes_nothing) {
    Env env;
    env.add(pbtest::fixture("mail/with_attachments.eml"));
    env.sync();
    env.connect();
    const int64_t id = env.attachment_id("2");
    const std::string fresh = env.dir.path() + "/Downloads";
    PB_CHECK(pbo::download_attachment(*env.imap, *env.store, id, 10u << 20, fresh).ok());
    PB_CHECK_EQ(read_file(fresh + "/report.pdf"), kPdf);

    const int64_t id2 = env.attachment_id("3");
    const auto e = pbo::download_attachment(*env.imap, *env.store, id2, 10u << 20, env.dir.path() + "/no/such/parent/Downloads");
    PB_CHECK_EQ(e.key, std::string("write_failed"));
    PB_CHECK_EQ(env.store->attachment(id2)->attachment.state, std::string("remote"));
    PB_CHECK_EQ(env.store->attachment(id2)->attachment.error_key, std::string(""));
    if (::geteuid() != 0) {
        ::chmod(fresh.c_str(), 0500);                                        // read-only folder
        const auto e2 = pbo::download_attachment(*env.imap, *env.store, id2, 10u << 20, fresh);
        ::chmod(fresh.c_str(), 0700);
        PB_CHECK_EQ(e2.key, std::string("write_failed"));
    }
}

PB_TEST_MAIN()
