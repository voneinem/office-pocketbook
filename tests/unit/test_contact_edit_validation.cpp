// T044 (spec 002, research R-14, data-model.md "Validation rules"): what a contact must hold to be saved, the limits of the input, the clean-up of
// typed text, and the rule that the limits apply to values the user adds or changes only. The size of the written card (card_too_large) is checked
// where the card is written; see test_contact_push.cpp.
#include <algorithm>
#include <string>
#include <vector>

#include "pbo/charset.h"
#include "pbo/contact_edit.h"
#include "test_support.h"

namespace {

pbo::ContactEdit named(const std::string& given = "Anna", const std::string& family = "Beispiel") {
    pbo::ContactEdit e;
    e.given = given;
    e.family = family;
    return e;
}

pbo::LabeledValue lv(const std::string& label, const std::string& value) {
    pbo::LabeledValue v;
    v.label = label;
    v.value = value;
    return v;
}

bool has(const std::vector<pbo::FieldError>& errors, const std::string& key, const std::string& field = "") {
    return std::any_of(errors.begin(), errors.end(), [&](const pbo::FieldError& e) { return e.key == key && (field.empty() || e.field == field); });
}

}  // namespace

PB_TEST(a_contact_needs_a_name_an_organization_a_phone_number_or_an_email_address) {
    PB_CHECK(pbo::validate_contact_edit(named()).empty());
    pbo::ContactEdit only_org;
    only_org.org = "Example Works";
    PB_CHECK(pbo::validate_contact_edit(only_org).empty());
    pbo::ContactEdit only_phone;
    only_phone.phones.push_back(lv("mobile", "+1 555 0100"));
    PB_CHECK(pbo::validate_contact_edit(only_phone).empty());
    pbo::ContactEdit only_email;
    only_email.emails.push_back(lv("work", "a@example.org"));
    PB_CHECK(pbo::validate_contact_edit(only_email).empty());
    pbo::ContactEdit only_family;
    only_family.family = "Beispiel";
    PB_CHECK(pbo::validate_contact_edit(only_family).empty());

    PB_CHECK(has(pbo::validate_contact_edit(pbo::ContactEdit()), "empty"));
    pbo::ContactEdit blank;   // spaces only count as nothing
    blank.given = "   ";
    blank.org = "\t";
    blank.phones.push_back(lv("mobile", "  "));
    blank.emails.push_back(lv("home", ""));
    PB_CHECK(has(pbo::validate_contact_edit(blank), "empty"));
    pbo::ContactEdit address_and_note_only;   // an address or a note alone is not enough
    pbo::PostalAddress a;
    a.street = "1 Example Street";
    address_and_note_only.addresses.push_back(a);
    address_and_note_only.note = "a note";
    PB_CHECK(has(pbo::validate_contact_edit(address_and_note_only), "empty"));
}

PB_TEST(an_email_address_must_have_one_at_sign_a_local_part_and_a_domain_without_spaces_or_control_characters) {
    auto bad = [&](const std::string& addr) {
        pbo::ContactEdit e = named();
        e.emails.push_back(lv("work", "ok@example.org"));
        e.emails.push_back(lv("home", addr));
        return pbo::validate_contact_edit(e);
    };
    for (const char* good : {"a@b", "anna.beispiel+tag@mail.example.org", "ünal@example.org", "a@[192.0.2.1]", "o'neil@example.org"}) PB_CHECK(bad(good).empty());
    for (const char* wrong : {"no-at-sign", "two@@example.org", "a@b@example.org", "@example.org", "anna@", "an na@example.org", "anna@exa mple.org", "anna@example.org\n", "a\x01@example.org", "anna@exa\tmple.org"}) {
        const auto errors = bad(wrong);
        PB_CHECK(has(errors, "email_invalid", "emails[1]"));   // names the entry
        PB_CHECK(!has(errors, "email_invalid", "emails[0]"));
    }
    PB_CHECK(bad("").empty());   // an empty entry is ignored, not an error
}

PB_TEST(the_input_limits_of_research_r14_are_enforced) {
    const std::string long200(200, 'x'), long201(201, 'x');
    pbo::ContactEdit e = named(long200, long200);
    e.org = long200;
    PB_CHECK(pbo::validate_contact_edit(e).empty());
    e.given = long201;
    PB_CHECK(has(pbo::validate_contact_edit(e), "too_long", "given"));
    e = named(long200, long201);
    PB_CHECK(has(pbo::validate_contact_edit(e), "too_long", "family"));
    e = named();
    e.org = long201;
    PB_CHECK(has(pbo::validate_contact_edit(e), "too_long", "org"));

    e = named();
    e.phones.push_back(lv("mobile", std::string(64, '1')));
    PB_CHECK(pbo::validate_contact_edit(e).empty());
    e.phones.push_back(lv("work", std::string(65, '1')));
    PB_CHECK(has(pbo::validate_contact_edit(e), "too_long", "phones[1]"));

    e = named();
    e.emails.push_back(lv("work", std::string(242, 'a') + "@example.org"));   // 254 characters
    PB_CHECK(pbo::validate_contact_edit(e).empty());
    e.emails.push_back(lv("home", std::string(243, 'a') + "@example.org"));
    PB_CHECK(has(pbo::validate_contact_edit(e), "too_long", "emails[1]"));

    e = named();
    pbo::PostalAddress a;
    a.label = "home";
    a.street = long200;
    a.city = long200;
    a.region = long200;
    a.postcode = long200;
    a.country = long200;
    e.addresses.push_back(a);
    PB_CHECK(pbo::validate_contact_edit(e).empty());
    e.addresses[0].city = long201;
    PB_CHECK(has(pbo::validate_contact_edit(e), "too_long", "addresses[0]"));

    e = named();
    e.note = std::string(8192, 'n');
    PB_CHECK(pbo::validate_contact_edit(e).empty());
    e.note = std::string(8193, 'n');
    PB_CHECK(has(pbo::validate_contact_edit(e), "too_long", "note"));
}

PB_TEST(at_most_20_entries_of_each_kind) {
    pbo::ContactEdit e = named();
    for (int i = 0; i < 20; ++i) {
        e.phones.push_back(lv("mobile", "+1 555 " + std::to_string(1000 + i)));
        e.emails.push_back(lv("work", "u" + std::to_string(i) + "@example.org"));
        e.addresses.emplace_back();
        e.addresses.back().street = std::to_string(i) + " Example Street";
    }
    PB_CHECK(pbo::validate_contact_edit(e).empty());
    pbo::ContactEdit p = e;
    p.phones.push_back(lv("mobile", "+1 555 9999"));
    PB_CHECK(has(pbo::validate_contact_edit(p), "too_many", "phones"));
    pbo::ContactEdit m = e;
    m.emails.push_back(lv("work", "x@example.org"));
    PB_CHECK(has(pbo::validate_contact_edit(m), "too_many", "emails"));
    pbo::ContactEdit ad = e;
    ad.addresses.emplace_back();
    ad.addresses.back().street = "one more";
    PB_CHECK(has(pbo::validate_contact_edit(ad), "too_many", "addresses"));
}

PB_TEST(typed_text_is_cleaned_before_it_is_validated_and_stored) {
    pbo::ContactEdit e;
    e.given = "  Anna\u0001 \n Maria\t";
    e.family = "Bei\x7f" "spiel\r\n";
    e.org = "Example\nWorks";
    e.phones.push_back(lv("mobile", "  +1 555\n0100\u0007 "));
    e.emails.push_back(lv("work", "  anna@example.org \n"));
    pbo::PostalAddress a;
    a.street = "1 Example\nStreet";
    a.city = "Exampletown\r";
    e.addresses.push_back(a);
    e.note = "line one\r\nline two\n\u0001line three\t";
    const pbo::ContactEdit n = pbo::normalize_contact_edit(e);
    PB_CHECK_EQ(n.given, std::string("Anna Maria"));            // control characters go, a line break becomes a space, the ends are trimmed
    PB_CHECK_EQ(n.family, std::string("Beispiel"));
    PB_CHECK_EQ(n.org, std::string("Example Works"));
    PB_CHECK_EQ(n.phones[0].value, std::string("+1 555 0100"));
    PB_CHECK_EQ(n.emails[0].value, std::string("anna@example.org"));
    PB_CHECK_EQ(n.addresses[0].street, std::string("1 Example Street"));
    PB_CHECK_EQ(n.addresses[0].city, std::string("Exampletown"));
    PB_CHECK_EQ(n.note, std::string("line one\nline two\nline three\t"));   // the note keeps its line breaks (CRLF becomes LF) and tabs
    // entries with nothing in them are dropped; the order and the labels of the others stay
    pbo::ContactEdit with_empty = named();
    with_empty.phones = {lv("mobile", "+1 555 0100"), lv("home", "   "), lv("work", "+1 555 0200")};
    const pbo::ContactEdit c = pbo::normalize_contact_edit(with_empty);
    PB_CHECK_EQ(c.phones.size(), size_t(2));
    PB_CHECK_EQ(c.phones[1].label, std::string("work"));
    // text that is not UTF-8 is replaced, never stored as it is
    pbo::ContactEdit bad = named();
    bad.note = "caf\xe9 \xff";
    PB_CHECK(pbo::is_valid_utf8(pbo::normalize_contact_edit(bad).note));
}

PB_TEST(the_limits_apply_only_to_values_the_user_adds_or_changes) {
    // a contact from another app: a phone number over 64 characters and a note over 8 KiB are already in the card
    pbo::ContactEdit original = named();
    original.phones.push_back(lv("work", std::string(90, '9')));
    original.phones.back().orig_index = 0;
    original.phones.back().locked = true;
    original.phones.push_back(lv("mobile", "+1 555 0101"));
    original.phones.back().orig_index = 1;
    original.note_locked = true;

    pbo::ContactEdit edit = original;   // the user changed nothing but the family name
    edit.family = "Neu";
    PB_CHECK(pbo::validate_contact_edit(edit, &original).empty());   // the locked entry and the locked note pass untouched

    // a changed value of a locked entry is refused
    pbo::ContactEdit changed = original;
    changed.phones[0].value = std::string(90, '8');
    PB_CHECK(has(pbo::validate_contact_edit(changed, &original), "locked_changed", "phones[0]"));
    pbo::ContactEdit relabeled = original;
    relabeled.phones[0].label = "home";
    PB_CHECK(has(pbo::validate_contact_edit(relabeled, &original), "locked_changed", "phones[0]"));

    // removing a locked entry is allowed
    pbo::ContactEdit removed = original;
    removed.phones.erase(removed.phones.begin());
    PB_CHECK(pbo::validate_contact_edit(removed, &original).empty());

    // removing the locked note is allowed; a new note is checked against the limit
    pbo::ContactEdit no_note = original;
    no_note.note_locked = false;
    PB_CHECK(pbo::validate_contact_edit(no_note, &original).empty());
    pbo::ContactEdit new_note = original;
    new_note.note_locked = false;
    new_note.note = std::string(9000, 'n');
    PB_CHECK(has(pbo::validate_contact_edit(new_note, &original), "too_long", "note"));

    // a new over-limit value is refused as always
    pbo::ContactEdit added = original;
    added.phones.push_back(lv("other", std::string(65, '5')));
    PB_CHECK(has(pbo::validate_contact_edit(added, &original), "too_long", "phones[2]"));

    // an unchanged name that is longer than the limit also passes
    pbo::ContactEdit long_name = named(std::string(250, 'g'), "Beispiel");
    PB_CHECK(has(pbo::validate_contact_edit(long_name), "too_long", "given"));
    PB_CHECK(pbo::validate_contact_edit(long_name, &long_name).empty());
}

PB_TEST(the_first_error_is_about_the_first_field_in_form_order) {
    pbo::ContactEdit e;
    e.emails.push_back(lv("work", "broken"));
    e.note = std::string(9000, 'n');
    const auto errors = pbo::validate_contact_edit(e);
    PB_CHECK(errors.size() >= 2);
    PB_CHECK_EQ(errors[0].field, std::string("emails[0]"));   // the form shows the first one beside its field
}

PB_TEST_MAIN()
