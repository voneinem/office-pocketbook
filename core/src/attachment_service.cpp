#include "pbo/attachment_service.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cctype>
#include <cerrno>

#include "pbo/log.h"
#include "pbo/mime.h"

namespace pbo {

namespace {

int b64v(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

int hexv(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Decodes the transfer encoding while the data arrives and writes the result to a file, counting decoded bytes against the limit.
class DecodingFileSink : public ByteSink {
public:
    enum class Mode { Identity, Base64, QuotedPrintable };

    DecodingFileSink(int fd, Mode mode, uint64_t limit) : fd_(fd), mode_(mode), limit_(limit) {}

    bool write(const char* data, size_t n) override {
        std::string out;
        out.reserve(n);
        switch (mode_) {
            case Mode::Identity: out.assign(data, n); break;
            case Mode::Base64: base64(data, n, &out); break;
            case Mode::QuotedPrintable: qp(data, n, &out); break;
        }
        return emit(out);
    }

    // Flushes what a split encoded sequence still held; false if the file could not be written.
    bool finish() {
        std::string out(pending_);
        pending_.clear();
        return emit(out);
    }

    bool too_large() const { return too_large_; }
    int write_errno() const { return errno_; }

private:
    bool emit(const std::string& out) {
        if (decoded_ + out.size() > limit_) {   // the announced size was wrong, or the part is simply too big
            too_large_ = true;
            return false;
        }
        size_t off = 0;
        while (off < out.size()) {
            const ssize_t w = ::write(fd_, out.data() + off, out.size() - off);
            if (w < 0) {
                if (errno == EINTR) continue;
                errno_ = errno;
                return false;
            }
            off += static_cast<size_t>(w);
        }
        decoded_ += out.size();
        return true;
    }

    void base64(const char* data, size_t n, std::string* out) {
        for (size_t i = 0; i < n; ++i) {
            const int v = b64v(data[i]);
            if (v < 0) continue;   // line breaks, padding and anything else outside the alphabet
            acc_ = (acc_ << 6) | static_cast<uint32_t>(v);
            bits_ += 6;
            if (bits_ >= 8) {
                bits_ -= 8;
                *out += static_cast<char>((acc_ >> bits_) & 0xFF);
            }
        }
    }

    void qp(const char* data, size_t n, std::string* out) {
        std::string in = pending_ + std::string(data, n);
        pending_.clear();
        for (size_t i = 0; i < in.size(); ++i) {
            if (in[i] != '=') { *out += in[i]; continue; }
            // an escape needs two more characters (or a line break); if they have not arrived yet, wait for them
            if (i + 1 >= in.size()) { pending_ = in.substr(i); return; }
            if (in[i + 1] == '\n') { ++i; continue; }
            if (in[i + 1] == '\r') {
                if (i + 2 >= in.size()) { pending_ = in.substr(i); return; }
                i += in[i + 2] == '\n' ? 2 : 1;
                continue;
            }
            if (i + 2 >= in.size()) { pending_ = in.substr(i); return; }
            const int h = hexv(in[i + 1]), l = hexv(in[i + 2]);
            if (h >= 0 && l >= 0) { *out += static_cast<char>(h * 16 + l); i += 2; }
            else *out += '=';
        }
    }

    int fd_;
    Mode mode_;
    uint64_t limit_, decoded_ = 0;
    uint32_t acc_ = 0;
    int bits_ = 0;
    std::string pending_;
    bool too_large_ = false;
    int errno_ = 0;
};

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool file_exists(const std::string& path) {
    struct stat st;
    return ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

}  // namespace

std::string attachment_save_name(const std::string& name, const std::string& mime_type) {
    if (!name.empty()) return safe_file_name(name);
    static const struct { const char* type; const char* ext; } kExt[] = {
        {"application/pdf", ".pdf"}, {"image/png", ".png"}, {"image/jpeg", ".jpg"}, {"image/gif", ".gif"}, {"text/plain", ".txt"},
        {"text/html", ".html"}, {"text/calendar", ".ics"}, {"application/zip", ".zip"}, {"message/rfc822", ".eml"}, {"application/msword", ".doc"},
        {"application/vnd.openxmlformats-officedocument.wordprocessingml.document", ".docx"},
        {"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet", ".xlsx"},
        {"application/vnd.openxmlformats-officedocument.presentationml.presentation", ".pptx"}, {"audio/mpeg", ".mp3"}, {"video/mp4", ".mp4"}};
    const std::string type = lower(mime_type);
    for (const auto& e : kExt)
        if (type == e.type) return std::string("attachment") + e.ext;
    return "attachment";
}

Error download_attachment(ImapTransport& imap, Store& store, int64_t attachment_id, uint64_t limit_bytes, const std::string& downloads_dir) {
    const auto ref = store.attachment(attachment_id);
    if (!ref) return make_error(ErrorClass::Storage, "not_found", "no such attachment");
    const StoredAttachment& a = ref->attachment;
    if (a.state == "downloaded" && !a.saved_path.empty() && file_exists(a.saved_path)) return {};

    if (a.size > 0 && static_cast<uint64_t>(a.size) > limit_bytes) {   // announced as too big: nothing is fetched
        store.set_attachment_state(attachment_id, "remote", "too_large");
        return make_error(ErrorClass::Server, "too_large", "the attachment is larger than the allowed size");
    }

    const std::string enc = lower(a.encoding);
    DecodingFileSink::Mode mode = DecodingFileSink::Mode::Identity;
    uint64_t max_encoded = limit_bytes + 4096;
    if (enc == "base64") { mode = DecodingFileSink::Mode::Base64; max_encoded = limit_bytes + limit_bytes / 2 + 4096; }
    else if (enc == "quoted-printable") { mode = DecodingFileSink::Mode::QuotedPrintable; max_encoded = limit_bytes * 3 + 4096; }

    // the folder (one level is created, a missing parent is an error: the path is the owner's, not guessed)
    if (::mkdir(downloads_dir.c_str(), 0755) != 0 && errno != EEXIST) {
        log::event("attachment.write_failed", {log::count("errno", errno)});
        return make_error(ErrorClass::Storage, "write_failed", "the Downloads folder is not usable");
    }
    // a hidden part file: a half-written file never shows up under its real name, also if the app is stopped in the middle
    const std::string part = downloads_dir + "/.pbo-" + std::to_string(attachment_id) + ".part";
    const int fd = ::open(part.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        log::event("attachment.write_failed", {log::count("errno", errno)});
        return make_error(ErrorClass::Storage, errno == ENOSPC ? "storage_full" : "write_failed", "the file could not be created");
    }
    DecodingFileSink sink(fd, mode, limit_bytes);
    Error e = imap.fetch_to(ref->folder_raw_name, ref->uid, a.section, max_encoded, sink);
    if (e.ok() && !sink.finish()) e = make_error(ErrorClass::Storage, "write_failed", "the file could not be written");
    if (e.ok() && ::fsync(fd) != 0) e = make_error(ErrorClass::Storage, "write_failed", "the file could not be written");
    ::close(fd);

    if (sink.too_large()) e = make_error(ErrorClass::Server, "too_large", "the attachment is larger than the allowed size");
    else if (!e.ok() && sink.write_errno() != 0) e = make_error(ErrorClass::Storage, sink.write_errno() == ENOSPC ? "storage_full" : "write_failed", "the file could not be written");

    std::string final_path;
    if (e.ok()) {
        // the final name: the real one, or "name (n).ext"; an existing file is never replaced (O_EXCL claims the name)
        const std::string name = attachment_save_name(a.name, a.mime_type);
        const size_t dot = name.rfind('.');
        const bool has_ext = dot != std::string::npos && dot > 0 && name.size() - dot <= 12;
        const std::string stem = has_ext ? name.substr(0, dot) : name, ext = has_ext ? name.substr(dot) : std::string();
        for (int n = 0; n < 1000 && final_path.empty(); ++n) {
            const std::string candidate = downloads_dir + "/" + (n == 0 ? name : stem + " (" + std::to_string(n) + ")" + ext);
            const int claim = ::open(candidate.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
            if (claim >= 0) {
                ::close(claim);
                if (::rename(part.c_str(), candidate.c_str()) == 0) final_path = candidate;
                else ::unlink(candidate.c_str());
                break;
            }
            if (errno != EEXIST) break;
        }
        if (final_path.empty()) e = make_error(ErrorClass::Storage, "write_failed", "the file could not be saved");
    }
    if (!e.ok()) {
        ::unlink(part.c_str());   // no partial file stays behind
        if (e.key == "too_large") store.set_attachment_state(attachment_id, "remote", "too_large");
        else if (e.key == "not_found") store.set_attachment_state(attachment_id, "remote", "refused");   // the server no longer has it
        log_failure("attachment.failed", e);
        return e;
    }
    Error st = store.set_attachment_state(attachment_id, "downloaded", "", final_path);
    if (!st.ok()) ::unlink(final_path.c_str());   // the row could not record it: do not leave a file nobody knows about
    return st;
}

}  // namespace pbo
