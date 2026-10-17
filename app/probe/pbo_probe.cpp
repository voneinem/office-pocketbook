// pbo-probe: prints the facts about the device runtime that design decisions wait for (research R-02,
// open items O-1, O-6, O-7, O-8). Plain "key: value" lines, no personal data: no host name, user name, home
// directory, account or network address. Every answer is labelled with what was observed, not guessed.
//
//   pbo-probe [data-dir]       data-dir defaults to /mnt/ext1/system/pboffice
//   pbo-probe --imap-selftest <host> <port> <ca.pem> [--bad-login] [--append <mailbox>]   (imap_selftest.h)
#include <curl/curl.h>
#include <dirent.h>
#include <iconv.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

#include "imap_selftest.h"

namespace {

void section(const char* title) { std::printf("\n== %s\n", title); }

bool read_file(const std::string& path, std::string& out, size_t max = 1 << 20) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::stringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    if (out.size() > max) out.resize(max);
    return true;
}

bool has_protocol(const curl_version_info_data* info, const char* name) {
    if (!info->protocols) return false;
    for (const char* const* p = info->protocols; *p; ++p)
        if (std::strcmp(*p, name) == 0) return true;
    return false;
}

void probe_libcurl() {
    section("libcurl");
    const curl_version_info_data* info = curl_version_info(CURLVERSION_NOW);
    std::printf("version: %s\n", info->version ? info->version : "unknown");
    std::printf("ssl: %s\n", info->ssl_version ? info->ssl_version : "none");
    std::string protocols;
    for (const char* const* p = info->protocols; p && *p; ++p) protocols += std::string(protocols.empty() ? "" : " ") + *p;
    std::printf("protocols: %s\n", protocols.c_str());
    for (const char* name : {"imaps", "smtps", "imap", "smtp", "https"})
        std::printf("%s: %s\n", name, has_protocol(info, name) ? "yes" : "no");
    struct Feature { int bit; const char* name; };
    const Feature features[] = {{CURL_VERSION_SSL, "SSL"}, {CURL_VERSION_LIBZ, "libz"}, {CURL_VERSION_IPV6, "IPv6"},
                                {CURL_VERSION_NTLM, "NTLM"}, {CURL_VERSION_HTTP2, "HTTP2"}, {CURL_VERSION_THREADSAFE, "threadsafe"}};
    std::string feats;
    for (const Feature& f : features)
        if (info->features & f.bit) feats += std::string(feats.empty() ? "" : " ") + f.name;
    std::printf("features: %s\n", feats.c_str());
}

// Charsets mail commonly uses; iconv_open succeeds only if the C library has the conversion module.
void probe_charsets() {
    section("charsets (iconv to UTF-8)");
    for (const char* cs : {"ISO-8859-1", "ISO-8859-2", "ISO-8859-15", "WINDOWS-1250", "WINDOWS-1251", "WINDOWS-1252",
                           "KOI8-R", "ISO-2022-JP", "SHIFT_JIS", "EUC-JP", "GB2312", "GBK", "BIG5", "EUC-KR", "UTF-7", "UTF-16LE"}) {
        iconv_t cd = iconv_open("UTF-8", cs);
        const bool ok = cd != reinterpret_cast<iconv_t>(-1);
        std::printf("%s: %s\n", cs, ok ? "yes" : "no");
        if (ok) iconv_close(cd);
    }
}

size_t count_certificates(const std::string& text) {
    size_t n = 0;
    for (size_t pos = text.find("BEGIN CERTIFICATE"); pos != std::string::npos; pos = text.find("BEGIN CERTIFICATE", pos + 1)) ++n;
    return n;
}

void probe_ca_files() {
    section("CA files");
    for (const char* b : {"/etc/ssl/certs/ca-certificates.crt", "/etc/ssl/cert.pem", "/etc/ssl/certs/ca-bundle.crt"}) {
        std::string text;
        if (!read_file(b, text, 8 << 20)) {
            std::printf("%s: not present\n", b);
            continue;
        }
        std::printf("%s: %zu bytes, %zu certificates\n", b, text.size(), count_certificates(text));
    }
    size_t files = 0, usable = 0;
    if (DIR* d = opendir("/etc/ssl/certs")) {
        while (dirent* e = readdir(d)) {
            const std::string name = e->d_name;
            if (name.size() < 5) continue;
            const std::string ext = name.substr(name.size() - 4);
            if (ext != ".pem" && ext != ".crt") continue;
            ++files;
            std::string text;
            if (read_file("/etc/ssl/certs/" + name, text, 1 << 16) && count_certificates(text) > 0) ++usable;
        }
        closedir(d);
    }
    std::printf("/etc/ssl/certs: %zu pem/crt files, %zu with a certificate\n", files, usable);
}

const char* fs_name(unsigned long type) {
    switch (type) {
        case 0x4d44: return "vfat";
        case 0x2011BAB0: return "exfat";
        case 0xEF53: return "ext2/3/4";
        case 0x65735546: return "fuse";
        case 0x01021994: return "tmpfs";
        case 0x58465342: return "xfs";
        case 0x9123683E: return "btrfs";
        case 0x5346544e: return "ntfs";
        case 0xF2F52010: return "f2fs";
        default: return "other";
    }
}

void probe_data_dir(const std::string& dir) {
    section("data directory");
    std::printf("path: %s\n", dir.c_str());
    struct stat st {};
    if (stat(dir.c_str(), &st) != 0 && mkdir(dir.c_str(), 0700) != 0) {
        std::printf("filesystem: unknown (directory missing and cannot be created)\n");
        std::printf("chmod 0600: not tested\nfree: unknown\n");
        return;
    }
    struct statfs sf {};
    if (statfs(dir.c_str(), &sf) == 0)
        std::printf("filesystem: %s (0x%lx)\n", fs_name(static_cast<unsigned long>(sf.f_type)), static_cast<unsigned long>(sf.f_type));
    else
        std::printf("filesystem: unknown\n");
    // chmod 0600 is "observable" when the mode read back is the mode set (FAT does not keep it)
    const std::string file = dir + "/.pbo-probe." + std::to_string(static_cast<long>(getpid()));
    bool written = false;
    if (FILE* f = std::fopen(file.c_str(), "w")) {
        std::fputs("x", f);
        std::fclose(f);
        written = true;
    }
    if (!written) {
        std::printf("chmod 0600: not tested (directory not writable)\n");
    } else {
        struct stat fst {};
        if (chmod(file.c_str(), 0600) == 0 && stat(file.c_str(), &fst) == 0)
            std::printf("chmod 0600: %s (mode read back: %04o)\n", (fst.st_mode & 0777) == 0600 ? "observable" : "NOT observable",
                        static_cast<unsigned>(fst.st_mode & 0777));
        else
            std::printf("chmod 0600: failed\n");
        std::remove(file.c_str());
    }
    struct statvfs sv {};
    if (statvfs(dir.c_str(), &sv) == 0)
        std::printf("free: %llu MiB\n", static_cast<unsigned long long>(sv.f_bavail) * sv.f_frsize / (1024 * 1024));
    else
        std::printf("free: unknown\n");
}

void probe_system() {
    section("system");
    const char* tz = std::getenv("TZ");
    std::printf("TZ: %s\n", tz && *tz ? tz : "(unset)");
    char target[512];
    const ssize_t n = readlink("/etc/localtime", target, sizeof target - 1);
    if (n > 0) {
        target[n] = 0;
        std::printf("localtime: -> %s\n", target);
    } else {
        struct stat st {};
        std::printf("localtime: %s\n", stat("/etc/localtime", &st) == 0 ? "regular file" : "missing");
    }
    std::string meminfo;
    long kb = -1;
    if (read_file("/proc/meminfo", meminfo)) {
        const size_t p = meminfo.find("MemTotal:");
        if (p != std::string::npos) kb = std::atol(meminfo.c_str() + p + 9);
    }
    if (kb >= 0) std::printf("MemTotal: %ld kB\n", kb);
    else std::printf("MemTotal: unknown\n");
    struct stat st {};
    std::printf("gconv: %s\n", stat("/usr/lib/gconv", &st) == 0 ? "/usr/lib/gconv present" : "/usr/lib/gconv absent");
    // the frame buffer size is the nearest answer to the screen size that needs no display library
    std::string fb;
    if (read_file("/sys/class/graphics/fb0/virtual_size", fb)) {
        while (!fb.empty() && (fb.back() == '\n' || fb.back() == '\r')) fb.pop_back();
        std::printf("screen: frame buffer %s (width,height in pixels)\n", fb.c_str());
    } else {
        std::printf("screen: unknown (no frame buffer information)\n");
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "--imap-selftest") == 0) {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        const int rc = run_imap_selftest(argc - 2, argv + 2);
        curl_global_cleanup();
        return rc;
    }
    const std::string data_dir = argc > 1 ? argv[1] : "/mnt/ext1/system/pboffice";
    curl_global_init(CURL_GLOBAL_DEFAULT);
    std::printf("pbo-probe (prints no personal data)\n");
    probe_libcurl();
    probe_charsets();
    probe_ca_files();
    probe_data_dir(data_dir);
    probe_system();
    curl_global_cleanup();
    return 0;
}
