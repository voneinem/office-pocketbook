#include "pbo/mail_roles.h"

#include <algorithm>
#include <cctype>

namespace pbo {

namespace {

std::string upper_ascii(const std::string& s) {
    std::string u;
    for (char c : s) u += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return u;
}

// Lower case for ASCII and the Latin-1 letters (U+00C0..U+00DE in UTF-8 are C3 80..C3 9E); enough for the names below.
std::string fold(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == 0xC3 && i + 1 < s.size() && static_cast<unsigned char>(s[i + 1]) >= 0x80 && static_cast<unsigned char>(s[i + 1]) <= 0x9E &&
            static_cast<unsigned char>(s[i + 1]) != 0x97) {
            out += static_cast<char>(0xC3);
            out += static_cast<char>(static_cast<unsigned char>(s[++i]) + 0x20);
        } else {
            out += static_cast<char>(std::tolower(c));
        }
    }
    return out;
}

bool in(const std::string& name, std::initializer_list<const char*> names) {
    for (const char* n : names)
        if (name == n) return true;
    return false;
}

}  // namespace

RoleResult detect_role(const FolderListItem& item) {
    RoleResult r;
    if (upper_ascii(item.raw_name) == "INBOX") {
        r.role = "inbox";
        return r;
    }
    for (const std::string& a : item.attributes) {
        const std::string u = upper_ascii(a);
        if (u == "\\SENT") r.role = "sent";
        else if (u == "\\DRAFTS") r.role = "drafts";
        else if (u == "\\TRASH") r.role = "trash";
        else if (u == "\\JUNK") r.role = "junk";
        else if (u == "\\ALL") r.role = "all";
        else if (u == "\\ARCHIVE") r.role = "archive";
        else continue;
        break;
    }
    if (r.role == "other") {
        std::string name = fold(item.name.empty() ? decode_mutf7(item.raw_name) : item.name);
        if (name.size() > 6 && name.compare(0, 5, "inbox") == 0 && (name[5] == '.' || name[5] == '/')) name.erase(0, 6);
        if (in(name, {"sent", "sent items", "sent messages", "gesendet", "gesendete elemente", "gesendete objekte"})) r.role = "sent";
        else if (in(name, {"drafts", "entw\xC3\xBCrfe", "entwurf"})) r.role = "drafts";
        else if (in(name, {"trash", "deleted items", "deleted messages", "papierkorb", "gel\xC3\xB6scht", "gel\xC3\xB6schte elemente"})) r.role = "trash";
    }
    r.sync_enabled = !(r.role == "all" || r.role == "junk");
    return r;
}

std::vector<std::string> roles_needing_choice(const std::vector<Folder>& folders) {
    std::vector<std::string> missing;
    for (const char* role : {"sent", "drafts", "trash"})
        if (std::none_of(folders.begin(), folders.end(), [&](const Folder& f) { return f.role == role; })) missing.push_back(role);
    return missing;
}

}  // namespace pbo
