#include "pbo/image_service.h"

#include <algorithm>

#include "mime_internal.h"
#include "pbo/log.h"
#include "pbo/curl_session.h"

namespace pbo {

namespace {

// Collects the (still encoded) bytes of one part and stops the transfer at `max_bytes`.
class MemorySink : public ByteSink {
public:
    bool write(const char* data, size_t n) override {
        bytes_.append(data, n);
        return true;
    }
    std::string& bytes() { return bytes_; }

private:
    std::string bytes_;
};

bool starts_with(std::string_view s, std::string_view p) { return s.size() >= p.size() && s.compare(0, p.size(), p) == 0; }

}  // namespace

std::string sniff_image_type(std::string_view b) {
    if (starts_with(b, std::string_view("\x89PNG\r\n\x1a\n", 8))) return "image/png";
    if (b.size() >= 3 && static_cast<unsigned char>(b[0]) == 0xFF && static_cast<unsigned char>(b[1]) == 0xD8 && static_cast<unsigned char>(b[2]) == 0xFF) return "image/jpeg";
    if (starts_with(b, "GIF87a") || starts_with(b, "GIF89a")) return "image/gif";
    if (b.size() >= 12 && starts_with(b, "RIFF") && b.compare(8, 4, "WEBP") == 0) return "image/webp";
    if (b.size() >= 14 && starts_with(b, "BM")) return "image/bmp";
    return std::string();
}

Error load_cid_images(ImapTransport& imap, Store& store, int64_t message_id, const ImageLimits& limits, ImageLoadReport* report) {
    ImageLoadReport local;
    ImageLoadReport& rep = report ? *report : local;
    const auto where = store.message_location(message_id);
    if (!where) return make_error(ErrorClass::Storage, "not_found", "no such message");
    uint64_t total = 0;
    int count = 0;
    for (const StoredImage& im : store.message_images(message_id)) {
        if (im.kind != "cid" || im.section.empty() || (im.state != "pending" && im.state != "failed")) continue;
        if (++count > limits.max_count || (im.size > 0 && static_cast<uint64_t>(im.size) > limits.max_each) || total + static_cast<uint64_t>(std::max<int64_t>(im.size, 0)) > limits.max_total) {
            ++rep.skipped;   // stays pending: the user's tap uses larger limits
            continue;
        }
        MemorySink sink;
        const uint64_t max_encoded = limits.max_each + limits.max_each / 2 + 4096;
        Error e = imap.fetch_to(where->folder_raw_name, where->uid, im.section, max_encoded, sink);
        if (!e.ok()) {
            if (e.key == "too_large") {
                store.set_image_state(message_id, im.idx, "too_large");
                ++rep.too_large;
                continue;
            }
            if (e.cls == ErrorClass::Network || e.cls == ErrorClass::Tls || e.cls == ErrorClass::Auth || e.cls == ErrorClass::Cancelled) return e;   // the connection is the problem
            store.set_image_state(message_id, im.idx, "failed");
            ++rep.failed;
            continue;
        }
        const std::string cte = im.encoding.empty() ? "7bit" : im.encoding;
        std::string decoded = mime_internal::decode_transfer(sink.bytes(), cte, limits.max_each + 1);
        if (decoded.size() > limits.max_each) {
            store.set_image_state(message_id, im.idx, "too_large");
            ++rep.too_large;
            continue;
        }
        const std::string type = sniff_image_type(decoded);
        if (type.empty() || !store.set_image_loaded(message_id, im.idx, type, decoded).ok()) {
            store.set_image_state(message_id, im.idx, "failed");
            ++rep.failed;
            continue;
        }
        total += decoded.size();
        ++rep.loaded;
    }
    if (rep.loaded > 0 || rep.failed > 0) log::event("image.loaded", {log::count("loaded", rep.loaded), log::count("failed", rep.failed), log::count("remote", 0)});
    return Error();
}

Error load_remote_images(HttpTransport& http, Store& store, int64_t message_id, const ImageLimits& limits, Cancel& cancel, ImageLoadReport* report) {
    ImageLoadReport local;
    ImageLoadReport& rep = report ? *report : local;
    if (!store.message_location(message_id)) return make_error(ErrorClass::Storage, "not_found", "no such message");
    uint64_t total = 0;
    int count = 0;
    bool out_of_room = false;   // the total of this call is used up: the rest stays pending, nothing more is asked for
    for (const StoredImage& im : store.message_images(message_id)) {
        if (im.kind != "remote" || (im.state != "pending" && im.state != "failed")) continue;
        if (cancel.requested() || out_of_room || ++count > limits.max_count || total >= limits.max_total) {
            ++rep.skipped;
            continue;
        }
        if (im.src.rfind("https://", 0) != 0) {   // only TLS; the converter lists nothing else, and this is the second line of defence
            store.set_image_state(message_id, im.idx, "failed");
            ++rep.failed;
            continue;
        }
        const uint64_t allowed = std::min<uint64_t>(limits.max_each, limits.max_total - total);
        Request req;
        req.method = "GET";
        req.url = im.src;
        req.headers = {{"Accept", "image/png,image/jpeg,image/gif,image/webp,image/bmp;q=0.9"}};   // no credentials, no cookie, no referrer
        req.max_response_bytes = static_cast<long long>(allowed) + 1;
        req.connect_timeout_s = 8;
        req.total_timeout_s = 25;
        const Response r = http.perform(req);
        if (r.body_too_large || (r.status >= 200 && r.status < 300 && r.body.size() > allowed)) {
            if (allowed < limits.max_each) {   // only the total of this call is short: not the picture's fault, it stays pending for the next tap
                ++rep.skipped;
                out_of_room = true;
            } else {
                store.set_image_state(message_id, im.idx, "too_large");
                ++rep.too_large;
            }
            continue;
        }
        if (r.status < 200 || r.status >= 300 || r.error != ErrorClass::None) {
            store.set_image_state(message_id, im.idx, "failed");
            ++rep.failed;
            continue;
        }
        const std::string type = sniff_image_type(r.body);   // what the first bytes say, not what the server claims
        if (type.empty() || !store.set_image_loaded(message_id, im.idx, type, r.body).ok()) {
            store.set_image_state(message_id, im.idx, "failed");
            ++rep.failed;
            continue;
        }
        total += r.body.size();
        ++rep.loaded;
    }
    log::event("image.loaded", {log::count("loaded", rep.loaded), log::count("failed", rep.failed + rep.too_large), log::count("remote", 1)});
    return Error();
}

}  // namespace pbo
