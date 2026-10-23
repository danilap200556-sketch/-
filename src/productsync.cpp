#include "productsync.h"
#include "barcode.h"
#include "marketcards.h"
#include "productexport.h"
#include "productmarket.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QRegularExpression>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>
#include <cmath>

namespace ProductSync {

namespace {

constexpr double kMaxPrice = 10'000'000;

int limitOf(const QString &field)
{
    if (field == QLatin1String("name")) return 300;
    if (field == QLatin1String("description")) return 2000;
    if (field == QLatin1String("ym_vendor")) return 100;
    return 200; // market_sku, custom_code
}

QString fieldLabel(const QString &field)
{
    if (field == QLatin1String("name")) return QStringLiteral("Название");
    if (field == QLatin1String("description")) return QStringLiteral("Описание");
    if (field == QLatin1String("price")) return QStringLiteral("Цена");
    if (field == QLatin1String("market_sku")) return QStringLiteral("Артикул на Маркете");
    if (field == QLatin1String("custom_code")) return QStringLiteral("Свой код");
    if (field == QLatin1String("ym_vendor")) return QStringLiteral("Бренд");
    if (field == QLatin1String("ym_countries")) return QStringLiteral("Страна производства");
    if (field == QLatin1String("ym_length")) return QStringLiteral("Длина, см");
    if (field == QLatin1String("ym_width")) return QStringLiteral("Ширина, см");
    if (field == QLatin1String("ym_height")) return QStringLiteral("Высота, см");
    if (field == QLatin1String("ym_weight")) return QStringLiteral("Вес, кг");
    if (field == QLatin1String("ym_commodity")) return QStringLiteral("ТН ВЭД");
    if (field == QLatin1String("ym_tags")) return QStringLiteral("Теги");
    if (field == QLatin1String("ym_adult")) return QStringLiteral("18+");
    return QStringLiteral("Штрихкоды");
}

QString cleanText(const QString &v)
{
    QString s = v;
    s.replace(QStringLiteral("\r\n"), QStringLiteral("\n")).replace(QLatin1Char('\r'), QLatin1Char('\n'));
    s.replace(QChar(0x00A0), QLatin1Char(' '));
    return s.trimmed();
}

// 12990.5, 9000, 250.99 - без хвостовых нулей, точка как разделитель
QString fmtPrice(double v)
{
    QString s = QString::number(std::round(v * 100.0) / 100.0, 'f', 2);
    if (s.contains(QLatin1Char('.'))) {
        while (s.endsWith(QLatin1Char('0')))
            s.chop(1);
        if (s.endsWith(QLatin1Char('.')))
            s.chop(1);
    }
    return s;
}

struct ProductRow {
    int id = 0;
    QString sku, name, description, marketSku, customCode;
    double price = 0;
    ProductMarketInfo market;
};

const QString kProductCols = QStringLiteral("p.id, p.sku, p.name, COALESCE(p.description, ''), p.price::float8, "
                                            "COALESCE(p.market_sku, ''), COALESCE(p.custom_code, ''), ") +
                             ProductMarket::selectColumns(QStringLiteral("p."));

ProductRow readProduct(const QSqlQuery &q, int offset)
{
    ProductRow r;
    r.id = q.value(offset).toInt();
    r.sku = q.value(offset + 1).toString();
    r.name = q.value(offset + 2).toString();
    r.description = q.value(offset + 3).toString();
    r.price = q.value(offset + 4).toDouble();
    r.marketSku = q.value(offset + 5).toString();
    r.customCode = q.value(offset + 6).toString();
    r.market = ProductMarket::fromRow(q, offset + 7);
    return r;
}

QString valuesList(int n)
{
    QStringList marks;
    for (int i = 0; i < n; ++i)
        marks << QStringLiteral("(?::text)");
    return marks.join(QLatin1Char(','));
}


// ---- Параметры для карточки Яндекс Маркета: разбор ячеек и значения «как в таблице»
struct YmField {
    int role;
    const char *field;
    const char *column;
};
const YmField kYmFields[] = {
    {RoleVendor, "ym_vendor", "market_vendor"},       {RoleCountries, "ym_countries", "market_countries"},
    {RoleLength, "ym_length", "market_length_cm"},    {RoleWidth, "ym_width", "market_width_cm"},
    {RoleHeight, "ym_height", "market_height_cm"},    {RoleWeight, "ym_weight", "market_weight_kg"},
    {RoleCommodity, "ym_commodity", "market_commodity_code"}, {RoleTags, "ym_tags", "market_tags"},
    {RoleAdult, "ym_adult", "market_adult"},
};

// 30.5, 12, 1.25 - без хвостовых нулей
QString fmtNumber(double v, int decimals)
{
    QString s = QString::number(v, 'f', decimals);
    if (s.contains(QLatin1Char('.'))) {
        while (s.endsWith(QLatin1Char('0')))
            s.chop(1);
        if (s.endsWith(QLatin1Char('.')))
            s.chop(1);
    }
    return s;
}

QString listText(const QStringList &l) { return l.join(QStringLiteral("; ")); }

QStringList parseList(const QString &raw)
{
    static const QRegularExpression sep(QStringLiteral("[;,\\n]+"));
    QStringList out;
    for (const QString &part : raw.split(sep, Qt::SkipEmptyParts)) {
        const QString t = part.trimmed();
        if (!t.isEmpty() && !out.contains(t))
            out << t;
    }
    return out;
}

// Значение ячейки в виде, в котором оно показывается и сравнивается; пусто + *why - ячейка не подходит.
QString parseYm(const QString &field, const QString &raw, QString *why)
{
    if (field == QLatin1String("ym_vendor")) {
        if (raw.size() > limitOf(field)) { *why = QStringLiteral("длиннее %1 знаков").arg(limitOf(field)); return QString(); }
        return raw;
    }
    if (field == QLatin1String("ym_countries") || field == QLatin1String("ym_tags")) {
        const QStringList l = parseList(raw);
        const QString t = listText(l);
        if (t.size() > 500) { *why = QStringLiteral("слишком длинный список"); return QString(); }
        return t;
    }
    if (field == QLatin1String("ym_length") || field == QLatin1String("ym_width") || field == QLatin1String("ym_height") ||
        field == QLatin1String("ym_weight")) {
        QString t;
        for (QChar ch : raw)
            if (!ch.isSpace())
                t += ch;
        t.replace(QLatin1Char(','), QLatin1Char('.'));
        static const QRegularExpression number(QStringLiteral("^[0-9]+(\\.[0-9]+)?$"));
        const bool weight = field == QLatin1String("ym_weight");
        if (!number.match(t).hasMatch() || t.toDouble() > 1000) {
            *why = weight ? QStringLiteral("нужно число от 0 до 1000 (килограммы)") : QStringLiteral("нужно число от 0 до 1000 (сантиметры)");
            return QString();
        }
        const int dec = weight ? 3 : 2;
        return fmtNumber(std::round(t.toDouble() * std::pow(10, dec)) / std::pow(10, dec), dec);
    }
    if (field == QLatin1String("ym_commodity")) {
        QString t;
        for (QChar ch : raw)
            if (!ch.isSpace() && ch != QLatin1Char('.') && ch != QLatin1Char(','))
                t += ch;
        static const QRegularExpression digits(QStringLiteral("^[0-9]{4,14}$"));
        if (!digits.match(t).hasMatch()) { *why = QStringLiteral("код ТН ВЭД - от 4 до 14 цифр"); return QString(); }
        return t;
    }
    // ym_adult
    const QString v = raw.toLower();
    for (const char *yes : {"да", "yes", "true", "1", "+", "y"})
        if (v == QString::fromUtf8(yes))
            return QStringLiteral("да");
    for (const char *no : {"нет", "no", "false", "0", "-", "n"})
        if (v == QString::fromUtf8(no))
            return QStringLiteral("нет");
    *why = QStringLiteral("нужно «да» или «нет»");
    return QString();
}

QString currentYm(const QString &field, const ProductMarketInfo &m)
{
    if (field == QLatin1String("ym_vendor")) return m.vendor;
    if (field == QLatin1String("ym_countries")) return listText(m.countries);
    if (field == QLatin1String("ym_length")) return fmtNumber(m.lengthCm, 2);
    if (field == QLatin1String("ym_width")) return fmtNumber(m.widthCm, 2);
    if (field == QLatin1String("ym_height")) return fmtNumber(m.heightCm, 2);
    if (field == QLatin1String("ym_weight")) return fmtNumber(m.weightKg, 3);
    if (field == QLatin1String("ym_commodity")) return m.commodityCode;
    if (field == QLatin1String("ym_tags")) return listText(m.tags);
    return m.adult ? QStringLiteral("да") : QStringLiteral("нет");
}

// Одним запросом (JSON) записывает значения параметров для Маркета нескольким товарам; в строке только те поля, что менялись.
bool writeYm(const QList<QPair<int, QMap<QString, QString>>> &items, QString *error)
{
    for (int i = 0; i < items.size(); i += 200) {
        QJsonArray arr;
        for (int k = i; k < qMin<int>(i + 200, items.size()); ++k) {
            QJsonObject cols;
            for (const YmField &f : kYmFields) {
                const auto it = items[k].second.constFind(QLatin1String(f.field));
                if (it == items[k].second.constEnd())
                    continue;
                QString v = it.value();
                if (f.role == RoleCountries || f.role == RoleTags)
                    v = parseList(v).join(QLatin1Char(';'));
                else if (f.role == RoleAdult)
                    v = v == QStringLiteral("да") ? QStringLiteral("true") : QStringLiteral("false");
                cols.insert(QLatin1String(f.column), v);
            }
            QJsonObject row;
            row.insert(QStringLiteral("id"), items[k].first);
            row.insert(QStringLiteral("c"), cols);
            arr.append(row);
        }
        QString set;
        for (const YmField &f : kYmFields) {
            const QString col = QLatin1String(f.column);
            QString expr = QStringLiteral("j.c->>'%1'").arg(col);
            if (f.role == RoleLength || f.role == RoleWidth || f.role == RoleHeight || f.role == RoleWeight)
                expr = QStringLiteral("(%1)::numeric").arg(expr);
            else if (f.role == RoleAdult)
                expr = QStringLiteral("(%1)::boolean").arg(expr);
            if (!set.isEmpty())
                set += QLatin1String(", ");
            set += QStringLiteral("%1 = CASE WHEN jsonb_exists(j.c, '%1') THEN %2 ELSE p.%1 END").arg(col, expr);
        }
        QSqlQuery q;
        q.prepare(QStringLiteral("UPDATE products p SET %1 FROM jsonb_to_recordset(?::jsonb) AS j(id int, c jsonb) WHERE p.id = j.id").arg(set));
        q.addBindValue(QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)));
        if (!q.exec()) {
            if (error)
                *error = q.lastError().text();
            return false;
        }
    }
    return true;
}

} // namespace

QStringList roleLabels()
{
    return {QStringLiteral("— пропустить —"), QStringLiteral("Артикул (по нему ищем товар)"),
            QStringLiteral("Штрихкод (тоже ищет товар; новый код привяжется)"), QStringLiteral("Название"),
            QStringLiteral("Описание"), QStringLiteral("Цена"), QStringLiteral("Артикул на Маркете"),
            QStringLiteral("Свой код (для QR-этикеток)"), QStringLiteral("Бренд (для карточки Маркета)"),
            QStringLiteral("Страна производства (через «;»)"), QStringLiteral("Длина упаковки, см"),
            QStringLiteral("Ширина упаковки, см"), QStringLiteral("Высота упаковки, см"), QStringLiteral("Вес упаковки, кг"),
            QStringLiteral("ТН ВЭД"), QStringLiteral("Теги (через «;»)"), QStringLiteral("Для взрослых 18+ (да/нет)")};
}

int guessRole(const QString &header)
{
    const QString h = header.toLower().trimmed();
    auto has = [&](const char16_t *s) { return h.contains(QString::fromUtf16(s)); };
    auto starts = [&](const char16_t *s) { return h.startsWith(QString::fromUtf16(s)); };
    if (h.isEmpty())
        return RoleIgnore;
    if (starts(u"остаток") || starts(u"всего") || starts(u"места") || starts(u"фото") || starts(u"добавлен") || starts(u"категори"))
        return RoleIgnore;
    if (has(u"штрих") || has(u"ean") || has(u"barcode"))
        return RoleBarcode;
    if (has(u"на маркет") || has(u"market") || has(u"offer"))
        return RoleMarketSku;
    if (has(u"свой код") || has(u"custom") || has(u"qr"))
        return RoleCustomCode;
    if (has(u"бренд") || has(u"brand") || has(u"vendor"))
        return RoleVendor;
    if (has(u"страна") || has(u"country"))
        return RoleCountries;
    if (has(u"длина") || has(u"length"))
        return RoleLength;
    if (has(u"ширина") || has(u"width"))
        return RoleWidth;
    if (has(u"высота") || has(u"height"))
        return RoleHeight;
    if (starts(u"вес") || has(u"weight"))
        return RoleWeight;
    if (has(u"тн вэд") || has(u"твэд") || has(u"tnved") || has(u"commodity"))
        return RoleCommodity;
    if (h == QStringLiteral("теги") || h == QStringLiteral("tags") || h == QStringLiteral("тег"))
        return RoleTags;
    if (h == QLatin1String("18+") || has(u"взросл") || has(u"adult"))
        return RoleAdult;
    if (has(u"артикул") || has(u"sku") || has(u"ключ"))
        return RoleKey;
    if (has(u"наимен") || has(u"назв") || h == QLatin1String("name"))
        return RoleName;
    if (has(u"описан") || has(u"descr"))
        return RoleDescription;
    if (has(u"цена") || has(u"price") || has(u"стоим"))
        return RolePrice;
    return RoleIgnore;
}

QList<int> guessRoles(const QStringList &header, int width)
{
    const int w = width < 0 ? int(header.size()) : width;
    QList<int> roles;
    QSet<int> used;
    for (int i = 0; i < w; ++i) {
        int r = guessRole(header.value(i));
        if (r == RoleIgnore || used.contains(r))
            r = RoleIgnore;
        else
            used.insert(r);
        roles << r;
    }
    return roles;
}

int guessHeaderRow(const QVector<QStringList> &rows)
{
    int best = 0, bestScore = 0;
    for (int r = 0; r < qMin<int>(rows.size(), 30); ++r) {
        int score = 0;
        for (const QString &c : rows[r])
            if (guessRole(c) != RoleIgnore)
                ++score;
        if (score > bestScore) {
            bestScore = score;
            best = r;
        }
    }
    return bestScore >= 2 ? best : 0;
}

std::optional<double> parsePrice(const QString &raw)
{
    QString s;
    for (QChar ch : raw)
        if (!ch.isSpace())
            s += ch;
    static const QRegularExpression lead(QStringLiteral("^₽"));
    static const QRegularExpression tail(QStringLiteral("(₽|руб\\.?|р\\.?|rub)$"), QRegularExpression::CaseInsensitiveOption);
    s.remove(lead);
    s.remove(tail);
    if (s.contains(QLatin1Char(',')) && s.contains(QLatin1Char('.'))) {
        const int sep = qMax(s.lastIndexOf(QLatin1Char(',')), s.lastIndexOf(QLatin1Char('.')));
        QString head = s.left(sep);
        head.remove(QLatin1Char('.')).remove(QLatin1Char(','));
        s = head + QLatin1Char('.') + s.mid(sep + 1);
    } else {
        s.replace(QLatin1Char(','), QLatin1Char('.'));
    }
    static const QRegularExpression number(QStringLiteral("^[0-9]+(\\.[0-9]+)?$"));
    if (!number.match(s).hasMatch())
        return std::nullopt;
    const double n = std::round(s.toDouble() * 100.0) / 100.0;
    if (n > kMaxPrice)
        return std::nullopt;
    return n;
}

Plan plan(const QVector<QStringList> &rows, int headerIdx, const QList<int> &roles, const Options &options)
{
    Plan out;
    auto col = [&](int role) { return int(roles.indexOf(role)); };
    {
        QSet<int> seen;
        for (int r : roles) {
            if (r == RoleIgnore)
                continue;
            if (seen.contains(r)) {
                out.fatal = QStringLiteral("Роль «%1» назначена нескольким колонкам - оставьте её одной колонке").arg(roleLabels().value(r));
                return out;
            }
            seen.insert(r);
        }
    }
    const int keyCol = col(RoleKey), codeCol = col(RoleBarcode);
    if (keyCol < 0 && codeCol < 0) {
        out.fatal = QStringLiteral("Укажите колонку «Артикул» (или «Штрихкод»): по ней ищется товар");
        return out;
    }
    const int dataRoles[] = {RoleName, RoleDescription, RolePrice, RoleMarketSku, RoleCustomCode};
    bool anyData = false;
    for (int r : dataRoles)
        if (col(r) >= 0)
            anyData = true;
    for (const YmField &f : kYmFields)
        if (col(f.role) >= 0)
            anyData = true;
    if (!anyData && codeCol < 0) {
        out.fatal = QStringLiteral("Не выбрано, что обновлять: укажите хотя бы одну колонку - название, описание, цену, "
                                   "артикул на Маркете, свой код, штрихкод или параметры для Маркета (бренд, габариты и т. д.)");
        return out;
    }
    if (rows.size() - headerIdx - 1 > kMaxRows + 5000) {
        out.fatal = QStringLiteral("Слишком много строк (больше %1) - разбейте файл на части").arg(kMaxRows);
        return out;
    }

    // строки файла
    struct Line { int n; QStringList row; QString key; QStringList codes; };
    QList<Line> lines;
    static const QRegularExpression splitCodes(QStringLiteral("[,;\\s]+"));
    for (int i = headerIdx + 1; i < rows.size(); ++i) {
        const QStringList &row = rows[i];
        Line l;
        l.n = i + 1;
        l.row = row;
        l.key = cleanText(keyCol >= 0 ? row.value(keyCol) : QString());
        if (codeCol >= 0)
            for (const QString &part : row.value(codeCol).split(splitCodes, Qt::SkipEmptyParts))
                l.codes << Barcode::normalize(part);
        if (l.key.isEmpty() && l.codes.isEmpty())
            continue;
        if (l.key.toLower().startsWith(QStringLiteral("итого")))
            continue;
        lines << l;
    }
    if (lines.size() > kMaxRows) {
        out.fatal = QStringLiteral("Слишком много строк (%1, больше %2) - разбейте файл на части").arg(lines.size()).arg(kMaxRows);
        return out;
    }

    // товары по артикулам и штрихкодам - несколькими запросами на весь файл
    QStringList keys, codes;
    {
        QSet<QString> seenKeys, seenCodes;
        for (const Line &l : std::as_const(lines)) {
            if (!l.key.isEmpty() && !seenKeys.contains(l.key)) { seenKeys.insert(l.key); keys << l.key; }
            for (const QString &c : l.codes)
                if (!seenCodes.contains(c)) { seenCodes.insert(c); codes << c; }
        }
    }
    QHash<int, ProductRow> products;
    QHash<QString, QList<int>> bySku;   // ключ из файла -> товары (без учёта регистра)
    QHash<QString, int> byCode;
    for (int i = 0; i < keys.size(); i += 400) {
        const QStringList part = keys.mid(i, 400);
        QSqlQuery q;
        q.prepare(QStringLiteral("SELECT k.key, %1 FROM (VALUES %2) AS k(key) JOIN products p ON lower(p.sku) = lower(k.key)")
                      .arg(kProductCols, valuesList(int(part.size()))));
        for (const QString &k : part)
            q.addBindValue(k);
        if (!q.exec())
            continue;
        while (q.next()) {
            const ProductRow r = readProduct(q, 1);
            products.insert(r.id, r);
            bySku[q.value(0).toString()] << r.id;
        }
    }
    for (int i = 0; i < codes.size(); i += 400) {
        const QStringList part = codes.mid(i, 400);
        QStringList marks;
        for (int k = 0; k < part.size(); ++k)
            marks << QStringLiteral("?");
        QSqlQuery q;
        q.prepare(QStringLiteral("SELECT b.barcode, %1 FROM product_barcodes b JOIN products p ON p.id = b.product_id WHERE b.barcode IN (%2)")
                      .arg(kProductCols, marks.join(QLatin1Char(','))));
        for (const QString &c : part)
            q.addBindValue(c);
        if (!q.exec())
            continue;
        while (q.next()) {
            const ProductRow r = readProduct(q, 1);
            products.insert(r.id, r);
            byCode.insert(q.value(0).toString(), r.id);
        }
    }
    QHash<int, QSet<QString>> ownCodes;
    {
        const QList<int> ids = products.keys();
        for (int i = 0; i < ids.size(); i += 2000) {
            QStringList list;
            for (int k = i; k < qMin<int>(i + 2000, ids.size()); ++k)
                list << QString::number(ids[k]);
            QSqlQuery q;
            if (q.exec(QStringLiteral("SELECT product_id, barcode FROM product_barcodes WHERE product_id IN (%1)").arg(list.join(QLatin1Char(',')))))
                while (q.next())
                    ownCodes[q.value(0).toInt()].insert(q.value(1).toString());
        }
    }

    QHash<int, int> doneProduct;       // товар -> строка файла
    QHash<QString, int> claimed;       // новый штрихкод -> строка файла
    QHash<QString, int> newSkus;       // создаваемый артикул (в нижнем регистре) -> строка

    for (const Line &l : std::as_const(lines)) {
        Item item;
        item.n = l.n;
        item.status = QStringLiteral("same");
        item.sku = l.key;
        auto fail = [&](const QString &msg) { item.status = QStringLiteral("error"); item.message = msg; out.items << item; };
        const QString label = l.key.isEmpty() ? l.codes.join(QStringLiteral(", ")) : l.key;

        // 1) какой товар
        int pid = 0;
        if (!l.key.isEmpty()) {
            const QList<int> hits = bySku.value(l.key);
            QList<int> exact;
            for (int id : hits)
                if (products.value(id).sku == l.key)
                    exact << id;
            if (exact.size() == 1)
                pid = exact.first();
            else if (hits.size() == 1)
                pid = hits.first();
            else if (hits.size() > 1) {
                fail(QStringLiteral("Строка %1: под артикулом «%2» подходит несколько товаров - впишите артикул точно, как в базе").arg(l.n).arg(l.key));
                continue;
            }
        }
        QList<int> codeOwners;
        for (const QString &c : l.codes)
            if (byCode.contains(c) && !codeOwners.contains(byCode.value(c)))
                codeOwners << byCode.value(c);
        if (pid == 0 && codeOwners.size() == 1) {
            pid = codeOwners.first();
            item.byCode = true;
        } else if (codeOwners.size() > 1 && pid == 0) {
            fail(QStringLiteral("Строка %1: штрихкоды из этой строки принадлежат разным товарам").arg(l.n));
            continue;
        } else if (pid != 0) {
            bool other = false;
            for (int o : codeOwners)
                if (o != pid)
                    other = true;
            if (other) {
                fail(QStringLiteral("Строка %1: штрихкод из этой строки уже привязан к другому товару (не к «%2»)").arg(l.n).arg(products.value(pid).sku));
                continue;
            }
        }

        // 2) новые значения
        QMap<QString, QString> want;
        std::optional<double> wantPrice;
        QString problem;
        auto text = [&](int role, const QString &field) {
            const int c = col(role);
            if (c < 0)
                return;
            const QString v = cleanText(l.row.value(c));
            if (v.isEmpty()) {
                if (options.clearEmpty && (field == QLatin1String("description") || field == QLatin1String("market_sku") || field == QLatin1String("custom_code")))
                    want[field] = QString();
                return;
            }
            if (v.size() > limitOf(field)) {
                problem = QStringLiteral("Строка %1: «%2» длиннее %3 знаков").arg(l.n).arg(fieldLabel(field)).arg(limitOf(field));
                return;
            }
            want[field] = v;
        };
        text(RoleName, QStringLiteral("name"));
        text(RoleDescription, QStringLiteral("description"));
        text(RoleMarketSku, QStringLiteral("market_sku"));
        text(RoleCustomCode, QStringLiteral("custom_code"));
        if (col(RolePrice) >= 0) {
            const QString raw = cleanText(l.row.value(col(RolePrice)));
            if (!raw.isEmpty()) {
                const auto p = parsePrice(raw);
                if (!p) {
                    if (problem.isEmpty())
                        problem = QStringLiteral("Строка %1: цена «%2» - нужно число от 0 до 10 000 000, до двух знаков после запятой").arg(l.n).arg(raw);
                } else {
                    wantPrice = *p;
                }
            }
        }
        QMap<QString, QString> wantYm;
        for (const YmField &f : kYmFields) {
            const int c = col(f.role);
            if (c < 0)
                continue;
            const QString raw = cleanText(l.row.value(c));
            if (raw.isEmpty())
                continue;
            QString why;
            const QString v = parseYm(QLatin1String(f.field), raw, &why);
            if (!why.isEmpty()) {
                if (problem.isEmpty())
                    problem = QStringLiteral("Строка %1: «%2» - %3").arg(l.n).arg(fieldLabel(QLatin1String(f.field)), why);
                continue;
            }
            wantYm.insert(QLatin1String(f.field), v);
        }
        if (!problem.isEmpty()) {
            fail(problem);
            continue;
        }
        for (const QString &c : l.codes) {
            QString bad;
            if (!Barcode::isValid(c, &bad)) {
                problem = QStringLiteral("Строка %1: штрихкод %2: %3").arg(l.n).arg(c, bad);
                break;
            }
        }
        if (!problem.isEmpty()) {
            fail(problem);
            continue;
        }

        // 3) найден - сравниваем; не найден - создаём или пропускаем
        if (pid != 0) {
            const ProductRow cur = products.value(pid);
            item.id = pid;
            item.sku = cur.sku;
            item.name = cur.name;
            if (doneProduct.contains(pid)) {
                fail(QStringLiteral("Строка %1: товар «%2» уже был в строке %3 - повтор пропущен").arg(l.n).arg(cur.sku).arg(doneProduct.value(pid)));
                continue;
            }
            const QSet<QString> own = ownCodes.value(pid);
            QStringList add;
            for (const QString &c : l.codes)
                if (!own.contains(c))
                    add << c;
            QString clash;
            for (const QString &c : add)
                if (claimed.contains(c)) { clash = c; break; }
            if (!clash.isEmpty()) {
                fail(QStringLiteral("Строка %1: штрихкод %2 уже указан в строке %3").arg(l.n).arg(clash).arg(claimed.value(clash)));
                continue;
            }
            auto curText = [&](const QString &f) {
                if (f == QLatin1String("name")) return cur.name;
                if (f == QLatin1String("description")) return cur.description;
                if (f == QLatin1String("market_sku")) return cur.marketSku;
                return cur.customCode;
            };
            for (const QString &f : {QStringLiteral("name"), QStringLiteral("description"), QStringLiteral("market_sku"), QStringLiteral("custom_code")})
                if (want.contains(f) && want.value(f) != cleanText(curText(f)))
                    item.changes << Change{f, fieldLabel(f), curText(f), want.value(f)};
            if (wantPrice && std::abs(*wantPrice - cur.price) >= 0.005)
                item.changes << Change{QStringLiteral("price"), fieldLabel(QStringLiteral("price")), fmtPrice(cur.price), fmtPrice(*wantPrice)};
            for (const YmField &f : kYmFields) {
                const QString field = QLatin1String(f.field);
                if (!wantYm.contains(field))
                    continue;
                const QString now = currentYm(field, cur.market);
                if (wantYm.value(field) != now)
                    item.changes << Change{field, fieldLabel(field), now, wantYm.value(field)};
            }
            if (!add.isEmpty()) {
                QSet<QString> uniq;
                for (const QString &c : add)
                    if (!uniq.contains(c)) { uniq.insert(c); item.newCodes << c; }
                item.changes << Change{QStringLiteral("barcodes"), fieldLabel(QStringLiteral("barcodes")), QString(), item.newCodes.join(QStringLiteral(", "))};
                for (const QString &c : std::as_const(item.newCodes))
                    claimed.insert(c, l.n);
            }
            doneProduct.insert(pid, l.n);
            item.status = item.changes.isEmpty() ? QStringLiteral("same") : QStringLiteral("update");
        } else if (options.createMissing && !l.key.isEmpty()) {
            if (newSkus.contains(l.key.toLower())) {
                fail(QStringLiteral("Строка %1: товар «%2» уже был в строке %3 - повтор пропущен").arg(l.n).arg(l.key).arg(newSkus.value(l.key.toLower())));
                continue;
            }
            if (l.key.size() > 100) {
                fail(QStringLiteral("Строка %1: артикул длиннее 100 знаков").arg(l.n));
                continue;
            }
            QStringList add;
            {
                QSet<QString> uniq;
                for (const QString &c : l.codes)
                    if (!uniq.contains(c)) { uniq.insert(c); add << c; }
            }
            QString clash;
            for (const QString &c : add)
                if (claimed.contains(c)) { clash = c; break; }
            if (!clash.isEmpty()) {
                fail(QStringLiteral("Строка %1: штрихкод %2 уже указан в строке %3").arg(l.n).arg(clash).arg(claimed.value(clash)));
                continue;
            }
            item.createName = want.contains(QStringLiteral("name")) && !want.value(QStringLiteral("name")).isEmpty() ? want.value(QStringLiteral("name")) : l.key;
            item.createDescription = want.value(QStringLiteral("description"));
            item.createMarketSku = want.value(QStringLiteral("market_sku"));
            item.createCustomCode = want.value(QStringLiteral("custom_code"));
            item.createPrice = wantPrice.value_or(0);
            item.createYm = wantYm;
            item.name = item.createName;
            item.newCodes = add;
            for (const QString &c : add)
                claimed.insert(c, l.n);
            newSkus.insert(l.key.toLower(), l.n);
            item.status = QStringLiteral("create");
        } else {
            item.status = QStringLiteral("missing");
            item.message = QStringLiteral("Строка %1: товар «%2» не найден%3").arg(l.n).arg(label,
                options.createMissing ? QString() : QStringLiteral(" (включите «Создавать новые товары», если его нужно добавить)"));
        }
        out.items << item;
    }
    for (const Item &it : std::as_const(out.items)) {
        ++out.rows;
        if (it.status == QLatin1String("update")) ++out.update;
        else if (it.status == QLatin1String("same")) ++out.same;
        else if (it.status == QLatin1String("create")) ++out.create;
        else if (it.status == QLatin1String("missing")) ++out.missing;
        else ++out.error;
        if ((it.status == QLatin1String("error") || it.status == QLatin1String("missing")) && !it.message.isEmpty()) {
            QString t = it.message;
            t.remove(QRegularExpression(QStringLiteral("^Строка \\d+: ")));
            out.problems << Problem{it.n, t};
        }
    }
    return out;
}

bool apply(const QVector<QStringList> &rows, int headerIdx, const QList<int> &roles, const Options &options,
           Result *result, QString *error)
{
    QSqlDatabase db = QSqlDatabase::database();
    if (!db.transaction()) {
        if (error)
            *error = db.lastError().text();
        return false;
    }
    auto refuse = [&](const QString &msg) {
        db.rollback();
        if (error)
            *error = msg;
        return false;
    };

    Plan p = plan(rows, headerIdx, roles, options);
    if (!p.fatal.isEmpty())
        return refuse(p.fatal);

    QList<const Item *> upd, withCodes, ymUpd;
    QList<int> backupIds;
    for (const Item &it : std::as_const(p.items)) {
        if (it.status != QLatin1String("update"))
            continue;
        bool dataChange = false, ymChange = false;
        for (const Change &c : it.changes) {
            if (c.field.startsWith(QLatin1String("ym_")))
                ymChange = true;
            else if (c.field != QLatin1String("barcodes"))
                dataChange = true;
        }
        if (dataChange)
            upd << &it;
        if (ymChange)
            ymUpd << &it;
        if (!it.newCodes.isEmpty())
            withCodes << &it;
        if (!backupIds.contains(it.id))
            backupIds << it.id;
    }
    Result res;
    res.plan = p;
    res.problems = p.problems;
    if (!backupIds.isEmpty())
        res.backup = ProductExport::build(&backupIds); // до записи: так видны прежние значения

    // обновления - пачками, одним запросом на пачку. Флаги «менять ли поле» нужны, чтобы пустая строка
    // («очистить поле») отличалась от «не трогать» независимо от того, как драйвер передаёт пустой текст.
    for (int i = 0; i < upd.size(); i += 100) {
        const QList<const Item *> part = upd.mid(i, 100);
        QStringList marks;
        for (int k = 0; k < part.size(); ++k)
            marks << QStringLiteral("(?::int, ?::boolean, ?::text, ?::boolean, ?::text, ?::boolean, ?::float8, ?::boolean, ?::text, ?::boolean, ?::text)");
        QSqlQuery q;
        q.prepare(QStringLiteral("UPDATE products p SET "
                                 "name = CASE WHEN v.sn THEN COALESCE(v.name, '') ELSE p.name END, "
                                 "description = CASE WHEN v.sd THEN COALESCE(v.description, '') ELSE p.description END, "
                                 "price = CASE WHEN v.sp THEN v.price ELSE p.price END, "
                                 "market_sku = CASE WHEN v.sm THEN COALESCE(v.market_sku, '') ELSE p.market_sku END, "
                                 "custom_code = CASE WHEN v.sc THEN COALESCE(v.custom_code, '') ELSE p.custom_code END "
                                 "FROM (VALUES %1) AS v(id, sn, name, sd, description, sp, price, sm, market_sku, sc, custom_code) WHERE p.id = v.id")
                      .arg(marks.join(QLatin1Char(','))));
        for (const Item *it : part) {
            auto find = [&](const char *f) -> const Change * {
                for (const Change &c : it->changes)
                    if (c.field == QLatin1String(f))
                        return &c;
                return nullptr;
            };
            auto addText = [&](const char *f) {
                const Change *c = find(f);
                q.addBindValue(c != nullptr);
                q.addBindValue(c ? c->to : QString());
            };
            q.addBindValue(it->id);
            addText("name");
            addText("description");
            const Change *price = find("price");
            q.addBindValue(price != nullptr);
            q.addBindValue(price ? price->to.toDouble() : 0.0);
            addText("market_sku");
            addText("custom_code");
        }
        if (!q.exec())
            return refuse(q.lastError().text());
    }

    {
        QList<QPair<int, QMap<QString, QString>>> ymRows;
        for (const Item *it : std::as_const(ymUpd)) {
            QMap<QString, QString> values;
            for (const Change &c : it->changes)
                if (c.field.startsWith(QLatin1String("ym_")))
                    values.insert(c.field, c.to);
            ymRows.append({it->id, values});
        }
        QString ymError;
        if (!writeYm(ymRows, &ymError))
            return refuse(ymError);
    }

    auto linkCodes = [&](const QList<QPair<QString, int>> &pairs, int n) -> bool {
        for (int i = 0; i < pairs.size(); i += 200) {
            const auto part = pairs.mid(i, 200);
            QStringList marks;
            for (int k = 0; k < part.size(); ++k)
                marks << QStringLiteral("(?, ?)");
            QSqlQuery q;
            q.prepare(QStringLiteral("INSERT INTO product_barcodes (barcode, product_id) VALUES %1 ON CONFLICT (barcode) DO NOTHING").arg(marks.join(QLatin1Char(','))));
            for (const auto &pr : part) {
                q.addBindValue(pr.first);
                q.addBindValue(pr.second);
            }
            if (!q.exec())
                return false;
            if (q.numRowsAffected() != part.size()) {
                for (const auto &pr : part) {
                    QSqlQuery chk;
                    chk.prepare("SELECT 1 FROM product_barcodes WHERE barcode = ? AND product_id = ?");
                    chk.addBindValue(pr.first);
                    chk.addBindValue(pr.second);
                    if (!chk.exec() || !chk.next())
                        res.problems << Problem{n, QStringLiteral("штрихкод %1 за это время привязали к другому товару - не привязан").arg(pr.first)};
                }
            }
        }
        return true;
    };
    QList<QPair<QString, int>> pairs;
    for (const Item *it : std::as_const(withCodes))
        for (const QString &c : it->newCodes)
            pairs << qMakePair(c, it->id);
    if (!linkCodes(pairs, 0))
        return refuse(QStringLiteral("Не удалось привязать штрихкоды"));

    // Новые товары - пачками по несколько штук одним запросом (а не по запросу на товар и ещё один на штрихкоды).
    int created = 0;
    QList<const Item *> toCreate;
    for (const Item &it : std::as_const(p.items))
        if (it.status == QLatin1String("create"))
            toCreate << &it;
    for (int i = 0; i < toCreate.size(); i += 100) {
        const auto part = toCreate.mid(i, 100);
        QStringList marks;
        for (int k = 0; k < part.size(); ++k)
            marks << QStringLiteral("(?, ?, ?, '', ?, ?, ?)");
        QSqlQuery q;
        q.prepare(QStringLiteral("INSERT INTO products (sku, name, description, photo_path, price, custom_code, market_sku) VALUES %1 "
                                 "ON CONFLICT (sku) DO NOTHING RETURNING id, sku").arg(marks.join(QLatin1Char(','))));
        for (const Item *it : part) {
            q.addBindValue(it->sku);
            q.addBindValue(it->createName);
            q.addBindValue(it->createDescription);
            q.addBindValue(it->createPrice);
            q.addBindValue(it->createCustomCode);
            q.addBindValue(it->createMarketSku);
        }
        if (!q.exec())
            return refuse(q.lastError().text());
        QHash<QString, int> newIds;
        while (q.next())
            newIds.insert(q.value(1).toString(), q.value(0).toInt());
        QList<QPair<QString, int>> codePairs;
        QList<QPair<int, QMap<QString, QString>>> ymRows;
        for (const Item *it : part) {
            if (!newIds.contains(it->sku)) {
                res.problems << Problem{it->n, QStringLiteral("товар «%1» за это время уже создали - пропущен").arg(it->sku)};
                continue;
            }
            ++created;
            if (!it->createYm.isEmpty())
                ymRows.append({newIds.value(it->sku), it->createYm});
            for (const QString &c : it->newCodes)
                codePairs << qMakePair(c, newIds.value(it->sku));
        }
        QString ymError;
        if (!writeYm(ymRows, &ymError))
            return refuse(ymError);
        if (!codePairs.isEmpty() && !linkCodes(codePairs, 0))
            return refuse(QStringLiteral("Не удалось привязать штрихкоды"));
    }
    if (!db.commit())
        return refuse(db.lastError().text());

    QSet<int> changed;
    for (const Item *it : std::as_const(upd))
        changed.insert(it->id);
    for (const Item *it : std::as_const(withCodes))
        changed.insert(it->id);
    for (const Item *it : std::as_const(ymUpd))
        changed.insert(it->id);
    res.updated = int(changed.size());
    res.created = created;
    if (result)
        *result = res;
    return true;
}

} // namespace ProductSync
