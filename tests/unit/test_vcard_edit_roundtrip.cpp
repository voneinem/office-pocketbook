// T045 / T062 (spec 002, research R-06, contracts/carddav-vcard-subset.md section 4): writing contacts. This file holds the new card first; the lossless
// edit of an existing card is added with the story that needs it.
#include <dirent.h>

#include <algorithm>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "pbo/charset.h"
#include "pbo/vcard.h"
#include "test_support.h"

namespace {

constexpr int64_t kNow = 1791979200;   // 2026-10-14 12:00:00 UTC

pbo::LabeledValue lv(const std::string& label, const std::string& value) {
    pbo::LabeledValue v;
    v.label = label;
    v.value = value;
    return v;
}

std::vector<std::string> physical_lines(const std::string& text) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos < text.size()) {
        const size_t e = text.find("\r\n", pos);
        if (e == std::string::npos) {
            out.push_back(text.substr(pos));
            break;
        }
        out.push_back(text.substr(pos, e - pos));
        pos = e + 2;
    }
    return out;
}

std::string unfolded(const std::string& text) {
    std::string out;
    for (const std::string& l : physical_lines(text)) {
        if (!l.empty() && l[0] == ' ' && !out.empty()) out += l.substr(1);
        else out += (out.empty() ? "" : "\n") + l;
    }
    return out;
}

bool contains_line(const std::string& text, const std::string& line) {
    std::string flat = "\n" + unfolded(text) + "\n";
    return flat.find("\n" + line + "\n") != std::string::npos;
}

}  // namespace

PB_TEST(a_new_card_has_the_documented_lines_in_the_documented_order) {
    pbo::ContactEdit e;
    e.given = "Anna";
    e.family = "Beispiel";
    e.org = "Example Works";
    e.phones = {lv("mobile", "+1 555 0101"), lv("work", "+1 555 0100"), lv("home", "+1 555 0102"), lv("other", "+1 555 0103")};
    e.emails = {lv("work", "anna@example.org"), lv("home", "anna.home@example.org"), lv("other", "anna.other@example.org")};
    pbo::PostalAddress a;
    a.label = "home";
    a.street = "1 Example Street";
    a.city = "Exampletown";
    a.region = "EX";
    a.postcode = "12345";
    a.country = "Exampleland";
    e.addresses.push_back(a);
    e.note = "Met at the fair.";
    const std::string text = pbo::new_contact_vcf(e, "0123456789abcdef0123456789abcdef@pboffice.invalid", kNow);

    const std::vector<std::string> expect = {
        "BEGIN:VCARD", "VERSION:3.0", "PRODID:-//office-pocketbook//EN", "UID:0123456789abcdef0123456789abcdef@pboffice.invalid", "FN:Anna Beispiel",
        "N:Beispiel;Anna;;;", "ORG:Example Works", "TEL;TYPE=CELL:+1 555 0101", "TEL;TYPE=WORK:+1 555 0100", "TEL;TYPE=HOME:+1 555 0102", "TEL:+1 555 0103",
        "EMAIL;TYPE=WORK:anna@example.org", "EMAIL;TYPE=HOME:anna.home@example.org", "EMAIL:anna.other@example.org",
        "ADR;TYPE=HOME:;;1 Example Street;Exampletown;EX;12345;Exampleland", "NOTE:Met at the fair.", "REV:20261014T120000Z", "END:VCARD"};
    PB_CHECK((physical_lines(text) == expect));
    PB_CHECK(text.size() >= 2 && text.compare(text.size() - 2, 2, "\r\n") == 0);   // CRLF line ends, also after the last line
    PB_CHECK(text.find('\n') == text.find("\r\n") + 1);
}

PB_TEST(the_display_name_of_a_nameless_card_is_the_organization_the_email_or_the_phone) {
    auto fn = [&](const pbo::ContactEdit& e) {
        const auto lines = physical_lines(pbo::new_contact_vcf(e, "u@pboffice.invalid", kNow));
        for (const std::string& l : lines)
            if (l.compare(0, 3, "FN:") == 0) return l.substr(3);
        return std::string("<none>");
    };
    pbo::ContactEdit org;
    org.org = "Example Works";
    PB_CHECK_EQ(fn(org), std::string("Example Works"));
    pbo::ContactEdit mail;
    mail.emails = {lv("work", "anna@example.org")};
    mail.phones = {lv("mobile", "+1 555 0101")};
    PB_CHECK_EQ(fn(mail), std::string("anna@example.org"));
    pbo::ContactEdit phone;
    phone.phones = {lv("mobile", "+1 555 0101")};
    PB_CHECK_EQ(fn(phone), std::string("+1 555 0101"));
    pbo::ContactEdit family_only;
    family_only.family = "Beispiel";
    PB_CHECK_EQ(fn(family_only), std::string("Beispiel"));
    // a card always has N, even when empty
    const std::string text = pbo::new_contact_vcf(phone, "u@pboffice.invalid", kNow);
    PB_CHECK(contains_line(text, "N:;;;;"));
}

PB_TEST(text_is_escaped_and_long_lines_are_folded_at_75_octets_without_cutting_a_character) {
    pbo::ContactEdit e;
    e.given = "Zoë";
    e.family = "Meier, Schulze; \\ Co";
    e.org = "Chi, Psi & Co; Ltd";
    e.phones = {lv("mobile", "+1 555 0101")};
    std::string umlauts;
    for (int i = 0; i < 60; ++i) umlauts += "ä";
    e.note = "first line\nsecond, with comma; and semicolon \\ backslash\n" + std::string(200, 'x') + " 🎉 " + umlauts;
    const std::string text = pbo::new_contact_vcf(e, "u@pboffice.invalid", kNow);
    for (const std::string& l : physical_lines(text)) PB_CHECK(l.size() <= 75);
    PB_CHECK(pbo::is_valid_utf8(text));
    PB_CHECK(contains_line(text, "N:Meier\\, Schulze\\; \\\\ Co;Zoë;;;"));
    PB_CHECK(contains_line(text, "ORG:Chi\\, Psi & Co\\; Ltd"));
    // what was written reads back as written
    const auto card = pbo::parse_vcard(text);
    PB_CHECK(card.readable);
    PB_CHECK_EQ(card.family, std::string("Meier, Schulze; \\ Co"));
    PB_CHECK_EQ(card.given, std::string("Zoë"));
    PB_CHECK_EQ(card.org, std::string("Chi, Psi & Co; Ltd"));
    PB_CHECK_EQ(card.note, e.note);
}

PB_TEST(a_new_card_reads_back_as_the_same_edit) {
    pbo::ContactEdit e;
    e.given = "Zoë";
    e.family = "Ünal";
    e.org = "Beispiel GmbH";
    e.phones = {lv("mobile", "+49 170 555 0120"), lv("work", "030 123456"), lv("other", "+1 555 0100")};
    e.emails = {lv("work", "zoe@example.org"), lv("home", "zoe.home@example.org")};
    pbo::PostalAddress a;
    a.label = "work";
    a.street = "2 Example Lane";
    a.city = "Exampletown";
    a.postcode = "54321";
    pbo::PostalAddress b;
    b.label = "home";
    b.street = "3 Example Road";
    b.country = "Exampleland";
    e.addresses = {a, b};
    e.note = "Umlaute: ä ö ü ß 🙂\nsecond line";
    const std::string text = pbo::new_contact_vcf(e, "u@pboffice.invalid", kNow);
    const auto card = pbo::parse_vcard(text);
    PB_CHECK(card.readable);
    const pbo::ContactEdit back = pbo::contact_edit_of(card);
    PB_CHECK_EQ(back.given, e.given);
    PB_CHECK_EQ(back.family, e.family);
    PB_CHECK_EQ(back.org, e.org);
    PB_CHECK_EQ(back.phones.size(), e.phones.size());
    for (size_t i = 0; i < e.phones.size(); ++i) {
        PB_CHECK_EQ(back.phones[i].label, e.phones[i].label);
        PB_CHECK_EQ(back.phones[i].value, e.phones[i].value);
        PB_CHECK_EQ(back.phones[i].orig_index, static_cast<int>(i));
    }
    PB_CHECK_EQ(back.emails.size(), e.emails.size());
    for (size_t i = 0; i < e.emails.size(); ++i) {
        PB_CHECK_EQ(back.emails[i].label, e.emails[i].label);
        PB_CHECK_EQ(back.emails[i].value, e.emails[i].value);
    }
    PB_CHECK_EQ(back.addresses.size(), size_t(2));
    PB_CHECK_EQ(back.addresses[0].street, std::string("2 Example Lane"));
    PB_CHECK_EQ(back.addresses[0].postcode, std::string("54321"));
    PB_CHECK_EQ(back.addresses[0].label, std::string("work"));
    PB_CHECK_EQ(back.addresses[1].street, std::string("3 Example Road"));
    PB_CHECK_EQ(back.addresses[1].country, std::string("Exampleland"));
    PB_CHECK_EQ(back.note, e.note);
    // the derived texts of the new card
    const pbo::ContactFields f = pbo::contact_derive(card);
    PB_CHECK_EQ(f.display_name, std::string("Zoë Ünal"));
    PB_CHECK_EQ(f.emails.size(), size_t(2));
}

PB_TEST(entries_with_nothing_in_them_are_not_written) {
    pbo::ContactEdit e;
    e.given = "Anna";
    e.phones = {lv("mobile", "  "), lv("work", "+1 555 0100")};
    e.emails = {lv("work", "")};
    pbo::PostalAddress empty;
    e.addresses.push_back(empty);
    const std::string text = pbo::new_contact_vcf(e, "u@pboffice.invalid", kNow);
    PB_CHECK(text.find("EMAIL") == std::string::npos);
    PB_CHECK(text.find("ADR") == std::string::npos);
    PB_CHECK(text.find("NOTE") == std::string::npos);
    const auto card = pbo::parse_vcard(text);
    PB_CHECK_EQ(card.phones.size(), size_t(1));
}

// ---- the lossless edit of a card that exists (US3) ----------------------------------------------------------------------------------------------

namespace {

std::string slurp(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) PB_FAIL_AT("cannot read " + path);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::string fixture(const std::string& name) { return slurp(std::string(PBO_FIXTURE_DIR) + "/vcard/" + name); }

std::vector<std::string> fixture_names() {
    std::vector<std::string> out;
    for (const std::string& sub : {std::string(), std::string("corpus/")}) {
        const std::string dir = std::string(PBO_FIXTURE_DIR) + "/vcard/" + sub;
        if (DIR* d = opendir(dir.c_str())) {
            while (dirent* e = readdir(d)) {
                const std::string n = e->d_name;
                if (n.size() > 4 && n.compare(n.size() - 4, 4, ".vcf") == 0) out.push_back(sub + n);
            }
            closedir(d);
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

pbo::ContactEdit edit_of(const std::string& text) { return pbo::contact_edit_of(pbo::parse_vcard(text)); }

std::string apply_ok(const std::string& text, const pbo::ContactEdit& edit) {
    auto r = pbo::apply_contact_edit(text, edit, kNow);
    if (!r.ok()) PB_FAIL_AT("apply_contact_edit failed: " + r.error.key);
    return r.value;
}

// the content lines of a card (unfolded, no line ends), in order
std::vector<std::string> content_lines(const std::string& text) {
    std::vector<std::string> out;
    for (const auto& l : pbo::parse_vcard(text).lines) {
        std::string raw = l.raw;
        while (!raw.empty() && (raw.back() == '\r' || raw.back() == '\n')) raw.pop_back();
        if (!raw.empty()) out.push_back(raw);
    }
    return out;
}

std::multiset<std::string> as_multiset(const std::vector<std::string>& v) { return std::multiset<std::string>(v.begin(), v.end()); }

// what the edit removed and what it added, as lines
void diff(const std::string& before, const std::string& after, std::vector<std::string>* removed, std::vector<std::string>* added) {
    std::multiset<std::string> a = as_multiset(content_lines(before)), b = as_multiset(content_lines(after));
    for (const auto& l : a) if (b.count(l) < a.count(l) && std::find(removed->begin(), removed->end(), l) == removed->end()) removed->push_back(l);
    for (const auto& l : b) if (a.count(l) < b.count(l) && std::find(added->begin(), added->end(), l) == added->end()) added->push_back(l);
}

bool has_prefix_line(const std::vector<std::string>& lines, const std::string& prefix) {
    return std::any_of(lines.begin(), lines.end(), [&](const std::string& l) { return l.compare(0, prefix.size(), prefix) == 0; });
}

}  // namespace

PB_TEST(an_edit_that_changes_nothing_returns_the_card_byte_for_byte_for_every_fixture) {
    const auto names = fixture_names();
    PB_CHECK(names.size() >= 30);
    for (const std::string& n : names) {
        const std::string text = fixture(n);
        const auto card = pbo::parse_vcard(text);
        if (!card.readable) continue;
        auto r = pbo::apply_contact_edit(text, pbo::contact_edit_of(card), kNow);
        PB_CHECK(r.ok());
        if (r.value != text) PB_FAIL_AT("an edit without a change altered " + n);
    }
}

PB_TEST(changing_one_phone_number_rewrites_that_line_and_the_revision_and_nothing_else) {
    const std::string text = fixture("nextcloud_sabre_30.unverified.vcf");
    pbo::ContactEdit e = edit_of(text);
    e.phones[1].value = "+1 555 0199";
    const std::string out = apply_ok(text, e);
    std::vector<std::string> removed, added;
    diff(text, out, &removed, &added);
    PB_CHECK_EQ(removed.size(), size_t(2));
    PB_CHECK_EQ(added.size(), size_t(2));
    PB_CHECK(std::find(removed.begin(), removed.end(), "TEL;TYPE=CELL:+1 555 0101") != removed.end());
    PB_CHECK(std::find(removed.begin(), removed.end(), "REV:20260101T120000Z") != removed.end());
    PB_CHECK(std::find(added.begin(), added.end(), "TEL;TYPE=CELL:+1 555 0199") != added.end());
    PB_CHECK(std::find(added.begin(), added.end(), "REV:20261014T120000Z") != added.end());
    // the line stayed where it was
    const auto lines = content_lines(out);
    const auto before = content_lines(text);
    PB_CHECK_EQ(lines.size(), before.size());
    for (size_t i = 0; i < lines.size(); ++i) PB_CHECK_EQ(lines[i].compare(0, 3, "TEL") == 0, before[i].compare(0, 3, "TEL") == 0);
    PB_CHECK(pbo::parse_vcard(out).readable);
}

PB_TEST(changing_a_label_writes_the_new_type_and_drops_the_old_group_label) {
    const std::string text = fixture("apple_groups_30.unverified.vcf");
    pbo::ContactEdit e = edit_of(text);
    PB_CHECK_EQ(e.phones[1].label, std::string("Mother"));
    e.phones[1].label = "work";   // item3.TEL with item3.X-ABLabel:_$!<Mother>!$_
    const std::string out = apply_ok(text, e);
    const auto lines = content_lines(out);
    PB_CHECK(!has_prefix_line(lines, "item3.X-ABLabel"));
    const auto back = edit_of(out);
    PB_CHECK_EQ(back.phones[1].label, std::string("work"));
    PB_CHECK_EQ(back.phones[1].value, std::string("+1 555 0111"));
    PB_CHECK_EQ(back.phones[0].label, std::string("mobile"));   // the other phone is untouched
    PB_CHECK(has_prefix_line(lines, "item1.X-ABLabel"));       // and so are the other groups
}

PB_TEST(a_changed_value_keeps_its_group_and_label) {
    const std::string text = fixture("apple_groups_30.unverified.vcf");
    pbo::ContactEdit e = edit_of(text);
    e.emails[1].value = "max.neu@example.org";   // item2.EMAIL with item2.X-ABLabel:Private mail
    const std::string out = apply_ok(text, e);
    const auto back = edit_of(out);
    PB_CHECK_EQ(back.emails[1].value, std::string("max.neu@example.org"));
    PB_CHECK_EQ(back.emails[1].label, std::string("Private mail"));
    PB_CHECK(has_prefix_line(content_lines(out), "item2.X-ABLabel:Private mail"));
}

PB_TEST(removing_an_entry_takes_its_line_and_its_whole_group_with_it) {
    const std::string text = fixture("apple_groups_30.unverified.vcf");
    pbo::ContactEdit e = edit_of(text);
    e.addresses.clear();   // item4.ADR, item4.X-ABADR, item4.X-ABLabel
    e.emails.erase(e.emails.begin());   // item1.EMAIL, item1.X-ABLabel
    const std::string out = apply_ok(text, e);
    const auto lines = content_lines(out);
    for (const char* gone : {"item4.", "item1."}) PB_CHECK(!has_prefix_line(lines, gone));
    PB_CHECK(has_prefix_line(lines, "item2.EMAIL"));
    PB_CHECK(has_prefix_line(lines, "item3.TEL"));
    PB_CHECK(has_prefix_line(lines, "BDAY:1990-11-02"));
    PB_CHECK(has_prefix_line(lines, "X-SOCIALPROFILE"));
    const auto back = edit_of(out);
    PB_CHECK_EQ(back.emails.size(), size_t(1));
    PB_CHECK(back.addresses.empty());
}

PB_TEST(new_entries_are_added_with_the_documented_types) {
    const std::string text = fixture("nextcloud_sabre_30.unverified.vcf");
    pbo::ContactEdit e = edit_of(text);
    e.phones.push_back(lv("home", "+1 555 0177"));
    e.emails.push_back(lv("other", "other@example.org"));
    pbo::PostalAddress a;
    a.label = "work";
    a.street = "9 Example Alley";
    a.city = "Exampletown";
    e.addresses.push_back(a);
    const std::string out = apply_ok(text, e);
    const auto lines = content_lines(out);
    PB_CHECK(std::find(lines.begin(), lines.end(), "TEL;TYPE=HOME:+1 555 0177") != lines.end());
    PB_CHECK(std::find(lines.begin(), lines.end(), "EMAIL:other@example.org") != lines.end());
    PB_CHECK(std::find(lines.begin(), lines.end(), "ADR;TYPE=WORK:;;9 Example Alley;Exampletown;;;") != lines.end());
    const auto back = edit_of(out);
    PB_CHECK_EQ(back.phones.size(), size_t(3));
    PB_CHECK_EQ(back.emails.size(), size_t(3));
    PB_CHECK_EQ(back.addresses.size(), size_t(2));
    PB_CHECK_EQ(lines.back(), std::string("END:VCARD"));
}

PB_TEST(a_new_name_changes_family_and_given_only_and_rebuilds_the_formatted_name) {
    const std::string text = fixture("android_30.unverified.vcf");   // N:Schulze;Karl;Heinz;Dr.;jun.
    pbo::ContactEdit e = edit_of(text);
    e.family = "Neumann";
    const std::string out = apply_ok(text, e);
    const auto lines = content_lines(out);
    PB_CHECK(std::find(lines.begin(), lines.end(), "N:Neumann;Karl;Heinz;Dr.;jun.") != lines.end());   // prefix, suffix and additional name stay
    PB_CHECK(std::find(lines.begin(), lines.end(), "FN:Dr. Karl Heinz Neumann jun.") != lines.end());
    // a change of the organization alone also rewrites FN (it is derived from name and organization)
    pbo::ContactEdit o = edit_of(fixture("nextcloud_sabre_30.unverified.vcf"));
    o.org = "Other Works";
    const auto olines = content_lines(apply_ok(fixture("nextcloud_sabre_30.unverified.vcf"), o));
    PB_CHECK(std::find(olines.begin(), olines.end(), "ORG:Other Works;Sales") != olines.end());   // the department stays
    PB_CHECK(std::find(olines.begin(), olines.end(), "FN:Anna Beispiel") != olines.end());
}

PB_TEST(a_card_without_a_name_gets_one_and_removing_the_organization_removes_the_line) {
    const std::string nameless = fixture("nameless_org.unverified.vcf");
    pbo::ContactEdit e = edit_of(nameless);
    e.given = "Hans";
    e.family = "Laden";
    auto lines = content_lines(apply_ok(nameless, e));
    PB_CHECK(std::find(lines.begin(), lines.end(), "N:Laden;Hans;;;") != lines.end());
    PB_CHECK(std::find(lines.begin(), lines.end(), "FN:Hans Laden") != lines.end());
    pbo::ContactEdit g = edit_of(nameless);
    g.org.clear();
    g.given = "Hans";
    lines = content_lines(apply_ok(nameless, g));
    PB_CHECK(!has_prefix_line(lines, "ORG"));
}

PB_TEST(the_note_is_replaced_added_removed_or_kept_when_locked) {
    const std::string text = fixture("nextcloud_sabre_30.unverified.vcf");
    pbo::ContactEdit e = edit_of(text);
    e.note = "Changed, with comma\nsecond line";
    auto lines = content_lines(apply_ok(text, e));
    PB_CHECK(std::find(lines.begin(), lines.end(), "NOTE:Changed\\, with comma\\nsecond line") != lines.end());
    e.note.clear();
    PB_CHECK(!has_prefix_line(content_lines(apply_ok(text, e)), "NOTE"));
    const std::string no_note = fixture("apple_groups_30.unverified.vcf");
    pbo::ContactEdit f = edit_of(no_note);
    f.note = "added";
    PB_CHECK(has_prefix_line(content_lines(apply_ok(no_note, f)), "NOTE:added"));
    // a locked note is never touched, a removed locked note goes
    const std::string big = slurp(std::string(PBO_FIXTURE_DIR) + "/vcard/corpus/" + [] {
        for (const std::string& n : fixture_names())
            if (n.find("over_limit_note") != std::string::npos) return n.substr(7);
        return std::string();
    }());
    pbo::ContactEdit g = edit_of(big);
    PB_CHECK(g.note_locked);
    g.family = "Neu";
    const std::string kept = apply_ok(big, g);
    PB_CHECK(has_prefix_line(content_lines(kept), "NOTE:long note text"));
    g.note_locked = false;
    g.note.clear();
    PB_CHECK(!has_prefix_line(content_lines(apply_ok(big, g)), "NOTE"));
}

PB_TEST(everything_the_edit_does_not_touch_survives_byte_for_byte_for_every_fixture) {
    for (const std::string& n : fixture_names()) {
        const std::string text = fixture(n);
        const auto card = pbo::parse_vcard(text);
        if (!card.readable || card.phones.empty()) continue;
        pbo::ContactEdit e = pbo::contact_edit_of(card);
        if (e.phones[0].locked) continue;
        e.phones[0].value = "+1 555 9999";
        auto r = pbo::apply_contact_edit(text, e, kNow);
        PB_CHECK(r.ok());
        // every line of the card except the changed phone line and REV is still there, once
        std::multiset<std::string> before = as_multiset(content_lines(text)), after = as_multiset(content_lines(r.value));
        const std::string phone_line = [&] {
            std::string raw = card.lines[static_cast<size_t>(card.phone_lines[0])].raw;
            while (!raw.empty() && (raw.back() == '\r' || raw.back() == '\n')) raw.pop_back();
            return raw;
        }();
        auto drop = [&](std::multiset<std::string>* set, const std::string& line) {
            auto it = set->find(line);
            if (it != set->end()) set->erase(it);
        };
        drop(&before, phone_line);
        for (const std::string& l : std::vector<std::string>(before.begin(), before.end()))
            if (l.compare(0, 4, "REV:") == 0) drop(&before, l);
        for (const std::string& l : std::vector<std::string>(after.begin(), after.end()))
            if (l.compare(0, 4, "REV:") == 0) drop(&after, l);
        std::vector<std::string> lost;
        for (const std::string& l : before)
            if (after.count(l) < before.count(l)) lost.push_back(l.substr(0, 40));
        if (!lost.empty()) PB_FAIL_AT(n + ": the edit lost the line " + lost.front());
    }
}

PB_TEST(a_locked_entry_stays_and_can_be_removed) {
    const std::string text = [] {
        for (const std::string& n : fixture_names())
            if (n.find("over_limit_phone") != std::string::npos) return slurp(std::string(PBO_FIXTURE_DIR) + "/vcard/" + n);
        return std::string();
    }();
    pbo::ContactEdit e = edit_of(text);
    PB_CHECK(e.phones[0].locked);
    e.phones[1].value = "+1 555 9999";   // the other phone is changed, the locked one is only carried along
    const std::string out = apply_ok(text, e);
    const auto lines = content_lines(out);
    PB_CHECK(std::any_of(lines.begin(), lines.end(), [](const std::string& l) { return l.find("ext 9999999999999999") != std::string::npos; }));
    e.phones.erase(e.phones.begin());   // removed
    const auto gone = content_lines(apply_ok(text, e));
    PB_CHECK(!std::any_of(gone.begin(), gone.end(), [](const std::string& l) { return l.find("ext 99999999") != std::string::npos; }));
    PB_CHECK_EQ(edit_of(apply_ok(text, e)).phones.size(), size_t(1));
}

PB_TEST(the_line_ends_and_the_version_of_the_card_are_kept) {
    const std::string lf = fixture("nextcloud_sabre_30.unverified.vcf");
    std::string unix_text;
    for (char c : lf) if (c != '\r') unix_text += c;
    pbo::ContactEdit e = edit_of(unix_text);
    e.phones[0].value = "+1 555 0188";
    const std::string out = apply_ok(unix_text, e);
    PB_CHECK(out.find('\r') == std::string::npos);
    PB_CHECK(out.find("VERSION:3.0\n") != std::string::npos);
    const std::string v4 = fixture("thunderbird_40.unverified.vcf");
    pbo::ContactEdit f = edit_of(v4);
    f.family = "Neu";
    const std::string out4 = apply_ok(v4, f);
    PB_CHECK(out4.find("VERSION:4.0") != std::string::npos);
    PB_CHECK(out4.find("NICKNAME:Lenchen") != std::string::npos);
}

PB_TEST(a_card_with_quoted_printable_lines_keeps_them_and_writes_a_changed_one_in_utf8) {
    const std::string text = fixture("outlook_21_qp.unverified.vcf");
    pbo::ContactEdit e = edit_of(text);
    e.emails[0].value = "joerg.neu@example.org";
    const std::string out = apply_ok(text, e);
    PB_CHECK(out.find("N;CHARSET=ISO-8859-1;ENCODING=QUOTED-PRINTABLE:M=FCller;J=F6rg;;;") != std::string::npos);   // untouched, as it was
    PB_CHECK(out.find("EMAIL") != std::string::npos);
    PB_CHECK(out.find("joerg.neu@example.org") != std::string::npos);
    pbo::ContactEdit f = edit_of(text);
    f.family = "Müller-Neu";
    const std::string out2 = apply_ok(text, f);
    const auto card = pbo::parse_vcard(out2);
    PB_CHECK(card.readable);
    PB_CHECK_EQ(card.family, std::string("Müller-Neu"));
    PB_CHECK_EQ(card.given, std::string("Jörg"));
    PB_CHECK(pbo::is_valid_utf8(out2));
}

PB_TEST(an_unreadable_card_is_not_edited) {
    auto r = pbo::apply_contact_edit("not a card\r\n", pbo::ContactEdit(), kNow);
    PB_CHECK(!r.ok());
    PB_CHECK_EQ(r.error.key, std::string("unreadable"));
    auto two = pbo::apply_contact_edit("BEGIN:VCARD\r\nUID:a\r\nFN:A\r\nEND:VCARD\r\nBEGIN:VCARD\r\nUID:b\r\nFN:B\r\nEND:VCARD\r\n", pbo::ContactEdit(), kNow);
    PB_CHECK(!two.ok());
}

// ---- the merge of an edited card into the full card of the server (research R-05) --------------------------------------------------------------

PB_TEST(an_edit_of_a_stripped_card_is_merged_into_the_full_card_and_the_photo_survives) {
    const std::string full = fixture("photo_card.unverified.vcf");   // PHOTO, BDAY, URL, X-CUSTOM-FIELD
    bool removed = false;
    const std::string stored = pbo::strip_heavy_properties(full, &removed);   // what the reader keeps
    PB_CHECK(removed);
    pbo::ContactEdit e = edit_of(stored);
    e.family = "Neu";
    e.phones.push_back(lv("mobile", "+1 555 0123"));
    const std::string edited = apply_ok(stored, e);
    const std::string merged = pbo::merge_into_full_card(edited, full);
    PB_CHECK(merged.find("PHOTO;ENCODING=b;TYPE=JPEG:") != std::string::npos);   // the photo is on the card again
    PB_CHECK(merged.find("BDAY:2000-01-31") != std::string::npos);
    PB_CHECK(merged.find("X-CUSTOM-FIELD:stays") != std::string::npos);
    PB_CHECK(merged.find("N:Neu;Pia;;;") != std::string::npos);                 // and the edit is in
    PB_CHECK(merged.find("TEL;TYPE=CELL:+1 555 0123") != std::string::npos);
    PB_CHECK(merged.find("N:Foto;Pia;;;") == std::string::npos);
    const auto card = pbo::parse_vcard(merged);
    PB_CHECK(card.readable);
    PB_CHECK_EQ(card.family, std::string("Neu"));
    // the photo lines are exactly the ones of the full card
    std::string photo_full, photo_merged;
    for (const auto& l : pbo::parse_vcard(full).lines) if (l.name == "PHOTO") photo_full += l.raw;
    for (const auto& l : card.lines) if (l.name == "PHOTO") photo_merged += l.raw;
    PB_CHECK(photo_full == photo_merged);
    PB_CHECK_EQ(card.uid, std::string("7a1c3f10-0000-4000-8000-000000000006"));
}

PB_TEST(properties_that_were_not_asked_for_come_back_and_nothing_appears_twice) {
    // a server that ignored the property list sent everything but the photo: the stored card holds BDAY already
    const std::string full = fixture("photo_card.unverified.vcf");
    const std::string stored = pbo::strip_heavy_properties(full);   // BDAY, URL, X-CUSTOM-FIELD are in it
    pbo::ContactEdit e = edit_of(stored);
    e.org = "New Org";
    const std::string merged = pbo::merge_into_full_card(apply_ok(stored, e), full);
    auto count = [&](const std::string& needle) {
        size_t n = 0, pos = 0;
        while ((pos = merged.find(needle, pos)) != std::string::npos) { ++n; pos += needle.size(); }
        return n;
    };
    PB_CHECK_EQ(count("BDAY:2000-01-31"), size_t(1));
    PB_CHECK_EQ(count("URL:https://www.example.org/pia"), size_t(1));
    PB_CHECK_EQ(count("X-CUSTOM-FIELD:stays"), size_t(1));
    PB_CHECK_EQ(count("PHOTO;"), size_t(1));
    PB_CHECK_EQ(count("BEGIN:VCARD"), size_t(1));
    PB_CHECK_EQ(count("UID:"), size_t(1));
    PB_CHECK(merged.find("ORG:New Org") != std::string::npos);
}

PB_TEST(a_removed_entry_does_not_leave_its_unlisted_group_lines_behind) {
    // the full card has item4.X-ABADR (not in the property list); the stored card has item4.ADR and item4.X-ABLabel
    const std::string full =
        "BEGIN:VCARD\r\nVERSION:3.0\r\nUID:g1\r\nFN:Max Muster\r\nN:Muster;Max;;;\r\nitem4.ADR;type=HOME:;;2 Example Lane;Exampletown;;54321;Exampleland\r\nitem4.X-ABADR:xx\r\n"
        "item4.X-ABLabel:_$!<Home>!$_\r\nitem5.X-SOCIALPROFILE;type=twitter:x-apple:examplemax\r\nPHOTO;ENCODING=b:AAAA\r\nEND:VCARD\r\n";
    const std::string stored =
        "BEGIN:VCARD\r\nVERSION:3.0\r\nUID:g1\r\nFN:Max Muster\r\nN:Muster;Max;;;\r\nitem4.ADR;type=HOME:;;2 Example Lane;Exampletown;;54321;Exampleland\r\nitem4.X-ABLabel:_$!<Home>!$_\r\nEND:VCARD\r\n";
    pbo::ContactEdit e = edit_of(stored);
    e.addresses.clear();
    const std::string merged = pbo::merge_into_full_card(apply_ok(stored, e), full);
    PB_CHECK(merged.find("item4.") == std::string::npos);                       // the whole group is gone, including the unlisted line
    PB_CHECK(merged.find("item5.X-SOCIALPROFILE") != std::string::npos);        // another group, not about an address: stays
    PB_CHECK(merged.find("PHOTO;ENCODING=b:AAAA") != std::string::npos);
}

PB_TEST(an_oversized_line_of_a_listed_property_is_carried_over_too) {
    const std::string huge(70u << 10, '5');
    const std::string full = "BEGIN:VCARD\r\nVERSION:3.0\r\nUID:h1\r\nFN:Hugo\r\nN:;Hugo;;;\r\nTEL;TYPE=WORK:" + huge + "\r\nTEL;TYPE=CELL:+1 555 0101\r\nEND:VCARD\r\n";
    const std::string stored = pbo::strip_heavy_properties(full);   // the 70 KiB line is gone
    PB_CHECK(stored.find(huge) == std::string::npos);
    pbo::ContactEdit e = edit_of(stored);
    e.phones[0].value = "+1 555 0102";
    const std::string merged = pbo::merge_into_full_card(apply_ok(stored, e), full);
    PB_CHECK(merged.find(huge) != std::string::npos);       // the server keeps its giant number
    PB_CHECK(merged.find("+1 555 0102") != std::string::npos);
    PB_CHECK(merged.find("+1 555 0101") == std::string::npos);
}

PB_TEST_MAIN()
