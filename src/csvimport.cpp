#include "csvimport.h"

#include <QFile>
#include <QStringDecoder>

namespace {

// Windows-1251 -> Unicode, для байтов 0x80-0xFF (0x00-0x7F совпадает с ASCII).
// 0xC0-0xFF - обычные русские буквы А-я, идут по порядку линейно.
QString decodeCp1251(const QByteArray &data)
{
    static const char16_t table[128] = {
        0x0402, 0x0403, 0x201A, 0x0453, 0x201E, 0x2026, 0x2020, 0x2021, // 0x80-0x87
        0x20AC, 0x2030, 0x0409, 0x2039, 0x040A, 0x040C, 0x040B, 0x040F, // 0x88-0x8F
        0x0452, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, // 0x90-0x97
        0xFFFD, 0x2122, 0x045A, 0x203A, 0x045C, 0x045B, 0x045F, 0xFFFD, // 0x98-0x9F
        0x00A0, 0x040E, 0x045E, 0x0408, 0x00A4, 0x0490, 0x00A6, 0x00A7, // 0xA0-0xA7
        0x0401, 0x00A9, 0x0404, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x0407, // 0xA8-0xAF
        0x00B0, 0x00B1, 0x0406, 0x0456, 0x0491, 0x00B5, 0x00B6, 0x00B7, // 0xB0-0xB7
        0x0451, 0x2116, 0x0454, 0x00BB, 0x0458, 0x0405, 0x0455, 0x0457, // 0xB8-0xBF
        0x0410, 0x0411, 0x0412, 0x0413, 0x0414, 0x0415, 0x0416, 0x0417, // 0xC0-0xC7
        0x0418, 0x0419, 0x041A, 0x041B, 0x041C, 0x041D, 0x041E, 0x041F, // 0xC8-0xCF
        0x0420, 0x0421, 0x0422, 0x0423, 0x0424, 0x0425, 0x0426, 0x0427, // 0xD0-0xD7
        0x0428, 0x0429, 0x042A, 0x042B, 0x042C, 0x042D, 0x042E, 0x042F, // 0xD8-0xDF
        0x0430, 0x0431, 0x0432, 0x0433, 0x0434, 0x0435, 0x0436, 0x0437, // 0xE0-0xE7
        0x0438, 0x0439, 0x043A, 0x043B, 0x043C, 0x043D, 0x043E, 0x043F, // 0xE8-0xEF
        0x0440, 0x0441, 0x0442, 0x0443, 0x0444, 0x0445, 0x0446, 0x0447, // 0xF0-0xF7
        0x0448, 0x0449, 0x044A, 0x044B, 0x044C, 0x044D, 0x044E, 0x044F  // 0xF8-0xFF
    };
    QString out;
    out.reserve(data.size());
    for (unsigned char c : data) {
        if (c < 0x80)
            out += QChar(c);
        else
            out += QChar(table[c - 0x80]);
    }
    return out;
}

QString decodeBytes(QByteArray data)
{
    if (data.startsWith("\xEF\xBB\xBF"))
        data = data.mid(3);

    QStringDecoder decoder(QStringConverter::Utf8);
    const QString text = decoder.decode(data);
    if (!decoder.hasError())
        return text;
    return decodeCp1251(data);
}

QChar detectDelimiter(const QString &sample)
{
    const int semicolons = sample.count(QChar(';'));
    const int commas = sample.count(QChar(','));
    return semicolons >= commas ? QChar(';') : QChar(',');
}

QVector<QStringList> parseDelimited(const QString &text, QChar delimiter)
{
    QVector<QStringList> rows;
    QStringList current;
    QString field;
    bool inQuotes = false;
    const int n = text.size();
    int i = 0;

    while (i < n) {
        const QChar ch = text[i];
        if (inQuotes) {
            if (ch == QChar('"')) {
                if (i + 1 < n && text[i + 1] == QChar('"')) {
                    field += QChar('"');
                    i += 2;
                    continue;
                }
                inQuotes = false;
                ++i;
                continue;
            }
            field += ch;
            ++i;
            continue;
        }

        if (ch == QChar('"')) {
            inQuotes = true;
            ++i;
        } else if (ch == delimiter) {
            current << field;
            field.clear();
            ++i;
        } else if (ch == QChar('\r')) {
            ++i;
        } else if (ch == QChar('\n')) {
            current << field;
            field.clear();
            rows << current;
            current.clear();
            ++i;
        } else {
            field += ch;
            ++i;
        }
    }
    if (!field.isEmpty() || !current.isEmpty()) {
        current << field;
        rows << current;
    }
    return rows;
}

} // namespace

namespace CsvImport {

Table readFile(const QString &path)
{
    Table result;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        result.error = file.errorString();
        return result;
    }
    const QByteArray raw = file.readAll();
    const QString text = decodeBytes(raw);

    const QChar delimiter = detectDelimiter(text.left(qMin(text.size(), 4000)));

    result.rows = parseDelimited(text, delimiter);
    result.ok = true;
    return result;
}

} // namespace CsvImport
