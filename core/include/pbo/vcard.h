#pragma once
// vCard reader for contacts (spec 002, research R-05/R-06, contracts/carddav-vcard-subset.md section 3). Written from RFC 2426 (3.0), RFC 6350
// (4.0) and the 2.1 leftovers servers still hand out. Total: any bytes give a result, never an exception, never a recursion, bounded by the limits
// of contact_types.h. Text a card carries is untrusted: it is returned as valid UTF-8 and never interpreted. Behaviour against the cards of real
// clients is UNVERIFIED until a live run (Principle IX).

#include <cstdint>
#include <string>
#include <string_view>

#include "pbo/contact_types.h"
#include "pbo/types.h"

namespace pbo {

// Reads one card. `readable` is false (with `problem` set) for: text over kMaxCardBytes, more than kMaxCardLines lines, a line over
// kMaxCardLineBytes, more than kMaxCardEntries phones / emails / addresses ("too_large"); no BEGIN/END pair, a nested BEGIN, a truncated card
// ("malformed"); more than one card ("multiple_cards"); no usable UID ("no_uid"; the other fields are still read). Every logical line of the
// text is kept (ParsedContact::lines) so that a writer can reproduce the card byte for byte.
ParsedContact parse_vcard(std::string_view text);

// Removes the properties PHOTO, LOGO, SOUND and KEY (with their group prefix, their folded continuation lines) and every property whose value is
// longer than kMaxCardLineBytes; every other byte of the text stays. *removed (optional) tells whether anything was removed (research R-05).
std::string strip_heavy_properties(const std::string& text, bool* removed = nullptr);

// What the store keeps next to the card (one function writes these columns): display name, derived list text, sort key, folded search text and
// the email rows. Display name: FN, else "given family", else the organization, else the first email, else the first phone, else "(no name)".
ContactFields contact_derive(const ParsedContact& card);

// The editor's starting point for a stored card: names, labelled entries with their position in the card, `locked` for an entry or a note that
// is longer than the input limits (shown read-only, kept byte for byte, removable).
ContactEdit contact_edit_of(const ParsedContact& card);

// A new card (vCard 3.0, CRLF line ends, lines folded at 75 octets, text escaped): BEGIN, VERSION, PRODID, UID, FN, N, ORG, TEL, EMAIL, ADR, NOTE,
// REV, END. The edit has been cleaned up and checked (contact_edit.h); entries with nothing in them are left out. `uid` is the whole UID text.
std::string new_contact_vcf(const ContactEdit& edit, const std::string& uid, int64_t now_epoch);

// The edit of an existing card (research R-06): only what the user changed is rewritten, every other line stays byte for byte (including its line end
// and its QUOTED-PRINTABLE form), a removed entry takes its whole group with it, new entries come before END, `REV` is set. An edit that changes
// nothing returns `original_text` itself. The entries of `edit` refer to those of the card by `orig_index` (as contact_edit_of numbered them); a
// `locked` entry and a locked note are kept as they are. Error "unreadable" (the original is not a readable card).
Result<std::string> apply_contact_edit(const std::string& original_text, const ContactEdit& edit, int64_t now_epoch);

// The card `edited_stored` (the reader's stored text, edited) written back into the server's `full_card` (research R-05): everything of the edited text,
// plus every line of the full card that the reader never held: properties outside the property list the app asks for (photo, birthday, links, ...)
// and lines over the size limit, unless the edited card already has them; the unlisted lines of a group whose entry the user removed go with it.
std::string merge_into_full_card(const std::string& edited_stored, const std::string& full_card);

// The same contact as far as the user can see (names, organization, labelled entries, note); used to recognise the app's own earlier write.
bool same_contact_content(const std::string& a, const std::string& b);

// `card` with a new UID (the first UID line keeps its group and parameters); `card` itself when it has none.
std::string with_new_uid(const std::string& card, const std::string& uid);

// A card as pulled from the server, ready to store: heavy properties removed (partial = true), the card read and derived; a card that is not
// readable gets the uid "href:<href>", `unreadable = true` and, when it is over kMaxCardBytes, no text (research R-05, data-model.md).
ContactUpsert contact_upsert_from_card(const std::string& href, const std::string& etag, const std::string& raw_card);

}  // namespace pbo
