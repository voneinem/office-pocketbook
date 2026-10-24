#include "pbo/handoff_inbox.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>

namespace pbo {

namespace {

bool safe_id(const std::string& id) {
    return !id.empty() && id != "." && id != ".." && id.find('/') == std::string::npos && id.find('\0') == std::string::npos;
}

bool ends_with_tmp(const std::string& s) { return s.size() > 4 && s.compare(s.size() - 4, 4, ".tmp") == 0; }

std::vector<std::string> list_names(const std::string& dir) {
    std::vector<std::string> names;
    DIR* d = ::opendir(dir.c_str());
    if (!d) return names;
    while (dirent* e = ::readdir(d)) {
        const std::string n = e->d_name;
        if (n != "." && n != "..") names.push_back(n);
    }
    ::closedir(d);
    return names;
}

// Removes a folder with everything in it; symbolic links are removed, never followed.
bool remove_tree(const std::string& path, int depth = 0) {
    struct stat st;
    if (::lstat(path.c_str(), &st) != 0) return false;
    if (!S_ISDIR(st.st_mode) || depth > 4) return ::unlink(path.c_str()) == 0;
    for (const std::string& n : list_names(path)) remove_tree(path + "/" + n, depth + 1);
    return ::rmdir(path.c_str()) == 0;
}

bool numeric_name(const std::string& n) {
    if (n.empty() || n.size() > 6) return false;
    for (char c : n)
        if (c < '0' || c > '9') return false;
    return true;
}

// Reads one file as raw bytes; false when it is not a plain readable file within the size limit.
bool read_plain_file(const std::string& path, std::string* out) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return false;
    struct stat st;
    bool ok = ::fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && static_cast<size_t>(st.st_size) <= HandoffInbox::kMaxFileBytes;
    out->clear();
    char buf[4096];
    while (ok) {
        const ssize_t n = ::read(fd, buf, sizeof buf);
        if (n < 0) {
            if (errno == EINTR) continue;
            ok = false;
        } else if (n == 0) {
            break;
        } else {
            out->append(buf, static_cast<size_t>(n));
            if (out->size() > HandoffInbox::kMaxFileBytes) ok = false;   // it grew while being read
        }
    }
    ::close(fd);
    return ok;
}

PendingHandoff read_entry(const std::string& dir, const std::string& id) {
    PendingHandoff p;
    p.id = id;
    const std::string folder = dir + "/" + id;
    std::vector<std::string> names = list_names(folder);
    if (names.size() > static_cast<size_t>(HandoffInbox::kMaxFiles)) {
        p.refused = true;
        return p;
    }
    for (const std::string& n : names)
        if (!numeric_name(n)) {
            p.refused = true;   // a folder, a link or a name the launcher never writes
            return p;
        }
    std::sort(names.begin(), names.end(), [](const std::string& a, const std::string& b) { return std::stol(a) < std::stol(b); });
    for (const std::string& n : names) {
        struct stat st;
        std::string bytes;
        if (::lstat((folder + "/" + n).c_str(), &st) != 0 || !S_ISREG(st.st_mode) || !read_plain_file(folder + "/" + n, &bytes)) {
            p.args.clear();
            p.refused = true;
            return p;
        }
        p.args.push_back(std::move(bytes));
    }
    return p;
}

}  // namespace

HandoffInbox::HandoffInbox(std::string dir, Clock& clock) : dir_(std::move(dir)), clock_(clock) {}

std::vector<PendingHandoff> HandoffInbox::take_ready(int* expired) {
    if (expired) *expired = 0;
    std::vector<PendingHandoff> ready;
    std::vector<std::string> names = list_names(dir_);
    std::sort(names.begin(), names.end());
    const int64_t now = clock_.now();
    for (const std::string& n : names) {
        struct stat st;
        if (::lstat((dir_ + "/" + n).c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) continue;   // a file that is not an entry is not touched
        if (now - static_cast<int64_t>(st.st_mtime) > kExpirySeconds) {
            remove_tree(dir_ + "/" + n);
            if (expired) ++*expired;
            continue;
        }
        if (ends_with_tmp(n)) continue;   // the launcher is still writing it
        ready.push_back(read_entry(dir_, n));
    }
    return ready;
}

bool HandoffInbox::remove(const std::string& id) {
    if (!safe_id(id)) return false;
    struct stat st;
    const std::string path = dir_ + "/" + id;
    if (::lstat(path.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) return false;
    return remove_tree(path);
}

}  // namespace pbo
