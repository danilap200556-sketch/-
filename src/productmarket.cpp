#include "productmarket.h"

#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace ProductMarket {

namespace {

QString nn(const QString &s) { return s.isNull() ? QStringLiteral("") : s; }

} // namespace

QString selectColumns(const QString &prefix)
{
    const QString p = prefix;
    return QStringLiteral("COALESCE(%1market_vendor, ''), COALESCE(%1market_countries, ''), %1market_length_cm::float8, "
                          "%1market_width_cm::float8, %1market_height_cm::float8, %1market_weight_kg::float8, "
                          "COALESCE(%1market_commodity_code, ''), COALESCE(%1market_tags, ''), COALESCE(%1market_adult, FALSE), "
                          "COALESCE(%1market_category_id, 0), COALESCE(%1market_category_name, ''), COALESCE(%1market_params, '{}')")
        .arg(p);
}

ProductMarketInfo fromValues(const QVariantList &v, int first)
{
    ProductMarketInfo m;
    int i = first;
    m.vendor = v.value(i++).toString();
    m.countries = MarketCards::splitList(v.value(i++).toString());
    m.lengthCm = v.value(i++).toDouble();
    m.widthCm = v.value(i++).toDouble();
    m.heightCm = v.value(i++).toDouble();
    m.weightKg = v.value(i++).toDouble();
    m.commodityCode = v.value(i++).toString();
    m.tags = MarketCards::splitList(v.value(i++).toString());
    m.adult = v.value(i++).toBool();
    m.categoryId = v.value(i++).toLongLong();
    m.categoryName = v.value(i++).toString();
    m.params = MarketCards::paramsFromJson(v.value(i++).toString());
    return m;
}

ProductMarketInfo fromRow(const QSqlQuery &q, int first)
{
    QVariantList v;
    for (int i = 0; i < first + kColumnCount; ++i)
        v << q.value(i);
    return fromValues(v, first);
}

QString updateAssignments()
{
    return QStringLiteral("market_vendor = ?, market_countries = ?, market_length_cm = ?, market_width_cm = ?, "
                          "market_height_cm = ?, market_weight_kg = ?, market_commodity_code = ?, market_tags = ?, "
                          "market_adult = ?, market_category_id = ?, market_category_name = ?, market_params = ?");
}

QString insertColumns()
{
    return QStringLiteral("market_vendor, market_countries, market_length_cm, market_width_cm, market_height_cm, "
                          "market_weight_kg, market_commodity_code, market_tags, market_adult, market_category_id, "
                          "market_category_name, market_params");
}

QString insertMarks()
{
    return QStringList(kColumnCount, QStringLiteral("?")).join(QStringLiteral(", "));
}

void bind(QSqlQuery &q, const ProductMarketInfo &m)
{
    q.addBindValue(nn(m.vendor.trimmed()));
    q.addBindValue(nn(m.countries.join(QStringLiteral(";"))));
    q.addBindValue(m.lengthCm);
    q.addBindValue(m.widthCm);
    q.addBindValue(m.heightCm);
    q.addBindValue(m.weightKg);
    q.addBindValue(nn(m.commodityCode.trimmed()));
    q.addBindValue(nn(m.tags.join(QStringLiteral(";"))));
    q.addBindValue(m.adult);
    q.addBindValue(m.categoryId > 0 ? QVariant(m.categoryId) : QVariant(QMetaType(QMetaType::LongLong)));
    q.addBindValue(nn(m.categoryName));
    q.addBindValue(MarketCards::paramsToJson(m.params));
}

QString summary(const ProductMarketInfo &m)
{
    QStringList parts;
    if (!m.vendor.isEmpty())
        parts << m.vendor;
    if (!m.countries.isEmpty())
        parts << m.countries.join(QStringLiteral(", "));
    if (m.hasPack())
        parts << QStringLiteral("%1×%2×%3 см, %4 кг").arg(m.lengthCm).arg(m.widthCm).arg(m.heightCm).arg(m.weightKg);
    if (m.categoryId > 0)
        parts << QStringLiteral("%1, характеристик: %2").arg(m.categoryName.isEmpty() ? QString::number(m.categoryId) : m.categoryName).arg(m.params.size());
    return parts.join(QStringLiteral(" · "));
}

bool load(int productId, ProductMarketInfo *out)
{
    QSqlQuery q;
    q.prepare(QStringLiteral("SELECT %1 FROM products WHERE id = ?").arg(selectColumns()));
    q.addBindValue(productId);
    if (!q.exec() || !q.next())
        return false;
    *out = fromRow(q, 0);
    return true;
}

QHash<int, ProductMarketInfo> loadMany(const QList<int> &ids, QString *error)
{
    QHash<int, ProductMarketInfo> result;
    if (ids.isEmpty())
        return result;
    QStringList marks(ids.size(), QStringLiteral("?"));
    QSqlQuery q;
    q.prepare(QStringLiteral("SELECT p.id, %1 FROM products p WHERE p.id IN (%2)").arg(selectColumns(QStringLiteral("p.")), marks.join(QLatin1Char(','))));
    for (int id : ids)
        q.addBindValue(id);
    if (!q.exec()) {
        if (error)
            *error = q.lastError().text();
        return result;
    }
    while (q.next())
        result.insert(q.value(0).toInt(), fromRow(q, 1));
    return result;
}

bool saveMany(const QList<QPair<int, ProductMarketInfo>> &items, QString *error)
{
    if (items.isEmpty())
        return true;
    // Один запрос: значения приводятся к типам столбцов, строки сопоставляются по id.
    QStringList rows;
    for (int i = 0; i < items.size(); ++i)
        rows << QStringLiteral("(?::int, ?::text, ?::text, ?::numeric, ?::numeric, ?::numeric, ?::numeric, ?::text, ?::text, ?::boolean, "
                               "?::bigint, ?::text, ?::text)");
    QSqlQuery q;
    q.prepare(QStringLiteral("UPDATE products p SET market_vendor = v.vendor, market_countries = v.countries, market_length_cm = v.l, "
                             "market_width_cm = v.w, market_height_cm = v.h, market_weight_kg = v.kg, market_commodity_code = v.code, "
                             "market_tags = v.tags, market_adult = v.adult, market_category_id = v.cat, market_category_name = v.catname, "
                             "market_params = v.params FROM (VALUES %1) AS v(id, vendor, countries, l, w, h, kg, code, tags, adult, cat, catname, params) "
                             "WHERE p.id = v.id").arg(rows.join(QLatin1Char(','))));
    for (const auto &it : items) {
        const ProductMarketInfo &m = it.second;
        q.addBindValue(it.first);
        q.addBindValue(nn(m.vendor.trimmed()));
        q.addBindValue(nn(m.countries.join(QStringLiteral(";"))));
        q.addBindValue(m.lengthCm);
        q.addBindValue(m.widthCm);
        q.addBindValue(m.heightCm);
        q.addBindValue(m.weightKg);
        q.addBindValue(nn(m.commodityCode.trimmed()));
        q.addBindValue(nn(m.tags.join(QStringLiteral(";"))));
        q.addBindValue(m.adult);
        q.addBindValue(m.categoryId > 0 ? QVariant(m.categoryId) : QVariant(QMetaType(QMetaType::LongLong)));
        q.addBindValue(nn(m.categoryName));
        q.addBindValue(MarketCards::paramsToJson(m.params));
    }
    if (!q.exec()) {
        if (error)
            *error = q.lastError().text();
        return false;
    }
    return true;
}

} // namespace ProductMarket
