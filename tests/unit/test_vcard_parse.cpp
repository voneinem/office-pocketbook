// T016 (spec 002, research R-05/R-06): the vCard reader. Fixtures are fictional cards written by hand from the specifications (.unverified).
#include <dirent.h>

#include <algorithm>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "pbo/charset.h"
#include "pbo/utf8.h"
#include "pbo/vcard.h"
#include "test_support.h"

namespace {

std::string slurp(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) PB_FAIL_AT("cannot read " + path);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::string fixture(const std::string& name) { return slurp(std::string(PBO_FIXTURE_DIR) + "/vcard/" + name); }

std::string corpus(const std::string& suffix) {
    const std::string dir = std::string(PBO_FIXTURE_DIR) + "/vcard/corpus";
    if (DIR* d = opendir(dir.c_str())) {
        std::vector<std::string> names;
        while (dirent* e = readdir(d)) names.push_back(e->d_name);
        closedir(d);
        for (const std::string& n : names)
            if (n.size() > suffix.size() && n.find("_" + suffix + ".unverified.vcf") != std::string::npos) return slurp(dir + "/" + n);
    }
    PB_FAIL_AT("no corpus card named " + suffix);
    return std::string();
}

std::string card(const std::string& body, const std::string& version = "3.0") {
    return "BEGIN:VCARD\r\nVERSION:" + version + "\r\nUID:test-uid-1\r\n" + body + "END:VCARD\r\n";
}

}  // namespace

PB_TEST(a_sabre_style_card_gives_every_field) {
    const auto c = pbo::parse_vcard(fixture("nextcloud_sabre_30.unverified.vcf"));
    PB_CHECK(c.readable);
    PB_CHECK_EQ(c.problem, std::string());
    PB_CHECK_EQ(c.version, std::string("3.0"));
    PB_CHECK_EQ(c.uid, std::string("7a1c3f10-0000-4000-8000-000000000001"));
    PB_CHECK_EQ(c.fn, std::string("Anna Beispiel"));
    PB_CHECK_EQ(c.family, std::string("Beispiel"));
    PB_CHECK_EQ(c.given, std::string("Anna"));
    PB_CHECK_EQ(c.org, std::string("Example Works GmbH"));   // the first component only
    PB_CHECK_EQ(c.phones.size(), static_cast<size_t>(2));
    PB_CHECK_EQ(c.phones[0].value, std::string("+1 555 0100"));
    PB_CHECK_EQ(c.phones[0].label, std::string("work"));
    PB_CHECK_EQ(c.phones[0].orig_index, 0);
    PB_CHECK_EQ(c.phones[1].label, std::string("mobile"));
    PB_CHECK_EQ(c.phones[1].orig_index, 1);
    PB_CHECK_EQ(c.emails.size(), static_cast<size_t>(2));
    PB_CHECK_EQ(c.emails[0].value, std::string("anna@example.org"));
    PB_CHECK_EQ(c.emails[0].label, std::string("work"));
    PB_CHECK_EQ(c.emails[1].label, std::string("home"));
    PB_CHECK_EQ(c.addresses.size(), static_cast<size_t>(1));
    PB_CHECK_EQ(c.addresses[0].label, std::string("work"));
    PB_CHECK_EQ(c.addresses[0].street, std::string("1 Example Street"));
    PB_CHECK_EQ(c.addresses[0].city, std::string("Exampletown"));
    PB_CHECK_EQ(c.addresses[0].region, std::string("EX"));
    PB_CHECK_EQ(c.addresses[0].postcode, std::string("12345"));
    PB_CHECK_EQ(c.addresses[0].country, std::string("Exampleland"));
    PB_CHECK_EQ(c.note, std::string("Met at the fair.\nSecond line."));
}

PB_TEST(every_line_of_the_text_is_kept_so_a_writer_can_reproduce_it_byte_for_byte) {
    const std::string text = fixture("nextcloud_sabre_30.unverified.vcf");
    const auto c = pbo::parse_vcard(text);
    std::string joined;
    for (const auto& l : c.lines) joined += l.raw;
    PB_CHECK(joined == text);
    PB_CHECK(c.begin_line >= 0 && c.end_line > c.begin_line);
    PB_CHECK_EQ(c.lines[static_cast<size_t>(c.fn_line)].name, std::string("FN"));
    PB_CHECK_EQ(c.lines[static_cast<size_t>(c.phone_lines[1])].value, std::string("+1 555 0101"));
}

PB_TEST(labels_follow_the_type_parameters_in_every_spelling) {
    const auto c = pbo::parse_vcard(card(
        "TEL;TYPE=HOME,VOICE,PREF:1\r\nTEL;TYPE=WORK,FAX:2\r\nTEL;TYPE=PAGER:3\r\nTEL;CELL;VOICE:4\r\nTEL;type=cell;type=VOICE;type=pref:5\r\n"
        "TEL;TYPE=\"work,voice\":6\r\nTEL:7\r\nTEL;TYPE=X-CUSTOM:8\r\nEMAIL;TYPE=INTERNET,HOME:a@example.org\r\nEMAIL;TYPE=INTERNET:b@example.org\r\n"));
    PB_CHECK(c.readable);
    const char* want[] = {"home", "fax", "pager", "mobile", "mobile", "work", "other", "x-custom"};
    PB_CHECK_EQ(c.phones.size(), static_cast<size_t>(8));
    for (size_t i = 0; i < 8; ++i) PB_CHECK_EQ(c.phones[i].label, std::string(want[i]));
    PB_CHECK_EQ(c.emails[0].label, std::string("home"));
    PB_CHECK_EQ(c.emails[1].label, std::string("other"));
}

PB_TEST(apple_group_labels_win_over_the_type_and_keep_their_own_text) {
    const auto c = pbo::parse_vcard(fixture("apple_groups_30.unverified.vcf"));
    PB_CHECK(c.readable);
    PB_CHECK_EQ(c.emails.size(), static_cast<size_t>(2));
    PB_CHECK_EQ(c.emails[0].label, std::string("work"));            // _$!<Work>!$_
    PB_CHECK_EQ(c.emails[1].label, std::string("Private mail"));    // a custom label, as written
    PB_CHECK_EQ(c.phones.size(), static_cast<size_t>(2));
    PB_CHECK_EQ(c.phones[0].label, std::string("mobile"));
    PB_CHECK_EQ(c.phones[1].label, std::string("Mother"));          // _$!<Mother>!$_
    PB_CHECK_EQ(c.addresses.size(), static_cast<size_t>(1));
    PB_CHECK_EQ(c.addresses[0].label, std::string("home"));
    PB_CHECK_EQ(c.addresses[0].street, std::string("2 Example Lane"));
    PB_CHECK_EQ(c.lines[static_cast<size_t>(c.email_lines[0])].group, std::string("item1"));
}

PB_TEST(vcard_4_uri_telephone_numbers_lose_the_scheme) {
    const auto c = pbo::parse_vcard(fixture("thunderbird_40.unverified.vcf"));
    PB_CHECK(c.readable);
    PB_CHECK_EQ(c.version, std::string("4.0"));
    PB_CHECK_EQ(c.uid, std::string("urn:uuid:7a1c3f10-0000-4000-8000-000000000004"));
    PB_CHECK_EQ(c.phones.size(), static_cast<size_t>(1));
    PB_CHECK_EQ(c.phones[0].value, std::string("+1-555-0130"));
    PB_CHECK_EQ(c.phones[0].label, std::string("mobile"));
    PB_CHECK_EQ(c.fn, std::string("Lena Köhler"));
    PB_CHECK_EQ(c.note, std::string("Umlaute: ä ö ü ß"));
}

PB_TEST(text_escapes_are_removed_in_values_and_components) {
    const auto c = pbo::parse_vcard(corpus("escaped_characters"));
    PB_CHECK(c.readable);
    PB_CHECK_EQ(c.org, std::string("Chi, Psi & Co; Ltd"));
    PB_CHECK_EQ(c.note, std::string("back\\slash, comma; semicolon\nnew line"));
}

PB_TEST(folded_lines_are_joined_with_a_space_or_a_tab_and_all_line_ends_are_understood) {
    const std::string folded = "BEGIN:VCARD\r\nVERSION:3.0\r\nUID:u\r\nFN:Fold\r\n ed Name\r\nNOTE:one\r\n\ttwo\r\n three\r\nEND:VCARD\r\n";
    const auto a = pbo::parse_vcard(folded);
    PB_CHECK_EQ(a.fn, std::string("Folded Name"));
    PB_CHECK_EQ(a.note, std::string("onetwothree"));
    std::string lf = folded, cr = folded;
    lf.erase(std::remove(lf.begin(), lf.end(), '\r'), lf.end());
    std::replace(cr.begin(), cr.end(), '\n', '\r');
    cr.erase(std::unique(cr.begin(), cr.end(), [](char x, char y) { return x == '\r' && y == '\r'; }), cr.end());
    for (const std::string& t : {lf, cr}) {
        const auto b = pbo::parse_vcard(t);
        PB_CHECK(b.readable);
        PB_CHECK_EQ(b.fn, std::string("Folded Name"));
        PB_CHECK_EQ(b.note, std::string("onetwothree"));
    }
}

PB_TEST(a_vcard_2_1_card_with_quoted_printable_and_a_charset_is_decoded) {
    const auto c = pbo::parse_vcard(fixture("outlook_21_qp.unverified.vcf"));
    PB_CHECK(c.readable);
    PB_CHECK_EQ(c.family, std::string("Müller"));
    PB_CHECK_EQ(c.given, std::string("Jörg"));
    PB_CHECK_EQ(c.fn, std::string("Jörg Müller"));
    PB_CHECK_EQ(c.note, std::string("Zeile eins\nZeile zwei"));
    PB_CHECK_EQ(c.phones[0].label, std::string("mobile"));
    PB_CHECK_EQ(c.phones[1].label, std::string("fax"));
    PB_CHECK(pbo::is_valid_utf8(c.fn));
}

PB_TEST(po_box_and_extended_address_are_shown_joined_to_the_street) {
    const auto c = pbo::parse_vcard(corpus("pobox_and_extended_address"));
    PB_CHECK_EQ(c.addresses.size(), static_cast<size_t>(1));
    PB_CHECK_EQ(c.addresses[0].street, std::string("PO Box 7 Suite 2 7 Example Plaza"));
    PB_CHECK_EQ(c.addresses[0].postcode, std::string("44444"));
}

PB_TEST(the_display_name_follows_the_fallback_order) {
    auto name = [](const std::string& body) { return pbo::contact_derive(pbo::parse_vcard(card(body))).display_name; };
    PB_CHECK_EQ(name("FN:Full Name\r\nN:Family;Given;;;\r\nORG:Org\r\n"), std::string("Full Name"));
    PB_CHECK_EQ(name("N:Family;Given;;;\r\nORG:Org\r\n"), std::string("Given Family"));
    PB_CHECK_EQ(name("N:;Mama;;;\r\n"), std::string("Mama"));
    PB_CHECK_EQ(name("ORG:Org Only\r\nEMAIL:a@example.org\r\n"), std::string("Org Only"));
    PB_CHECK_EQ(name("EMAIL:a@example.org\r\nTEL:+1 555 0000\r\n"), std::string("a@example.org"));
    PB_CHECK_EQ(name("TEL:+1 555 0000\r\n"), std::string("+1 555 0000"));
    PB_CHECK_EQ(name("NOTE:only a note\r\n"), std::string("(no name)"));
    PB_CHECK_EQ(name("FN:   \r\nN:Family;;;;\r\n"), std::string("Family"));
}

PB_TEST(the_list_text_the_sort_key_and_the_search_text_are_derived) {
    const auto f = pbo::contact_derive(pbo::parse_vcard(fixture("nextcloud_sabre_30.unverified.vcf")));
    PB_CHECK_EQ(f.display_name, std::string("Anna Beispiel"));
    PB_CHECK_EQ(f.family, std::string("Beispiel"));
    PB_CHECK_EQ(f.given, std::string("Anna"));
    PB_CHECK_EQ(f.org, std::string("Example Works GmbH"));
    PB_CHECK_EQ(f.subtitle, std::string("Example Works GmbH"));   // a contact with a name: the organization, else the first email, else the first phone
    PB_CHECK(f.search_text.find("beispiel") != std::string::npos);
    PB_CHECK(f.search_text.find("anna") != std::string::npos);
    PB_CHECK(f.search_text.find("example works") != std::string::npos);
    PB_CHECK(f.search_text.find("anna@example.org") != std::string::npos);
    PB_CHECK(f.search_text.find("anna.home@example.org") != std::string::npos);
    PB_CHECK(f.search_text.find("+15550100") != std::string::npos);   // phone numbers reduced to digits and +
    PB_CHECK(f.search_text.find("555 0100") == std::string::npos);
    PB_CHECK_EQ(f.emails.size(), static_cast<size_t>(2));
    PB_CHECK_EQ(f.emails[0].addr, std::string("anna@example.org"));
    PB_CHECK_EQ(f.emails[0].label, std::string("work"));

    // a nameless card is listed under its organization, then email, then phone; the subtitle is the next thing that differs from the title
    const auto org = pbo::contact_derive(pbo::parse_vcard(fixture("nameless_org.unverified.vcf")));
    PB_CHECK_EQ(org.display_name, std::string("Example Hardware Store"));
    PB_CHECK_EQ(org.subtitle, std::string("shop@example.org"));
    const auto phone = pbo::contact_derive(pbo::parse_vcard(corpus("nameless_phone_only")));
    PB_CHECK_EQ(phone.display_name, std::string("+1 555 0210"));
    PB_CHECK_EQ(phone.subtitle, std::string());
}

PB_TEST(the_sort_key_orders_by_family_then_given_name_with_umlauts_folded) {
    auto key = [](const std::string& body) { return pbo::contact_derive(pbo::parse_vcard(card(body))).sort_key; };
    const std::string anna = key("FN:Anna Beispiel\r\nN:Beispiel;Anna;;;\r\n");
    const std::string bert = key("FN:Bert Beispiel\r\nN:Beispiel;Bert;;;\r\n");
    const std::string aal = key("FN:Zoe Aal\r\nN:Aal;Zoe;;;\r\n");
    const std::string umlaut = key("FN:Uta Ärger\r\nN:Ärger;Uta;;;\r\n");
    const std::string zed = key("FN:Zed Zimmer\r\nN:Zimmer;Zed;;;\r\n");
    PB_CHECK(aal < umlaut);          // "Aal" before "Ärger" (folded to "arger")
    PB_CHECK(umlaut < anna);
    PB_CHECK(anna < bert);
    PB_CHECK(bert < zed);
    // a card with only a given name sorts under that name, not before everything
    const std::string mama = key("FN:Mama\r\nN:;Mama;;;\r\n");
    const std::string leo = key("FN:Leo Lang\r\nN:Lang;Leo;;;\r\n");
    const std::string max = key("FN:Max Muster\r\nN:Muster;Max;;;\r\n");
    PB_CHECK(leo < mama);
    PB_CHECK(mama < max);
    // a card with FN only
    PB_CHECK(!key("FN:Only A Formatted Name\r\n").empty());
    // nameless cards sort by organization, email, phone
    PB_CHECK_EQ(key("ORG:Zeta Org\r\n"), key("ORG:zeta org\r\n"));
}

PB_TEST(heavy_properties_are_removed_and_everything_else_stays_byte_for_byte) {
    const std::string text = fixture("photo_card.unverified.vcf");
    PB_CHECK(text.size() > 20000);
    bool removed = false;
    const std::string out = pbo::strip_heavy_properties(text, &removed);
    PB_CHECK(removed);
    PB_CHECK(out.find("PHOTO") == std::string::npos);
    PB_CHECK(out.size() < 600);
    PB_CHECK(out.find("FN:Pia Foto\r\n") != std::string::npos);
    PB_CHECK(out.find("X-CUSTOM-FIELD:stays\r\n") != std::string::npos);
    PB_CHECK(out.find("BDAY:2000-01-31\r\n") != std::string::npos);
    // the lines that remain are exactly the lines of the original that are not part of PHOTO
    const auto before = pbo::parse_vcard(text);
    const auto after = pbo::parse_vcard(out);
    std::string expect;
    for (const auto& l : before.lines)
        if (l.name != "PHOTO") expect += l.raw;
    PB_CHECK(out == expect);
    PB_CHECK(after.readable);
    PB_CHECK_EQ(after.emails[0].value, std::string("pia@example.org"));

    // nothing to remove: the text comes back unchanged and the flag stays false
    bool again = true;
    const std::string plain = fixture("nameless_org.unverified.vcf");
    PB_CHECK(pbo::strip_heavy_properties(plain, &again) == plain);
    PB_CHECK(!again);
}

PB_TEST(logo_sound_key_grouped_photos_and_oversized_values_are_removed_too) {
    std::string big(70u << 10, 'x');
    const std::string text = card("FN:Heavy\r\nitem1.PHOTO;ENCODING=b:AAAA\r\nitem1.X-ABLabel:photo\r\nLOGO;ENCODING=b:BBBB\r\n CCCC\r\nSOUND:DDDD\r\nKEY:EEEE\r\nNOTE:" + big + "\r\nTEL:+1 555 0000\r\nphoto:lowercase name\r\n");
    bool removed = false;
    const std::string out = pbo::strip_heavy_properties(text, &removed);
    PB_CHECK(removed);
    for (const char* gone : {"PHOTO", "LOGO", "SOUND", "KEY", "photo:lowercase", "CCCC", "xxxx"}) PB_CHECK(out.find(gone) == std::string::npos);
    PB_CHECK(out.find("item1.X-ABLabel:photo\r\n") != std::string::npos);   // other properties of the group stay
    PB_CHECK(out.find("TEL:+1 555 0000\r\n") != std::string::npos);
    const auto c = pbo::parse_vcard(out);
    PB_CHECK(c.readable);
    PB_CHECK_EQ(c.phones.size(), static_cast<size_t>(1));
}

PB_TEST(a_card_over_a_limit_is_unreadable_with_the_key_too_large) {
    // 5,001 lines
    std::string lines;
    for (int i = 0; i < 5000; ++i) lines += "X-A:b\r\n";
    auto c = pbo::parse_vcard(card(lines));
    PB_CHECK(!c.readable);
    PB_CHECK_EQ(c.problem, std::string("too_large"));
    // a line of 64 KiB + 1
    c = pbo::parse_vcard(card("NOTE:" + std::string(pbo::kMaxCardLineBytes, 'x') + "\r\n"));
    PB_CHECK(!c.readable);
    PB_CHECK_EQ(c.problem, std::string("too_large"));
    // 101 phone numbers
    std::string phones;
    for (int i = 0; i < 101; ++i) phones += "TEL:+1 555 " + std::to_string(1000 + i) + "\r\n";
    c = pbo::parse_vcard(card(phones));
    PB_CHECK(!c.readable);
    PB_CHECK_EQ(c.problem, std::string("too_large"));
    // exactly 100 are fine
    c = pbo::parse_vcard(card(phones.substr(0, phones.rfind("TEL:"))));
    PB_CHECK(c.readable);
    PB_CHECK_EQ(c.phones.size(), static_cast<size_t>(100));
    // text over 256 KiB
    std::string many;
    while (many.size() < pbo::kMaxCardBytes) many += "X-FILL:0123456789012345678901234567890123456789012345678901234567890123456789\r\n";
    c = pbo::parse_vcard("BEGIN:VCARD\r\nVERSION:3.0\r\nUID:u\r\n" + many.substr(0, pbo::kMaxCardBytes) + "END:VCARD\r\n");
    PB_CHECK(!c.readable);
    PB_CHECK_EQ(c.problem, std::string("too_large"));
}

PB_TEST(a_truncated_or_unbalanced_card_is_malformed_and_two_cards_are_refused) {
    PB_CHECK_EQ(pbo::parse_vcard("BEGIN:VCARD\r\nVERSION:3.0\r\nUID:u\r\nFN:No End\r\n").problem, std::string("malformed"));
    PB_CHECK_EQ(pbo::parse_vcard("VERSION:3.0\r\nUID:u\r\nEND:VCARD\r\n").problem, std::string("malformed"));
    PB_CHECK_EQ(pbo::parse_vcard("").problem, std::string("malformed"));
    PB_CHECK_EQ(pbo::parse_vcard(card("BEGIN:VCARD\r\nFN:Inner\r\nEND:VCARD\r\n")).problem, std::string("malformed"));
    const auto two = pbo::parse_vcard(card("FN:One\r\n") + card("FN:Two\r\n"));
    PB_CHECK(!two.readable);
    PB_CHECK_EQ(two.problem, std::string("multiple_cards"));
}

PB_TEST(a_card_without_a_uid_is_unreadable_but_its_fields_are_read) {
    const auto c = pbo::parse_vcard("BEGIN:VCARD\r\nVERSION:3.0\r\nFN:No Uid\r\nTEL:+1 555 0301\r\nEND:VCARD\r\n");
    PB_CHECK(!c.readable);
    PB_CHECK_EQ(c.problem, std::string("no_uid"));
    PB_CHECK_EQ(c.fn, std::string("No Uid"));
    PB_CHECK_EQ(c.phones.size(), static_cast<size_t>(1));
}

PB_TEST(contact_edit_of_marks_entries_over_the_input_limits_as_locked) {
    const auto plain = pbo::contact_edit_of(pbo::parse_vcard(fixture("nextcloud_sabre_30.unverified.vcf")));
    PB_CHECK_EQ(plain.given, std::string("Anna"));
    PB_CHECK_EQ(plain.family, std::string("Beispiel"));
    PB_CHECK_EQ(plain.org, std::string("Example Works GmbH"));
    PB_CHECK_EQ(plain.phones.size(), static_cast<size_t>(2));
    PB_CHECK_EQ(plain.phones[1].orig_index, 1);
    PB_CHECK(!plain.phones[0].locked && !plain.phones[1].locked);
    PB_CHECK(!plain.note_locked);
    PB_CHECK_EQ(plain.note, std::string("Met at the fair.\nSecond line."));

    const auto long_phone = pbo::contact_edit_of(pbo::parse_vcard(corpus("over_limit_phone")));
    PB_CHECK_EQ(long_phone.phones.size(), static_cast<size_t>(2));
    PB_CHECK(long_phone.phones[0].locked);        // longer than 64 characters
    PB_CHECK(!long_phone.phones[1].locked);
    PB_CHECK_EQ(long_phone.phones[0].orig_index, 0);

    const auto long_note = pbo::contact_edit_of(pbo::parse_vcard(corpus("over_limit_note")));
    PB_CHECK(long_note.note_locked);              // longer than 8 KiB
    PB_CHECK(long_note.note.empty());             // never loaded into an editable field
}

PB_TEST(the_derived_texts_are_valid_utf8_for_umlaut_and_emoji_names) {
    const auto f = pbo::contact_derive(pbo::parse_vcard(corpus("umlaut_and_emoji_names")));
    PB_CHECK(pbo::is_valid_utf8(f.display_name));
    PB_CHECK(pbo::is_valid_utf8(f.search_text));
    PB_CHECK(f.search_text.find("zoe") != std::string::npos);      // folded: Zoë -> zoe
    PB_CHECK(f.search_text.find("unal") != std::string::npos);     // Ünal -> unal
    PB_CHECK_EQ(f.display_name, std::string("Zoë Ünal 🙂"));
}

PB_TEST(a_card_from_the_server_is_made_ready_to_store) {
    const std::string photo = fixture("photo_card.unverified.vcf");
    const auto u = pbo::contact_upsert_from_card("/b/pia.vcf", "\"e1\"", photo);
    PB_CHECK_EQ(u.href, std::string("/b/pia.vcf"));
    PB_CHECK_EQ(u.etag, std::string("\"e1\""));
    PB_CHECK_EQ(u.uid, std::string("7a1c3f10-0000-4000-8000-000000000006"));
    PB_CHECK(u.partial);   // every card from the server may lack what the request left out
    PB_CHECK(!u.unreadable);
    PB_CHECK(u.vcf.find("PHOTO") == std::string::npos);
    PB_CHECK(u.vcf.size() < 600);
    PB_CHECK_EQ(u.fields.display_name, std::string("Pia Foto"));
    PB_CHECK_EQ(u.fields.emails.size(), static_cast<size_t>(1));

    // a card that cannot be read: the uid is made from the path, nothing is lost, the text is kept when it is small
    const auto bad = pbo::contact_upsert_from_card("/b/bad.vcf", "\"e2\"", "this is not a card\r\n");
    PB_CHECK(bad.unreadable);
    PB_CHECK_EQ(bad.uid, std::string("href:/b/bad.vcf"));
    PB_CHECK_EQ(bad.vcf, std::string("this is not a card\r\n"));
    PB_CHECK(!bad.fields.display_name.empty());
    const auto nouid = pbo::contact_upsert_from_card("/b/nouid.vcf", "\"e3\"", "BEGIN:VCARD\r\nVERSION:3.0\r\nFN:No Uid\r\nEND:VCARD\r\n");
    PB_CHECK(nouid.unreadable);
    PB_CHECK_EQ(nouid.uid, std::string("href:/b/nouid.vcf"));
    PB_CHECK_EQ(nouid.fields.display_name, std::string("No Uid"));

    // a card over the limit is stored without its text
    std::string huge = "BEGIN:VCARD\r\nVERSION:3.0\r\nUID:big\r\nFN:Big\r\n";
    for (int i = 0; i < 4000; ++i) huge += "X-FILL:0123456789012345678901234567890123456789012345678901234567890123456789\r\n";
    huge += "END:VCARD\r\n";
    const auto big = pbo::contact_upsert_from_card("/b/big.vcf", "\"e4\"", huge);
    PB_CHECK(big.unreadable);
    PB_CHECK(big.vcf.empty());
    PB_CHECK_EQ(big.uid, std::string("href:/b/big.vcf"));
}

PB_TEST_MAIN()
