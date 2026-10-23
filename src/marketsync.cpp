#include "marketsync.h"

#include <QSqlQuery>
#include <QStringList>

namespace MarketSync {

QHash<QString, LocalItem> loadLocal(const MarketAccount &acc)
{
    QString warehouseFilter;
    if (!acc.localWarehouseIds.isEmpty()) {
        QStringList ids;
        for (int id : acc.localWarehouseIds)
            ids << QString::number(id);
        warehouseFilter = QStringLiteral(" AND s.warehouse_id IN (%1)").arg(ids.join(','));
    }
    QSqlQuery q(QStringLiteral(
                    "SELECT p.id, COALESCE(NULLIF(TRIM(p.market_sku), ''), p.sku), p.name, p.price, "
                    "COALESCE((SELECT SUM(s.quantity) FROM stock s WHERE s.product_id = p.id%1), 0) "
                    "FROM products p")
                    .arg(warehouseFilter));
    QHash<QString, LocalItem> result;
    while (q.next()) {
        // Несколько наших товаров могут быть привязаны к одному артикулу
        // Маркета - тогда их остатки складываются.
        LocalItem &item = result[q.value(1).toString().trimmed()];
        item.productIds.append(q.value(0).toInt());
        item.name = item.name.isEmpty() ? q.value(2).toString() : item.name + " + " + q.value(2).toString();
        if (item.price <= 0)
            item.price = q.value(3).toDouble();
        item.stock += qMax<qint64>(0, q.value(4).toLongLong());
    }
    return result;
}

bool compareStocks(const MarketAccount &acc, MarketApi &api, QList<StockRow> *rows, QString *error)
{
    QHash<QString, MarketOffer> catalog;
    QHash<QString, MarketStock> stocks;
    if (!api.catalog(acc, &catalog, error) || !api.stocks(acc, &stocks, error))
        return false;

    const auto local = loadLocal(acc);
    rows->clear();
    for (auto it = local.cbegin(); it != local.cend(); ++it) {
        StockRow row;
        row.offerId = it.key();
        row.name = it->name;
        row.local = it->stock;
        const MarketStock ms = stocks.value(it.key());
        row.marketFit = ms.fit;
        row.marketAvailable = ms.available;
        const auto cat = catalog.constFind(it.key());
        if (cat == catalog.cend())
            row.status = Status::NotInCatalog;
        else if (cat->archived)
            row.status = Status::Archived;
        else if (!cat->inCampaign)
            row.status = Status::NotInCampaign;
        else
            row.status = row.local == row.marketFit ? Status::Match : Status::Differ;
        rows->append(row);
    }
    // Товары, которые есть на Маркете с ненулевым остатком, но которых нет у нас.
    for (auto it = catalog.cbegin(); it != catalog.cend(); ++it) {
        if (local.contains(it.key()) || it->archived || !it->inCampaign)
            continue;
        const MarketStock ms = stocks.value(it.key());
        if (ms.fit <= 0)
            continue;
        StockRow row;
        row.offerId = it.key();
        row.name = it->name;
        row.marketFit = ms.fit;
        row.marketAvailable = ms.available;
        row.status = Status::NotInApp;
        rows->append(row);
    }
    return true;
}

QList<QPair<QString, qint64>> itemsToPush(const QList<StockRow> &rows, bool zeroMissing, int *zeroed)
{
    QList<QPair<QString, qint64>> items;
    if (zeroed)
        *zeroed = 0;
    for (const StockRow &r : rows) {
        if (r.status == Status::Differ) {
            items.append({r.offerId, r.local});
        } else if (r.status == Status::NotInApp && zeroMissing) {
            items.append({r.offerId, 0});
            if (zeroed)
                ++*zeroed;
        }
    }
    return items;
}

} // namespace MarketSync
