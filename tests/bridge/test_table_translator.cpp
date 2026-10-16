// T175: the German texts come from a plain table (qml/strings_de.tsv) read by TableTranslator, because lrelease and .qm files are not usable with the SDK.
// Missing entries fall back to the English text; %n texts have a singular and a plural form.
#include <QCoreApplication>
#include <QFile>
#include <QTest>

#include "table_translator.h"
#include "test_support.h"

namespace {

int g_argc = 1;
char g_arg0[] = "test_table_translator";
char* g_argv[] = {g_arg0, nullptr};
QCoreApplication g_app(g_argc, g_argv);

QString tr(const char* source, int n = -1) { return QCoreApplication::translate("qml", source, nullptr, n); }

}  // namespace

PB_TEST(a_table_translates_known_texts_and_leaves_the_others_alone) {
    TableTranslator t;
    PB_CHECK(t.loadTable("# comment\nBack\tZur\\u00fcck\nSave\tSpeichern\n\nSync\tAbgleichen\n"));
    PB_CHECK_EQ(t.entryCount(), 3);
    QCoreApplication::installTranslator(&t);
    PB_CHECK_EQ(tr("Save").toStdString(), std::string("Speichern"));
    PB_CHECK_EQ(tr("Not in the table").toStdString(), std::string("Not in the table"));
    QCoreApplication::removeTranslator(&t);
    PB_CHECK_EQ(tr("Save").toStdString(), std::string("Save"));
}

PB_TEST(escapes_umlauts_and_multi_line_texts_survive) {
    TableTranslator t;
    PB_CHECK(t.loadTable(QByteArray::fromStdString("Two lines\\nhere\tZwei Zeilen\\nhier\n") + QByteArray("Close\tSchlie\xC3\x9F" "en\n")));
    QCoreApplication::installTranslator(&t);
    PB_CHECK_EQ(tr("Two lines\nhere").toStdString(), std::string("Zwei Zeilen\nhier"));
    PB_CHECK_EQ(tr("Close").toStdString(), std::string("Schlie\xC3\x9F" "en"));
    QCoreApplication::removeTranslator(&t);
}

PB_TEST(plural_texts_pick_the_singular_for_one_and_the_plural_otherwise) {
    TableTranslator t;
    PB_CHECK(t.loadTable("%n day(s)\t%n Tag||%n Tage\n"));
    QCoreApplication::installTranslator(&t);
    PB_CHECK_EQ(tr("%n day(s)", 1).toStdString(), std::string("1 Tag"));
    PB_CHECK_EQ(tr("%n day(s)", 0).toStdString(), std::string("0 Tage"));
    PB_CHECK_EQ(tr("%n day(s)", 30).toStdString(), std::string("30 Tage"));
    QCoreApplication::removeTranslator(&t);
}

PB_TEST(a_broken_table_is_refused_whole_so_that_the_english_text_stays) {
    TableTranslator t;
    PB_CHECK(!t.loadTable("no tab on this line\n"));
    PB_CHECK_EQ(t.entryCount(), 0);
    PB_CHECK(!t.loadTable("A\tB\tC\n"));
    PB_CHECK_EQ(t.entryCount(), 0);
}

PB_TEST(the_real_german_table_loads_and_translates_the_common_buttons) {
    QFile f(QStringLiteral(PBO_SOURCE_DIR "/qml/strings_de.tsv"));
    PB_CHECK(f.open(QIODevice::ReadOnly));
    TableTranslator t;
    PB_CHECK(t.loadTable(f.readAll()));
    PB_CHECK(t.entryCount() > 300);
    QCoreApplication::installTranslator(&t);
    PB_CHECK_EQ(tr("Back").toStdString(), std::string("Zur\xC3\xBC" "ck"));
    PB_CHECK_EQ(tr("%n unread", 3).toStdString(), std::string("3 ungelesen"));
    QCoreApplication::removeTranslator(&t);
}

PB_TEST_MAIN()
