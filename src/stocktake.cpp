#include "stocktake.h"

#include "database.h"

#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace Stocktake {

namespace {

const char kRowSelect[] =
    "SELECT up.product_id, p.sku, p.name, COALESCE(s.quantity, 0), up.counted "
    "FROM up JOIN products p ON p.id = up.product_id JOIN stocktakes st ON st.id = up.stocktake_id "
    "LEFT JOIN stock s ON s.product_id = up.product_id AND s.warehouse_id = st.warehouse_id";

Row readRow(const QSqlQuery &q)
{
    Row r;
    r.productId = q.value(0).toInt();
    r.sku = q.value(1).toString();
    r.name = q.value(2).toString();
    r.book = q.value(3).toInt();
    r.counted = q.value(4).toInt();
    return r;
}

bool fail(QString *error, const QString &text)
{
    if (error)
        *error = text;
    return false;
}

Info readInfo(QSqlQuery &q)
{
    Info i;
    i.id = q.value(0).toInt();
    i.warehouseId = q.value(1).toInt();
    i.warehouse = q.value(2).toString();
    i.status = q.value(3).toString();
    i.startedBy = q.value(4).toString();
    i.startedAt = q.value(5).toDateTime();
    i.documentId = q.value(6).toInt();
    i.positions = q.value(7).toInt();
    i.units = q.value(8).toLongLong();
    return i;
}

const char kInfoSelect[] =
    "SELECT st.id, st.warehouse_id, w.name, st.status, st.started_by, st.started_at, COALESCE(st.document_id, 0), "
    "       (SELECT COUNT(*) FROM stocktake_items i WHERE i.stocktake_id = st.id), "
    "       (SELECT COALESCE(SUM(i.counted), 0) FROM stocktake_items i WHERE i.stocktake_id = st.id) "
    "FROM stocktakes st JOIN warehouses w ON w.id = st.warehouse_id ";

// Товар, ушедший из базы между сканом и записью, и закрытый подсчёт - частые причины отказа: объясняем по-человечески.
QString explain(const QSqlQuery &q)
{
    if (q.lastError().nativeErrorCode() == QLatin1String("23503"))
        return QStringLiteral("Товар уже удалён (возможно, с другого компьютера).");
    if (q.lastError().nativeErrorCode() == QLatin1String("22003"))
        return QStringLiteral("Слишком большое количество.");
    return q.lastError().text();
}

} // namespace

Info current(int warehouseId)
{
    QSqlQuery q;
    q.prepare(QString::fromLatin1(kInfoSelect) + "WHERE st.warehouse_id = ? AND st.status = 'open'");
    q.addBindValue(warehouseId);
    if (q.exec() && q.next())
        return readInfo(q);
    return {};
}

Info get(int id)
{
    QSqlQuery q;
    q.prepare(QString::fromLatin1(kInfoSelect) + "WHERE st.id = ?");
    q.addBindValue(id);
    if (q.exec() && q.next())
        return readInfo(q);
    return {};
}

int start(int warehouseId, QString *error)
{
    QSqlQuery q;
    q.prepare("INSERT INTO stocktakes (warehouse_id, started_by) VALUES (?, ?) "
              "ON CONFLICT (warehouse_id) WHERE status = 'open' DO NOTHING");
    q.addBindValue(warehouseId);
    q.addBindValue(Database::currentUser());
    if (!q.exec()) {
        fail(error, explain(q));
        return 0;
    }
    const Info i = current(warehouseId);
    if (!i.valid())
        fail(error, QStringLiteral("Не удалось начать подсчёт: склад уже удалён?"));
    return i.id;
}

bool add(int id, int productId, int delta, Row *row, QString *error)
{
    if (productId <= 0)
        return fail(error, QStringLiteral("Товар не выбран"));
    if (qAbs(delta) > kMaxCount)
        return fail(error, QStringLiteral("Слишком большое количество."));
    QSqlQuery q;
    q.prepare(QStringLiteral("WITH up AS (INSERT INTO stocktake_items AS i (stocktake_id, product_id, counted, updated_by) "
                             "SELECT st.id, ?::int, GREATEST(?::int, 0), ?::text FROM stocktakes st WHERE st.id = ?::int AND st.status = 'open' "
                             "ON CONFLICT (stocktake_id, product_id) DO UPDATE "
                             "SET counted = LEAST(GREATEST(i.counted + ?::int, 0), %1), updated_at = NOW(), updated_by = EXCLUDED.updated_by "
                             "RETURNING stocktake_id, product_id, counted) %2")
                  .arg(kMaxCount).arg(QString::fromLatin1(kRowSelect)));
    q.addBindValue(productId);
    q.addBindValue(delta);
    q.addBindValue(Database::currentUser());
    q.addBindValue(id);
    q.addBindValue(delta);
    if (!q.exec())
        return fail(error, explain(q));
    if (!q.next())
        return fail(error, QStringLiteral("Подсчёт уже завершён или сброшен: начните новый."));
    if (row)
        *row = readRow(q);
    return true;
}

bool setCounted(int id, int productId, int counted, Row *row, QString *error)
{
    if (productId <= 0)
        return fail(error, QStringLiteral("Товар не выбран"));
    if (counted > kMaxCount)
        return fail(error, QStringLiteral("Слишком большое количество."));
    QSqlQuery q;
    if (counted < 0) {
        // Товар убирают из подсчёта: строка удаляется, а в ответе - его состояние «не посчитано».
        q.prepare(QStringLiteral("WITH up AS (DELETE FROM stocktake_items i USING stocktakes st WHERE i.stocktake_id = ?::int AND i.product_id = ?::int "
                                 "AND st.id = i.stocktake_id AND st.status = 'open' RETURNING i.stocktake_id, i.product_id, 0 AS counted) %1")
                      .arg(QString::fromLatin1(kRowSelect)));
        q.addBindValue(id);
        q.addBindValue(productId);
    } else {
        q.prepare(QStringLiteral("WITH up AS (INSERT INTO stocktake_items (stocktake_id, product_id, counted, updated_by) "
                                 "SELECT st.id, ?::int, ?::int, ?::text FROM stocktakes st WHERE st.id = ?::int AND st.status = 'open' "
                                 "ON CONFLICT (stocktake_id, product_id) DO UPDATE "
                                 "SET counted = EXCLUDED.counted, updated_at = NOW(), updated_by = EXCLUDED.updated_by "
                                 "RETURNING stocktake_id, product_id, counted) %1")
                      .arg(QString::fromLatin1(kRowSelect)));
        q.addBindValue(productId);
        q.addBindValue(counted);
        q.addBindValue(Database::currentUser());
        q.addBindValue(id);
    }
    if (!q.exec())
        return fail(error, explain(q));
    if (!q.next()) {
        if (counted < 0 && get(id).status == QLatin1String("open")) {
            if (row)
                *row = Row{productId, QString(), QString(), 0, 0};
            return true; // этого товара в подсчёте и не было
        }
        return fail(error, QStringLiteral("Подсчёт уже завершён или сброшен: начните новый."));
    }
    if (row)
        *row = readRow(q);
    return true;
}

QList<Row> countedRows(int id)
{
    QList<Row> rows;
    QSqlQuery q;
    q.prepare("SELECT i.product_id, p.sku, p.name, COALESCE(s.quantity, 0), i.counted "
              "FROM stocktake_items i JOIN stocktakes st ON st.id = i.stocktake_id JOIN products p ON p.id = i.product_id "
              "LEFT JOIN stock s ON s.product_id = i.product_id AND s.warehouse_id = st.warehouse_id "
              "WHERE i.stocktake_id = ? ORDER BY i.updated_at DESC, p.sku");
    q.addBindValue(id);
    if (q.exec())
        while (q.next())
            rows << readRow(q);
    return rows;
}

QList<Row> missingRows(int id)
{
    QList<Row> rows;
    QSqlQuery q;
    q.prepare("SELECT s.product_id, p.sku, p.name, s.quantity, 0 "
              "FROM stocktakes st JOIN stock s ON s.warehouse_id = st.warehouse_id JOIN products p ON p.id = s.product_id "
              "WHERE st.id = ? AND s.quantity <> 0 "
              "AND NOT EXISTS (SELECT 1 FROM stocktake_items i WHERE i.stocktake_id = st.id AND i.product_id = s.product_id) "
              "ORDER BY p.sku");
    q.addBindValue(id);
    if (q.exec())
        while (q.next())
            rows << readRow(q);
    return rows;
}

Summary summary(int id)
{
    Summary s;
    QSqlQuery q;
    q.prepare("SELECT COUNT(*), COALESCE(SUM(i.counted), 0), "
              "       COUNT(*) FILTER (WHERE i.counted <> COALESCE(sk.quantity, 0)), "
              "       COALESCE(SUM(GREATEST(i.counted - COALESCE(sk.quantity, 0), 0)), 0), "
              "       COALESCE(SUM(GREATEST(COALESCE(sk.quantity, 0) - i.counted, 0)), 0) "
              "FROM stocktake_items i JOIN stocktakes st ON st.id = i.stocktake_id "
              "LEFT JOIN stock sk ON sk.product_id = i.product_id AND sk.warehouse_id = st.warehouse_id "
              "WHERE i.stocktake_id = ?");
    q.addBindValue(id);
    if (q.exec() && q.next()) {
        s.positions = q.value(0).toInt();
        s.units = q.value(1).toLongLong();
        s.diffPositions = q.value(2).toInt();
        s.surplus = q.value(3).toLongLong();
        s.shortage = q.value(4).toLongLong();
    }
    QSqlQuery m;
    m.prepare("SELECT COUNT(*), COALESCE(SUM(s.quantity), 0) FROM stocktakes st JOIN stock s ON s.warehouse_id = st.warehouse_id "
              "WHERE st.id = ? AND s.quantity <> 0 "
              "AND NOT EXISTS (SELECT 1 FROM stocktake_items i WHERE i.stocktake_id = st.id AND i.product_id = s.product_id)");
    m.addBindValue(id);
    if (m.exec() && m.next()) {
        s.missingPositions = m.value(0).toInt();
        s.missingUnits = m.value(1).toLongLong();
    }
    return s;
}

bool finish(int id, bool zeroMissing, FinishResult *result, QString *error)
{
    if (result)
        *result = {};
    QSqlDatabase db = QSqlDatabase::database();
    if (!db.transaction())
        return fail(error, db.lastError().text());
    auto abort = [&](const QString &text) {
        db.rollback();
        return fail(error, text);
    };

    // Сначала закрываем подсчёт: если его уже провёл другой компьютер, второй раз не проводим.
    QSqlQuery close;
    close.prepare("UPDATE stocktakes SET status = 'done', finished_at = NOW(), finished_by = ?, zero_missing = ? "
                  "WHERE id = ? AND status = 'open' RETURNING warehouse_id");
    close.addBindValue(Database::currentUser());
    close.addBindValue(zeroMissing);
    close.addBindValue(id);
    if (!close.exec())
        return abort(explain(close));
    if (!close.next())
        return abort(QStringLiteral("Подсчёт уже завершён или сброшен."));
    const int warehouseId = close.value(0).toInt();

    QList<Database::StockLevel> levels;
    QSqlQuery items;
    items.prepare("SELECT product_id, counted FROM stocktake_items WHERE stocktake_id = ?");
    items.addBindValue(id);
    if (!items.exec())
        return abort(explain(items));
    while (items.next())
        levels.append({items.value(0).toInt(), items.value(1).toInt()});

    QSqlQuery wh;
    wh.prepare("SELECT name FROM warehouses WHERE id = ?");
    wh.addBindValue(warehouseId);
    const QString warehouse = wh.exec() && wh.next() ? wh.value(0).toString() : QString();
    const QString comment = QStringLiteral("инвентаризация склада «%1» №%2").arg(warehouse).arg(id);

    int changed = 0;
    QString err;
    int documentId = 0;
    {
        Database::Document doc(QStringLiteral("inventory"), comment);
        if (!Database::setStockLevelsInTransaction(warehouseId, levels, zeroMissing, comment, &changed, &err))
            return abort(err);
    }
    documentId = changed > 0 ? Database::lastDocumentId() : 0;
    QSqlQuery link;
    link.prepare("UPDATE stocktakes SET document_id = ? WHERE id = ?");
    link.addBindValue(documentId > 0 ? QVariant(documentId) : QVariant(QMetaType(QMetaType::Int)));
    link.addBindValue(id);
    if (!link.exec())
        return abort(explain(link));
    if (!db.commit()) {
        db.rollback();
        return fail(error, db.lastError().text());
    }
    if (result) {
        result->changed = changed;
        result->documentId = documentId;
    }
    return true;
}

bool discard(int id, QString *error)
{
    QSqlQuery q;
    q.prepare("UPDATE stocktakes SET status = 'discarded', finished_at = NOW(), finished_by = ? WHERE id = ? AND status = 'open'");
    q.addBindValue(Database::currentUser());
    q.addBindValue(id);
    if (!q.exec())
        return fail(error, explain(q));
    if (q.numRowsAffected() < 1)
        return fail(error, QStringLiteral("Подсчёт уже завершён или сброшен."));
    return true;
}

} // namespace Stocktake
