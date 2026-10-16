// T190: pictures of HTML mail. Pictures that are parts of the message arrive with the body (small ones) or on request (larger ones); pictures on the
// sender's server are loaded ONLY when the user asks, by plain GETs over https without credentials; what is not a picture is not kept. UNVERIFIED against
// real servers (fake servers).
#include "fake_transport.h"
#include "mail_env.h"
#include "pbo/image_service.h"

PBO_FAKE_SERVER_TEST();

using pbtest::MailEnv;

namespace {

// a 4 x 3 PNG (73 bytes)
const char* kPngBase64 = "iVBORw0KGgoAAAANSUhEUgAAAAQAAAADCAIAAAA7ljmRAAAAEElEQVR4nGM4IScHRww4OQD1xwwxB+utmQAAAABJRU5ErkJggg==";
const std::string kPng = std::string("\x89PNG\r\n\x1a\n", 8) + "rest-of-a-picture";

std::string newsletter(const std::string& html_body, const std::string& related_parts, int n = 1) {
    return "From: Shop <shop@example.org>\r\nTo: alice@example.org\r\nSubject: Offer " + std::to_string(n) + "\r\nDate: Tue, 13 Oct 2026 10:00:00 +0000\r\nMessage-ID: <n" +
           std::to_string(n) + "@example.org>\r\nMIME-Version: 1.0\r\nContent-Type: multipart/alternative; boundary=\"alt\"\r\n\r\n"
           "--alt\r\nContent-Type: text/plain; charset=utf-8\r\n\r\nPlain version.\r\n"
           "--alt\r\nContent-Type: multipart/related; boundary=\"rel\"\r\n\r\n"
           "--rel\r\nContent-Type: text/html; charset=utf-8\r\n\r\n" + html_body + "\r\n" + related_parts + "--rel--\r\n--alt--\r\n";
}

std::string png_part(const std::string& cid, const std::string& base64) {
    return "--rel\r\nContent-Type: image/png; name=\"" + cid + ".png\"\r\nContent-Transfer-Encoding: base64\r\nContent-ID: <" + cid + ">\r\nContent-Disposition: inline\r\n\r\n" + base64 + "\r\n";
}

std::string base64_of(const std::string& bytes) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    size_t i = 0;
    for (; i + 2 < bytes.size(); i += 3) {
        const uint32_t v = (static_cast<uint8_t>(bytes[i]) << 16) | (static_cast<uint8_t>(bytes[i + 1]) << 8) | static_cast<uint8_t>(bytes[i + 2]);
        out += t[v >> 18]; out += t[(v >> 12) & 63]; out += t[(v >> 6) & 63]; out += t[v & 63];
    }
    if (i + 1 == bytes.size()) { const uint32_t v = static_cast<uint8_t>(bytes[i]) << 16; out += t[v >> 18]; out += t[(v >> 12) & 63]; out += "=="; }
    else if (i + 2 == bytes.size()) { const uint32_t v = (static_cast<uint8_t>(bytes[i]) << 16) | (static_cast<uint8_t>(bytes[i + 1]) << 8); out += t[v >> 18]; out += t[(v >> 12) & 63]; out += t[(v >> 6) & 63]; out += '='; }
    std::string wrapped;
    for (size_t k = 0; k < out.size(); k += 76) wrapped += out.substr(k, 76) + "\r\n";
    return wrapped;
}

// a marketing mail has a long HTML; a short one with only remote pictures keeps its plain-text alternative (the extra HTML is fetched for long ones only)
std::string padded(const std::string& html) { return "<!-- " + std::string(9000, 'x') + " -->" + html; }

int64_t sync_one(MailEnv& e, const std::string& raw) {
    const uint32_t uid = e.imap->add_message("INBOX", raw, {}, e.clock.t - 3600);
    PB_CHECK(e.sync().outcome == pbo::SyncOutcome::Success);
    return e.id_of("INBOX", uid);
}

pbo::Response ok_png(const std::string& body = kPng) {
    pbo::Response r;
    r.status = 200;
    r.headers = {{"Content-Type", "image/png"}};
    r.body = body;
    return r;
}

struct NoCancel : pbo::Cancel {
    bool requested() const override { return false; }
};

}  // namespace

PB_TEST(a_small_inline_picture_arrives_with_the_body_and_the_html_alternative_is_the_body) {
    MailEnv e;
    const int64_t id = sync_one(e, newsletter("<p>Hello</p><img src=\"cid:logo@x\" alt=\"Logo\" width=\"200\">", png_part("logo@x", kPngBase64)));
    const auto images = e.store->message_images(id);
    PB_CHECK_EQ(images.size(), size_t(1));
    PB_CHECK_EQ(images[0].kind, std::string("cid"));
    PB_CHECK_EQ(images[0].state, std::string("loaded"));
    const auto data = e.store->image_data(id, 1);
    PB_CHECK(data.has_value());
    PB_CHECK_EQ(data->size(), size_t(73));
    PB_CHECK_EQ(data->substr(1, 3), std::string("PNG"));
    const auto view = e.store->message(id);
    PB_CHECK(view.has_value());
    PB_CHECK(view->body_text.find("Plain version") == std::string::npos);
    PB_CHECK(view->body_text.find("Hello") != std::string::npos);
    PB_CHECK(view->attachments.empty());
}

PB_TEST(a_large_inline_picture_waits_for_the_tap_and_is_then_loaded_within_the_limits) {
    MailEnv e;
    std::string big = kPng + std::string(600000, 'x');   // 600 KB: over the limit of the sync, under the limit of the tap
    const int64_t id = sync_one(e, newsletter("<img src=\"cid:big@x\">", png_part("big@x", base64_of(big))));
    PB_CHECK_EQ(e.store->message_images(id)[0].state, std::string("pending"));
    auto imap = pbo::make_curl_imap_transport(e.pki.ca_cert());
    pbo::ImapEndpoint ep;
    ep.host = "localhost";
    ep.port = e.imap->port();
    ep.username = "alice";
    PB_CHECK(imap->connect(ep, "pw-for-tests").ok());
    pbo::ImageLoadReport report;
    PB_CHECK(pbo::load_cid_images(*imap, *e.store, id, pbo::ImageLimits{}, &report).ok());
    PB_CHECK_EQ(report.loaded, 1);
    PB_CHECK_EQ(e.store->message_images(id)[0].state, std::string("loaded"));
    PB_CHECK_EQ(e.store->image_data(id, 1)->size(), big.size());
    imap->disconnect();
}

PB_TEST(a_cid_that_names_no_part_is_unavailable_and_nothing_is_fetched_for_it) {
    MailEnv e;
    const int64_t id = sync_one(e, newsletter("<img src=\"cid:missing@x\" alt=\"Gone\">", png_part("other@x", kPngBase64)));
    const auto images = e.store->message_images(id);
    PB_CHECK_EQ(images.size(), size_t(1));
    PB_CHECK_EQ(images[0].state, std::string("unavailable"));
}

PB_TEST(remote_pictures_are_not_loaded_by_the_sync_and_only_by_the_tap) {
    MailEnv e;
    const int64_t id = sync_one(e, newsletter(padded("<p>x</p><img src=\"https://img.example.org/a.png\" alt=\"A\"><img src=\"https://img.example.org/b.png\" alt=\"B\">"), ""));
    auto images = e.store->message_images(id);
    PB_CHECK_EQ(images.size(), size_t(2));
    PB_CHECK_EQ(images[0].kind, std::string("remote"));
    PB_CHECK_EQ(images[0].state, std::string("pending"));
    pbtest::FakeTransport http;
    http.set_handler([](const pbo::Request& r) {
        if (r.url.find("a.png") != std::string::npos) return ok_png();
        pbo::Response nf;
        nf.status = 404;
        return nf;
    });
    PB_CHECK(http.requests().empty());   // the sync made no request of its own
    NoCancel cancel;
    pbo::ImageLoadReport report;
    PB_CHECK(pbo::load_remote_images(http, *e.store, id, pbo::ImageLimits{}, cancel, &report).ok());
    PB_CHECK_EQ(report.loaded, 1);
    PB_CHECK_EQ(report.failed, 1);
    images = e.store->message_images(id);
    PB_CHECK_EQ(images[0].state, std::string("loaded"));
    PB_CHECK_EQ(images[1].state, std::string("failed"));
    PB_CHECK_EQ(e.store->image_data(id, 1).value_or(""), kPng);
    // one plain GET each: no credentials, no cookie, no referrer
    PB_CHECK_EQ(http.requests().size(), size_t(2));
    for (const auto& r : http.requests()) {
        PB_CHECK_EQ(r.method, std::string("GET"));
        PB_CHECK(r.url.rfind("https://img.example.org/", 0) == 0);
        PB_CHECK(r.username.empty() && r.password.empty());
        PB_CHECK(!pbo::has_header(r.headers, "Authorization") && !pbo::has_header(r.headers, "Cookie") && !pbo::has_header(r.headers, "Referer"));
    }
    // a second tap asks only for the one that failed
    http.clear_requests();
    PB_CHECK(pbo::load_remote_images(http, *e.store, id, pbo::ImageLimits{}, cancel, &report).ok());
    PB_CHECK_EQ(http.requests().size(), size_t(1));
    PB_CHECK(http.requests()[0].url.find("b.png") != std::string::npos);
}

PB_TEST(what_is_not_a_picture_is_not_kept_whatever_the_server_claims) {
    MailEnv e;
    const int64_t id = sync_one(e, newsletter(padded("<img src=\"https://img.example.org/a.png\"><img src=\"https://img.example.org/b.png\"><img src=\"https://img.example.org/c.png\">"), ""));
    pbtest::FakeTransport http;
    http.set_handler([](const pbo::Request& r) {
        if (r.url.find("a.png") != std::string::npos) return ok_png("<html><script>alert(1)</script></html>");   // claims image/png, is HTML
        if (r.url.find("b.png") != std::string::npos) return ok_png(std::string(3u << 20, 'x'));                    // over the limit of one picture
        pbo::Response redirect_to_http;
        redirect_to_http.status = 200;
        redirect_to_http.body = "GIF89a-bytes";
        redirect_to_http.headers = {{"Content-Type", "text/html"}};
        return redirect_to_http;                                                                                    // GIF bytes under a wrong claim are still a picture
    });
    NoCancel cancel;
    pbo::ImageLoadReport report;
    PB_CHECK(pbo::load_remote_images(http, *e.store, id, pbo::ImageLimits{}, cancel, &report).ok());
    const auto images = e.store->message_images(id);
    PB_CHECK_EQ(images[0].state, std::string("failed"));
    PB_CHECK_EQ(images[1].state, std::string("too_large"));
    PB_CHECK_EQ(images[2].state, std::string("loaded"));
    PB_CHECK_EQ(e.store->image_data(id, 3)->substr(0, 6), std::string("GIF89a"));
    PB_CHECK(!e.store->image_data(id, 1).has_value());
}

PB_TEST(the_total_limit_of_a_call_leaves_the_rest_pending) {
    MailEnv e;
    std::string html;
    for (int i = 1; i <= 6; ++i) html += "<img src=\"https://img.example.org/" + std::to_string(i) + ".png\">";
    const int64_t id = sync_one(e, newsletter(padded(html), ""));
    pbtest::FakeTransport http;
    http.set_handler([](const pbo::Request&) { return ok_png(std::string("\x89PNG\r\n\x1a\n", 8) + std::string(1500000, 'p')); });
    NoCancel cancel;
    pbo::ImageLimits limits;
    limits.max_total = 4u << 20;   // 1.5 MB each: two fit, the third would pass 4 MiB
    pbo::ImageLoadReport report;
    PB_CHECK(pbo::load_remote_images(http, *e.store, id, limits, cancel, &report).ok());
    PB_CHECK_EQ(report.loaded, 2);
    PB_CHECK_EQ(report.skipped, 4);
    int pending = 0;
    for (const auto& im : e.store->message_images(id)) pending += im.state == "pending";
    PB_CHECK_EQ(pending, 4);
    PB_CHECK_EQ(http.requests().size(), size_t(3));   // the third was asked for, did not fit, and nothing more was asked after it
}

PB_TEST(sniffing_names_the_picture_types_by_their_first_bytes) {
    PB_CHECK_EQ(pbo::sniff_image_type(std::string("\x89PNG\r\n\x1a\n", 8)), std::string("image/png"));
    PB_CHECK_EQ(pbo::sniff_image_type("\xFF\xD8\xFF\xE0xxxx"), std::string("image/jpeg"));
    PB_CHECK_EQ(pbo::sniff_image_type("GIF89a...."), std::string("image/gif"));
    PB_CHECK_EQ(pbo::sniff_image_type("GIF87a...."), std::string("image/gif"));
    PB_CHECK_EQ(pbo::sniff_image_type(std::string("BM") + std::string(20, '\0')), std::string("image/bmp"));
    PB_CHECK_EQ(pbo::sniff_image_type(std::string("RIFF\x10\x00\x00\x00WEBPVP8 ", 16)), std::string("image/webp"));
    PB_CHECK_EQ(pbo::sniff_image_type("<svg xmlns=...>"), std::string(""));
    PB_CHECK_EQ(pbo::sniff_image_type("<html>"), std::string(""));
    PB_CHECK_EQ(pbo::sniff_image_type(""), std::string(""));
}

PB_TEST_MAIN()
