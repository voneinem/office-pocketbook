#pragma once
// Downloading one attachment on request (FR-010): streamed from the server, decoded on the way, with a hard size limit, and
// saved in the Downloads folder under the attachment's real file name (made safe). Never part of the sync.

#include <cstdint>
#include <string>

#include "pbo/imap_transport.h"
#include "pbo/store.h"

namespace pbo {

// `imap` must be connected. Steps and results:
//  - unknown id: Error "not_found";
//  - already downloaded and the saved file is still there: success, nothing is fetched;
//  - the size the structure announced is over `limit_bytes`: Error "too_large", NOTHING is fetched; the row gets error_key too_large;
//  - otherwise the part is streamed (BODY.PEEK, so the message is not marked as read), decoded (base64, quoted-printable, as is)
//    into a hidden ".pbo-<id>.part" file in `downloads_dir` (created if missing) and, when complete, moved to its final name:
//    safe_file_name() of the attachment name ("attachment" plus an extension from the type when it has no name), and "name (1).ext",
//    "name (2).ext" ... when that name is taken: an existing file is NEVER overwritten.
//    More than `limit_bytes` decoded bytes: the transfer is aborted, the part file removed, Error "too_large". A part the server does
//    not have: error_key "refused".
//  - a failure on the way (network, disk) leaves no file and no changed row, so the next tap simply tries again.
// On success the row is "downloaded" with an empty error_key and saved_path set.
Error download_attachment(ImapTransport& imap, Store& store, int64_t attachment_id, uint64_t limit_bytes, const std::string& downloads_dir);

// The name an attachment is saved under (before a number is added for a name that is taken).
std::string attachment_save_name(const std::string& name, const std::string& mime_type);

}  // namespace pbo
