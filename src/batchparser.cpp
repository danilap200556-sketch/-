#include "batchparser.h"
#include "barcode.h"

#include <QRegularExpression>

namespace BatchParser {

QList<Line> parse(const QString &text)
{
    static const QRegularExpression lineBreak(QStringLiteral("\r\n|\n|\r"));
    static const QRegularExpression tail(QStringLiteral("^(.*\\S)\\s+(\\S+)$"),
                                         QRegularExpression::UseUnicodePropertiesOption);
    static const QRegularExpression onlyNumber(QStringLiteral("^[0-9.,]+$"));
    static const QRegularExpression barcodeDigits(QStringLiteral("^[0-9]{8,14}$"));
    static const QRegularExpression quantity(QStringLiteral("^([0-9]+)(?:[.,]0+)?$"));

    QList<Line> out;
    const QStringList raw = text.left(kMaxTextLength).split(lineBreak);
    for (int i = 0; i < raw.size(); ++i) {
        QString line = raw.at(i);
        line.replace(QChar(0x00A0), QLatin1Char(' '));
        line = line.trimmed();
        if (line.isEmpty())
            continue;

        Line item;
        item.n = i + 1;
        QString qtyRaw;
        const QString compact = Barcode::normalize(line);
        if (barcodeDigits.match(compact).hasMatch() && Barcode::isValid(compact)) {
            item.key = compact; // целиком штрихкод (возможно, с пробелами) - одна штука
        } else if (line.contains(QLatin1Char('\t')) || line.contains(QLatin1Char(';'))) {
            const QStringList parts = line.split(QRegularExpression(QStringLiteral("[\t;]")));
            item.key = parts.at(0).trimmed();
            qtyRaw = parts.value(1).trimmed();
        } else {
            const QRegularExpressionMatch m = tail.match(line);
            if (m.hasMatch() && onlyNumber.match(m.captured(2)).hasMatch()) {
                item.key = m.captured(1).trimmed();
                qtyRaw = m.captured(2);
            } else {
                item.key = line;
            }
        }

        if (item.key.isEmpty())
            item.error = QStringLiteral("нет артикула или штрихкода");
        if (!qtyRaw.isEmpty()) {
            const QRegularExpressionMatch q = quantity.match(qtyRaw);
            if (!q.hasMatch()) {
                if (item.error.isEmpty())
                    item.error = QStringLiteral("количество «%1» - нужно целое число").arg(qtyRaw);
            } else {
                QString digits = q.captured(1);
                while (digits.size() > 1 && digits.startsWith(QLatin1Char('0')))
                    digits.remove(0, 1);
                const bool huge = digits.size() > 12 || digits.toLongLong() > kMaxQty;
                if (!huge && digits.toLongLong() == 0) {
                    if (item.error.isEmpty())
                        item.error = QStringLiteral("количество должно быть больше нуля");
                } else if (huge) {
                    if (item.error.isEmpty())
                        item.error = QStringLiteral("слишком большое количество");
                } else {
                    item.qty = digits.toLongLong();
                }
            }
        }
        out << item;
    }
    return out;
}

} // namespace BatchParser
