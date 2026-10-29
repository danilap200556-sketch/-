#include "photoexcel.h"

#include "csvimport.h"
#include "photofetch.h"
#include "xlsx.h"

#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>
#include <QSqlQuery>
#include <QVariant>

namespace PhotoExcel {

namespace {

constexpr int kMaxUnknown = 40;

bool isKeyHeader(const QString &h)
{
    const QString t = h.trimmed().toLower();
    return t == QStringLiteral("артикул") || t == QLatin1String("sku") || t == QStringLiteral("артикул на маркете") || t == QStringLiteral("ключ товара (артикул)")
           || t == QStringLiteral("штрихкод") || t == QStringLiteral("штрихкоды") || t == QLatin1String("barcode") || t == QStringLiteral("код");
}

int keyRank(const QString &h)
{
    const QString t = h.trimmed().toLower();
    if (t == QStringLiteral("артикул") || t == QLatin1String("sku") || t == QStringLiteral("ключ товара (артикул)"))
        return 0;
    if (t == QStringLiteral("артикул на маркете"))
        return 1;
    return 2;
}

bool isPhotoHeader(const QString &h)
{
    const QString t = h.trimmed().toLower();
    return t.startsWith(QStringLiteral("фото")) || t.startsWith(QStringLiteral("ссылк")) || t.contains(QLatin1String("photo")) || t.contains(QLatin1String("image"))
           || t.contains(QStringLiteral("картин")) || t.contains(QLatin1String("picture"));
}

} // namespace

QStringList linksIn(const QString &cell)
{
    QStringList out;
    static const QRegularExpression split(QStringLiteral("[\\s;|]+"));
    for (const QString &part : cell.split(split, Qt::SkipEmptyParts)) {
        const QString link = PhotoFetch::usableLink(part);
        if (!link.isEmpty() && (link.startsWith(QLatin1String("http://")) || link.startsWith(QLatin1String("https://")) || link.startsWith(QLatin1String("//"))))
            out << link;
    }
    return out;
}

Parsed parseRows(const QVector<QStringList> &rows)
{
    Parsed res;
    // Строка заголовков: первая из первых десяти, где есть колонка ключа и колонка с фото.
    int headerRow = -1;
    for (int r = 0; r < qMin(10, int(rows.size())); ++r) {
        bool key = false, photo = false;
        for (const QString &c : rows[r]) {
            key = key || isKeyHeader(c);
            photo = photo || isPhotoHeader(c);
        }
        if (key && photo) {
            headerRow = r;
            break;
        }
    }
    if (headerRow < 0) {
        res.error = QStringLiteral("Не нашёл заголовков: нужна колонка «Артикул» (или «Артикул на Маркете», «Штрихкод») и колонки «Фото», «Фото 1», «Фото 2»… со ссылками.");
        return res;
    }
    const QStringList &header = rows[headerRow];
    QList<int> keyCols, photoCols;
    for (int c = 0; c < header.size(); ++c) {
        if (isKeyHeader(header[c]))
            keyCols << c;
        else if (isPhotoHeader(header[c]))
            photoCols << c;
    }
    std::stable_sort(keyCols.begin(), keyCols.end(), [&](int a, int b) { return keyRank(header[a]) < keyRank(header[b]); });
    res.keyHeader = header[keyCols.first()].trimmed();
    for (int c : std::as_const(photoCols))
        res.photoHeaders << header[c].trimmed();

    // Все ключи товаров одним запросом: артикул, артикул на Маркете, штрихкоды (в таком порядке важности).
    QHash<QString, int> byKey;
    {
        QSqlQuery codes(QStringLiteral("SELECT barcode, product_id FROM product_barcodes"));
        while (codes.next())
            byKey.insert(codes.value(0).toString().trimmed().toLower(), codes.value(1).toInt());
        QSqlQuery products(QStringLiteral("SELECT id, sku, COALESCE(NULLIF(TRIM(market_sku), ''), '') FROM products"));
        QList<QPair<QString, int>> skus;
        while (products.next()) {
            const QString market = products.value(2).toString().toLower();
            if (!market.isEmpty())
                byKey.insert(market, products.value(0).toInt());
            skus.append({products.value(1).toString().trimmed().toLower(), products.value(0).toInt()});
        }
        for (const auto &s : std::as_const(skus))
            byKey.insert(s.first, s.second);
    }

    QSet<QString> unknownSeen;
    for (int r = headerRow + 1; r < rows.size(); ++r) {
        const QStringList &row = rows[r];
        QString key;
        int productId = 0;
        for (int c : std::as_const(keyCols)) {
            const QString k = row.value(c).trimmed();
            if (k.isEmpty())
                continue;
            if (key.isEmpty())
                key = k;
            productId = byKey.value(k.toLower(), 0);
            if (productId > 0)
                break;
        }
        if (key.isEmpty())
            continue;
        ++res.rows;
        QStringList links;
        for (int c : std::as_const(photoCols))
            for (const QString &l : linksIn(row.value(c)))
                if (!links.contains(l))
                    links << l;
        if (productId == 0) {
            if (!unknownSeen.contains(key.toLower())) {
                unknownSeen.insert(key.toLower());
                ++res.unknownCount;
                if (res.unknown.size() < kMaxUnknown)
                    res.unknown << key;
            }
            continue;
        }
        if (links.isEmpty()) {
            ++res.withoutLinks;
            continue;
        }
        QStringList &mine = res.links[productId];
        for (const QString &l : std::as_const(links))
            if (!mine.contains(l))
                mine << l;
        res.linkCount += links.size();
    }
    res.ok = true;
    return res;
}

Parsed parseFile(const QString &path)
{
    Parsed res;
    const QString suffix = QFileInfo(path).suffix().toLower();
    QVector<QStringList> rows;
    if (suffix == QLatin1String("xlsx")) {
        const Xlsx::Table t = Xlsx::readFirstSheet(path);
        if (!t.ok) {
            res.error = t.error;
            return res;
        }
        rows = t.rows;
    } else if (suffix == QLatin1String("xls")) {
        res.error = QStringLiteral("Файлы .xls (Excel 97-2003) не поддерживаются: сохраните как «Книга Excel (.xlsx)».");
        return res;
    } else {
        const CsvImport::Table t = CsvImport::readFile(path);
        if (!t.ok) {
            res.error = t.error;
            return res;
        }
        rows = t.rows;
    }
    if (rows.isEmpty()) {
        res.error = QStringLiteral("В файле не найдено ни одной строки.");
        return res;
    }
    return parseRows(rows);
}

} // namespace PhotoExcel
