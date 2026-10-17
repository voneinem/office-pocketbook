#include "pbo/credentials_file.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "pbo/log.h"
#include "secure_wipe.h"

namespace pbo {

namespace {

constexpr const char* kVersionLine = "pbo-credentials 1";
constexpr size_t kMaxFile = 64 * 1024;

std::string escape(const std::string& v) {
    std::string r;
    for (char c : v) {
        if (c == '\\') r += "\\\\";
        else if (c == '\n') r += "\\n";
        else if (c == '\r') r += "\\r";
        else r += c;
    }
    return r;
}

bool unescape(const std::string& v, std::string* out) {
    out->clear();
    for (size_t i = 0; i < v.size(); ++i) {
        if (v[i] != '\\') {
            *out += v[i];
            continue;
        }
        if (++i >= v.size()) return false;
        if (v[i] == '\\') *out += '\\';
        else if (v[i] == 'n') *out += '\n';
        else if (v[i] == 'r') *out += '\r';
        else return false;
    }
    return true;
}

bool write_all(int fd, const std::string& data) {
    size_t off = 0;
    while (off < data.size()) {
        const ssize_t n = ::write(fd, data.data() + off, data.size() - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        off += static_cast<size_t>(n);
    }
    return true;
}

void ensure_dir(const std::string& dir) {
    ::mkdir(dir.c_str(), 0700);   // an existing directory keeps its mode
}

}  // namespace

std::string credentials_path(const std::string& dir) { return dir + "/credentials"; }

ProtectionStatus credentials_protection(const std::string& dir) {
    struct stat st {};
    if (::stat(credentials_path(dir).c_str(), &st) != 0) return ProtectionStatus::Unknown;
    return (st.st_mode & 077) == 0 ? ProtectionStatus::Enforced : ProtectionStatus::NotEnforced;
}

Error save_credentials(const std::string& dir, const Credentials& c, ProtectionStatus* status) {
    ensure_dir(dir);
    std::string text = std::string(kVersionLine) + "\n";
    text += "mail_password=" + escape(c.mail_password) + "\n";
    text += "smtp_password=" + escape(c.smtp_password) + "\n";
    text += "calendar_password=" + escape(c.calendar_password) + "\n";

    const std::string final_path = credentials_path(dir);
    const std::string temp_path = final_path + ".tmp." + std::to_string(static_cast<long>(::getpid()));
    // Created with 0600 from the start (K6): the content is never readable by others, even for a moment.
    const int fd = ::open(temp_path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) {
        log::event("credentials.write_failed", {log::count("errno", errno)});
        return make_error(errno == ENOSPC ? ErrorClass::Storage : ErrorClass::Storage, errno == ENOSPC ? "storage_full" : "write_failed",
                          "the credentials file could not be created");
    }
    ::fchmod(fd, 0600);   // does nothing on file systems without permissions
    bool ok = write_all(fd, text) && ::fsync(fd) == 0;
    const int saved_errno = errno;
    ::close(fd);
    secure_wipe(text);
    if (!ok || ::rename(temp_path.c_str(), final_path.c_str()) != 0) {
        const int e = ok ? errno : saved_errno;
        ::unlink(temp_path.c_str());
        log::event("credentials.write_failed", {log::count("errno", e)});
        return make_error(ErrorClass::Storage, e == ENOSPC ? "storage_full" : "write_failed", "the credentials file could not be written");
    }
    if (status) *status = credentials_protection(dir);
    log::event("credentials.saved", {});
    return {};
}

Result<Credentials> load_credentials(const std::string& dir) {
    Result<Credentials> out;
    const std::string path = credentials_path(dir);
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (errno == ENOENT) return out;   // no file: all passwords are empty
        out.error = make_error(ErrorClass::Storage, "credentials_unreadable", "the credentials file cannot be opened");
        return out;
    }
    std::string text;
    char buf[4096];
    for (;;) {
        const ssize_t n = ::read(fd, buf, sizeof buf);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        text.append(buf, static_cast<size_t>(n));
        if (text.size() > kMaxFile) break;
    }
    ::close(fd);
    auto fail = [&] {
        secure_wipe(text);
        out.value = Credentials{};
        out.error = make_error(ErrorClass::Storage, "credentials_unreadable", "the credentials file is damaged or from a newer version");
        log::event("credentials.unreadable", {});
        return out;
    };
    if (text.size() > kMaxFile) return fail();
    size_t pos = text.find('\n');
    if (pos == std::string::npos || text.compare(0, pos, kVersionLine) != 0) return fail();
    ++pos;
    while (pos < text.size()) {
        size_t eol = text.find('\n', pos);
        if (eol == std::string::npos) return fail();
        const std::string line = text.substr(pos, eol - pos);
        pos = eol + 1;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) return fail();
        const std::string key = line.substr(0, eq);
        std::string value;
        if (!unescape(line.substr(eq + 1), &value)) return fail();
        if (key == "mail_password") out.value.mail_password = value;
        else if (key == "smtp_password") out.value.smtp_password = value;
        else if (key == "calendar_password") out.value.calendar_password = value;
        // unknown keys are ignored (written by a newer minor version)
        secure_wipe(value);
    }
    secure_wipe(text);
    return out;
}

Error forget_credentials(const std::string& dir) {
    const std::string path = credentials_path(dir);
    struct stat st {};
    if (::stat(path.c_str(), &st) != 0) return {};   // nothing to forget
    const int fd = ::open(path.c_str(), O_WRONLY | O_CLOEXEC);
    if (fd >= 0) {
        const std::string zeros(static_cast<size_t>(st.st_size > 0 ? st.st_size : 1), '\0');
        write_all(fd, zeros);
        ::fsync(fd);
        ::close(fd);
    }
    if (::unlink(path.c_str()) != 0 && errno != ENOENT) return make_error(ErrorClass::Storage, "write_failed", "the credentials file could not be removed");
    log::event("credentials.forgotten", {});
    return {};
}

}  // namespace pbo
