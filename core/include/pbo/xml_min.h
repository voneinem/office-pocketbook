#pragma once
// A minimal, non-validating XML reader for the answers of CalDAV servers (research R-15). It understands elements,
// attributes, namespaces, text, CDATA, comments, the five predefined entities and numeric character references.
// It REFUSES <!DOCTYPE, every other entity, processing instructions (except the leading XML declaration), documents
// above the limits and anything not well formed. Total: any input gives a tree or an error, never a crash or a hang.

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "pbo/types.h"

namespace pbo {

struct XmlLimits {
    size_t max_bytes = 8u << 20;      // 8 MiB
    int max_depth = 64;
    size_t max_nodes = 200000;        // elements
};

struct XmlNode {
    std::string ns;                   // namespace URI ("" = none)
    std::string name;                 // local name
    std::string text;                 // the character data directly inside this element (entities decoded, CDATA included)
    std::vector<std::pair<std::string, std::string>> attrs;   // unprefixed attributes only (name, value)
    std::vector<XmlNode> children;

    const XmlNode* child(std::string_view ns_uri, std::string_view local) const;           // first match or nullptr
    std::vector<const XmlNode*> children_named(std::string_view ns_uri, std::string_view local) const;
    std::string attr(std::string_view attr_name) const;                                    // "" if absent
};

// Error: ErrorClass::Protocol, key "malformed_xml" (detail names the kind of problem, never the content)
Result<XmlNode> parse_xml(std::string_view xml, const XmlLimits& limits = XmlLimits());

}  // namespace pbo
