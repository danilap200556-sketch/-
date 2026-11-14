#include "ordermarking.h"

#include "markingcode.h"
#include "markingowner.h"
#include "marketapi.h"
#include "orderwriteoff.h"

#include <QHash>
#include <QSet>
#include <QSqlQuery>

namespace OrderMarking {

QString orderRef(int accountId, const QString &orderNumber)
{
    return QStringLiteral("market:%1:%2").arg(accountId).arg(orderNumber.trimmed());
}

int Plan::need() const
{
    int n = 0;
    for (const Line &l : lines)
        n += l.need;
    return n;
}

int Plan::pickedCount() const
{
    int n = 0;
    for (const Line &l : lines)
        n += int(l.picked.size());
    return n;
}

int Plan::issuedCount() const
{
    int n = 0;
    for (const Line &l : lines)
        n += l.issued;
    return n;
}

int Plan::missingCount() const
{
    int n = 0;
    for (const Line &l : lines)
        n += l.missing();
    return n;
}

QList<int> Plan::pickedIds() const
{
    QList<int> out;
    for (const Line &l : lines)
        out += l.picked;
    return out;
}

QList<int> Plan::issuedIds() const
{
    QList<int> out;
    for (const Line &l : lines)
        out += l.issuedIds;
    return out;
}

QList<QPair<int, int>> Plan::targetsWithMissing() const
{
    QList<QPair<int, int>> out;
    for (const Line &l : lines) {
        const QPair<int, int> t(l.ownerId, l.warehouseId);
        if (l.missing() > 0 && !out.contains(t))
            out << t;
    }
    return out;
}

namespace {
bool cancelled(const QString &status)
{
    return status.startsWith(QLatin1String("CANCELLED"), Qt::CaseInsensitive) || status.compare(QLatin1String("UNPAID"), Qt::CaseInsensitive) == 0;
}
} // namespace

Plan build(const QList<OrderInput> &orders)
{
    Plan plan;
    // ИП и склад кабинета: ИП задан в настройках кабинета, склад - склад списания; друг с другом они не связаны.
    QHash<int, QPair<int, int>> targetByAccount;   // кабинет -> (ИП, склад)
    for (const MarketAccount &a : MarketAccount::loadAll())
        targetByAccount.insert(a.id, {a.markingOwnerId, a.writeoffWarehouseId});
    QHash<int, QString> warehouseNames;
    {
        QSqlQuery q(QStringLiteral("SELECT id, name FROM warehouses"));
        while (q.next())
            warehouseNames.insert(q.value(0).toInt(), q.value(1).toString());
    }

    QStringList keys;
    for (const OrderInput &o : orders)
        for (const Item &it : o.items)
            keys << it.offerId;
    QStringList ambiguous;
    const QHash<QString, int> products = OrderWriteoff::productsForKeys(keys, &ambiguous);
    const QSet<QString> ambiguousSet(ambiguous.begin(), ambiguous.end());

    QHash<int, QPair<QString, QString>> info;   // товар -> артикул, название
    {
        QStringList ids;
        for (int id : products)
            ids << QString::number(id);
        if (!ids.isEmpty()) {
            QSqlQuery q(QStringLiteral("SELECT id, sku, name FROM products WHERE id IN (%1)").arg(ids.join(QLatin1Char(','))));
            while (q.next())
                info.insert(q.value(0).toInt(), {q.value(1).toString(), q.value(2).toString()});
        }
    }

    QStringList refs;
    for (const OrderInput &o : orders) {
        if (cancelled(o.status) || o.items.isEmpty()) {
            ++plan.skippedOrders;
            continue;
        }
        ++plan.ordersCount;
        const QString ref = orderRef(o.accountId, o.orderNumber);
        refs << ref;
        for (const Item &it : o.items) {
            Line l;
            l.ref = ref;
            l.accountName = o.accountName;
            l.orderNumber = o.orderNumber;
            l.offerId = it.offerId;
            l.name = it.name;
            l.need = it.count;
            const QPair<int, int> target = targetByAccount.value(o.accountId, {0, 0});
            l.ownerId = target.first;
            l.ownerName = MarkingOwner::nameOf(l.ownerId);
            l.warehouseId = target.second;
            l.warehouseName = warehouseNames.value(l.warehouseId);
            const QString key = it.offerId.trimmed().toLower();
            if (ambiguousSet.contains(key)) {
                l.problem = QStringLiteral("артикул подходит нескольким товарам");
            } else if (!products.contains(key)) {
                l.problem = QStringLiteral("товара нет в базе");
            } else {
                l.productId = products.value(key);
                l.sku = info.value(l.productId).first;
                if (l.name.isEmpty())
                    l.name = info.value(l.productId).second;
            }
            plan.lines << l;
        }
    }

    // Уже выданные под эти заказы коды (напечатаны раньше): считаются выполненной частью, новые для них не берутся.
    const QList<MarkingCode::SaleCode> issued = MarkingCode::codesForSales(refs);
    QHash<QString, QList<int>> issuedByRefProduct;
    for (const MarkingCode::SaleCode &c : issued)
        issuedByRefProduct[c.ref + QLatin1Char('|') + QString::number(c.productId)] << c.id;
    for (Line &l : plan.lines) {
        if (l.productId <= 0)
            continue;
        QList<int> &have = issuedByRefProduct[l.ref + QLatin1Char('|') + QString::number(l.productId)];
        const int take = qMin(l.need, int(have.size()));
        l.issuedIds = have.mid(0, take);
        l.issued = take;
        have = have.mid(take);   // у заказа две строки с одним товаром: выданные коды делятся между ними по очереди
    }

    // Свободные коды берутся по очереди заказов, самые старые. Для склада: сначала коды этого склада, потом коды без склада, чужого склада - никогда;
    // если у кабинета склада нет, подходит любой код.
    QList<QPair<int, int>> pairs;
    for (const Line &l : std::as_const(plan.lines))
        if (l.productId > 0 && l.need - l.issued > 0)
            pairs.append({l.productId, l.ownerId});
    const QHash<QString, QList<MarkingCode::FreeCode>> pool = MarkingCode::freeCodes(pairs);
    QSet<int> taken;
    QHash<QString, int> anyCursor;                         // товар|ИП -> позиция в общем списке
    QHash<QString, QHash<int, int>> whCursor;              // товар|ИП -> склад -> позиция в списке этого склада
    QHash<QString, QHash<int, QList<int>>> byWarehouse;    // товар|ИП -> склад -> номера кодов
    QHash<QString, QList<int>> allIds;                     // товар|ИП -> все номера (для кабинета без склада)
    for (auto it = pool.cbegin(); it != pool.cend(); ++it)
        for (const MarkingCode::FreeCode &c : it.value()) {
            byWarehouse[it.key()][c.warehouseId] << c.id;
            allIds[it.key()] << c.id;
        }
    auto takeFrom = [&](const QList<int> &list, int *cursor, int want, QList<int> *out) {
        while (want > 0 && *cursor < list.size()) {
            const int id = list.at((*cursor)++);
            if (taken.contains(id))
                continue;
            taken.insert(id);
            out->append(id);
            --want;
        }
        return want;
    };
    for (Line &l : plan.lines) {
        if (l.productId <= 0)
            continue;
        int want = l.need - l.issued;
        if (want <= 0)
            continue;
        const QString key = MarkingCode::pickKey(l.productId, l.ownerId);
        if (l.warehouseId > 0) {
            want = takeFrom(byWarehouse.value(key).value(l.warehouseId), &whCursor[key][l.warehouseId], want, &l.picked);
            want = takeFrom(byWarehouse.value(key).value(0), &whCursor[key][0], want, &l.picked);
        } else {
            takeFrom(allIds.value(key), &anyCursor[key], want, &l.picked);   // у кабинета нет склада: любой код товара по порядку
        }
    }
    return plan;
}

int markIssued(const Plan &plan)
{
    // Только то, что в самом деле напечатано (в окне печати могли отказаться или принтер не принял).
    const QList<int> pickedIds = plan.pickedIds();
    if (pickedIds.isEmpty())
        return 0;
    QSet<int> printed;
    for (const MarkingCode::Row &r : MarkingCode::getMany(pickedIds))
        if (r.status == QLatin1String("printed"))
            printed.insert(r.id);
    QHash<QString, QList<int>> byRef;
    for (const Line &l : plan.lines)
        for (int id : l.picked)
            if (printed.contains(id))
                byRef[l.ref] << id;
    return MarkingCode::assignSales(byRef);
}

} // namespace OrderMarking
