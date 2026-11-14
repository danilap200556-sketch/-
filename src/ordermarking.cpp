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

QList<int> Plan::ownersWithMissing() const
{
    QList<int> out;
    for (const Line &l : lines)
        if (l.missing() > 0 && !out.contains(l.ownerId))
            out << l.ownerId;
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
    // ИП кабинета: ИП склада списания.
    QHash<int, int> ownerByAccount;
    for (const MarketAccount &a : MarketAccount::loadAll())
        ownerByAccount.insert(a.id, a.writeoffWarehouseId > 0 ? MarkingOwner::warehouseOwner(a.writeoffWarehouseId) : 0);

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
            l.ownerId = ownerByAccount.value(o.accountId, 0);
            l.ownerName = MarkingOwner::nameOf(l.ownerId);
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

    // Свободные коды: сколько нужно по каждой паре «товар - ИП», берутся самые старые.
    QList<MarkingCode::PickNeed> needs;
    for (const Line &l : std::as_const(plan.lines))
        if (l.productId > 0 && l.need - l.issued > 0)
            needs.append({l.productId, l.ownerId, l.need - l.issued});
    QHash<QString, QList<int>> pool = MarkingCode::pickFree(needs);
    for (Line &l : plan.lines) {
        if (l.productId <= 0)
            continue;
        QList<int> &p = pool[MarkingCode::pickKey(l.productId, l.ownerId)];
        const int take = qMin(l.need - l.issued, int(p.size()));
        if (take > 0) {
            l.picked = p.mid(0, take);
            p = p.mid(take);
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
