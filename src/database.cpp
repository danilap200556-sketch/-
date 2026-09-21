#include "database.h"
#include "serverconfig.h"

#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
#include <QVariant>
#include <QDebug>
#include <QThread>

#ifdef QPSQL_EMBEDDED_AVAILABLE
#include "thirdparty/qpsql/qsql_psql_p.h"
#include <libpq-fe.h>
#endif

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

const QStringList &schemaDdl()
{
    static const QStringList ddl = {
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
    return ddl;
}

#ifdef QPSQL_EMBEDDED_AVAILABLE

QString pqQuote(const QString &s)
{
    QString v = s;
    v.replace(QLatin1Char('\\'), QLatin1String("\\\\"));
    v.replace(QLatin1Char('\''), QLatin1String("\\'"));
    return QLatin1Char('\'') + v + QLatin1Char('\'');
}

// Открывает соединение НАПРЯМУЮ через libpq (в обход QSqlDatabase::open()) и
// сразу одним пакетным запросом (все CREATE TABLE через ';' в одном PQexec)
// создаёт схему - это ровно ОДИН запрос с точки зрения соединения, а не 6+
// отдельных. Затем оборачивает уже открытое и проинициализированное
// соединение в QPSQLDriver(conn), которому для инициализации нужно всего
// 2 служебных запроса (а не 5, как при обычном QPSQLDriver::open()) - и то,
// и другое нужно, чтобы не перевалить за порог примерно в 4-5 запросов
// подряд, после которого Neon рвёт соединение (см. комментарий в CMakeLists.txt).
bool tryConnectEmbedded(const ServerConfig &config, QString *error)
{
    QString conninfo;
    conninfo += QLatin1String("host=") + pqQuote(config.host);
    conninfo += QLatin1String(" port=") + QString::number(config.port);
    conninfo += QLatin1String(" dbname=") + pqQuote(config.database);
    conninfo += QLatin1String(" user=") + pqQuote(config.user);
    conninfo += QLatin1String(" password=") + pqQuote(config.password);
    if (config.useSsl)
        conninfo += QLatin1String(" sslmode=require sslnegotiation=postgres");
    conninfo += QLatin1String(" connect_timeout=20");

    PGconn *conn = PQconnectdb(conninfo.toUtf8().constData());
    if (PQstatus(conn) != CONNECTION_OK) {
        if (error)
            *error = QString::fromUtf8(PQerrorMessage(conn)).trimmed();
        PQfinish(conn);
        return false;
    }

    const QString batch = schemaDdl().join(QLatin1String(";\n")) + QLatin1Char(';');
    const QByteArray batchUtf8 = batch.toUtf8();
    PGresult *res = PQexec(conn, batchUtf8.constData());
    const ExecStatusType status = PQresultStatus(res);
    if (status != PGRES_COMMAND_OK && status != PGRES_TUPLES_OK) {
        if (error)
            *error = QString::fromUtf8(PQresultErrorMessage(res)).trimmed();
        PQclear(res);
        PQfinish(conn);
        return false;
    }
    PQclear(res);

    if (QSqlDatabase::contains(QSqlDatabase::defaultConnection))
        QSqlDatabase::removeDatabase(QSqlDatabase::defaultConnection);

    // QPSQLDriver берёт на себя владение conn (включая PQfinish при закрытии) -
    // сами conn больше не трогаем. QSqlDatabase::addDatabase(QSqlDriver*)
    // берёт на себя владение самим драйвером.
    auto *driver = new QPSQLDriver(conn);
    QSqlDatabase db = QSqlDatabase::addDatabase(driver);
    if (!db.isOpen()) {
        if (error)
            *error = QStringLiteral("Драйвер не смог инициализироваться после подключения");
        return false;
    }
    return true;
}

#else

bool tryConnectViaQtDriver(const ServerConfig &config, QString *error)
{
    QSqlDatabase db = QSqlDatabase::addDatabase("QPSQL");
    db.setHostName(config.host);
    db.setPort(config.port);
    db.setDatabaseName(config.database);
    db.setUserName(config.user);
    db.setPassword(config.password);
    if (config.useSsl)
        db.setConnectOptions("sslmode=require;sslnegotiation=postgres;connect_timeout=20");

    if (!db.open()) {
        if (error)
            *error = db.lastError().text();
        return false;
    }

    QSqlQuery q(db);
    for (const QString &stmt : schemaDdl()) {
        if (!execOrFail(q, stmt, error))
            return false;
    }
    return true;
}

#endif

} // namespace

bool Database::open(const ServerConfig &config, QString *error)
{
    // У serverless-провайдеров (Neon и т.п.) соединение иногда обрывается
    // разово, без видимой причины - обычно повтор через секунду-другую уже
    // проходит. Пробуем несколько раз, прежде чем сдаться.
    QString lastError;
    for (int attempt = 1; attempt <= 3; ++attempt) {
#ifdef QPSQL_EMBEDDED_AVAILABLE
        if (tryConnectEmbedded(config, &lastError)) {
#else
        if (tryConnectViaQtDriver(config, &lastError)) {
#endif
            return true;
        }
        if (attempt < 3) {
            qWarning() << "Database::open: попытка" << attempt << "не удалась, повтор:" << lastError;
            QThread::msleep(1500);
        }
    }
    if (error)
        *error = lastError;
    return false;
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
