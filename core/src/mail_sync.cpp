#include "pbo/mail_sync.h"

#include <algorithm>
#include <functional>
#include <vector>

#include "pbo/bodystructure.h"
#include "pbo/imap_parse.h"
#include "pbo/image_service.h"
#include "pbo/log.h"
#include "pbo/mail_headers.h"
#include "pbo/mime.h"

namespace pbo {

namespace {

constexpr size_t kBatch = 50;   // messages per database transaction

// IMAP quoted string for a mailbox name: only " and \ need an escape; the name was sent by the server, but a
// hostile one must not be able to add a second command, so line breaks are refused.
bool quote_mailbox(const std::string& raw, std::string* out) {
    std::string q = "\"";
    for (char c : raw) {
        if (c == '\r' || c == '\n' || c == '\0') return false;
        if (c == '"' || c == '\\') q += '\\';
        q += c;
    }
    *out = q + "\"";
    return true;
}

// What a server answered that could not be used, as structure only (describe_shape), so a problem with a real server
// can be diagnosed from the log without any mail data in it.
void log_unparsed(const char* step, const ImapRaw& raw) {
    log::event("imap.unparsed", {log::text("step", step), log::count("bytes", static_cast<long>(raw.data.size())),
                                 log::text("shape", describe_shape(raw, 700))});
}

Error cancelled() { return make_error(ErrorClass::Cancelled, "cancelled", "the sync was cancelled"); }
Error protocol(const char* detail) { return make_error(ErrorClass::Protocol, "protocol_error", detail); }

PulledMessage to_message(const FetchItem& item) {
    PulledMessage m;
    m.uid = item.uid;
    const std::string& h = item.header_bytes;
    m.message_id = header_value(h, "Message-ID");
    m.in_reply_to = header_value(h, "In-Reply-To");
    m.references = header_value(h, "References");
    const std::string from = header_value(h, "From");
    const std::vector<MailAddress> senders = parse_mailboxes(from);
    if (!senders.empty()) {
        m.from_name = senders.front().name;
        m.from_addr = senders.front().address;
    } else {
        split_first_address(from, &m.from_name, &m.from_addr);   // not a valid address: keep what the header says
        m.from_name = decode_header_words(m.from_name);
    }
    m.to_text = format_address_header(header_value(h, "To"));
    m.cc_text = format_address_header(header_value(h, "Cc"));
    m.subject = decode_header_words(header_value(h, "Subject"));
    int64_t date = 0;
    if (parse_rfc5322_date(header_value(h, "Date"), &date)) m.date_utc = date;
    else if (item.internal_date) m.date_utc = *item.internal_date;
    m.internal_date = item.internal_date;
    m.size = item.size ? static_cast<int64_t>(*item.size) : 0;
    m.flags = item.flags_bits.value_or(0);
    return m;
}


// Headers of `todo`: ONE message per FETCH command, because the response carries a literal and libcurl does not deliver a
// response with several literals completely (research R-01). Every kBatch messages are committed in one transaction together with
// the cursor (the next UID and the modseq only with the last batch, and only when everything was read), so an interrupted run
// resumes. `*clean` is false when an answer could not be read; the unread messages are fetched again next time.
Error fetch_header_batches(ImapTransport& imap, Store& store, const Folder& folder, const std::vector<uint32_t>& todo, const PullBatch& cursor,
                           Cancel& cancel, MailCounts* counts, std::map<uint32_t, std::string>* structures, bool* clean_out) {
    PullBatch batch;
    batch.uidvalidity = cursor.uidvalidity;
    batch.window_low_uid = cursor.window_low_uid;
    batch.older_on_server = cursor.older_on_server;
    bool clean = true;
    bool reported = false;
    for (size_t i = 0; i < todo.size(); ++i) {
        if (cancel.requested()) {
            store.apply_pull(folder.id, batch);   // keep what was fetched so far
            return cancelled();
        }
        const uint32_t uid = todo[i];
        Result<ImapRaw> f = imap.command(folder.raw_name,
                                         "UID FETCH " + std::to_string(uid) +
                                             " (UID FLAGS INTERNALDATE RFC822.SIZE BODY.PEEK[HEADER.FIELDS (FROM TO CC SUBJECT DATE MESSAGE-ID IN-REPLY-TO REFERENCES)] BODYSTRUCTURE)");
        if (!f.ok()) {
            store.apply_pull(folder.id, batch);
            return f.error;
        }
        const auto items = parse_fetch(f.value);
        if (!items.ok) clean = false;
        bool matched = false;
        for (const FetchItem& item : items.value)
            if (item.uid == uid) {   // only the UID that was asked for
                batch.messages.push_back(to_message(item));
                if (!item.bodystructure_text.empty() && item.bodystructure_text.size() <= (256u << 10)) (*structures)[uid] = item.bodystructure_text;
                matched = true;
            }
        if ((!items.ok || !matched) && !reported) {   // once per run: the first unreadable answer shows what the server sends
            log_unparsed("fetch", f.value);
            reported = true;
        }
        if (!matched) clean = false;
        const bool last = i + 1 == todo.size();
        if (batch.messages.size() >= kBatch || last) {
            if (last && clean) {
                batch.uidnext = cursor.uidnext;
                batch.highestmodseq = cursor.highestmodseq;
            }
            const size_t n = batch.messages.size();
            Error e = store.apply_pull(folder.id, batch);
            if (!e.ok()) return e;
            if (counts) counts->new_messages += static_cast<int>(n);
            batch = PullBatch();
            batch.uidvalidity = cursor.uidvalidity;
            batch.window_low_uid = cursor.window_low_uid;
            batch.older_on_server = cursor.older_on_server;
        }
    }
    *clean_out = clean;
    return {};
}

}  // namespace

Error sync_folder_list(ImapTransport& imap, Store& store, MailCounts* counts) {
    Result<ImapRaw> r = imap.command("", "LIST \"\" \"*\"");
    if (!r.ok()) return r.error;
    const auto parsed = parse_list(r.value);
    if (parsed.value.empty()) {
        log_unparsed("list", r.value);
        return protocol("the folder list could not be read");
    }   // every account has at least INBOX
    Error e = store.upsert_folders(parsed.value);
    if (!e.ok()) return e;
    if (counts) counts->folders = static_cast<int>(store.folders().size());
    return {};
}

Error pull_folder(ImapTransport& imap, Store& store, const Folder& folder_in, int window_days, Clock& clock, Cancel& cancel,
                  UidvalidityGuard& guard, MailCounts* counts) {
    Folder folder = folder_in;
    std::string quoted;
    if (!quote_mailbox(folder.raw_name, &quoted)) return protocol("the folder name is not usable");
    if (cancel.requested()) return cancelled();

    // 1. the folder's identity (the guard's one STATUS, asked for before anything is written), next UID, size
    const bool condstore = imap.capabilities().count("condstore") != 0;
    Result<ImapRaw> st = imap.command("", "STATUS " + quoted + " (UIDNEXT UIDVALIDITY MESSAGES" + (condstore ? " HIGHESTMODSEQ" : "") + ")");
    if (!st.ok()) return st.error;
    const auto status = parse_status(st.value);
    if (!status.value.has_uidvalidity || !status.value.has_uidnext || status.value.uidvalidity == 0) {
        guard.accept(folder, 0);
        log_unparsed("status", st.value);
        return protocol("the folder state could not be read");   // nothing is written for this folder
    }
    const GuardResult verdict = guard.accept(folder, status.value.uidvalidity);
    if (verdict == GuardResult::Mismatch) {
        // The folder was reset: the stored UIDs mean other messages. Nothing is sent by UID; the stored rows are set aside and
        // the window is downloaded again; pending local work is carried over by Message-ID (research R-04).
        Error e = store.begin_rebuild(folder.id, status.value.uidvalidity);
        if (!e.ok()) return e;
        if (const auto fresh = store.folder(folder.id)) folder = *fresh;
        guard.accept(folder, status.value.uidvalidity);
    }
    if (!guard.allows_writes(folder.id)) return protocol("the folder state could not be checked");

    // 2. the window
    if (cancel.requested()) return cancelled();
    const int64_t since = clock.now() - static_cast<int64_t>(window_days) * 86400;
    Result<ImapRaw> se = imap.command(folder.raw_name, "UID SEARCH SINCE " + imap_date(since));
    if (!se.ok()) return se.error;
    const auto found = parse_search(se.value);
    if (!found.ok) {
        log_unparsed("search", se.value);
        return protocol("the search result could not be read");
    }
    std::vector<uint32_t> uids = found.value;
    std::sort(uids.begin(), uids.end());
    uids.erase(std::unique(uids.begin(), uids.end()), uids.end());

    // 3. what the store has: messages that were deleted or moved away on the server, and flags that changed there
    const std::set<uint32_t> known = store.known_uids(folder.id);
    size_t gone_count = 0;
    if (!known.empty()) {
        if (cancel.requested()) return cancelled();
        const uint32_t low = *known.begin();
        Result<ImapRaw> all = imap.command(folder.raw_name, "UID SEARCH UID " + std::to_string(low) + ":*");
        if (!all.ok()) return all.error;
        const auto live = parse_search(all.value);
        if (!live.ok) {
            log_unparsed("search", all.value);
            return protocol("the search result could not be read");
        }
        const std::set<uint32_t> on_server(live.value.begin(), live.value.end());
        std::vector<uint32_t> gone;
        for (uint32_t uid : known)
            if (!on_server.count(uid)) gone.push_back(uid);
        gone_count = gone.size();
        int removed = 0, kept = 0;
        Error e = store.remove_gone(folder.id, gone, &removed, &kept);
        if (!e.ok()) return e;
        if (counts) counts->removed += removed + kept;

        // flags: with CONDSTORE only what changed since the stored value, otherwise all flags of the known range
        if (cancel.requested()) return cancelled();
        const bool incremental = condstore && folder.highestmodseq && status.value.has_highestmodseq;
        if (!incremental || static_cast<uint64_t>(*folder.highestmodseq) != status.value.highestmodseq) {
            const std::string cmd = incremental ? "UID FETCH 1:* (UID FLAGS) (CHANGEDSINCE " + std::to_string(*folder.highestmodseq) + ")"
                                                : "UID FETCH " + std::to_string(low) + ":* (UID FLAGS)";
            Result<ImapRaw> fl = imap.command(folder.raw_name, cmd);
            if (!fl.ok()) return fl.error;
            const auto items = parse_fetch(fl.value);
            if (!items.ok) {
                log_unparsed("flags", fl.value);
                return protocol("the flags could not be read");
            }
            std::vector<std::pair<uint32_t, uint32_t>> updates;
            for (const FetchItem& item : items.value)
                if (item.uid != 0 && item.flags_bits && known.count(item.uid)) updates.emplace_back(item.uid, *item.flags_bits);
            e = store.apply_flags(folder.id, updates);
            if (!e.ok()) return e;
            if (counts) counts->flags_updated += static_cast<int>(updates.size());
        }
    }

    // 4. only what the store does not have yet
    std::vector<uint32_t> todo;
    for (uint32_t uid : uids)
        if (!known.count(uid)) todo.push_back(uid);

    PullBatch cursor;
    cursor.uidvalidity = status.value.uidvalidity;
    cursor.uidnext = status.value.uidnext;
    if (status.value.has_highestmodseq) cursor.highestmodseq = static_cast<int64_t>(status.value.highestmodseq);
    if (!uids.empty()) cursor.window_low_uid = uids.front();
    // older mail on the server: more messages there than the ones this app has (kept ones) or is about to fetch (the window)
    cursor.older_on_server = status.value.has_messages && status.value.messages > (known.size() - gone_count) + todo.size();

    std::map<uint32_t, std::string> structures;   // BODYSTRUCTURE of the new messages, for the body pass
    auto finish = [&]() -> Error {
        if (store.rebuild_in_progress(folder.id)) {   // the window is complete: what did not come back is gone
            int removed = 0, kept = 0;
            Error e = store.finish_rebuild(folder.id, &removed, &kept);
            if (!e.ok()) return e;
            if (counts) counts->removed += removed + kept;
        }
        return fetch_bodies(imap, store, folder, cancel, counts, &structures);   // messages stored without their body earlier
    };
    if (todo.empty()) {
        // nothing new, but the cursor of an empty or fully synced folder is still recorded
        Error e = store.apply_pull(folder.id, cursor);
        if (!e.ok()) return e;
        return finish();
    }

    // 5. the headers of the new messages
    bool clean = true;
    Error he = fetch_header_batches(imap, store, folder, todo, cursor, cancel, counts, &structures, &clean);
    if (!he.ok()) return he;
    if (!clean) return protocol("part of the answer could not be read");   // the rest is fetched again next time
    return finish();
}

Error fetch_older(ImapTransport& imap, Store& store, const Folder& folder_in, int count, Cancel& cancel, UidvalidityGuard& guard, MailCounts* counts) {
    Folder folder = store.folder(folder_in.id).value_or(folder_in);   // the stored cursor, not the caller's copy
    if (cancel.requested()) return cancelled();
    if (guard.check(imap, folder) != GuardResult::Ok) return make_error(ErrorClass::Protocol, "uidvalidity_changed", "the folder was reset on the server");   // UIDs of another numbering: write nothing
    const std::set<uint32_t> known = store.known_uids(folder.id);
    if (known.empty()) return {};   // nothing to extend: the next sync downloads the window first
    const uint32_t low = *known.begin();
    if (low <= 1) {   // nothing can be older
        PullBatch done;
        done.older_on_server = false;
        return store.apply_pull(folder.id, done);
    }
    if (cancel.requested()) return cancelled();
    Result<ImapRaw> se = imap.command(folder.raw_name, "UID SEARCH UID 1:" + std::to_string(low - 1));
    if (!se.ok()) return se.error;
    const auto found = parse_search(se.value);
    if (!found.ok) {
        log_unparsed("search", se.value);
        return protocol("the search result could not be read");
    }
    std::vector<uint32_t> older;
    for (uint32_t uid : found.value)
        if (uid < low && !known.count(uid)) older.push_back(uid);
    std::sort(older.begin(), older.end());
    older.erase(std::unique(older.begin(), older.end()), older.end());
    const size_t take = std::min<size_t>(older.size(), static_cast<size_t>(count < 1 ? 1 : count));
    const std::vector<uint32_t> todo(older.end() - static_cast<std::ptrdiff_t>(take), older.end());   // the newest of the older ones

    PullBatch cursor;
    cursor.older_on_server = older.size() > take;   // more below the ones taken now
    if (!todo.empty()) cursor.window_low_uid = todo.front();
    std::map<uint32_t, std::string> structures;
    if (todo.empty()) {
        Error e = store.apply_pull(folder.id, cursor);
        if (!e.ok()) return e;
        return {};
    }
    bool clean = true;
    Error he = fetch_header_batches(imap, store, folder, todo, cursor, cancel, counts, &structures, &clean);
    if (!he.ok()) return he;
    if (!clean) return protocol("part of the answer could not be read");
    return fetch_bodies(imap, store, folder, cancel, counts, &structures);
}

namespace {

constexpr int kMaxBodiesPerCall = 1000;
constexpr int kTextPartLimit = 2097152;   // 2 MiB: the largest text part kept (research R-06)
constexpr int kWholeMessageLimit = 262144;

// The literal of one fetched section, found by the section name the server echoes ("1", "1.2", "" for the whole message).
bool find_section(const std::vector<FetchItem>& items, uint32_t uid, const std::string& section, std::string* bytes) {
    for (const FetchItem& item : items) {
        if (item.uid != uid) continue;
        const auto it = item.sections.find(section);
        if (it == item.sections.end()) continue;
        *bytes = it->second;
        return true;
    }
    return false;
}

}  // namespace

Error fetch_bodies(ImapTransport& imap, Store& store, const Folder& folder, Cancel& cancel, MailCounts* counts,
                   const std::map<uint32_t, std::string>* known_structures) {
    for (const MissingBody& mb : store.messages_missing_body(folder.id, kMaxBodiesPerCall)) {
        if (cancel.requested()) return cancelled();
        const std::string uid = std::to_string(mb.uid);

        // 1. the structure (the header fetch already brought it for new messages)
        std::string structure;
        if (known_structures) {
            const auto it = known_structures->find(mb.uid);
            if (it != known_structures->end()) structure = it->second;
        }
        if (structure.empty()) {
            Result<ImapRaw> r = imap.command(folder.raw_name, "UID FETCH " + uid + " (BODYSTRUCTURE)");
            if (!r.ok()) return r.error;
            const auto items = parse_fetch(r.value);
            for (const FetchItem& item : items.value)
                if (item.uid == mb.uid) structure = item.bodystructure_text;
        }

        // 2. the readable part, or the start of the whole message when the structure is unusable
        BodyUpdate update;
        ParsedMessage parsed;
        const std::optional<BodyPart> bs = structure.empty() ? std::nullopt : parse_bodystructure(structure);
        bool got_any = true;
        if (bs) {
            std::map<std::string, std::string> bytes;
            const std::vector<std::string> required = choose_text_sections(*bs);
            std::vector<std::string> wanted = required;
            for (const std::string& extra : choose_picture_html_sections(*bs))
                if (std::find(wanted.begin(), wanted.end(), extra) == wanted.end()) wanted.push_back(extra);   // the html next to the plain text: its pictures decide
            for (const std::string& section : wanted) {
                if (cancel.requested()) return cancelled();
                Result<ImapRaw> r = imap.command(folder.raw_name, "UID FETCH " + uid + " (BODY.PEEK[" + section + "]<0.2097152>)");
                if (!r.ok()) return r.error;
                const auto items = parse_fetch(r.value);
                std::string data;
                if (find_section(items.value, mb.uid, section, &data)) bytes[section] = std::move(data);
                else if (std::find(required.begin(), required.end(), section) != required.end()) got_any = false;   // the extra html is optional
            }
            parsed = assemble_from_parts(*bs, bytes);
            // a part longer than the limit was fetched only in part
            std::vector<std::string> chosen = choose_text_sections(*bs);
            std::function<void(const BodyPart&)> check = [&](const BodyPart& p) {
                if (!p.multipart() && std::find(chosen.begin(), chosen.end(), p.section) != chosen.end() && p.size > static_cast<uint64_t>(kTextPartLimit) && parsed.note.empty())
                    parsed.note = "truncated";
                for (const BodyPart& c : p.children) check(c);
            };
            check(*bs);
        } else {
            log_unparsed("structure", ImapRaw{structure});
            if (cancel.requested()) return cancelled();
            Result<ImapRaw> r = imap.command(folder.raw_name, "UID FETCH " + uid + " (BODY.PEEK[]<0.262144>)");
            if (!r.ok()) return r.error;
            const auto items = parse_fetch(r.value);
            std::string data;
            if (find_section(items.value, mb.uid, "", &data)) {
                parsed = parse_message(data);
                if (data.size() >= static_cast<size_t>(kWholeMessageLimit) && parsed.note.empty()) parsed.note = "truncated";
            } else {
                got_any = false;
            }
        }
        if (!got_any) continue;   // the message is gone or the server sent nothing: it stays as it is and is tried again next time

        {   // what was found, as numbers; when a message has several parts but no attachment, the structure's shape (media types only)
            int leaves = 0;
            std::function<void(const BodyPart&)> count = [&](const BodyPart& p) {
                if (p.multipart()) for (const BodyPart& c : p.children) count(c);
                else ++leaves;
            };
            if (bs) count(*bs);
            log::event("body.fetched", {log::count("structure", bs ? 1 : 0), log::count("parts", leaves),
                                        log::count("attachments", static_cast<long>(parsed.attachments.size())), log::count("bytes", static_cast<long>(parsed.text.size()))});
            // text and html alternatives of one body are the normal case; the shape is only worth a look for other structures
            if (bs && leaves > 1 && parsed.attachments.empty() && !(bs->multipart() && bs->subtype == "alternative")) log_unparsed("no_attachment", ImapRaw{structure});
        }
        update.state = parsed.body_kind == ParsedMessage::Body::Unreadable ? "unreadable" : "text";
        update.text = std::move(parsed.text);
        update.note = parsed.body_kind == ParsedMessage::Body::NoReadableText ? "no_readable_text" : parsed.note;
        update.links = std::move(parsed.links);
        const std::vector<MessageImage> parsed_images = std::move(parsed.images);
        for (const AttachmentInfo& a : parsed.attachments) {
            StoredAttachment s;
            s.section = a.section;
            s.name = a.name;
            s.mime_type = a.mime_type;
            s.encoding = a.encoding;
            s.size = static_cast<int64_t>(a.size);
            update.attachments.push_back(std::move(s));
        }
        for (size_t k = 0; k < parsed_images.size(); ++k) {
            const MessageImage& mi = parsed_images[k];
            StoredImage si;
            si.idx = static_cast<int>(k) + 1;
            si.kind = mi.kind;
            si.src = mi.src;
            si.alt = mi.alt;
            si.section = mi.section;
            si.mime_type = mi.mime_type;
            si.encoding = mi.encoding;
            si.size = static_cast<int64_t>(mi.size);
            si.width = mi.width;
            si.height = mi.height;
            if (mi.kind == "cid" && mi.section.empty()) si.state = "unavailable";   // no part has that Content-ID
            update.images.push_back(std::move(si));
        }
        Error e = store.apply_body(mb.id, update);
        if (!e.ok()) return e;
        if (counts) ++counts->bodies;
        // pictures that are parts of the message and small come along with the body; the others wait for the user (remote ones always)
        if (!update.images.empty() && update.state == "text") {
            ImageLoadReport report;
            const Error ie = load_cid_images(imap, store, mb.id, sync_image_limits(), &report);
            if (!ie.ok() && ie.cls != ErrorClass::Storage) return ie;   // the connection is the problem: the sync reports it
        }
    }
    return {};
}

}  // namespace pbo
