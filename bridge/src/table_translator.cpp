#include "table_translator.h"

#include <QList>

namespace {

QString unescape(const QString& s) {
    QString out;
    out.reserve(s.size());
    for (int i = 0; i < s.size(); ++i) {
        if (s[i] == QLatin1Char('\\') && i + 1 < s.size()) {
            const QChar c = s[++i];
            out += c == QLatin1Char('n') ? QLatin1Char('\n') : (c == QLatin1Char('t') ? QLatin1Char('\t') : c);
        } else {
            out += s[i];
        }
    }
    return out;
}

}  // namespace

bool TableTranslator::loadTable(const QByteArray& utf8) {
    table_.clear();
    QHash<QString, QString> next;
    const QString text = QString::fromUtf8(utf8);
    const QList<QStringView> lines = QStringView(text).split(QLatin1Char('\n'));
    for (QStringView line : lines) {
        if (line.endsWith(QLatin1Char('\r'))) line.chop(1);
        if (line.trimmed().isEmpty() || line.startsWith(QLatin1Char('#'))) continue;
        const QList<QStringView> parts = line.split(QLatin1Char('\t'));
        if (parts.size() != 2) return false;
        next.insert(unescape(parts[0].toString()), unescape(parts[1].toString()));
    }
    table_ = std::move(next);
    return true;
}

QString TableTranslator::translate(const char* /*context*/, const char* sourceText, const char* /*disambiguation*/, int n) const {
    if (!sourceText) return QString();
    const auto it = table_.constFind(QString::fromUtf8(sourceText));
    if (it == table_.constEnd()) return QString();   // a null string: Qt shows the source text
    const int cut = it->indexOf(QLatin1String("||"));
    if (cut < 0) return *it;
    return n == 1 || n < 0 ? it->left(cut) : it->mid(cut + 2);
}
