#include "documents.h"

#include "database.h"

#include <QHash>
#include <QMap>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace Documents {

QString kindTitle(const QString &kind)
{
    if (kind == QLatin1String("receipt")) return QStringLiteral("Приход");
    if (kind == QLatin1String("writeoff")) return QStringLiteral("Списание");
    if (kind == QLatin1String("transfer")) return QStringLiteral("Перемещение");
    if (kind == QLatin1String("inventory")) return QStringLiteral("Инвентаризация");
    if (kind == QLatin1String("cancel")) return QStringLiteral("Отмена");
    return QStringLiteral("Операция");
}

QString title(const Info &d)
{
    return QStringLiteral("%1 №%2").arg(kindTitle(d.kind)).arg(d.id);
}

bool isCancelled(const Info &d)
{
    return d.kind != QLatin1String("cancel") && d.lines > 0 && d.reverted >= d.lines;
}

QString statusText(const Info &d)
{
    if (d.kind == QLatin1String("cancel"))
        return d.reverts.isEmpty() ? QStringLiteral("отмена") : QStringLiteral("отмена документа №%1").arg(d.reverts);
    if (d.lines == 0)
        return QStringLiteral("пустой");
    if (d.reverted >= d.lines)
        return QStringLiteral("отменён");
    if (d.reverted > 0)
        return QStringLiteral("отменён частично (%1 из %2)").arg(d.reverted).arg(d.lines);
    return QStringLiteral("действует");
}

namespace {

QString warehousesText(const QString &kind, int count, const QString &first, const QString &from, const QString &to)
{
    if (kind == QLatin1String("transfer") && !from.isEmpty() && !to.isEmpty())
        return QStringLiteral("%1 → %2").arg(from, to);
    if (count <= 1)
        return first;
    return QStringLiteral("несколько складов (%1)").arg(count);
}

QString likeOf(const QString &term)
{
    QString t = term;
    t.replace(QLatin1Char('\\'), QLatin1String("\\\\")).replace(QLatin1Char('%'), QLatin1String("\\%")).replace(QLatin1Char('_'), QLatin1String("\\_"));
    return QLatin1Char('%') + t + QLatin1Char('%');
}

const char kInfoSelect[] =
    "SELECT d.id, d.doc_key, d.kind, d.created_at, d.username, d.comment, "
    "       COUNT(m.id), COUNT(DISTINCT m.product_id), COALESCE(SUM(ABS(m.delta)), 0), COUNT(rv.id), "
    "       COUNT(DISTINCT m.warehouse_id), COALESCE(MIN(w.name), ''), "
    "       COALESCE(MAX(CASE WHEN m.type = 'transfer_out' THEN w.name END), ''), "
    "       COALESCE(MAX(CASE WHEN m.type = 'transfer_in' THEN w.name END), ''), "
    "       CASE WHEN d.kind = 'cancel' THEN COALESCE((SELECT string_agg(DISTINCT od.id::text, ', ') "
    "            FROM stock_movements c JOIN stock_movements o ON o.id = c.reverts_id "
    "            JOIN stock_documents od ON od.doc_key = o.doc_id WHERE c.doc_id = d.doc_key), '') ELSE '' END, "
    "       d.edited_at, COALESCE(d.edited_by, '') ";

const char kInfoJoins[] =
    "LEFT JOIN stock_movements m ON m.doc_id = d.doc_key "
    "LEFT JOIN warehouses w ON w.id = m.warehouse_id "
    "LEFT JOIN stock_movements rv ON rv.reverts_id = m.id ";

Info readInfo(const QSqlQuery &q)
{
    Info d;
    d.id = q.value(0).toInt();
    d.key = q.value(1).toString();
    d.kind = q.value(2).toString();
    d.createdAt = q.value(3).toDateTime();
    d.username = q.value(4).toString();
    d.comment = q.value(5).toString();
    d.lines = q.value(6).toInt();
    d.positions = q.value(7).toInt();
    const qint64 abs = q.value(8).toLongLong();
    d.units = d.kind == QLatin1String("transfer") ? abs / 2 : abs; // перемещение - расход и приход одних и тех же штук
    d.reverted = q.value(9).toInt();
    d.warehouses = warehousesText(d.kind, q.value(10).toInt(), q.value(11).toString(), q.value(12).toString(), q.value(13).toString());
    d.reverts = q.value(14).toString();
    d.editedAt = q.value(15).toDateTime();
    d.editedBy = q.value(16).toString();
    return d;
}

} // namespace

QList<Info> list(const QString &kind, const QString &search, int limit, QString *error)
{
    QList<Info> result;
    QString where = QStringLiteral("TRUE");
    QVariantList binds;
    if (!kind.isEmpty()) {
        where += QStringLiteral(" AND kind = ?");
        binds << kind;
    }
    const QString term = search.trimmed();
    if (!term.isEmpty()) {
        const QString like = likeOf(term);
        where += QStringLiteral(" AND (id::text = ? OR comment ILIKE ? OR username ILIKE ? OR EXISTS ("
                                "SELECT 1 FROM stock_movements mm JOIN products pp ON pp.id = mm.product_id "
                                "WHERE mm.doc_id = stock_documents.doc_key AND (pp.sku ILIKE ? OR pp.name ILIKE ?)))");
        binds << term << like << like << like << like;
    }
    // Сначала отбираются нужные документы (с ограничением), и только к ним подсчитываются записи.
    QSqlQuery q;
    q.prepare(QString::fromLatin1(kInfoSelect) +
              QStringLiteral("FROM (SELECT * FROM stock_documents WHERE %1 ORDER BY id DESC LIMIT %2) d ").arg(where).arg(limit) +
              QString::fromLatin1(kInfoJoins) + QStringLiteral("GROUP BY d.id, d.doc_key, d.kind, d.created_at, d.username, d.comment, d.edited_at, d.edited_by ORDER BY d.id DESC"));
    for (const QVariant &b : binds)
        q.addBindValue(b);
    if (!q.exec()) {
        if (error)
            *error = q.lastError().text();
        return result;
    }
    while (q.next())
        result << readInfo(q);
    return result;
}

Info get(int id)
{
    QSqlQuery q;
    q.prepare(QString::fromLatin1(kInfoSelect) + QStringLiteral("FROM (SELECT * FROM stock_documents WHERE id = ?) d ") +
              QString::fromLatin1(kInfoJoins) + QStringLiteral("GROUP BY d.id, d.doc_key, d.kind, d.created_at, d.username, d.comment, d.edited_at, d.edited_by"));
    q.addBindValue(id);
    if (q.exec() && q.next())
        return readInfo(q);
    return Info();
}

QList<Line> lines(const QString &docKey)
{
    QList<Line> result;
    QSqlQuery q;
    q.prepare("SELECT m.id, m.product_id, p.sku, p.name, w.name, m.type, m.delta, COALESCE(m.comment, ''), "
              "       COALESCE((SELECT r.id FROM stock_movements r WHERE r.reverts_id = m.id), 0) "
              "FROM stock_movements m JOIN products p ON p.id = m.product_id JOIN warehouses w ON w.id = m.warehouse_id "
              "WHERE m.doc_id = ? ORDER BY m.id");
    q.addBindValue(docKey);
    if (!q.exec())
        return result;
    while (q.next()) {
        Line l;
        l.id = q.value(0).toInt();
        l.productId = q.value(1).toInt();
        l.sku = q.value(2).toString();
        l.name = q.value(3).toString();
        l.warehouse = q.value(4).toString();
        l.type = q.value(5).toString();
        l.delta = q.value(6).toInt();
        l.comment = q.value(7).toString();
        l.revertedBy = q.value(8).toInt();
        result << l;
    }
    return result;
}

QList<int> cancellableIds(const QString &docKey)
{
    QList<int> ids;
    QSqlQuery q;
    q.prepare("SELECT m.id FROM stock_movements m WHERE m.doc_id = ? AND m.type <> 'cancel' "
              "AND NOT EXISTS (SELECT 1 FROM stock_movements r WHERE r.reverts_id = m.id) ORDER BY m.id");
    q.addBindValue(docKey);
    if (q.exec())
        while (q.next())
            ids << q.value(0).toInt();
    return ids;
}

int lastCancellable()
{
    QSqlQuery q("SELECT d.id FROM stock_documents d WHERE d.kind <> 'cancel' AND EXISTS ("
                "SELECT 1 FROM stock_movements m WHERE m.doc_id = d.doc_key AND m.type <> 'cancel' "
                "AND NOT EXISTS (SELECT 1 FROM stock_movements r WHERE r.reverts_id = m.id)) ORDER BY d.id DESC LIMIT 1");
    return q.next() ? q.value(0).toInt() : 0;
}

Xlsx::Sheet toSheet(const Info &d, const QList<Line> &ls)
{
    Xlsx::Sheet sheet;
    sheet.name = QStringLiteral("Документ %1").arg(d.id);
    sheet.rows.append({QStringLiteral("Документ"), title(d), QString(), QString(), QString(), QString()});
    sheet.rows.append({QStringLiteral("Дата"), d.createdAt.toLocalTime().toString(QStringLiteral("dd.MM.yyyy HH:mm")), QString(), QString(), QString(), QString()});
    sheet.rows.append({QStringLiteral("Автор"), d.username, QString(), QString(), QString(), QString()});
    sheet.rows.append({QStringLiteral("Комментарий"), d.comment, QString(), QString(), QString(), QString()});
    sheet.rows.append({QStringLiteral("Состояние"), statusText(d), QString(), QString(), QString(), QString()});
    sheet.rows.append({QString(), QString(), QString(), QString(), QString(), QString()});
    sheet.rows.append({QStringLiteral("Артикул"), QStringLiteral("Название"), QStringLiteral("Склад"), QStringLiteral("Изменение, шт"),
                       QStringLiteral("Запись журнала"), QStringLiteral("Отмена")});
    for (const Line &l : ls)
        sheet.rows.append({l.sku, l.name, l.warehouse, QString::number(l.delta), QString::number(l.id),
                           l.revertedBy ? QStringLiteral("отменена записью №%1").arg(l.revertedBy) : QString()});
    sheet.numericColumns = {3};
    sheet.columnWidths = {18, 40, 20, 14, 14, 26};
    return sheet;
}

// ---------------------------------------------------------------- правка и удаление

namespace {

// Пустая строка, а не NULL: у документа комментарий и автор NOT NULL.
QString nn(const QString &s) { return s.isNull() ? QString(QLatin1String("")) : s; }

constexpr int kMaxQty = 100'000'000;
constexpr qint64 kMaxStock = 2'000'000'000LL;

struct Move {
    int id = 0;
    int productId = 0;
    int warehouseId = 0;
    int related = 0;
    QString type;
    int delta = 0;
};

struct Key {
    int pid = 0;
    int wid = 0;
    bool operator<(const Key &o) const { return pid != o.pid ? pid < o.pid : wid < o.wid; }
};

QList<Move> loadMoves(const QString &docKey, bool lock)
{
    QList<Move> out;
    QSqlQuery q;
    q.prepare(QStringLiteral("SELECT id, product_id, warehouse_id, COALESCE(related_warehouse_id, 0), type, delta FROM stock_movements "
                             "WHERE doc_id = ? ORDER BY id%1").arg(lock ? QStringLiteral(" FOR UPDATE") : QString()));
    q.addBindValue(docKey);
    if (q.exec())
        while (q.next())
            out.append({q.value(0).toInt(), q.value(1).toInt(), q.value(2).toInt(), q.value(3).toInt(), q.value(4).toString(), q.value(5).toInt()});
    return out;
}

// Заказ (Маркета или сайта), который списал или вернул пару этим документом: такой документ меняется только через заказ.
QString linkedOrder(int docId)
{
    QSqlQuery q;
    q.prepare(QStringLiteral("SELECT source, order_ref FROM order_writeoffs WHERE doc_id = ? OR return_doc_id = ? ORDER BY id LIMIT 1"));
    q.addBindValue(docId);
    q.addBindValue(docId);
    if (q.exec() && q.next())
        return QStringLiteral("%1 №%2").arg(q.value(0).toString() == QLatin1String("site") ? QStringLiteral("заказ сайта") : QStringLiteral("заказ Маркета"),
                                            q.value(1).toString());
    return QString();
}

// Что делает документ сейчас, в виде черновика для правки. false - структура не подходит (несколько складов, чужие виды записей).
bool draftFromMoves(const QString &kind, const QList<Move> &moves, Draft *out, QString *why)
{
    auto fail = [&](const QString &text) {
        if (why)
            *why = text;
        return false;
    };
    Draft d;
    QMap<int, int> qty;
    if (kind == QLatin1String("receipt") || kind == QLatin1String("writeoff")) {
        const QString type = kind;
        for (const Move &m : moves) {
            if (m.type != type || (type == QLatin1String("receipt") ? m.delta <= 0 : m.delta >= 0))
                return fail(QStringLiteral("В документе есть записи другого вида: править его нельзя, можно отменить или удалить"));
            if (d.warehouseId == 0)
                d.warehouseId = m.warehouseId;
            else if (d.warehouseId != m.warehouseId)
                return fail(QStringLiteral("В документе несколько складов: править его нельзя, можно отменить или удалить"));
            qty[m.productId] += qAbs(m.delta);
        }
    } else if (kind == QLatin1String("transfer")) {
        QMap<int, int> in;
        for (const Move &m : moves) {
            if (m.type == QLatin1String("transfer_out") && m.delta < 0) {
                if ((d.warehouseId && d.warehouseId != m.warehouseId) || (d.warehouseToId && d.warehouseToId != m.related))
                    return fail(QStringLiteral("В перемещении несколько пар складов: править его нельзя, можно отменить или удалить"));
                d.warehouseId = m.warehouseId;
                d.warehouseToId = m.related;
                qty[m.productId] += -m.delta;
            } else if (m.type == QLatin1String("transfer_in") && m.delta > 0) {
                in[m.productId] += m.delta;
            } else {
                return fail(QStringLiteral("В документе есть записи другого вида: править его нельзя, можно отменить или удалить"));
            }
        }
        if (qty != in)
            return fail(QStringLiteral("Расход и приход перемещения не совпадают: править его нельзя, можно отменить или удалить"));
    } else {
        return fail(QStringLiteral("Править можно приход, списание и перемещение; этот документ можно только отменить или удалить"));
    }
    if (qty.isEmpty())
        return fail(QStringLiteral("В документе нет записей"));
    for (auto it = qty.cbegin(); it != qty.cend(); ++it)
        d.items.append({it.key(), it.value()});
    if (out)
        *out = d;
    return true;
}

QHash<int, QString> skuOf(const QList<int> &ids)
{
    QHash<int, QString> out;
    if (ids.isEmpty())
        return out;
    QStringList list;
    for (int id : ids)
        list << QString::number(id);
    QSqlQuery q;
    if (q.exec(QStringLiteral("SELECT id, sku FROM products WHERE id IN (%1)").arg(list.join(QLatin1Char(',')))))
        while (q.next())
            out.insert(q.value(0).toInt(), q.value(1).toString());
    return out;
}

QHash<int, QString> warehouseNames()
{
    QHash<int, QString> out;
    QSqlQuery q(QStringLiteral("SELECT id, name FROM warehouses"));
    while (q.next())
        out.insert(q.value(0).toInt(), q.value(1).toString());
    return out;
}

// Остатки по парам «товар - склад» (при lock - с блокировкой строк до конца транзакции).
QMap<Key, qint64> stockOf(const QList<Key> &keys, bool lock)
{
    QMap<Key, qint64> out;
    if (keys.isEmpty())
        return out;
    QStringList pairs;
    for (int i = 0; i < keys.size(); ++i)
        pairs << QStringLiteral("(?, ?)");
    QSqlQuery q;
    q.prepare(QStringLiteral("SELECT product_id, warehouse_id, quantity FROM stock WHERE (product_id, warehouse_id) IN (%1)%2")
                  .arg(pairs.join(QLatin1Char(',')), lock ? QStringLiteral(" FOR UPDATE") : QString()));
    for (const Key &k : keys) {
        q.addBindValue(k.pid);
        q.addBindValue(k.wid);
    }
    if (q.exec())
        while (q.next())
            out[Key{q.value(0).toInt(), q.value(1).toInt()}] = q.value(2).toLongLong();
    return out;
}

bool addToStock(const QMap<Key, qint64> &net, QString *error)
{
    QStringList rows;
    for (auto it = net.cbegin(); it != net.cend(); ++it)
        if (it.value() != 0)
            rows << QStringLiteral("(?, ?, ?)");
    if (rows.isEmpty())
        return true;
    QSqlQuery q;
    q.prepare(QStringLiteral("INSERT INTO stock (product_id, warehouse_id, quantity) VALUES %1 "
                             "ON CONFLICT (product_id, warehouse_id) DO UPDATE SET quantity = stock.quantity + excluded.quantity")
                  .arg(rows.join(QLatin1Char(','))));
    for (auto it = net.cbegin(); it != net.cend(); ++it)
        if (it.value() != 0) {
            q.addBindValue(it.key().pid);
            q.addBindValue(it.key().wid);
            q.addBindValue(it.value());
        }
    if (!q.exec()) {
        if (error)
            *error = q.lastError().text();
        return false;
    }
    return true;
}

QString placeOf(const QHash<int, QString> &skus, const QHash<int, QString> &warehouses, const Key &k)
{
    return QStringLiteral("%1, склад «%2»").arg(skus.value(k.pid, QStringLiteral("товар №%1").arg(k.pid)), warehouses.value(k.wid, QStringLiteral("№%1").arg(k.wid)));
}

} // namespace

bool draftOf(const Info &d, Draft *out, QString *why)
{
    if (d.id <= 0) {
        if (why)
            *why = QStringLiteral("Документ не найден");
        return false;
    }
    if (d.kind != QLatin1String("receipt") && d.kind != QLatin1String("writeoff") && d.kind != QLatin1String("transfer")) {
        if (why)
            *why = QStringLiteral("Править можно приход, списание и перемещение; этот документ можно только отменить или удалить");
        return false;
    }
    if (d.reverted > 0) {
        if (why)
            *why = QStringLiteral("Документ уже отменён (%1): править его нельзя. Удалите его или проведите новый документ").arg(statusText(d));
        return false;
    }
    const QString order = linkedOrder(d.id);
    if (!order.isEmpty()) {
        if (why)
            *why = QStringLiteral("Документ создан списанием: %1. Чтобы изменить, верните пару через заказ - иначе заказ и склад разойдутся").arg(order);
        return false;
    }
    Draft draft;
    if (!draftFromMoves(d.kind, loadMoves(d.key, false), &draft, why))
        return false;
    draft.comment = d.comment;
    if (out)
        *out = draft;
    return true;
}

bool canEdit(const Info &d, QString *why)
{
    return draftOf(d, nullptr, why);
}

bool update(int docId, const Draft &draft, QStringList *problems, QString *error)
{
    if (problems)
        problems->clear();
    auto fail = [&](const QString &text) {
        if (error)
            *error = text;
        if (problems && problems->isEmpty())
            *problems = QStringList{text};
        return false;
    };
    // Повторы одного товара складываются; количество и склады проверяются до обращения к базе.
    QMap<int, qint64> total;
    for (const Item &it : draft.items) {
        if (it.productId <= 0 || it.qty <= 0)
            return fail(QStringLiteral("Количество должно быть больше нуля"));
        total[it.productId] += it.qty;
    }
    if (total.isEmpty())
        return fail(QStringLiteral("В документе не осталось позиций. Чтобы убрать документ совсем, удалите его"));
    for (auto it = total.cbegin(); it != total.cend(); ++it)
        if (it.value() > kMaxQty)
            return fail(QStringLiteral("Слишком большое количество (%1)").arg(it.value()));
    if (draft.warehouseId <= 0)
        return fail(QStringLiteral("Выберите склад"));

    QSqlDatabase db = QSqlDatabase::database();
    if (!db.transaction())
        return fail(db.lastError().text());
    auto refuse = [&](const QStringList &list) {
        db.rollback();
        if (problems)
            *problems = list;
        if (error)
            *error = list.size() == 1 ? list.first() : QStringLiteral("Ничего не изменено:\n- ") + list.join(QStringLiteral("\n- "));
        return false;
    };

    QSqlQuery dq;
    dq.prepare(QStringLiteral("SELECT doc_key, kind FROM stock_documents WHERE id = ? FOR UPDATE"));
    dq.addBindValue(docId);
    if (!dq.exec() || !dq.next())
        return refuse({QStringLiteral("Документ №%1 не найден (возможно, его уже удалили)").arg(docId)});
    const QString key = dq.value(0).toString(), kind = dq.value(1).toString();
    if (kind == QLatin1String("transfer") && (draft.warehouseToId <= 0 || draft.warehouseToId == draft.warehouseId))
        return refuse({QStringLiteral("Выберите два разных склада: перемещать на тот же склад нельзя")});

    const QList<Move> old = loadMoves(key, true);
    QString why;
    Draft current;
    if (!draftFromMoves(kind, old, &current, &why))
        return refuse({why});
    {
        QStringList ids;
        for (const Move &m : old)
            ids << QString::number(m.id);
        QSqlQuery r;
        if (!r.exec(QStringLiteral("SELECT COUNT(*) FROM stock_movements WHERE reverts_id IN (%1)").arg(ids.join(QLatin1Char(',')))) || !r.next())
            return refuse({r.lastError().text()});
        if (r.value(0).toInt() > 0)
            return refuse({QStringLiteral("Документ уже отменён (хотя бы частично): править его нельзя")});
    }
    const QString order = linkedOrder(docId);
    if (!order.isEmpty())
        return refuse({QStringLiteral("Документ создан списанием: %1. Чтобы изменить, верните пару через заказ").arg(order)});

    // Что должно получиться в журнале и во сколько это обойдётся остаткам: новые записи минус старые.
    QList<Move> neu;
    for (auto it = total.cbegin(); it != total.cend(); ++it) {
        const int q = int(it.value());
        if (kind == QLatin1String("receipt"))
            neu.append({0, it.key(), draft.warehouseId, 0, QStringLiteral("receipt"), q});
        else if (kind == QLatin1String("writeoff"))
            neu.append({0, it.key(), draft.warehouseId, 0, QStringLiteral("writeoff"), -q});
        else {
            neu.append({0, it.key(), draft.warehouseId, draft.warehouseToId, QStringLiteral("transfer_out"), -q});
            neu.append({0, it.key(), draft.warehouseToId, draft.warehouseId, QStringLiteral("transfer_in"), q});
        }
    }
    QMap<Key, qint64> net;
    for (const Move &m : old)
        net[Key{m.productId, m.warehouseId}] -= m.delta;
    for (const Move &m : neu)
        net[Key{m.productId, m.warehouseId}] += m.delta;
    QMap<Key, qint64> change;
    for (auto it = net.cbegin(); it != net.cend(); ++it)
        if (it.value() != 0)
            change.insert(it.key(), it.value());

    const QMap<Key, qint64> have = stockOf(change.keys(), true);
    QList<int> pids;
    for (auto it = change.cbegin(); it != change.cend(); ++it)
        pids << it.key().pid;
    QStringList bad;
    {
        const QHash<int, QString> skus = skuOf(pids);
        const QHash<int, QString> whs = warehouseNames();
        for (auto it = change.cbegin(); it != change.cend(); ++it) {
            const qint64 cur = have.value(it.key(), 0), next = cur + it.value();
            if (next < 0)
                bad << QStringLiteral("%1: на складе сейчас %2 шт, а после правки понадобится ещё %3 шт - не хватает").arg(placeOf(skus, whs, it.key())).arg(cur).arg(-it.value());
            else if (next > kMaxStock)
                bad << QStringLiteral("%1: остаток получится слишком большим").arg(placeOf(skus, whs, it.key()));
        }
    }
    if (!bad.isEmpty())
        return refuse(bad);

    QString err;
    if (!addToStock(change, &err))
        return refuse({err});

    // Новые записи пишутся с прежним временем документа (он остаётся на своём месте в истории), старые убираются.
    QStringList oldIds;
    for (const Move &m : old)
        oldIds << QString::number(m.id);
    const QString comment = nn(draft.comment.trimmed().left(500));
    {
        QStringList rows;
        for (int i = 0; i < neu.size(); ++i)
            rows << QStringLiteral("(?, ?, ?, ?, ?, ?, ?, (SELECT MIN(created_at) FROM stock_movements WHERE id IN (%1)))").arg(oldIds.join(QLatin1Char(',')));
        QSqlQuery q;
        q.prepare(QStringLiteral("INSERT INTO stock_movements (product_id, warehouse_id, related_warehouse_id, type, delta, comment, doc_id, created_at) VALUES %1")
                      .arg(rows.join(QLatin1Char(','))));
        for (const Move &m : neu) {
            q.addBindValue(m.productId);
            q.addBindValue(m.warehouseId);
            q.addBindValue(m.related ? QVariant(m.related) : QVariant(QMetaType(QMetaType::Int)));
            q.addBindValue(m.type);
            q.addBindValue(m.delta);
            q.addBindValue(comment);
            q.addBindValue(key);
        }
        if (!q.exec())
            return refuse({q.lastError().text()});
    }
    {
        QSqlQuery q;
        if (!q.exec(QStringLiteral("DELETE FROM stock_movements WHERE id IN (%1)").arg(oldIds.join(QLatin1Char(',')))))
            return refuse({q.lastError().text()});
        QSqlQuery u;
        u.prepare(QStringLiteral("UPDATE stock_documents SET comment = ?, edited_at = NOW(), edited_by = ? WHERE id = ?"));
        u.addBindValue(comment);
        u.addBindValue(nn(Database::currentUser()));
        u.addBindValue(docId);
        if (!u.exec())
            return refuse({u.lastError().text()});
    }
    if (!db.commit()) {
        const QString e = db.lastError().text();
        db.rollback();
        return fail(e);
    }
    return true;
}

bool setComment(int docId, const QString &comment, QString *error)
{
    QSqlDatabase db = QSqlDatabase::database();
    if (!db.transaction()) {
        if (error)
            *error = db.lastError().text();
        return false;
    }
    const QString text = nn(comment.trimmed().left(500));
    QSqlQuery u;
    u.prepare(QStringLiteral("UPDATE stock_documents SET comment = ?, edited_at = NOW(), edited_by = ? WHERE id = ? RETURNING doc_key"));
    u.addBindValue(text);
    u.addBindValue(nn(Database::currentUser()));
    u.addBindValue(docId);
    if (!u.exec() || !u.next()) {
        if (error)
            *error = u.lastError().isValid() ? u.lastError().text() : QStringLiteral("Документ не найден (возможно, его уже удалили)");
        db.rollback();
        return false;
    }
    QSqlQuery m;
    m.prepare(QStringLiteral("UPDATE stock_movements SET comment = ? WHERE doc_id = ? AND type <> 'cancel'"));
    m.addBindValue(text);
    m.addBindValue(u.value(0).toString());
    if (!m.exec() || !db.commit()) {
        if (error)
            *error = m.lastError().isValid() ? m.lastError().text() : db.lastError().text();
        db.rollback();
        return false;
    }
    return true;
}

namespace {

struct RemovalSet {
    QString key;
    QString kind;
    QList<Move> own;           // записи самого документа
    QList<Move> reverters;     // записи отмен, которые отменяют записи документа
    QStringList reverterKeys;  // документы этих отмен
};

DeletePlan planDeleteIn(int docId, bool restoreStock, bool lock, RemovalSet *set)
{
    DeletePlan plan;
    QSqlQuery dq;
    dq.prepare(QStringLiteral("SELECT doc_key, kind FROM stock_documents WHERE id = ?%1").arg(lock ? QStringLiteral(" FOR UPDATE") : QString()));
    dq.addBindValue(docId);
    if (!dq.exec() || !dq.next()) {
        plan.problems << QStringLiteral("Документ №%1 не найден (возможно, его уже удалили)").arg(docId);
        return plan;
    }
    set->key = dq.value(0).toString();
    set->kind = dq.value(1).toString();
    const QString order = linkedOrder(docId);
    if (!order.isEmpty())
        plan.problems << QStringLiteral("Документ создан списанием: %1. Сначала верните пару через заказ (или отмените заказ), иначе заказ и склад разойдутся").arg(order);
    if (set->kind == QLatin1String("cancel") && !restoreStock)
        plan.problems << QStringLiteral("Документ-отмену можно удалить только с возвратом остатков: отменённые документы снова станут действующими");

    set->own = loadMoves(set->key, lock);
    if (!set->own.isEmpty()) {
        QStringList ids;
        for (const Move &m : std::as_const(set->own))
            ids << QString::number(m.id);
        QSqlQuery q;
        if (q.exec(QStringLiteral("SELECT id, product_id, warehouse_id, COALESCE(related_warehouse_id, 0), type, delta, doc_id FROM stock_movements "
                                  "WHERE reverts_id IN (%1) ORDER BY id%2").arg(ids.join(QLatin1Char(',')), lock ? QStringLiteral(" FOR UPDATE") : QString()))) {
            QSet<QString> keys;
            while (q.next()) {
                set->reverters.append({q.value(0).toInt(), q.value(1).toInt(), q.value(2).toInt(), q.value(3).toInt(), q.value(4).toString(), q.value(5).toInt()});
                keys.insert(q.value(6).toString());
            }
            set->reverterKeys = QStringList(keys.cbegin(), keys.cend());
        }
    }
    plan.movements = int(set->own.size() + set->reverters.size());

    // Документы-отмены, которые уйдут целиком (в них не останется других записей).
    for (const QString &k : std::as_const(set->reverterKeys)) {
        QSqlQuery q;
        QStringList mine;
        for (const Move &m : std::as_const(set->reverters))
            mine << QString::number(m.id);
        q.prepare(QStringLiteral("SELECT COUNT(*) FROM stock_movements WHERE doc_id = ? AND id NOT IN (%1)").arg(mine.join(QLatin1Char(','))));
        q.addBindValue(k);
        if (q.exec() && q.next() && q.value(0).toInt() == 0)
            ++plan.cancelDocuments;
    }

    if (!restoreStock)
        return plan;
    QMap<Key, qint64> net;
    for (const Move &m : std::as_const(set->own))
        net[Key{m.productId, m.warehouseId}] -= m.delta;
    for (const Move &m : std::as_const(set->reverters))
        net[Key{m.productId, m.warehouseId}] -= m.delta;
    QMap<Key, qint64> change;
    for (auto it = net.cbegin(); it != net.cend(); ++it)
        if (it.value() != 0)
            change.insert(it.key(), it.value());
    const QMap<Key, qint64> have = stockOf(change.keys(), lock);
    QList<int> pids;
    for (auto it = change.cbegin(); it != change.cend(); ++it)
        pids << it.key().pid;
    QHash<int, QString> names;
    if (!pids.isEmpty()) {
        QStringList list;
        for (int id : pids)
            list << QString::number(id);
        QSqlQuery q;
        if (q.exec(QStringLiteral("SELECT id, name FROM products WHERE id IN (%1)").arg(list.join(QLatin1Char(',')))))
            while (q.next())
                names.insert(q.value(0).toInt(), q.value(1).toString());
    }
    const QHash<int, QString> skus = skuOf(pids);
    const QHash<int, QString> whs = warehouseNames();
    for (auto it = change.cbegin(); it != change.cend(); ++it) {
        DeleteRow r;
        r.productId = it.key().pid;
        r.sku = skus.value(r.productId);
        r.name = names.value(r.productId);
        r.warehouseId = it.key().wid;
        r.warehouse = whs.value(r.warehouseId);
        r.current = have.value(it.key(), 0);
        r.after = r.current + it.value();
        if (r.after < 0)
            plan.problems << QStringLiteral("%1: сейчас на складе %2 шт, а документ нужно «вернуть» на %3 шт - товар уже расходован после этого документа. "
                                            "Выберите «оставить остатки как есть» или сначала верните товар").arg(placeOf(skus, whs, it.key())).arg(r.current).arg(-it.value());
        else if (r.after > kMaxStock)
            plan.problems << QStringLiteral("%1: остаток получится слишком большим").arg(placeOf(skus, whs, it.key()));
        plan.rows << r;
    }
    return plan;
}

} // namespace

DeletePlan planDelete(int docId, bool restoreStock)
{
    RemovalSet set;
    return planDeleteIn(docId, restoreStock, false, &set);
}

bool remove(int docId, bool restoreStock, QStringList *problems, QString *error)
{
    if (problems)
        problems->clear();
    QSqlDatabase db = QSqlDatabase::database();
    if (!db.transaction()) {
        if (error)
            *error = db.lastError().text();
        return false;
    }
    auto refuse = [&](const QStringList &list) {
        db.rollback();
        if (problems)
            *problems = list;
        if (error)
            *error = list.size() == 1 ? list.first() : QStringLiteral("Удалить нельзя:\n- ") + list.join(QStringLiteral("\n- "));
        return false;
    };
    RemovalSet set;
    const DeletePlan plan = planDeleteIn(docId, restoreStock, true, &set);
    if (!plan.ok())
        return refuse(plan.problems);

    if (restoreStock) {
        QMap<Key, qint64> change;
        for (const DeleteRow &r : plan.rows)
            change[Key{r.productId, r.warehouseId}] = r.after - r.current;
        QString err;
        if (!addToStock(change, &err))
            return refuse({err});
    }
    // Сначала отмены (они ссылаются на записи документа), потом сами записи, потом документы.
    auto deleteMoves = [&](const QList<Move> &moves) {
        if (moves.isEmpty())
            return true;
        QStringList ids;
        for (const Move &m : moves)
            ids << QString::number(m.id);
        QSqlQuery q;
        return q.exec(QStringLiteral("DELETE FROM stock_movements WHERE id IN (%1)").arg(ids.join(QLatin1Char(','))));
    };
    if (!deleteMoves(set.reverters) || !deleteMoves(set.own))
        return refuse({QStringLiteral("Не удалось удалить записи журнала")});
    for (const QString &k : std::as_const(set.reverterKeys)) {
        QSqlQuery q;
        q.prepare(QStringLiteral("DELETE FROM stock_documents WHERE doc_key = ? AND NOT EXISTS (SELECT 1 FROM stock_movements WHERE doc_id = ?)"));
        q.addBindValue(k);
        q.addBindValue(k);
        if (!q.exec())
            return refuse({q.lastError().text()});
    }
    {
        QSqlQuery q;
        q.prepare(QStringLiteral("UPDATE stocktakes SET document_id = NULL WHERE document_id = ?"));
        q.addBindValue(docId);
        q.exec(); // подсчёт, который провели этим документом, остаётся, но без ссылки на него
        QSqlQuery d;
        d.prepare(QStringLiteral("DELETE FROM stock_documents WHERE id = ?"));
        d.addBindValue(docId);
        if (!d.exec())
            return refuse({d.lastError().text()});
    }
    if (!db.commit()) {
        const QString e = db.lastError().text();
        db.rollback();
        if (error)
            *error = e;
        return false;
    }
    return true;
}

} // namespace Documents
