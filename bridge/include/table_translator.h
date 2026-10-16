#pragma once
// The interface texts in another language from a plain table (qml/strings_de.tsv), because lrelease and .qm files are not usable with the SDK.
// One line per text: the English source, a TAB, the translation. \n, \t and \\ are escapes; "||" in the translation separates the singular from the
// plural form of a text with %n. A text that is not in the table stays English. Lines starting with # and empty lines are ignored.
// Install it with QCoreApplication::installTranslator and call QQmlEngine::retranslate() to show a change at once.

#include <QByteArray>
#include <QHash>
#include <QString>
#include <QTranslator>

class TableTranslator : public QTranslator {
public:
    explicit TableTranslator(QObject* parent = nullptr) : QTranslator(parent) {}

    // Replaces the table. A malformed table (a line without exactly one TAB) is refused as a whole: false, and the table stays empty.
    bool loadTable(const QByteArray& utf8);
    int entryCount() const { return static_cast<int>(table_.size()); }

    QString translate(const char* context, const char* sourceText, const char* disambiguation = nullptr, int n = -1) const override;
    bool isEmpty() const override { return table_.isEmpty(); }

private:
    QHash<QString, QString> table_;
};
