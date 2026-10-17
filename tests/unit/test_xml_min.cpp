// T092: the minimal XML reader for CalDAV answers (research R-15): namespaces, CDATA, the five predefined entities and numeric
// references; DOCTYPE, other entities, processing instructions and oversized input are refused; no input crashes it.
#include <string>

#include "mutate.h"
#include "pbo/xml_min.h"
#include "test_support.h"

namespace {
const char* kMultistatus =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
    "<d:multistatus xmlns:d=\"DAV:\" xmlns:c=\"urn:ietf:params:xml:ns:caldav\" xmlns:cs=\"http://calendarserver.org/ns/\">\n"
    "  <!-- a comment -->\n"
    "  <d:response>\n"
    "    <d:href>/cal/work/</d:href>\n"
    "    <d:propstat><d:prop><d:displayname>Work &amp; Home</d:displayname><cs:getctag>c-42</cs:getctag></d:prop>\n"
    "    <d:status>HTTP/1.1 200 OK</d:status></d:propstat>\n"
    "  </d:response>\n"
    "</d:multistatus>";
}  // namespace

PB_TEST(namespaces_are_resolved_by_uri_not_by_prefix) {
    auto r = pbo::parse_xml(kMultistatus);
    PB_CHECK(r.ok());
    const pbo::XmlNode& root = r.value;
    PB_CHECK_EQ(root.ns, std::string("DAV:"));
    PB_CHECK_EQ(root.name, std::string("multistatus"));
    const pbo::XmlNode* resp = root.child("DAV:", "response");
    PB_CHECK(resp != nullptr);
    PB_CHECK_EQ(resp->child("DAV:", "href")->text, std::string("/cal/work/"));
    const pbo::XmlNode* prop = resp->child("DAV:", "propstat")->child("DAV:", "prop");
    PB_CHECK(prop != nullptr);
    PB_CHECK_EQ(prop->child("DAV:", "displayname")->text, std::string("Work & Home"));
    PB_CHECK_EQ(prop->child("http://calendarserver.org/ns/", "getctag")->text, std::string("c-42"));
    PB_CHECK(prop->child("DAV:", "getctag") == nullptr);          // same local name, other namespace
}

PB_TEST(another_prefix_and_a_default_namespace_give_the_same_tree) {
    auto r = pbo::parse_xml("<multistatus xmlns=\"DAV:\"><response><href>/x</href></response></multistatus>");
    PB_CHECK(r.ok());
    PB_CHECK_EQ(r.value.child("DAV:", "response")->child("DAV:", "href")->text, std::string("/x"));
    auto q = pbo::parse_xml("<A:multistatus xmlns:A=\"DAV:\"><A:response><A:href>/x</A:href></A:response></A:multistatus>");
    PB_CHECK(q.ok());
    PB_CHECK_EQ(q.value.child("DAV:", "response")->child("DAV:", "href")->text, std::string("/x"));
}

PB_TEST(cdata_and_the_predefined_entities_and_numeric_references_are_decoded) {
    auto r = pbo::parse_xml("<a>x &lt;&gt;&amp;&quot;&apos; &#65;&#x42; &#xE4;&#x1F4E7; <![CDATA[<raw & text>]]></a>");
    PB_CHECK(r.ok());
    PB_CHECK_EQ(r.value.text, std::string("x <>&\"' AB \xC3\xA4\xF0\x9F\x93\xA7 <raw & text>"));
}

PB_TEST(attributes_are_read_and_text_is_kept_per_element) {
    auto r = pbo::parse_xml("<a x='1' y=\"two &amp; more\"><b>one</b>tail<b>two</b></a>");
    PB_CHECK(r.ok());
    PB_CHECK_EQ(r.value.attr("x"), std::string("1"));
    PB_CHECK_EQ(r.value.attr("y"), std::string("two & more"));
    PB_CHECK_EQ(r.value.attr("z"), std::string(""));
    PB_CHECK_EQ(r.value.children.size(), size_t(2));
    PB_CHECK_EQ(r.value.children_named("", "b").size(), size_t(2));
    PB_CHECK_EQ(r.value.children[1].text, std::string("two"));
}

PB_TEST(self_closing_elements_and_the_prolog_are_accepted) {
    auto r = pbo::parse_xml("<?xml version='1.0'?>\n<a><b/><c />  </a>\n");
    PB_CHECK(r.ok());
    PB_CHECK_EQ(r.value.children.size(), size_t(2));
}

PB_TEST(doctype_other_entities_and_processing_instructions_are_refused) {
    for (const char* bad : {
             "<!DOCTYPE a [<!ENTITY x \"y\">]><a>&x;</a>",
             "<!DOCTYPE a><a/>",
             "<a>&nbsp;</a>",
             "<a>&unknown;</a>",
             "<a><?php echo 1; ?></a>",
             "<a>&#0;</a>",
             "<a>&#xD800;</a>",
             "<a>&#x110000;</a>",
             "<a>&amp</a>",
         }) {
        auto r = pbo::parse_xml(bad);
        PB_CHECK(!r.ok());
        PB_CHECK_EQ(r.error.key, std::string("malformed_xml"));
    }
}

PB_TEST(structural_errors_are_refused) {
    for (const char* bad : {"", "text only", "<a>", "<a></b>", "<a><b></a></b>", "<a></a><b/>", "<a x=1/>", "<a x='1' x='2'/>",
                            "<a><", "<a attr", "<p:a/>", "<a><![CDATA[never closed</a>", "<a><!-- never closed </a>"}) {
        auto r = pbo::parse_xml(bad);
        PB_CHECK(!r.ok());
    }
}

PB_TEST(depth_node_and_size_limits_are_enforced) {
    std::string deep;
    for (int i = 0; i < 64; ++i) deep += "<a>";
    for (int i = 0; i < 64; ++i) deep += "</a>";
    PB_CHECK(pbo::parse_xml(deep).ok());
    std::string too_deep = "<a>" + deep + "</a>";
    auto d = pbo::parse_xml(too_deep);
    PB_CHECK(!d.ok());
    PB_CHECK_EQ(d.error.key, std::string("malformed_xml"));

    pbo::XmlLimits small;
    small.max_nodes = 100;
    std::string wide = "<r>";
    for (int i = 0; i < 100; ++i) wide += "<n/>";
    wide += "</r>";                                              // 101 elements
    PB_CHECK(!pbo::parse_xml(wide, small).ok());
    small.max_nodes = 101;
    PB_CHECK(pbo::parse_xml(wide, small).ok());

    pbo::XmlLimits tiny;
    tiny.max_bytes = 20;
    PB_CHECK(!pbo::parse_xml("<a>0123456789012345678</a>", tiny).ok());
    PB_CHECK(pbo::parse_xml("<a>012345678</a>", tiny).ok());
}

PB_TEST(a_document_of_200001_nodes_is_refused_with_the_default_limits) {
    std::string wide = "<r>";
    for (int i = 0; i < 200000; ++i) wide += "<n/>";
    wide += "</r>";
    auto r = pbo::parse_xml(wide);
    PB_CHECK(!r.ok());
}

PB_TEST(truncation_and_mutation_never_crash_or_hang) {
    const std::string doc = kMultistatus;
    for (size_t at = 0; at <= doc.size(); ++at) (void)pbo::parse_xml(pbtest::truncated(doc, at));
    for (uint64_t seed = 1; seed <= 300; ++seed) {
        (void)pbo::parse_xml(pbtest::flipped(doc, seed, 4));
        (void)pbo::parse_xml(pbtest::with_insertions(doc, seed, 3));
    }
}

PB_TEST_MAIN()
