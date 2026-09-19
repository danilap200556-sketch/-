#include "database.h"
#include "serverconfig.h"

#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
#include <QVariant>
#include <QDebug>

namespace {

bool execOrFail(QSqlQuery &q, const QString &sql, QString *error)
{
    if (!q.exec(sql)) {
        if (error)
            *error = q.lastError().text();
        return false;
    }
    return true;
}

} // namespace

bool Database::open(const ServerConfig &config, QString *error)
{
    QSqlDatabase db = QSqlDatabase::addDatabase("QPSQL");
    db.setHostName(config.host);
    db.setPort(config.port);
    db.setDatabaseName(config.database);
    db.setUserName(config.user);
    db.setPassword(config.password);
    if (config.useSsl)
        // connect_timeout - у serverless-провайдеров (Neon, Supabase) сервер может
        // "просыпаться" на первое подключение после простоя, дефолтный таймаут
        // libpq для этого маловат.
        // sslnegotiation=postgres - явно классический способ согласования TLS
        // (SSLRequest, затем апгрейд до TLS). Новый режим "direct" (libpq 17+)
        // многие коннекшн-пулеры (в т.ч. PgBouncer, на котором у Neon работает
        // pooler-эндпоинт) ещё не понимают и обрывают соединение.
        db.setConnectOptions("sslmode=require;sslnegotiation=postgres;connect_timeout=20");

    if (!db.open()) {
        if (error)
            *error = db.lastError().text();
        return false;
    }

    QSqlQuery q(db);

    const QStringList ddl = {
        R"(CREATE TABLE IF NOT EXISTS products (
            id INTEGER GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
            sku TEXT NOT NULL UNIQUE,
            name TEXT NOT NULL,
            description TEXT,
            photo_path TEXT,
            price REAL NOT NULL DEFAULT 0,
            custom_code TEXT,
            created_at TIMESTAMPTZ NOT NULL DEFAULT NOW()
        ))",
        R"(CREATE TABLE IF NOT EXISTS warehouses (
            id INTEGER GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
            name TEXT NOT NULL UNIQUE,
            address TEXT
        ))",
        R"(CREATE TABLE IF NOT EXISTS locations (
            id INTEGER GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
            warehouse_id INTEGER NOT NULL REFERENCES warehouses(id) ON DELETE CASCADE,
            code TEXT NOT NULL,
            description TEXT,
            UNIQUE(warehouse_id, code)
        ))",
        R"(CREATE TABLE IF NOT EXISTS stock (
            product_id INTEGER NOT NULL REFERENCES products(id) ON DELETE CASCADE,
            warehouse_id INTEGER NOT NULL REFERENCES warehouses(id) ON DELETE CASCADE,
            location_id INTEGER REFERENCES locations(id) ON DELETE SET NULL,
            quantity INTEGER NOT NULL DEFAULT 0,
            PRIMARY KEY (product_id, warehouse_id)
        ))",
        R"(CREATE TABLE IF NOT EXISTS users (
            id INTEGER GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
            username TEXT NOT NULL UNIQUE,
            password_hash TEXT NOT NULL,
            salt TEXT NOT NULL,
            created_at TIMESTAMPTZ NOT NULL DEFAULT NOW()
        ))",
        R"(CREATE TABLE IF NOT EXISTS stock_movements (
            id INTEGER GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
            product_id INTEGER NOT NULL REFERENCES products(id) ON DELETE CASCADE,
            warehouse_id INTEGER NOT NULL REFERENCES warehouses(id) ON DELETE CASCADE,
            related_warehouse_id INTEGER REFERENCES warehouses(id),
            type TEXT NOT NULL,
            delta INTEGER NOT NULL,
            comment TEXT,
            created_at TIMESTAMPTZ NOT NULL DEFAULT NOW()
        ))",
    };

    for (const QString &stmt : ddl) {
        if (!execOrFail(q, stmt, error))
            return false;
    }

    return true;
}

QString Database::movementTypeLabel(MovementType type)
{
    switch (type) {
    case MovementType::Receipt: return QStringLiteral("receipt");
    case MovementType::WriteOff: return QStringLiteral("writeoff");
    case MovementType::TransferOut: return QStringLiteral("transfer_out");
    case MovementType::TransferIn: return QStringLiteral("transfer_in");
    case MovementType::InventoryAdjust: return QStringLiteral("inventory_adjust");
    }
    return QString();
}

int Database::currentStock(int productId, int warehouseId)
{
    QSqlQuery q;
    q.prepare("SELECT quantity FROM stock WHERE product_id = ? AND warehouse_id = ?");
    q.addBindValue(productId);
    q.addBindValue(warehouseId);
    if (!q.exec() || !q.next())
        return 0;
    return q.value(0).toInt();
}

namespace {

// Выполняет одну проводку (запись в журнал + обновление агрегированного остатка)
// без управления транзакцией - вызывающая сторона решает, когда commit/rollback.
bool applyMovementInternal(int productId, int warehouseId, int delta,
                            Database::MovementType type,
                            std::optional<int> relatedWarehouseId,
                            const QString &comment, QString *error)
{
    if (delta < 0) {
        const int have = Database::currentStock(productId, warehouseId);
        if (have + delta < 0) {
            if (error)
                *error = QStringLiteral("Недостаточно остатка: на складе %1 шт, требуется списать %2 шт")
                             .arg(have).arg(-delta);
            return false;
        }
    }

    QSqlQuery q;
    q.prepare("INSERT INTO stock_movements (product_id, warehouse_id, related_warehouse_id, type, delta, comment) "
              "VALUES (?, ?, ?, ?, ?, ?)");
    q.addBindValue(productId);
    q.addBindValue(warehouseId);
    if (relatedWarehouseId)
        q.addBindValue(*relatedWarehouseId);
    else
        q.addBindValue(QVariant(QMetaType(QMetaType::Int)));
    q.addBindValue(Database::movementTypeLabel(type));
    q.addBindValue(delta);
    q.addBindValue(comment);
    if (!q.exec()) {
        if (error)
            *error = q.lastError().text();
        return false;
    }

    QSqlQuery upsert;
    upsert.prepare(
        "INSERT INTO stock (product_id, warehouse_id, quantity) VALUES (?, ?, ?) "
        "ON CONFLICT(product_id, warehouse_id) DO UPDATE SET quantity = stock.quantity + excluded.quantity");
    upsert.addBindValue(productId);
    upsert.addBindValue(warehouseId);
    upsert.addBindValue(delta);
    if (!upsert.exec()) {
        if (error)
            *error = upsert.lastError().text();
        return false;
    }

    return true;
}

} // namespace

bool Database::adjustStock(int productId, int warehouseId, int delta, MovementType type,
                            const QString &comment, QString *error)
{
    QSqlDatabase db = QSqlDatabase::database();
    if (!db.transaction()) {
        if (error)
            *error = db.lastError().text();
        return false;
    }

    if (!applyMovementInternal(productId, warehouseId, delta, type, std::nullopt, comment, error)) {
        db.rollback();
        return false;
    }

    if (!db.commit()) {
        if (error)
            *error = db.lastError().text();
        db.rollback();
        return false;
    }
    return true;
}

bool Database::transferStock(int productId, int srcWarehouseId, int dstWarehouseId,
                              int quantity, const QString &comment, QString *error)
{
    if (quantity <= 0) {
        if (error)
            *error = QStringLiteral("Количество для перемещения должно быть больше нуля");
        return false;
    }
    if (srcWarehouseId == dstWarehouseId) {
        if (error)
            *error = QStringLiteral("Исходный и целевой склад совпадают");
        return false;
    }

    QSqlDatabase db = QSqlDatabase::database();
    if (!db.transaction()) {
        if (error)
            *error = db.lastError().text();
        return false;
    }

    if (!applyMovementInternal(productId, srcWarehouseId, -quantity, MovementType::TransferOut,
                                dstWarehouseId, comment, error)) {
        db.rollback();
        return false;
    }
    if (!applyMovementInternal(productId, dstWarehouseId, quantity, MovementType::TransferIn,
                                srcWarehouseId, comment, error)) {
        db.rollback();
        return false;
    }

    if (!db.commit()) {
        if (error)
            *error = db.lastError().text();
        db.rollback();
        return false;
    }
    return true;
}

bool Database::inventoryAdjust(int productId, int warehouseId, int countedQuantity,
                                const QString &comment, QString *error)
{
    if (countedQuantity < 0) {
        if (error)
            *error = QStringLiteral("Посчитанное количество не может быть отрицательным");
        return false;
    }
    const int current = currentStock(productId, warehouseId);
    const int delta = countedQuantity - current;
    if (delta == 0)
        return true; // расхождений нет, ничего писать не нужно

    QSqlDatabase db = QSqlDatabase::database();
    if (!db.transaction()) {
        if (error)
            *error = db.lastError().text();
        return false;
    }

    QString fullComment = comment;
    if (!fullComment.isEmpty())
        fullComment += " ";
    fullComment += QStringLiteral("(инвентаризация: было %1, стало %2)").arg(current).arg(countedQuantity);

    if (!applyMovementInternal(productId, warehouseId, delta, MovementType::InventoryAdjust,
                                std::nullopt, fullComment, error)) {
        db.rollback();
        return false;
    }

    if (!db.commit()) {
        if (error)
            *error = db.lastError().text();
        db.rollback();
        return false;
    }
    return true;
}

bool Database::setStockLocation(int productId, int warehouseId, std::optional<int> locationId, QString *error)
{
    QSqlQuery q;
    q.prepare(
        "INSERT INTO stock (product_id, warehouse_id, location_id, quantity) VALUES (?, ?, ?, 0) "
        "ON CONFLICT(product_id, warehouse_id) DO UPDATE SET location_id = excluded.location_id");
    q.addBindValue(productId);
    q.addBindValue(warehouseId);
    if (locationId)
        q.addBindValue(*locationId);
    else
        q.addBindValue(QVariant(QMetaType(QMetaType::Int)));
    if (!q.exec()) {
        if (error)
            *error = q.lastError().text();
        return false;
    }
    return true;
}
