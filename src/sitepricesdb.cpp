#include "siteprices.h"


#include <QHash>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

#include <cmath>

// Работа с таблицей site_prices (цены сайта); чистая логика - в siteprices.cpp.
namespace SitePrices {

namespace {

double round2(double v)
{
    return std::round(v * 100.0) / 100.0;
}

QDate parseDate(const QVariant &v)
{
    return v.isNull() ? QDate() : QDate::fromString(v.toString(), Qt::ISODate);
}

QVariant dateOrNull(const QDate &d)
{
    return d.isValid() ? QVariant(d.toString(Qt::ISODate)) : QVariant(QMetaType::fromType<QString>());
}

} // namespace

QList<ModelRow> loadModels(QString *error)
{
    // Два обычных запроса, а не один пакет через «;»: пакет с несколькими результатами работает только со встроенным
    // драйвером PostgreSQL; на вкладке это одно обращение при открытии, лишний запрос не заметен.
    QList<QVariantList> products;
    QList<QVariantList> prices;
    {
        QSqlQuery q;
        if (!q.exec(QStringLiteral(
                "SELECT p.sku, p.name, p.price::float8, COALESCE((SELECT SUM(s.quantity) FROM stock s WHERE s.product_id = p.id), 0)::bigint "
                "FROM products p ORDER BY p.sku"))) {
            if (error)
                *error = q.lastError().text();
            return {};
        }
        while (q.next())
            products.append({q.value(0), q.value(1), q.value(2), q.value(3)});
    }
    {
        QSqlQuery q;
        if (!q.exec(QStringLiteral(
                "SELECT model_key, price::float8, sale_price::float8, to_char(sale_from, 'YYYY-MM-DD'), to_char(sale_to, 'YYYY-MM-DD'), "
                "updated_by, updated_at FROM site_prices"))) {
            if (error)
                *error = q.lastError().text();
            return {};
        }
        while (q.next())
            prices.append({q.value(0), q.value(1), q.value(2), q.value(3), q.value(4), q.value(5), q.value(6)});
    }

    QList<ModelRow> out;
    QHash<QString, int> index;
    for (const QVariantList &r : products) {
        const QString sku = r[0].toString();
        const QString key = modelKey(sku);
        auto it = index.find(key);
        if (it == index.end()) {
            ModelRow m;
            m.key = key;
            index.insert(key, out.size());
            out.append(m);
            it = index.find(key);
        }
        ModelRow &m = out[it.value()];
        ++m.sizes;
        m.stock += r[3].toLongLong();
        const QString name = r[1].toString().trimmed();
        // Название берём у первого размера, у которого оно есть (иногда в названии стоит сам артикул - такое не годится).
        if (m.name.isEmpty() && !name.isEmpty() && name != sku.trimmed())
            m.name = name;
        const double wp = r[2].toDouble();
        if (wp > 0 && (m.warehousePrice <= 0 || wp < m.warehousePrice))
            m.warehousePrice = round2(wp);
    }
    for (ModelRow &m : out)
        if (m.name.isEmpty())
            m.name = m.key;

    for (const QVariantList &r : prices) {
        const QString key = r[0].toString();
        auto it = index.find(key);
        if (it == index.end()) {
            ModelRow m;
            m.key = key;
            m.name = QStringLiteral("(в товарах такой модели больше нет)");
            m.orphan = true;
            index.insert(key, out.size());
            out.append(m);
            it = index.find(key);
        }
        ModelRow &m = out[it.value()];
        m.hasSite = true;
        m.site.modelKey = key;
        m.site.price = r[1].toDouble();
        if (!r[2].isNull())
            m.site.salePrice = r[2].toDouble();
        m.site.saleFrom = parseDate(r[3]);
        m.site.saleTo = parseDate(r[4]);
        m.updatedBy = r[5].toString();
        m.updatedAt = r[6].toDateTime();
    }
    return out;
}

bool save(const QList<Entry> &entries, const QString &user, QString *error)
{
    if (entries.isEmpty())
        return true;
    for (const Entry &e : entries) {
        if (e.modelKey.trimmed().isEmpty() || !(e.price > 0)) {
            if (error)
                *error = QStringLiteral("У модели «%1» не задана цена сайта").arg(e.modelKey);
            return false;
        }
        if (e.salePrice && *e.salePrice >= e.price) {
            if (error)
                *error = QStringLiteral("У модели «%1» цена со скидкой должна быть меньше обычной").arg(e.modelKey);
            return false;
        }
        if (e.saleFrom.isValid() && e.saleTo.isValid() && e.saleTo < e.saleFrom) {
            if (error)
                *error = QStringLiteral("У модели «%1» скидка заканчивается раньше, чем начинается").arg(e.modelKey);
            return false;
        }
    }
    QSqlDatabase db = QSqlDatabase::database();
    if (!db.transaction()) {
        if (error)
            *error = db.lastError().text();
        return false;
    }
    for (const Entry &e : entries) {
        QSqlQuery q;
        q.prepare(QStringLiteral(
            "INSERT INTO site_prices (model_key, price, sale_price, sale_from, sale_to, updated_by, updated_at) "
            "VALUES (?, ?::numeric, ?::numeric, ?::date, ?::date, ?, NOW()) "
            "ON CONFLICT (model_key) DO UPDATE SET price = EXCLUDED.price, sale_price = EXCLUDED.sale_price, "
            "sale_from = EXCLUDED.sale_from, sale_to = EXCLUDED.sale_to, updated_by = EXCLUDED.updated_by, updated_at = NOW()"));
        q.addBindValue(e.modelKey.trimmed());
        q.addBindValue(round2(e.price));
        q.addBindValue(e.salePrice ? QVariant(round2(*e.salePrice)) : QVariant(QMetaType::fromType<double>()));
        // Дат нет, когда скидки нет: иначе в базе останутся хвосты от прежней акции.
        const bool hasSale = e.salePrice.has_value();
        q.addBindValue(hasSale ? dateOrNull(e.saleFrom) : dateOrNull(QDate()));
        q.addBindValue(hasSale ? dateOrNull(e.saleTo) : dateOrNull(QDate()));
        q.addBindValue(user);
        if (!q.exec()) {
            db.rollback();
            if (error)
                *error = q.lastError().text();
            return false;
        }
    }
    if (!db.commit()) {
        db.rollback();
        if (error)
            *error = db.lastError().text();
        return false;
    }
    return true;
}

bool remove(const QStringList &modelKeys, QString *error)
{
    if (modelKeys.isEmpty())
        return true;
    QSqlDatabase db = QSqlDatabase::database();
    if (!db.transaction()) {
        if (error)
            *error = db.lastError().text();
        return false;
    }
    constexpr int kChunk = 300;
    for (int start = 0; start < modelKeys.size(); start += kChunk) {
        const QStringList chunk = modelKeys.mid(start, kChunk);
        QStringList marks;
        for (int i = 0; i < chunk.size(); ++i)
            marks << QStringLiteral("?");
        QSqlQuery q;
        q.prepare(QStringLiteral("DELETE FROM site_prices WHERE model_key IN (%1)").arg(marks.join(',')));
        for (const QString &k : chunk)
            q.addBindValue(k);
        if (!q.exec()) {
            db.rollback();
            if (error)
                *error = q.lastError().text();
            return false;
        }
    }
    if (!db.commit()) {
        db.rollback();
        if (error)
            *error = db.lastError().text();
        return false;
    }
    return true;
}

} // namespace SitePrices
