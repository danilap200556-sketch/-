#include "documents.h"

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
    "            JOIN stock_documents od ON od.doc_key = o.doc_id WHERE c.doc_id = d.doc_key), '') ELSE '' END ";

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
              QString::fromLatin1(kInfoJoins) + QStringLiteral("GROUP BY d.id, d.doc_key, d.kind, d.created_at, d.username, d.comment ORDER BY d.id DESC"));
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
              QString::fromLatin1(kInfoJoins) + QStringLiteral("GROUP BY d.id, d.doc_key, d.kind, d.created_at, d.username, d.comment"));
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

} // namespace Documents
