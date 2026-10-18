// CardDAV client (carddav.h). The plumbing (host rule, credentials, status mapping, multistatus helpers) is shared with the CalDAV client
// (dav_common.h). See specs/002-server-contacts/contracts/carddav-vcard-subset.md. Server behaviour is UNVERIFIED until a live run.

#include "pbo/carddav.h"

#include <algorithm>
#include <functional>

namespace pbo {

const char* const kCardDataProperties = "VERSION UID FN N NICKNAME ORG TITLE TEL EMAIL ADR NOTE REV X-ABLabel";

namespace {

const char* kDav = dav::kNsDav;
const char* kCard = dav::kNsCard;
const char* kCs = dav::kNsCs;
const char* kXmlHeader = dav::kXmlHeader;

constexpr size_t kMultigetBatch = 30;
constexpr long long kMultigetCap = 4ll << 20;
constexpr long long kGetCap = 1ll << 20;

using dav::dav_headers;
using dav::escape;
using dav::ok_prop_set;
using dav::parse_multistatus;
using dav::path_of;
using dav::prop_in_ok_propstat;
using dav::status_code;
using dav::trim;

// the data request: every property the app shows or edits, named one by one
std::string address_data_request() {
    std::string out = "<card:address-data>";
    std::string name;
    const std::string list = kCardDataProperties;
    for (size_t i = 0; i <= list.size(); ++i) {
        if (i == list.size() || list[i] == ' ') {
            if (!name.empty()) out += "<card:prop name=\"" + escape(name) + "\"/>";
            name.clear();
        } else {
            name += list[i];
        }
    }
    return out + "</card:address-data>";
}

// hrefs and etags of a multistatus (a sync-collection or listing answer)
void read_refs(const XmlNode& ms, const std::function<std::string(const std::string&)>& to_path, const std::string& skip_path, CardChangeSet* cs, bool with_removed) {
    for (const XmlNode* resp : ms.children_named(kDav, "response")) {
        const XmlNode* href_node = resp->child(kDav, "href");
        if (!href_node) continue;
        const std::string path = to_path(trim(href_node->text));
        if (path.empty() || path == skip_path) continue;
        const XmlNode* st = resp->child(kDav, "status");
        if (st && status_code(trim(st->text)) == 404) {
            if (with_removed) cs->removed.push_back(path);
            continue;
        }
        const XmlNode* tag = prop_in_ok_propstat(*resp, kDav, "getetag");
        if (!tag && !ok_prop_set(*resp)) continue;   // no usable property set: skipped
        cs->changed.push_back({path, tag ? trim(tag->text) : std::string()});
    }
}

}  // namespace

CardDavClient::CardDavClient(HttpTransport& http, std::string base_url, std::string username, std::string password)
    : dav_(http, std::move(base_url), std::move(username), std::move(password)) {}

namespace {

// A server that knows no CardDAV answers 404 (or 405 / 501) to the questions about address books: that is "no address book", not a failure of the
// sync. Everything else (login refused, server errors, no answer, answers that cannot be read) stays an error.
Error soften(const Error& e) {
    if (e.key == "not_found" || e.key == "protocol_error") return make_error(ErrorClass::Protocol, "no_addressbook", "the server offers no address book");
    return e;
}

}  // namespace

Result<std::vector<AddressBookInfo>> CardDavClient::discover() {
    Result<std::vector<AddressBookInfo>> out;
    if (dav_.origin().empty()) {
        out.error = make_error(ErrorClass::Protocol, "invalid_input", "no server address");
        return out;
    }
    // 1. the principal; candidates: the address as entered, and the well-known address (first when the address is a bare server)
    std::vector<std::string> starts;
    const std::string well_known = dav_.origin() + "/.well-known/carddav";
    const bool bare = dav_.base_url().size() <= dav_.origin().size() + 1;
    if (bare) starts = {well_known, dav_.base_url()};
    else starts = {dav_.base_url(), well_known};

    std::string principal, home;
    Error first_error;
    for (const std::string& start : starts) {
        const Response r = dav_.send("PROPFIND", start,
                                     std::string(kXmlHeader) + "<d:propfind xmlns:d=\"DAV:\"><d:prop><d:current-user-principal/></d:prop></d:propfind>", dav_headers("0"));
        if (r.status == 401 || r.status == 0 || r.body_too_large) {
            out.error = dav_.status_error(r);
            return out;
        }
        if (r.status != 207) {
            if (first_error.ok()) first_error = dav_.status_error(r);
            continue;
        }
        const Result<XmlNode> x = parse_multistatus(r);
        if (!x.ok()) {
            out.error = x.error;
            return out;
        }
        for (const XmlNode* resp : x.value.children_named(kDav, "response")) {
            if (const XmlNode* h = prop_in_ok_propstat(*resp, kDav, "current-user-principal")) {
                if (const XmlNode* href = h->child(kDav, "href")) {
                    principal = dav_.url_for(trim(href->text));
                    break;
                }
            }
        }
        if (!principal.empty()) break;
    }
    if (principal.empty()) {
        // the address may itself be the address book home: its listing is tried below, and "no_addressbook" is reported if it holds none
        home = dav_.base_url();
    }

    // 2. the address book home
    if (!principal.empty()) {
        const Response r = dav_.send("PROPFIND", principal,
                                     std::string(kXmlHeader) + "<d:propfind xmlns:d=\"DAV:\" xmlns:card=\"urn:ietf:params:xml:ns:carddav\"><d:prop><card:addressbook-home-set/></d:prop></d:propfind>",
                                     dav_headers("0"));
        Error e = dav_.status_error(r);
        if (r.status != 207 && e.ok()) e = make_error(ErrorClass::Protocol, "protocol_error", "unexpected answer from the server");
        if (!e.ok()) {
            out.error = soften(e);
            return out;
        }
        const Result<XmlNode> x = parse_multistatus(r);
        if (!x.ok()) {
            out.error = x.error;
            return out;
        }
        home.clear();
        for (const XmlNode* resp : x.value.children_named(kDav, "response")) {
            if (const XmlNode* h = prop_in_ok_propstat(*resp, kCard, "addressbook-home-set"))
                if (const XmlNode* href = h->child(kDav, "href")) {
                    home = dav_.url_for(trim(href->text));
                    break;
                }
        }
        if (home.empty()) {
            out.error = make_error(ErrorClass::Protocol, "no_addressbook", "the server names no address book home");
            return out;
        }
    }

    // 3. the address books
    const char* body =
        "<d:propfind xmlns:d=\"DAV:\" xmlns:card=\"urn:ietf:params:xml:ns:carddav\" xmlns:cs=\"http://calendarserver.org/ns/\"><d:prop>"
        "<d:resourcetype/><d:displayname/><cs:getctag/><d:sync-token/>"
        "<d:current-user-privilege-set/><d:supported-report-set/></d:prop></d:propfind>";
    const Response r = dav_.send("PROPFIND", home, std::string(kXmlHeader) + body, dav_headers("1"));
    Error e = dav_.status_error(r);
    if (r.status != 207 && e.ok()) e = make_error(ErrorClass::Protocol, "protocol_error", "unexpected answer from the server");
    if (!e.ok()) {
        out.error = soften((principal.empty() && !first_error.ok()) ? first_error : e);
        return out;
    }
    const Result<XmlNode> x = parse_multistatus(r);
    if (!x.ok()) {
        out.error = x.error;
        return out;
    }
    const std::string home_path = path_of(dav_.origin(), home);
    for (const XmlNode* resp : x.value.children_named(kDav, "response")) {
        const XmlNode* href_node = resp->child(kDav, "href");
        if (!href_node) continue;
        const std::string url = dav_.url_for(trim(href_node->text));
        if (url.empty()) continue;   // another host: ignored
        const std::string path = path_of(dav_.origin(), url);
        if (path == home_path) continue;
        const XmlNode* rt = prop_in_ok_propstat(*resp, kDav, "resourcetype");
        if (!rt || !rt->child(kCard, "addressbook")) continue;
        AddressBookInfo b;
        b.href = path;
        if (const XmlNode* n = prop_in_ok_propstat(*resp, kDav, "displayname")) b.name = trim(n->text);
        if (b.name.empty()) {
            std::string seg = path;
            while (!seg.empty() && seg.back() == '/') seg.pop_back();
            b.name = seg.substr(seg.rfind('/') + 1);
        }
        if (const XmlNode* n = prop_in_ok_propstat(*resp, kCs, "getctag")) b.ctag = trim(n->text);
        if (const XmlNode* n = prop_in_ok_propstat(*resp, kDav, "sync-token")) b.sync_token = trim(n->text);
        if (const XmlNode* privs = prop_in_ok_propstat(*resp, kDav, "current-user-privilege-set")) {
            for (const XmlNode* p : privs->children_named(kDav, "privilege"))
                if (p->child(kDav, "write-content") || p->child(kDav, "all") || p->child(kDav, "write")) b.writable = true;
        }
        if (const XmlNode* reports = prop_in_ok_propstat(*resp, kDav, "supported-report-set")) {
            for (const XmlNode* sr : reports->children_named(kDav, "supported-report"))
                if (const XmlNode* rep = sr->child(kDav, "report"))
                    if (rep->child(kDav, "sync-collection")) b.supports_sync_report = true;
        }
        out.value.push_back(std::move(b));
    }
    if (out.value.empty()) {
        if (principal.empty() && !first_error.ok()) out.error = soften(first_error);
        else out.error = make_error(ErrorClass::Protocol, "no_addressbook", "the server holds no address book");
    }
    return out;
}

Result<CardChangeSet> CardDavClient::changes(const AddressBookInfo& book, const std::string& stored) {
    Result<CardChangeSet> out;
    const std::string book_url = dav_.url_for(book.href);
    if (book_url.empty()) {
        out.error = make_error(ErrorClass::Protocol, "invalid_input", "the address book is not on the server");
        return out;
    }
    auto to_path = [this](const std::string& href) {
        const std::string url = dav_.url_for(href);
        return url.empty() ? std::string() : path_of(dav_.origin(), url);
    };
    const std::string book_path = path_of(dav_.origin(), book_url);

    out.value.new_ctag = book.ctag;
    out.value.new_sync_token = book.sync_token;

    // incremental: the changes since the stored token
    if (book.supports_sync_report && !stored.empty()) {
        const std::string body = std::string(kXmlHeader) + "<d:sync-collection xmlns:d=\"DAV:\"><d:sync-token>" + escape(stored) +
                                 "</d:sync-token><d:sync-level>1</d:sync-level><d:prop><d:getetag/></d:prop></d:sync-collection>";
        const Response r = dav_.send("REPORT", book_url, body, dav_headers("1"));
        const bool invalid_token = r.status == 409 || (r.status >= 400 && r.status < 500 && r.body.find("valid-sync-token") != std::string::npos);
        if (!invalid_token) {
            Error e = dav_.status_error(r);
            if (r.status != 207 && e.ok()) e = make_error(ErrorClass::Protocol, "protocol_error", "unexpected answer from the server");
            if (!e.ok()) {
                out.error = e;
                return out;
            }
            const Result<XmlNode> x = parse_multistatus(r);
            if (!x.ok()) {
                out.error = x.error;
                return out;
            }
            bool truncated = false;
            for (const XmlNode* resp : x.value.children_named(kDav, "response"))
                if (const XmlNode* st = resp->child(kDav, "status"))
                    if (status_code(trim(st->text)) == 507) truncated = true;   // the answer was cut short: a complete listing follows
            if (!truncated) {
                read_refs(x.value, to_path, book_path, &out.value, true);
                if (const XmlNode* t = x.value.child(kDav, "sync-token")) out.value.new_sync_token = trim(t->text);
                out.value.full = false;
                return out;
            }
        }
        // an invalid or unusable token: a complete listing follows
    }

    // the cursor as it is NOW (before the listing, so that nothing changed meanwhile is skipped by the next run)
    const Response c = dav_.send("PROPFIND", book_url,
                                 std::string(kXmlHeader) + "<d:propfind xmlns:d=\"DAV:\" xmlns:cs=\"http://calendarserver.org/ns/\"><d:prop><cs:getctag/><d:sync-token/></d:prop></d:propfind>",
                                 dav_headers("0"));
    if (c.status == 207) {
        const Result<XmlNode> x = parse_multistatus(c);
        if (x.ok())
            for (const XmlNode* resp : x.value.children_named(kDav, "response")) {
                if (const XmlNode* t = prop_in_ok_propstat(*resp, kCs, "getctag")) out.value.new_ctag = trim(t->text);
                if (const XmlNode* t = prop_in_ok_propstat(*resp, kDav, "sync-token")) out.value.new_sync_token = trim(t->text);
            }
    } else {
        const Error e = dav_.status_error(c);
        if (!e.ok()) {
            out.error = e;
            return out;
        }
    }
    if (!book.supports_sync_report && !stored.empty() && stored == out.value.new_ctag) {
        out.value.unchanged = true;   // no sync-collection: the ctag tells whether anything changed
        return out;
    }

    // the complete list of the cards
    const Response r = dav_.send("PROPFIND", book_url,
                                 std::string(kXmlHeader) + "<d:propfind xmlns:d=\"DAV:\"><d:prop><d:getetag/></d:prop></d:propfind>", dav_headers("1"));
    Error e = dav_.status_error(r);
    if (r.status != 207 && e.ok()) e = make_error(ErrorClass::Protocol, "protocol_error", "unexpected answer from the server");
    if (!e.ok()) {
        out.error = e;
        return out;
    }
    const Result<XmlNode> x = parse_multistatus(r);
    if (!x.ok()) {
        out.error = x.error;
        return out;
    }
    read_refs(x.value, to_path, book_path, &out.value, false);
    out.value.full = true;
    return out;
}

Result<CardFetchResult> CardDavClient::fetch(const AddressBookInfo& book, const std::vector<std::string>& hrefs) {
    Result<CardFetchResult> out;
    const std::string book_url = dav_.url_for(book.href);
    if (book_url.empty()) {
        out.error = make_error(ErrorClass::Protocol, "invalid_input", "the address book is not on the server");
        return out;
    }
    std::vector<std::string> paths;
    for (const std::string& h : hrefs) {
        const std::string url = dav_.url_for(h);
        if (!url.empty()) paths.push_back(path_of(dav_.origin(), url));   // a card on another host is not asked for
    }

    // a stack: the first batch is on top
    std::vector<std::vector<std::string>> queue;
    for (size_t i = 0; i < paths.size(); i += kMultigetBatch)
        queue.emplace_back(paths.begin() + i, paths.begin() + std::min(paths.size(), i + kMultigetBatch));
    std::reverse(queue.begin(), queue.end());
    const std::string data_request = address_data_request();
    while (!queue.empty()) {
        std::vector<std::string> batch = std::move(queue.back());
        queue.pop_back();
        std::string body = std::string(kXmlHeader) +
            "<card:addressbook-multiget xmlns:d=\"DAV:\" xmlns:card=\"urn:ietf:params:xml:ns:carddav\"><d:prop><d:getetag/>" + data_request + "</d:prop>";
        for (const std::string& p : batch) body += "<d:href>" + escape(p) + "</d:href>";
        body += "</card:addressbook-multiget>";
        const Response r = dav_.send("REPORT", book_url, body, dav_headers("1"), kMultigetCap);
        if (r.body_too_large) {
            if (batch.size() > 1) {
                const size_t half = batch.size() / 2;
                queue.emplace_back(batch.begin() + half, batch.end());
                queue.emplace_back(batch.begin(), batch.begin() + half);
            } else {
                out.value.failed.push_back(batch[0]);   // one card over 4 MiB: skipped
            }
            continue;
        }
        Error e = dav_.status_error(r);
        if (r.status != 207 && e.ok()) e = make_error(ErrorClass::Protocol, "protocol_error", "unexpected answer from the server");
        if (!e.ok()) {
            out.error = e;
            return out;
        }
        const Result<XmlNode> x = parse_multistatus(r);
        if (!x.ok()) {
            out.error = x.error;
            return out;
        }
        for (const XmlNode* resp : x.value.children_named(kDav, "response")) {
            const XmlNode* href_node = resp->child(kDav, "href");
            if (!href_node) continue;
            const std::string url = dav_.url_for(trim(href_node->text));
            if (url.empty()) continue;
            const std::string path = path_of(dav_.origin(), url);
            const XmlNode* st = resp->child(kDav, "status");
            if (st) {
                const int code = status_code(trim(st->text));
                if (code == 404) { out.value.gone.push_back(path); continue; }
                if (code >= 500) { out.value.failed.push_back(path); continue; }
            }
            const XmlNode* data = prop_in_ok_propstat(*resp, kCard, "address-data");
            if (!data) {
                // a propstat with an error status for this card
                bool failed = false;
                for (const XmlNode* ps : resp->children_named(kDav, "propstat"))
                    if (const XmlNode* pst = ps->child(kDav, "status")) {
                        const int code = status_code(trim(pst->text));
                        if (code == 404) { out.value.gone.push_back(path); failed = false; break; }
                        if (code >= 500) failed = true;
                    }
                if (failed) out.value.failed.push_back(path);
                continue;
            }
            CardResource card;
            card.href = path;
            card.vcf = data->text;
            if (const XmlNode* tag = prop_in_ok_propstat(*resp, kDav, "getetag")) card.etag = trim(tag->text);
            out.value.cards.push_back(std::move(card));
        }
    }
    return out;
}

Result<CardResource> CardDavClient::get(const std::string& href) {
    Result<CardResource> out;
    const std::string url = dav_.url_for(href);
    if (url.empty()) {
        out.error = make_error(ErrorClass::Protocol, "invalid_input", "the address is not on the server");
        return out;
    }
    const Response r = dav_.send("GET", url, "", {{"Accept", "text/vcard, text/x-vcard;q=0.8, */*;q=0.1"}}, kGetCap);
    Error e = dav_.status_error(r);
    if (r.status != 200 && e.ok()) e = make_error(ErrorClass::Protocol, "protocol_error", "unexpected answer from the server");
    if (!e.ok()) {
        out.error = e;
        return out;
    }
    out.value.href = path_of(dav_.origin(), url);
    out.value.etag = header_value(r.headers, "ETag");
    out.value.vcf = r.body;
    return out;
}

namespace {

CardWriteResult write_outcome(const Response& r, bool creating) {
    CardWriteResult w;
    if (r.status == 0 || r.body_too_large) {
        w.kind = CardWriteResult::Kind::Failed;
        const ErrorClass cls = r.error == ErrorClass::None ? ErrorClass::Network : r.error;
        w.error = make_error(cls, r.error_key.empty() ? "server_unreachable" : r.error_key, r.error_detail);
        return w;
    }
    if (r.status >= 200 && r.status < 300) {
        w.kind = creating ? CardWriteResult::Kind::Created : CardWriteResult::Kind::Updated;
        w.etag = header_value(r.headers, "ETag");
        return w;
    }
    switch (r.status) {
        case 401:
            w.kind = CardWriteResult::Kind::Failed;
            w.error = make_error(ErrorClass::Auth, "auth_failed", "the server refused the login");
            break;
        case 403:
        case 405:
            w.kind = CardWriteResult::Kind::Refused;
            w.error = make_error(ErrorClass::Server, "refused", "the server does not accept this change");
            break;
        case 404:
        case 410:
            w.kind = CardWriteResult::Kind::NotFound;
            break;
        case 409:
        case 412:
            w.kind = CardWriteResult::Kind::PreconditionFailed;
            break;
        case 413:
            w.kind = CardWriteResult::Kind::Refused;
            w.error = make_error(ErrorClass::Server, "too_large", "the server refused the size");
            break;
        case 507:
            w.kind = CardWriteResult::Kind::Refused;
            w.error = make_error(ErrorClass::Server, "storage_full_server", "the server has no space left");
            break;
        default:
            w.kind = CardWriteResult::Kind::Failed;
            w.error = r.status >= 500 ? make_error(ErrorClass::Server, "server_error", "the server reported a problem")
                                      : make_error(ErrorClass::Protocol, "protocol_error", "unexpected answer from the server");
    }
    return w;
}

}  // namespace

CardWriteResult CardDavClient::put(const std::string& href, const std::string& vcf, const std::string* if_match) {
    const std::string url = dav_.url_for(href);
    if (url.empty()) {
        CardWriteResult w;
        w.error = make_error(ErrorClass::Protocol, "invalid_input", "the address is not on the server");
        return w;
    }
    Headers h = {{"Content-Type", "text/vcard; charset=utf-8"}};
    if (if_match) h.emplace_back("If-Match", *if_match);
    else h.emplace_back("If-None-Match", "*");
    const Response r = dav_.send("PUT", url, vcf, h);
    return write_outcome(r, if_match == nullptr);
}

CardWriteResult CardDavClient::remove(const std::string& href, const std::string& if_match) {
    const std::string url = dav_.url_for(href);
    if (url.empty()) {
        CardWriteResult w;
        w.error = make_error(ErrorClass::Protocol, "invalid_input", "the address is not on the server");
        return w;
    }
    const Response r = dav_.send("DELETE", url, "", {{"If-Match", if_match}});
    return write_outcome(r, false);
}

std::string new_card_href(const std::string& book_href, Random& random) {
    uint8_t raw[16];
    random.bytes(raw, sizeof raw);
    static const char* digits = "0123456789abcdef";
    std::string name;
    for (uint8_t b : raw) {
        name += digits[b >> 4];
        name += digits[b & 15];
    }
    std::string base = book_href;
    if (base.empty() || base.back() != '/') base += '/';
    return base + name + ".vcf";
}

}  // namespace pbo
