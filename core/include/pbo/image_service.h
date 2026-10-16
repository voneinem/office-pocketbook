#pragma once
// The pictures of an HTML mail (FR-009, owner decision 2026-10-16). Two kinds:
//  - "cid": a part of the message itself. Small ones arrive with the body at sync; the others when the user asks.
//  - "remote": an https address on the sender's server. NEVER loaded by itself: only when the user taps "Load images" for that one message, then all
//    of its remote pictures are asked for, over TLS with the usual verification, one plain GET each, without credentials, cookies or referrer. Loading
//    tells the sender that the mail was read, and from where; that is why the user decides per message.
// The bytes go into the store (Store::set_image_loaded); what is not a picture (checked by its first bytes, not by what the server claims) is not kept.

#include <cstdint>
#include <string>
#include <string_view>

#include "pbo/http_transport.h"
#include "pbo/imap_transport.h"
#include "pbo/store.h"
#include "pbo/types.h"

namespace pbo {

struct ImageLimits {
    uint64_t max_each = 2u << 20;    // one picture, decoded
    uint64_t max_total = 10u << 20;  // all pictures of one message in one call
    int max_count = 40;
};
// What is loaded together with the body at sync: small pictures that are part of the message.
inline ImageLimits sync_image_limits() { return ImageLimits{400u << 10, 2u << 20, 10}; }

struct ImageLoadReport {
    int loaded = 0;
    int failed = 0;      // not a picture, an error status, the connection broke
    int too_large = 0;   // over a limit: marked, not fetched again by itself
    int skipped = 0;     // left pending (over the limit of this call, or cancelled)
};

// The media type of picture data by its first bytes: image/png, image/jpeg, image/gif, image/bmp, image/webp; "" for anything else.
std::string sniff_image_type(std::string_view bytes);

// The pending "cid" pictures of the message from its parts on the server. `imap` must be connected. A picture whose announced size is over
// `limits.max_each` stays pending (the user's tap uses larger limits). Errors: not_found (no such message); network errors end the call, the rest stay pending.
Error load_cid_images(ImapTransport& imap, Store& store, int64_t message_id, const ImageLimits& limits, ImageLoadReport* report = nullptr);

// The pending or failed "remote" pictures of the message, each by one GET. Never called without the user's tap. A picture that fails stays marked
// "failed" (a new tap tries it again); over the size limit it is marked "too_large".
Error load_remote_images(HttpTransport& http, Store& store, int64_t message_id, const ImageLimits& limits, Cancel& cancel, ImageLoadReport* report = nullptr);

}  // namespace pbo
