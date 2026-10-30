#include "orderwriteoff.h"

#include "appsettings.h"
#include "database.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

#include <algorithm>

namespace OrderWriteoff {

namespace {

const char kSiteWarehouseKey[] = "site_orders_warehouse_id";
constexpr int kChunk = 100;

const QStringList &writeOffStatuses()
{
    // Заказ принят и собирается или уже уехал: пара ушла со склада. Остальное (не оплачен, ждёт подтверждения) - не списываем.
    static const QStringList s = {QStringLiteral("PROCESSING"), QStringLiteral("DELIVERY"), QStringLiteral("PICKUP"), QStringLiteral("DELIVERED")};
    return s;
}

QString humanStatus(const QString &status)
{
    static const QHash<QString, QString> names = {
        {"PROCESSING", "в обработке"}, {"DELIVERY", "в доставке"},          {"PICKUP", "в пункте выдачи"}, {"DELIVERED", "доставлен"},
        {"CANCELLED", "отменён"},      {"UNPAID", "не оплачен"},           {"PENDING", "ожидает подтверждения"},
        {"RETURNED", "возвращён"},     {"PARTIALLY_RETURNED", "частично возвращён"}, {"PLACING", "оформляется"}, {"RESERVED", "зарезервирован"},
    };
    return names.value(status, status);
}

QString orderTitle(const Order &o)
{
    if (!o.title.isEmpty())
        return o.title;
    if (o.source == QLatin1String("site"))
        return QStringLiteral("заказ сайта №%1").arg(o.ref);
    return o.accountName.isEmpty() ? QStringLiteral("заказ Маркета №%1").arg(o.ref)
                                   : QStringLiteral("заказ Маркета №%1 (%2)").arg(o.ref, o.accountName);
}

struct Record {
    int id = 0;
    QString state;
    QJsonArray written;
    int warehouseId = 0;
    int docId = 0;
    int returnDocId = 0;
};

QString recordKey(const QString &source, int accountId, const QString &ref)
{
    return QStringLiteral("%1|%2|%3").arg(source).arg(accountId).arg(ref);
}

QHash<QString, Record> loadRecords(const QList<Order> &orders, QString *error)
{
    QHash<QString, Record> out;
    for (int start = 0; start < orders.size(); start += kChunk) {
        QStringList conds;
        QVariantList binds;
        for (int i = start; i < qMin(int(orders.size()), start + kChunk); ++i) {
            conds << QStringLiteral("(source = ? AND account_id = ? AND order_ref = ?)");
            binds << orders[i].source << orders[i].accountId << orders[i].ref;
        }
        QSqlQuery q;
        q.prepare(QStringLiteral("SELECT id, source, account_id, order_ref, state, written::text, COALESCE(warehouse_id, 0), COALESCE(doc_id, 0), "
                                 "COALESCE(return_doc_id, 0) FROM order_writeoffs WHERE %1")
                      .arg(conds.join(QStringLiteral(" OR "))));
        for (const QVariant &b : binds)
            q.addBindValue(b);
        if (!q.exec()) {
            if (error)
                *error = q.lastError().text();
            return out;
        }
        while (q.next()) {
            Record r;
            r.id = q.value(0).toInt();
            r.state = q.value(4).toString();
            r.written = QJsonDocument::fromJson(q.value(5).toString().toUtf8()).array();
            r.warehouseId = q.value(6).toInt();
            r.docId = q.value(7).toInt();
            r.returnDocId = q.value(8).toInt();
            out.insert(recordKey(q.value(1).toString(), q.value(2).toInt(), q.value(3).toString()), r);
        }
    }
    return out;
}

struct Candidate {
    int id = 0;
    QString sku;
    QString name;
};

QString normKey(const QString &s)
{
    return s.trimmed().toLower();
}

// Товары по ключам из заказов: сначала по артикулу на Маркете (а если его нет - по артикулу), затем просто по артикулу и,
// для заказов сайта, по штрихкоду. Ключ без совпадений в результат не попадает.
QHash<QString, QList<Candidate>> resolveKeys(const QStringList &keys)
{
    QHash<QString, QList<Candidate>> out;
    QStringList rest = keys;
    auto run = [&](const QString &sql) {
        // sql содержит %1 - список «?» по ключам rest
        QStringList todo = rest;
        for (int start = 0; start < todo.size(); start += 200) {
            const QStringList part = todo.mid(start, 200);
            QStringList marks(part.size(), QStringLiteral("?"));
            QSqlQuery q;
            q.prepare(sql.arg(marks.join(QLatin1Char(','))));
            for (const QString &k : part)
                q.addBindValue(k);
            if (!q.exec())
                continue;
            while (q.next())
                out[q.value(0).toString()].append({q.value(1).toInt(), q.value(2).toString(), q.value(3).toString()});
        }
        QStringList still;
        for (const QString &k : std::as_const(rest))
            if (!out.contains(k))
                still << k;
        rest = still;
    };
    run(QStringLiteral("SELECT lower(COALESCE(NULLIF(TRIM(p.market_sku), ''), p.sku)), p.id, p.sku, p.name FROM products p "
                       "WHERE lower(COALESCE(NULLIF(TRIM(p.market_sku), ''), p.sku)) IN (%1) ORDER BY p.id"));
    if (!rest.isEmpty())
        run(QStringLiteral("SELECT lower(TRIM(p.sku)), p.id, p.sku, p.name FROM products p WHERE lower(TRIM(p.sku)) IN (%1) ORDER BY p.id"));
    if (!rest.isEmpty())
        run(QStringLiteral("SELECT lower(b.barcode), p.id, p.sku, p.name FROM product_barcodes b JOIN products p ON p.id = b.product_id "
                           "WHERE lower(b.barcode) IN (%1) ORDER BY p.id"));
    return out;
}

QHash<int, int> loadStock(int warehouseId, const QSet<int> &productIds)
{
    QHash<int, int> out;
    const QList<int> ids = productIds.values();
    for (int start = 0; start < ids.size(); start += 200) {
        const QList<int> part = ids.mid(start, 200);
        QStringList marks(part.size(), QStringLiteral("?"));
        QSqlQuery q;
        q.prepare(QStringLiteral("SELECT product_id, quantity FROM stock WHERE warehouse_id = ? AND product_id IN (%1)").arg(marks.join(QLatin1Char(','))));
        q.addBindValue(warehouseId);
        for (int id : part)
            q.addBindValue(id);
        if (q.exec())
            while (q.next())
                out.insert(q.value(0).toInt(), q.value(1).toInt());
    }
    return out;
}

QHash<int, QString> loadWarehouseNames()
{
    QHash<int, QString> out;
    QSqlQuery q(QStringLiteral("SELECT id, name FROM warehouses"));
    while (q.next())
        out.insert(q.value(0).toInt(), q.value(1).toString());
    return out;
}

QHash<int, int> loadAccountWarehouses()
{
    QHash<int, int> out;
    QSqlQuery q(QStringLiteral("SELECT id, COALESCE(writeoff_warehouse_id, 0) FROM market_accounts"));
    while (q.next())
        out.insert(q.value(0).toInt(), q.value(1).toInt());
    return out;
}

QString jsonText(const QJsonArray &a)
{
    return QString::fromUtf8(QJsonDocument(a).toJson(QJsonDocument::Compact));
}

} // namespace

int siteWarehouse()
{
    return AppSettings::get(QString::fromLatin1(kSiteWarehouseKey)).toInt();
}

bool setSiteWarehouse(int warehouseId)
{
    return AppSettings::set(QString::fromLatin1(kSiteWarehouseKey), warehouseId > 0 ? QString::number(warehouseId) : QString());
}

QList<Order> siteQueue(QString *error)
{
    QList<Order> out;
    QSqlQuery q(QStringLiteral("SELECT order_ref, state, lines::text, to_char(created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD') FROM order_writeoffs "
                               "WHERE source = 'site' AND state IN ('pending', 'cancel_requested') ORDER BY id"));
    while (q.next()) {
        Order o;
        o.source = QStringLiteral("site");
        o.ref = q.value(0).toString();
        o.status = q.value(1).toString();
        o.shipment = q.value(3).toString();
        for (const QJsonValue &v : QJsonDocument::fromJson(q.value(2).toString().toUtf8()).array()) {
            const QJsonObject l = v.toObject();
            o.lines.append({l.value(QStringLiteral("key")).toString(), l.value(QStringLiteral("name")).toString(), l.value(QStringLiteral("qty")).toInt()});
        }
        out.append(o);
    }
    if (q.lastError().isValid() && error)
        *error = q.lastError().text();
    return out;
}

QList<Planned> plan(const QList<Order> &orders, QString *error)
{
    QList<Planned> out;
    const QHash<QString, Record> records = loadRecords(orders, error);
    const QHash<int, QString> whNames = loadWarehouseNames();
    const QHash<int, int> accountWh = loadAccountWarehouses();
    const int siteWh = siteWarehouse();

    // 1) Что делать с каждым заказом по его состоянию; для списания собираем ключи товаров.
    QStringList keys;
    for (const Order &o : orders) {
        Planned p;
        p.order = o;
        const auto rec = records.constFind(recordKey(o.source, o.accountId, o.ref));
        const bool isSite = o.source == QLatin1String("site");
        const bool cancelled = isSite ? o.status == QLatin1String("cancel_requested") : o.status == QLatin1String("CANCELLED");
        if (rec != records.cend()) {
            p.recordId = rec->id;
            p.docId = rec->docId;
            const QString st = rec->state;
            if (st == QLatin1String("written_off") || st == QLatin1String("cancel_requested")) {
                if (cancelled || st == QLatin1String("cancel_requested")) {
                    p.action = Action::Return;
                    p.warehouseId = rec->warehouseId;
                    for (const QJsonValue &v : rec->written) {
                        const QJsonObject w = v.toObject();
                        p.items.append({w.value(QStringLiteral("product_id")).toInt(), w.value(QStringLiteral("sku")).toString(), w.value(QStringLiteral("name")).toString(),
                                        w.value(QStringLiteral("qty")).toInt(), 0});
                    }
                    p.reason = QStringLiteral("заказ отменён - пара вернётся на склад приходом");
                } else {
                    p.action = Action::Done;
                    p.reason = QStringLiteral("уже списан (документ №%1)").arg(rec->docId);
                }
            } else if (st == QLatin1String("returned")) {
                p.action = Action::Done;
                p.reason = QStringLiteral("уже возвращён на склад (документ №%1)").arg(rec->returnDocId);
            } else if (st == QLatin1String("cancelled")) {
                p.action = Action::Skip;
                p.reason = QStringLiteral("отменён до списания - ничего делать не нужно");
            } else if (st == QLatin1String("writing") || st == QLatin1String("returning")) {
                p.action = Action::Problem;
                p.reason = QStringLiteral("операция по этому заказу уже идёт (или оборвалась) - проверьте раздел «Документы»");
            } else if (st == QLatin1String("pending")) {
                p.action = Action::WriteOff;
            }
        } else if (isSite) {
            p.action = Action::Skip;
            p.reason = QStringLiteral("заказа нет в очереди");
        } else if (o.status == QLatin1String("CANCELLED")) {
            p.action = Action::Skip;
            p.reason = QStringLiteral("отменён, а у нас не списывался");
        } else if (writeOffStatuses().contains(o.status)) {
            p.action = Action::WriteOff;
        } else {
            p.action = Action::Skip;
            p.reason = QStringLiteral("статус «%1» - заказ ещё не принят в работу, не списываем").arg(humanStatus(o.status));
        }
        if (p.action == Action::WriteOff)
            for (const Line &l : o.lines)
                if (l.qty > 0 && !l.key.trimmed().isEmpty())
                    keys << normKey(l.key);
        out.append(p);
    }
    keys.removeDuplicates();
    const QHash<QString, QList<Candidate>> found = resolveKeys(keys);

    // 2) Склады и остатки для списания.
    QHash<int, QHash<int, int>> stockByWh; // склад -> товар -> остаток (убывает по мере планирования)
    QHash<int, QSet<int>> idsByWh;
    auto warehouseOf = [&](const Planned &p) {
        if (p.order.warehouseId > 0)
            return p.order.warehouseId;
        return p.order.source == QLatin1String("site") ? siteWh : accountWh.value(p.order.accountId);
    };
    for (Planned &p : out) {
        if (p.action != Action::WriteOff)
            continue;
        const int wh = warehouseOf(p);
        if (wh <= 0) {
            p.action = Action::Problem;
            p.reason = p.order.source == QLatin1String("site")
                           ? QStringLiteral("не выбран склад для заказов сайта (кнопка «Заказы сайта...»)")
                           : QStringLiteral("не выбран склад списания у кабинета «%1» (меню «Маркет» → «Кабинеты»)").arg(p.order.accountName);
            continue;
        }
        p.warehouseId = wh;
        for (const Line &l : p.order.lines)
            for (const Candidate &c : found.value(normKey(l.key)))
                idsByWh[wh].insert(c.id);
    }
    for (auto it = idsByWh.cbegin(); it != idsByWh.cend(); ++it)
        stockByWh[it.key()] = loadStock(it.key(), it.value());

    // 3) Позиции заказов по порядку; остаток уменьшается нарастающим итогом.
    for (Planned &p : out) {
        p.warehouseName = whNames.value(p.warehouseId);
        if (p.action != Action::WriteOff)
            continue;
        QHash<int, int> &avail = stockByWh[p.warehouseId];
        QHash<int, Item> items; // товар -> позиция (повторы одного товара складываются)
        QStringList missing;
        for (const Line &l : p.order.lines) {
            if (l.qty <= 0 || l.key.trimmed().isEmpty())
                continue;
            const QList<Candidate> cands = found.value(normKey(l.key));
            if (cands.isEmpty()) {
                missing << l.key;
                continue;
            }
            // Несколько товаров на один артикул: берём тот, на котором хватает остатка (иначе - где остатка больше).
            const Candidate *chosen = &cands.first();
            int bestStock = -1;
            for (const Candidate &c : cands) {
                const int have = avail.value(c.id) - items.value(c.id).qty;
                if (have >= l.qty) {
                    chosen = &c;
                    bestStock = have;
                    break;
                }
                if (have > bestStock) {
                    chosen = &c;
                    bestStock = have;
                }
            }
            Item &it = items[chosen->id];
            it.productId = chosen->id;
            it.sku = chosen->sku;
            it.name = chosen->name;
            it.qty += l.qty;
            it.stock = avail.value(chosen->id);
        }
        p.items = items.values();
        std::sort(p.items.begin(), p.items.end(), [](const Item &a, const Item &b) { return a.sku < b.sku; });
        if (!missing.isEmpty()) {
            p.action = Action::Problem;
            p.reason = QStringLiteral("нет такого товара в базе: %1").arg(missing.join(QStringLiteral(", ")));
            continue;
        }
        if (p.items.isEmpty()) {
            p.action = Action::Skip;
            p.reason = QStringLiteral("в заказе нет позиций");
            continue;
        }
        QStringList lack;
        for (const Item &it : std::as_const(p.items))
            if (it.stock < it.qty)
                lack << QStringLiteral("%1: на складе «%2» %3 шт, нужно %4").arg(it.sku, p.warehouseName).arg(it.stock).arg(it.qty);
        if (!lack.isEmpty()) {
            p.action = Action::Problem;
            p.reason = QStringLiteral("не хватает остатка - %1").arg(lack.join(QStringLiteral("; ")));
            continue;
        }
        for (const Item &it : std::as_const(p.items))
            avail[it.productId] -= it.qty;
        p.reason = QStringLiteral("будет списано со склада «%1»").arg(p.warehouseName);
    }
    for (Planned &p : out)
        if (p.action == Action::Return)
            p.reason = QStringLiteral("заказ отменён - пара вернётся на склад «%1» приходом").arg(p.warehouseName.isEmpty() ? whNames.value(p.warehouseId) : p.warehouseName);
    return out;
}

Outcome apply(const QList<Planned> &selected, const std::function<bool(int, int)> &progress)
{
    Outcome res;
    int done = 0;
    for (const Planned &p : selected) {
        if (progress && !progress(done, selected.size())) {
            res.cancelled = true;
            break;
        }
        ++done;
        if (!p.actionable())
            continue;
        const QString title = orderTitle(p.order);
        QList<Database::BatchItem> items;
        QJsonArray written;
        for (const Item &it : p.items) {
            items.append({it.productId, it.qty});
            written.append(QJsonObject{{QStringLiteral("product_id"), it.productId}, {QStringLiteral("sku"), it.sku}, {QStringLiteral("name"), it.name}, {QStringLiteral("qty"), it.qty}});
        }
        int units = 0;
        for (const Item &it : p.items)
            units += it.qty;

        if (p.action == Action::WriteOff) {
            // Занимаем заказ: вторая такая попытка (другой компьютер, повторное нажатие) ничего не получит.
            int recordId = 0;
            QSqlQuery claim;
            if (p.order.source == QLatin1String("site")) {
                claim.prepare(QStringLiteral("UPDATE order_writeoffs SET state = 'writing', warehouse_id = ? WHERE id = ? AND state = 'pending' RETURNING id"));
                claim.addBindValue(p.warehouseId);
                claim.addBindValue(p.recordId);
            } else {
                QJsonArray lines;
                for (const Line &l : p.order.lines)
                    lines.append(QJsonObject{{QStringLiteral("key"), l.key}, {QStringLiteral("name"), l.name}, {QStringLiteral("qty"), l.qty}});
                claim.prepare(QStringLiteral("INSERT INTO order_writeoffs (source, account_id, order_ref, state, lines, warehouse_id) VALUES (?, ?, ?, 'writing', ?, ?) "
                                             "ON CONFLICT (source, account_id, order_ref) DO NOTHING RETURNING id"));
                claim.addBindValue(p.order.source);
                claim.addBindValue(p.order.accountId);
                claim.addBindValue(p.order.ref);
                claim.addBindValue(jsonText(lines));
                claim.addBindValue(p.warehouseId);
            }
            if (!claim.exec()) {
                ++res.failed;
                res.problems << QStringLiteral("%1: %2").arg(title, claim.lastError().text());
                continue;
            }
            if (claim.next())
                recordId = claim.value(0).toInt();
            if (recordId == 0) {
                ++res.skipped;
                res.problems << QStringLiteral("%1: уже обработан (возможно, с другого компьютера)").arg(title);
                continue;
            }
            QStringList problems;
            QString err;
            const QString commentTitle = title.left(1).toUpper() + title.mid(1); // «Заказ Маркета №100 (Магазин 1)»
            if (!Database::adjustStockBatch(items, p.warehouseId, true, commentTitle, &problems, &err)) {
                // Не списалось: заказ снова свободен.
                QSqlQuery undo;
                if (p.order.source == QLatin1String("site"))
                    undo.prepare(QStringLiteral("UPDATE order_writeoffs SET state = 'pending', warehouse_id = NULL WHERE id = ?"));
                else
                    undo.prepare(QStringLiteral("DELETE FROM order_writeoffs WHERE id = ?"));
                undo.addBindValue(recordId);
                undo.exec();
                ++res.failed;
                res.problems << QStringLiteral("%1: %2").arg(title, problems.isEmpty() ? err : problems.join(QStringLiteral("; ")));
                continue;
            }
            const int docId = Database::lastDocumentId();
            QSqlQuery fin;
            fin.prepare(QStringLiteral("UPDATE order_writeoffs SET state = 'written_off', written = ?, doc_id = ?, written_at = NOW() WHERE id = ?"));
            fin.addBindValue(jsonText(written));
            fin.addBindValue(docId > 0 ? QVariant(docId) : QVariant(QMetaType(QMetaType::Int)));
            fin.addBindValue(recordId);
            if (!fin.exec())
                res.problems << QStringLiteral("%1: списано (документ №%2), но запись о заказе не сохранилась: %3 - не списывайте заказ повторно").arg(title).arg(docId).arg(fin.lastError().text());
            ++res.written;
            res.units += units;
            if (docId > 0)
                res.documents << docId;
        } else { // Return
            QSqlQuery claim;
            claim.prepare(QStringLiteral("UPDATE order_writeoffs SET state = 'returning' WHERE id = ? AND state IN ('written_off', 'cancel_requested') "
                                         "RETURNING COALESCE(warehouse_id, 0)"));
            claim.addBindValue(p.recordId);
            // Прежнее состояние (чтобы вернуть заказ в него, если возврат не удался): у отмены от сайта - cancel_requested.
            const QString previous = p.order.source == QLatin1String("site") ? QStringLiteral("cancel_requested") : QStringLiteral("written_off");
            if (!claim.exec() || !claim.next()) {
                ++res.skipped;
                res.problems << QStringLiteral("%1: уже возвращён или изменён (возможно, с другого компьютера)").arg(title);
                continue;
            }
            const int wh = claim.value(0).toInt() > 0 ? claim.value(0).toInt() : p.warehouseId;
            QStringList problems;
            QString err;
            if (!Database::adjustStockBatch(items, wh, false, QStringLiteral("Возврат: %1 отменён").arg(title), &problems, &err)) {
                QSqlQuery undo;
                undo.prepare(QStringLiteral("UPDATE order_writeoffs SET state = ? WHERE id = ?"));
                undo.addBindValue(previous);
                undo.addBindValue(p.recordId);
                undo.exec();
                ++res.failed;
                res.problems << QStringLiteral("%1: %2").arg(title, problems.isEmpty() ? err : problems.join(QStringLiteral("; ")));
                continue;
            }
            const int docId = Database::lastDocumentId();
            QSqlQuery fin;
            fin.prepare(QStringLiteral("UPDATE order_writeoffs SET state = 'returned', return_doc_id = ?, returned_at = NOW() WHERE id = ?"));
            fin.addBindValue(docId > 0 ? QVariant(docId) : QVariant(QMetaType(QMetaType::Int)));
            fin.addBindValue(p.recordId);
            if (!fin.exec())
                res.problems << QStringLiteral("%1: возвращено (документ №%2), но запись о заказе не сохранилась: %3").arg(title).arg(docId).arg(fin.lastError().text());
            ++res.returned;
            res.unitsReturned += units;
            if (docId > 0)
                res.documents << docId;
        }
    }
    if (progress && !res.cancelled)
        progress(selected.size(), selected.size());
    return res;
}

namespace {

struct Existing {
    bool found = false;
    int id = 0;
    QString state;
    int docId = 0;
    int returnDocId = 0;
    int warehouseId = 0;
    QJsonArray written;
};

Existing lookup(const QString &source, const QString &ref)
{
    Existing e;
    QSqlQuery q;
    q.prepare(QStringLiteral("SELECT id, state, COALESCE(doc_id, 0), COALESCE(return_doc_id, 0), COALESCE(warehouse_id, 0), written::text FROM order_writeoffs "
                             "WHERE source = ? AND account_id = 0 AND order_ref = ?"));
    q.addBindValue(source);
    q.addBindValue(ref);
    if (q.exec() && q.next()) {
        e.found = true;
        e.id = q.value(0).toInt();
        e.state = q.value(1).toString();
        e.docId = q.value(2).toInt();
        e.returnDocId = q.value(3).toInt();
        e.warehouseId = q.value(4).toInt();
        e.written = QJsonDocument::fromJson(q.value(5).toString().toUtf8()).array();
    }
    return e;
}

Result fromOutcome(const Outcome &o, const QString &source, const QString &ref, bool returning)
{
    Result r;
    if ((returning ? o.returned : o.written) == 1 && !o.documents.isEmpty()) {
        r.status = Result::Written;
        r.docId = o.documents.first();
        return r;
    }
    if (o.skipped > 0) {
        // Другой компьютер успел раньше: отвечаем так же, как при повторном вызове.
        const Existing e = lookup(source, ref);
        r.status = Result::AlreadyDone;
        r.docId = returning ? e.returnDocId : e.docId;
        return r;
    }
    r.status = Result::Failed;
    r.message = o.problems.join(QStringLiteral("; "));
    if (r.message.contains(QStringLiteral("на складе")))
        r.status = Result::NotEnoughStock;
    return r;
}

} // namespace

Result writeOffItems(const QString &source, const QString &ref, const QString &title, int warehouseId, const QList<Item> &items)
{
    Result r;
    if (source.trimmed().isEmpty() || ref.trimmed().isEmpty()) {
        r.message = QStringLiteral("Не указан источник или номер заказа");
        return r;
    }
    if (warehouseId <= 0) {
        r.message = QStringLiteral("Не указан склад списания");
        return r;
    }
    if (items.isEmpty()) {
        r.message = QStringLiteral("В заказе нет позиций");
        return r;
    }
    for (const Item &it : items)
        if (it.productId <= 0 || it.qty <= 0) {
            r.message = QStringLiteral("Позиция без товара или с количеством не больше нуля");
            return r;
        }
    const Existing e = lookup(source, ref);
    if (e.found) {
        if (e.state == QLatin1String("written_off") || e.state == QLatin1String("cancel_requested") || e.state == QLatin1String("returned")) {
            r.status = Result::AlreadyDone;
            r.docId = e.docId;
            return r;
        }
        if (e.state == QLatin1String("writing") || e.state == QLatin1String("returning")) {
            r.message = QStringLiteral("Операция по этому заказу уже идёт (или оборвалась) - проверьте раздел «Документы»");
            return r;
        }
        r.message = QStringLiteral("Заказ уже есть в очереди списаний в состоянии «%1»").arg(e.state);
        return r;
    }
    Planned p;
    p.order.source = source;
    p.order.ref = ref;
    p.order.title = title.trimmed().isEmpty() ? QStringLiteral("заказ %1 №%2").arg(source, ref) : title.trimmed();
    p.order.status = QStringLiteral("PROCESSING");
    p.action = Action::WriteOff;
    p.warehouseId = warehouseId;
    p.items = items;
    for (const Item &it : items)
        p.order.lines.append({it.sku.isEmpty() ? QString::number(it.productId) : it.sku, it.name, it.qty});
    return fromOutcome(apply({p}), source, ref, false);
}

Result returnOrder(const QString &source, const QString &ref)
{
    Result r;
    const Existing e = lookup(source, ref);
    if (!e.found || e.state == QLatin1String("pending") || e.state == QLatin1String("cancelled")) {
        r.message = QStringLiteral("Этот заказ не списывался - возвращать нечего");
        return r;
    }
    if (e.state == QLatin1String("returned")) {
        r.status = Result::AlreadyDone;
        r.docId = e.returnDocId;
        return r;
    }
    if (e.state != QLatin1String("written_off") && e.state != QLatin1String("cancel_requested")) {
        r.message = QStringLiteral("Операция по этому заказу уже идёт (или оборвалась) - проверьте раздел «Документы»");
        return r;
    }
    Planned p;
    p.order.source = source;
    p.order.ref = ref;
    p.order.status = QStringLiteral("CANCELLED");
    p.action = Action::Return;
    p.recordId = e.id;
    p.warehouseId = e.warehouseId;
    for (const QJsonValue &v : e.written) {
        const QJsonObject w = v.toObject();
        p.items.append({w.value(QStringLiteral("product_id")).toInt(), w.value(QStringLiteral("sku")).toString(), w.value(QStringLiteral("name")).toString(),
                        w.value(QStringLiteral("qty")).toInt(), 0});
    }
    p.order.title = QStringLiteral("заказ %1 №%2").arg(source, ref);
    return fromOutcome(apply({p}), source, ref, true);
}

} // namespace OrderWriteoff
