#include "xlsx.h"

// QZipReader/QZipWriter: до Qt 6.6 лежали в QtGui, с Qt 6.6 - в QtCore.
#if __has_include(<QtCore/private/qzipreader_p.h>)
#include <QtCore/private/qzipreader_p.h>
#include <QtCore/private/qzipwriter_p.h>
#else
#include <QtGui/private/qzipreader_p.h>
#include <QtGui/private/qzipwriter_p.h>
#endif

#include <QFile>
#include <QHash>
#include <QXmlStreamReader>

#include <cmath>

namespace Xlsx {

namespace {

// Имя атрибута без префикса пространства имён ("r:id" -> "id").
QStringView localName(const QXmlStreamAttribute &a)
{
    const QStringView q = a.qualifiedName();
    const qsizetype colon = q.lastIndexOf(QLatin1Char(':'));
    return colon >= 0 ? q.mid(colon + 1) : q;
}

// "AB12" -> 27 (номер колонки с нуля); -1, если ссылка не распознана.
int columnFromRef(QStringView ref)
{
    int col = 0;
    int letters = 0;
    for (QChar ch : ref) {
        if (ch >= QLatin1Char('A') && ch <= QLatin1Char('Z')) {
            col = col * 26 + (ch.unicode() - 'A' + 1);
            ++letters;
        } else {
            break;
        }
    }
    return letters ? col - 1 : -1;
}

QString columnLetters(int col)
{
    QString s;
    for (++col; col > 0; col = (col - 1) / 26)
        s.prepend(QChar('A' + (col - 1) % 26));
    return s;
}

// Excel хранит числа как 3 или как 2.9999999999999996 - приводим к тому,
// что человек видит в ячейке.
QString normalizeNumber(const QString &raw)
{
    bool ok = false;
    const double d = raw.toDouble(&ok);
    if (!ok)
        return raw;
    const double rounded = std::round(d);
    if (std::abs(d - rounded) < 1e-9 && std::abs(rounded) < 1e15)
        return QString::number(qint64(rounded));
    return QString::number(d, 'g', 15);
}

// Текст всех <t> внутри текущего элемента (для <si> и <is>), кроме
// фонетических подсказок <rPh>.
QString readRichText(QXmlStreamReader &xml)
{
    const QString endName = xml.name().toString();
    QString text;
    int phonetic = 0;
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement()) {
            if (xml.name() == QLatin1String("rPh"))
                ++phonetic;
            else if (xml.name() == QLatin1String("t") && !phonetic)
                text += xml.readElementText();
        } else if (xml.isEndElement()) {
            if (xml.name() == QLatin1String("rPh"))
                --phonetic;
            else if (xml.name() == endName)
                break;
        }
    }
    return text;
}

QString firstSheetPath(const QZipReader &zip)
{
    QString relId;
    {
        QXmlStreamReader xml(zip.fileData(QStringLiteral("xl/workbook.xml")));
        xml.setNamespaceProcessing(false);
        while (!xml.atEnd() && relId.isEmpty()) {
            if (xml.readNext() == QXmlStreamReader::StartElement && xml.name() == QLatin1String("sheet")) {
                for (const auto &a : xml.attributes())
                    if (localName(a) == QLatin1String("id"))
                        relId = a.value().toString();
            }
        }
    }
    if (!relId.isEmpty()) {
        QXmlStreamReader xml(zip.fileData(QStringLiteral("xl/_rels/workbook.xml.rels")));
        while (!xml.atEnd()) {
            if (xml.readNext() == QXmlStreamReader::StartElement && xml.name() == QLatin1String("Relationship")
                && xml.attributes().value("Id") == relId) {
                QString target = xml.attributes().value("Target").toString();
                if (target.startsWith(QLatin1Char('/')))
                    return target.mid(1);
                return QStringLiteral("xl/") + target;
            }
        }
    }
    return QStringLiteral("xl/worksheets/sheet1.xml");
}

QString xmlText(const QString &s)
{
    QString clean;
    clean.reserve(s.size());
    for (QChar ch : s)
        if (ch.unicode() >= 0x20 || ch == QLatin1Char('\t') || ch == QLatin1Char('\n') || ch == QLatin1Char('\r'))
            clean += ch;
    return clean.toHtmlEscaped();
}

QString sheetName(QString name, int index)
{
    static const QString forbidden = QStringLiteral("[]:*?/\\");
    for (QChar &ch : name)
        if (forbidden.contains(ch))
            ch = QLatin1Char('_');
    name = name.left(31).trimmed();
    return name.isEmpty() ? QStringLiteral("Лист%1").arg(index + 1) : name;
}

} // namespace

Table readFirstSheet(const QString &path)
{
    Table table;
    QZipReader zip(path);
    if (!zip.isReadable() || zip.status() != QZipReader::NoError) {
        table.error = QStringLiteral("файл не открывается как .xlsx");
        return table;
    }

    QStringList shared;
    {
        QXmlStreamReader xml(zip.fileData(QStringLiteral("xl/sharedStrings.xml")));
        while (!xml.atEnd()) {
            if (xml.readNext() == QXmlStreamReader::StartElement && xml.name() == QLatin1String("si"))
                shared << readRichText(xml);
        }
    }

    const QByteArray sheetXml = zip.fileData(firstSheetPath(zip));
    if (sheetXml.isEmpty()) {
        table.error = QStringLiteral("в файле не найден лист с данными");
        return table;
    }

    QXmlStreamReader xml(sheetXml);
    int rowIndex = -1;
    QStringList row;
    int col = -1;
    QString type;
    QString value;
    bool haveValue = false;

    auto flushRow = [&] {
        while (!row.isEmpty() && row.last().isEmpty())
            row.removeLast();
        if (rowIndex >= 0) {
            while (table.rows.size() < rowIndex)
                table.rows.append(QStringList());
            table.rows.append(row);
        }
        row.clear();
    };

    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement()) {
            const auto name = xml.name();
            if (name == QLatin1String("row")) {
                const QString r = xml.attributes().value("r").toString();
                rowIndex = r.isEmpty() ? table.rows.size() : r.toInt() - 1;
                col = -1;
            } else if (name == QLatin1String("c")) {
                const int refCol = columnFromRef(xml.attributes().value("r"));
                col = refCol >= 0 ? refCol : col + 1;
                type = xml.attributes().value("t").toString();
                value.clear();
                haveValue = false;
            } else if (name == QLatin1String("v")) {
                value = xml.readElementText();
                haveValue = true;
            } else if (name == QLatin1String("is")) {
                value = readRichText(xml);
                haveValue = true;
            }
        } else if (xml.isEndElement()) {
            const auto name = xml.name();
            if (name == QLatin1String("c") && haveValue && col >= 0) {
                QString text;
                if (type == QLatin1String("s"))
                    text = shared.value(value.toInt());
                else if (type == QLatin1String("str") || type == QLatin1String("inlineStr") || type == QLatin1String("e"))
                    text = value;
                else if (type == QLatin1String("b"))
                    text = value == QLatin1String("1") ? QStringLiteral("TRUE") : QStringLiteral("FALSE");
                else
                    text = normalizeNumber(value);
                while (row.size() <= col)
                    row.append(QString());
                row[col] = text;
            } else if (name == QLatin1String("row")) {
                flushRow();
            }
        }
    }
    if (xml.hasError()) {
        table.error = QStringLiteral("файл повреждён: %1").arg(xml.errorString());
        return table;
    }
    table.ok = true;
    return table;
}

bool write(const QString &path, const QList<Sheet> &sheets, QString *error)
{
    QZipWriter zip(path);
    if (!zip.isWritable()) {
        if (error)
            *error = QStringLiteral("не удалось создать файл (возможно, он открыт в Excel)");
        return false;
    }
    zip.setCompressionPolicy(QZipWriter::AutoCompress);

    QString types = QStringLiteral(
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
        "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
        "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
        "<Override PartName=\"/xl/workbook.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml\"/>"
        "<Override PartName=\"/xl/styles.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.styles+xml\"/>");
    QString workbookSheets;
    QString workbookRels = QStringLiteral(
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">");

    for (int i = 0; i < sheets.size(); ++i) {
        const Sheet &sheet = sheets[i];
        const int n = i + 1;
        types += QStringLiteral("<Override PartName=\"/xl/worksheets/sheet%1.xml\" ContentType=\"application/"
                                "vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml\"/>").arg(n);
        workbookSheets += QStringLiteral("<sheet name=\"%1\" sheetId=\"%2\" r:id=\"rId%2\"/>")
                              .arg(xmlText(sheetName(sheet.name, i))).arg(n);
        workbookRels += QStringLiteral("<Relationship Id=\"rId%1\" Type=\"http://schemas.openxmlformats.org/"
                                       "officeDocument/2006/relationships/worksheet\" Target=\"worksheets/sheet%1.xml\"/>")
                            .arg(n);

        int maxCols = 0;
        for (const auto &r : sheet.rows)
            maxCols = qMax(maxCols, int(r.size()));

        QString xml = QStringLiteral(
            "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
            "<worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\">"
            "<sheetViews><sheetView workbookViewId=\"0\"><pane ySplit=\"1\" topLeftCell=\"A2\" "
            "activePane=\"bottomLeft\" state=\"frozen\"/></sheetView></sheetViews>");
        if (!sheet.columnWidths.isEmpty()) {
            xml += QStringLiteral("<cols>");
            for (int c = 0; c < sheet.columnWidths.size(); ++c)
                xml += QStringLiteral("<col min=\"%1\" max=\"%1\" width=\"%2\" customWidth=\"1\"/>")
                           .arg(c + 1).arg(sheet.columnWidths[c]);
            xml += QStringLiteral("</cols>");
        }
        xml += QStringLiteral("<sheetData>");
        for (int r = 0; r < sheet.rows.size(); ++r) {
            xml += QStringLiteral("<row r=\"%1\">").arg(r + 1);
            const QStringList &cells = sheet.rows[r];
            for (int c = 0; c < cells.size(); ++c) {
                if (cells[c].isEmpty())
                    continue;
                const QString ref = columnLetters(c) + QString::number(r + 1);
                const QString style = r == 0 ? QStringLiteral(" s=\"1\"") : QString();
                bool isNumber = false;
                if (r > 0 && sheet.numericColumns.contains(c))
                    cells[c].toDouble(&isNumber);
                if (isNumber)
                    xml += QStringLiteral("<c r=\"%1\"%2><v>%3</v></c>").arg(ref, style, cells[c]);
                else
                    xml += QStringLiteral("<c r=\"%1\"%2 t=\"inlineStr\"><is><t xml:space=\"preserve\">%3</t></is></c>")
                               .arg(ref, style, xmlText(cells[c]));
            }
            xml += QStringLiteral("</row>");
        }
        xml += QStringLiteral("</sheetData>");
        if (sheet.rows.size() > 1 && maxCols > 0)
            xml += QStringLiteral("<autoFilter ref=\"A1:%1%2\"/>").arg(columnLetters(maxCols - 1)).arg(sheet.rows.size());
        xml += QStringLiteral("</worksheet>");
        zip.addFile(QStringLiteral("xl/worksheets/sheet%1.xml").arg(n), xml.toUtf8());
    }

    types += QStringLiteral("</Types>");
    workbookRels += QStringLiteral("<Relationship Id=\"rId%1\" Type=\"http://schemas.openxmlformats.org/"
                                   "officeDocument/2006/relationships/styles\" Target=\"styles.xml\"/>"
                                   "</Relationships>").arg(sheets.size() + 1);

    zip.addFile(QStringLiteral("[Content_Types].xml"), types.toUtf8());
    zip.addFile(QStringLiteral("_rels/.rels"),
                QByteArrayLiteral("<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
                                  "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
                                  "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/"
                                  "2006/relationships/officeDocument\" Target=\"xl/workbook.xml\"/></Relationships>"));
    zip.addFile(QStringLiteral("xl/workbook.xml"),
                (QStringLiteral("<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
                                "<workbook xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\" "
                                "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\">"
                                "<sheets>") + workbookSheets + QStringLiteral("</sheets></workbook>")).toUtf8());
    zip.addFile(QStringLiteral("xl/_rels/workbook.xml.rels"), workbookRels.toUtf8());
    zip.addFile(QStringLiteral("xl/styles.xml"),
                QByteArrayLiteral("<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
                                  "<styleSheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\">"
                                  "<fonts count=\"2\"><font><sz val=\"11\"/><name val=\"Calibri\"/></font>"
                                  "<font><b/><sz val=\"11\"/><name val=\"Calibri\"/></font></fonts>"
                                  "<fills count=\"2\"><fill><patternFill patternType=\"none\"/></fill>"
                                  "<fill><patternFill patternType=\"gray125\"/></fill></fills>"
                                  "<borders count=\"1\"><border><left/><right/><top/><bottom/><diagonal/></border></borders>"
                                  "<cellStyleXfs count=\"1\"><xf numFmtId=\"0\" fontId=\"0\" fillId=\"0\" borderId=\"0\"/></cellStyleXfs>"
                                  "<cellXfs count=\"2\"><xf numFmtId=\"0\" fontId=\"0\" fillId=\"0\" borderId=\"0\" xfId=\"0\"/>"
                                  "<xf numFmtId=\"0\" fontId=\"1\" fillId=\"0\" borderId=\"0\" xfId=\"0\" applyFont=\"1\"/></cellXfs>"
                                  "<cellStyles count=\"1\"><cellStyle name=\"Normal\" xfId=\"0\" builtinId=\"0\"/></cellStyles>"
                                  "</styleSheet>"));
    zip.close();
    if (zip.status() != QZipWriter::NoError) {
        if (error)
            *error = QStringLiteral("ошибка записи файла");
        return false;
    }
    return true;
}

} // namespace Xlsx
