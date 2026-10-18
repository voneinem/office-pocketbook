// The user's actions on contacts (contact_edit.h).

#include "pbo/contact_edit.h"

#include <algorithm>
#include <cctype>

#include "pbo/carddav.h"
#include "pbo/charset.h"
#include "pbo/vcard.h"

namespace pbo {

namespace {

bool blank(const std::string& s) { return s.find_first_not_of(" \t\r\n") == std::string::npos; }

// A single-line field: control characters out, line breaks and tabs to spaces, runs of spaces to one, the ends trimmed.
std::string clean_line(const std::string& in) {
    const std::string text = sanitize_utf8(in);
    std::string out;
    bool space = true;   // collapses leading spaces too
    for (unsigned char c : text) {
        if (c == '\n' || c == '\r' || c == '\t' || c == ' ') {
            if (!space) out += ' ';
            space = true;
            continue;
        }
        if (c < 0x20 || c == 0x7f) continue;
        out += static_cast<char>(c);
        space = false;
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

// The note: line breaks and tabs stay; CR LF and CR become LF; other control characters go.
std::string clean_note(const std::string& in) {
    const std::string text = sanitize_utf8(in);
    std::string out;
    for (size_t i = 0; i < text.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c == '\r') {
            out += '\n';
            if (i + 1 < text.size() && text[i + 1] == '\n') ++i;
            continue;
        }
        if (c == '\n' || c == '\t') {
            out += static_cast<char>(c);
            continue;
        }
        if (c < 0x20 || c == 0x7f) continue;
        out += static_cast<char>(c);
    }
    return blank(out) ? std::string() : out;
}

bool address_blank(const PostalAddress& a) {
    return blank(a.street) && blank(a.city) && blank(a.region) && blank(a.postcode) && blank(a.country);
}

bool same_entry(const LabeledValue& a, const LabeledValue& b) { return a.label == b.label && a.value == b.value; }
bool same_entry(const PostalAddress& a, const PostalAddress& b) {
    return a.label == b.label && a.street == b.street && a.city == b.city && a.region == b.region && a.postcode == b.postcode && a.country == b.country;
}

bool email_ok(const std::string& s) {
    size_t at = std::string::npos;
    for (size_t i = 0; i < s.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c <= 0x20 || c == 0x7f) return false;   // spaces and control characters
        if (c == '@') {
            if (at != std::string::npos) return false;   // a second "@"
            at = i;
        }
    }
    return at != std::string::npos && at > 0 && at + 1 < s.size();
}

std::string idx(const char* kind, size_t i) { return std::string(kind) + "[" + std::to_string(i) + "]"; }

const LabeledValue* original_of(const std::vector<LabeledValue>& list, int orig_index) {
    for (const LabeledValue& v : list)
        if (v.orig_index == orig_index) return &v;
    return nullptr;
}
const PostalAddress* original_of(const std::vector<PostalAddress>& list, int orig_index) {
    for (const PostalAddress& v : list)
        if (v.orig_index == orig_index) return &v;
    return nullptr;
}

std::string hex_of(Random& random, size_t bytes) {
    std::vector<uint8_t> raw(bytes);
    random.bytes(raw.data(), raw.size());
    static const char* digits = "0123456789abcdef";
    std::string out;
    for (uint8_t b : raw) {
        out += digits[b >> 4];
        out += digits[b & 15];
    }
    return out;
}

}  // namespace

ContactEdit normalize_contact_edit(const ContactEdit& in) {
    ContactEdit out;
    out.given = clean_line(in.given);
    out.family = clean_line(in.family);
    out.org = clean_line(in.org);
    auto labeled = [](const std::vector<LabeledValue>& from, std::vector<LabeledValue>* to) {
        for (const LabeledValue& v : from) {
            LabeledValue n = v;
            if (!v.locked) {
                n.value = clean_line(v.value);
                n.label = clean_line(v.label);
                if (n.value.empty()) continue;
            }
            to->push_back(n);
        }
    };
    labeled(in.phones, &out.phones);
    labeled(in.emails, &out.emails);
    for (const PostalAddress& a : in.addresses) {
        PostalAddress n = a;
        if (!a.locked) {
            n.label = clean_line(a.label);
            n.street = clean_line(a.street);
            n.city = clean_line(a.city);
            n.region = clean_line(a.region);
            n.postcode = clean_line(a.postcode);
            n.country = clean_line(a.country);
            if (address_blank(n)) continue;
        }
        out.addresses.push_back(n);
    }
    out.note_locked = in.note_locked;
    out.note = in.note_locked ? std::string() : clean_note(in.note);
    return out;
}

std::vector<FieldError> validate_contact_edit(const ContactEdit& edit, const ContactEdit* original) {
    std::vector<FieldError> errors;
    auto add = [&](const std::string& field, const char* key) { errors.push_back({field, key}); };

    // something to call the contact by or to reach it by
    size_t phones = 0, emails = 0, addresses = 0;
    for (const LabeledValue& v : edit.phones)
        if (v.locked || !blank(v.value)) ++phones;
    for (const LabeledValue& v : edit.emails)
        if (v.locked || !blank(v.value)) ++emails;
    for (const PostalAddress& a : edit.addresses)
        if (a.locked || !address_blank(a)) ++addresses;
    if (blank(edit.given) && blank(edit.family) && blank(edit.org) && phones == 0 && emails == 0) {
        add("given", "empty");
        return errors;
    }

    auto name_limit = [&](const std::string& value, const std::string* was, const char* field, size_t max) {
        if (value.size() > max && !(was && *was == value)) add(field, "too_long");
    };
    name_limit(edit.given, original ? &original->given : nullptr, "given", kMaxNamePart);
    name_limit(edit.family, original ? &original->family : nullptr, "family", kMaxNamePart);
    name_limit(edit.org, original ? &original->org : nullptr, "org", kMaxOrg);

    auto labeled = [&](const std::vector<LabeledValue>& list, const std::vector<LabeledValue>* orig, const char* kind, size_t max, bool is_email) {
        for (size_t i = 0; i < list.size(); ++i) {
            const LabeledValue& v = list[i];
            const LabeledValue* was = (orig && v.orig_index >= 0) ? original_of(*orig, v.orig_index) : nullptr;
            if (v.locked && was) {   // kept as it is, or refused when it was changed
                if (!same_entry(v, *was)) add(idx(kind, i), "locked_changed");
                continue;
            }
            if (blank(v.value)) continue;
            if (was && same_entry(v, *was)) continue;   // untouched: never refused for its size or form
            if (v.value.size() > max) add(idx(kind, i), "too_long");
            else if (is_email && !email_ok(v.value)) add(idx(kind, i), "email_invalid");
        }
    };
    labeled(edit.phones, original ? &original->phones : nullptr, "phones", kMaxPhone, false);
    labeled(edit.emails, original ? &original->emails : nullptr, "emails", kMaxEmail, true);

    for (size_t i = 0; i < edit.addresses.size(); ++i) {
        const PostalAddress& a = edit.addresses[i];
        const PostalAddress* was = (original && a.orig_index >= 0) ? original_of(original->addresses, a.orig_index) : nullptr;
        if (a.locked && was) {
            if (!same_entry(a, *was)) add(idx("addresses", i), "locked_changed");
            continue;
        }
        if (address_blank(a)) continue;
        if (was && same_entry(a, *was)) continue;
        if (a.street.size() > kMaxAddressPart || a.city.size() > kMaxAddressPart || a.region.size() > kMaxAddressPart || a.postcode.size() > kMaxAddressPart ||
            a.country.size() > kMaxAddressPart)
            add(idx("addresses", i), "too_long");
    }

    if (!edit.note_locked && edit.note.size() > kMaxNote && !(original && !original->note_locked && original->note == edit.note)) add("note", "too_long");

    if (phones > kMaxEntriesPerKind) add("phones", "too_many");
    if (emails > kMaxEntriesPerKind) add("emails", "too_many");
    if (addresses > kMaxEntriesPerKind) add("addresses", "too_many");
    return errors;
}

std::string contact_edit_block_reason(const ContactView& c) {
    if (c.unreadable) return "unreadable";
    if (!c.addressbook_present) return "book_gone";
    if (!c.addressbook_writable) return "read_only";
    if (c.sync_state == "conflict") return "in_conflict";
    if (c.sync_state == "pending_delete") return "delete_pending";
    return std::string();
}

Result<int64_t> create_contact(ContactEditDeps& deps, int64_t addressbook_id, const ContactEdit& input) {
    Result<int64_t> out;
    const ContactEdit edit = normalize_contact_edit(input);
    const std::vector<FieldError> errors = validate_contact_edit(edit);
    if (!errors.empty()) {
        out.error = make_error(ErrorClass::Unsupported, errors.front().key, errors.front().field);
        return out;
    }
    const AddressBook* book = nullptr;
    const std::vector<AddressBook> books = deps.store.addressbooks();
    for (const AddressBook& b : books)
        if (b.id == addressbook_id) book = &b;
    if (!book || !book->present_on_server) {
        out.error = make_error(ErrorClass::Storage, "not_found", "no such address book");
        return out;
    }
    if (!book->writable) {
        out.error = make_error(ErrorClass::Unsupported, "read_only", "the address book does not accept changes");
        return out;
    }
    const std::string uid = hex_of(deps.random, 16) + "@pboffice.invalid";
    const std::string href = new_card_href(book->href, deps.random);
    const std::string vcf = new_contact_vcf(edit, uid, deps.clock.now());
    if (vcf.size() > kMaxWrittenCard) {
        out.error = make_error(ErrorClass::Unsupported, "card_too_large", "the contact is too big");
        return out;
    }
    ContactUpsert record = contact_upsert_from_card(href, std::string(), vcf);
    record.partial = false;   // the app wrote the whole card itself
    out = deps.store.save_contact_local(addressbook_id, 0, record);
    if (out.ok()) deps.store.set_setting("contacts_last_book", std::to_string(addressbook_id));
    return out;
}

Error retry_contact(Store& store, int64_t contact_id) { return store.clear_contact_error(contact_id); }

Error discard_contact_change(Store& store, int64_t contact_id) { return store.discard_contact_change(contact_id); }

Result<int64_t> change_contact(ContactEditDeps& deps, int64_t contact_id, const ContactEdit& input, bool* changed) {
    Result<int64_t> out;
    if (changed) *changed = false;
    const auto view = deps.store.contact(contact_id);
    if (!view) {
        out.error = make_error(ErrorClass::Storage, "not_found", "no such contact");
        return out;
    }
    if (const std::string why = contact_edit_block_reason(*view); !why.empty()) {
        out.error = make_error(ErrorClass::Unsupported, why, "the contact cannot be changed now");
        return out;
    }
    const ContactEdit edit = normalize_contact_edit(input);
    const ContactEdit original = contact_edit_of(view->card);
    const std::vector<FieldError> errors = validate_contact_edit(edit, &original);
    if (!errors.empty()) {
        out.error = make_error(ErrorClass::Unsupported, errors.front().key, errors.front().field);
        return out;
    }
    Result<std::string> text = apply_contact_edit(view->vcf, edit, deps.clock.now());
    if (!text.ok()) {
        out.error = text.error;
        return out;
    }
    out.value = contact_id;
    if (text.value == view->vcf) return out;   // nothing differs: nothing is written, nothing is sent
    if (text.value.size() > kMaxCardBytes) {
        out.error = make_error(ErrorClass::Unsupported, "card_too_large", "the contact is too big");
        out.value = 0;
        return out;
    }
    const ContactUpsert record = contact_upsert_from_card(view->href, view->etag, text.value);
    if (record.unreadable) {
        out.error = make_error(ErrorClass::Unsupported, "unreadable", "the card cannot be written");
        out.value = 0;
        return out;
    }
    Result<int64_t> saved = deps.store.save_contact_local(view->addressbook_id, contact_id, record);
    if (saved.ok() && changed) *changed = true;
    return saved;
}

Error resolve_contact_conflict(ContactEditDeps& deps, int64_t contact_id, ContactChoice choice) {
    const auto view = deps.store.contact(contact_id);
    if (!view) return make_error(ErrorClass::Storage, "not_found", "no such contact");
    if (view->sync_state != "conflict") return make_error(ErrorClass::Storage, "not_in_conflict", "the contact is not in conflict");
    if (choice == ContactChoice::KeepMine) return deps.store.keep_my_contact(contact_id);

    ContactProblem problem;
    bool found = false;
    for (const ContactProblem& p : deps.store.contact_problems())
        if (p.contact_id == contact_id && p.state == "conflict") {
            problem = p;
            found = true;
        }
    if (!found) return make_error(ErrorClass::Storage, "not_in_conflict", "the contact is not in conflict");
    const ContactUpsert server = contact_upsert_from_card(view->href, problem.server_etag, problem.server_vcf);
    if (server.unreadable) return make_error(ErrorClass::Protocol, "unreadable_server_version", "the server's version cannot be read");
    if (choice == ContactChoice::KeepServer) return deps.store.keep_server_contact(contact_id, server);

    // keep both: the reader's version becomes a new contact with its own identity and its own path
    std::string book_href;
    for (const AddressBook& b : deps.store.addressbooks())
        if (b.id == view->addressbook_id) book_href = b.href;
    const std::string uid = hex_of(deps.random, 16) + "@pboffice.invalid";
    ContactUpsert mine = contact_upsert_from_card(new_card_href(book_href, deps.random), std::string(), with_new_uid(view->vcf, uid));
    mine.partial = view->partial;
    return deps.store.keep_both_contacts(contact_id, server, mine);
}

Error recreate_contact(ContactEditDeps& deps, int64_t contact_id) {
    const auto view = deps.store.contact(contact_id);
    if (!view) return make_error(ErrorClass::Storage, "not_found", "no such contact");
    if (view->sync_state != "server_deleted") return make_error(ErrorClass::Storage, "not_server_deleted", "the server still has the contact");
    std::string book_href;
    for (const AddressBook& b : deps.store.addressbooks())
        if (b.id == view->addressbook_id) book_href = b.href;
    return deps.store.recreate_contact(contact_id, new_card_href(book_href, deps.random));
}

Error delete_contact(Store& store, int64_t contact_id) { return store.request_contact_delete(contact_id); }
Error undo_delete_contact(Store& store, int64_t contact_id) { return store.undo_contact_delete(contact_id); }

Error discard_server_deleted_contact(Store& store, int64_t contact_id) { return store.discard_contact(contact_id); }

}  // namespace pbo
