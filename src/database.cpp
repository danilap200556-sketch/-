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
        // --- Пользователи: права администратора. Если админа ещё нет (база
        // создана до появления этого поля) - им становится первый пользователь.
        "ALTER TABLE users ADD COLUMN IF NOT EXISTS is_admin BOOLEAN NOT NULL DEFAULT FALSE",
        "UPDATE users SET is_admin = TRUE WHERE id = (SELECT MIN(id) FROM users) "
        "AND NOT EXISTS (SELECT 1 FROM users WHERE is_admin)",
        // --- Яндекс Маркет ---
        // Артикул товара в каталоге Маркета (offerId), если он отличается от
        // нашего sku. Пусто - используется sku.
        "ALTER TABLE products ADD COLUMN IF NOT EXISTS market_sku TEXT",
        R"(CREATE TABLE IF NOT EXISTS market_accounts (
            id INTEGER GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
            name TEXT NOT NULL,
            api_key TEXT NOT NULL,
            business_id BIGINT NOT NULL,
            campaign_id BIGINT NOT NULL,
            warehouse_groups BOOLEAN NOT NULL DEFAULT FALSE,
            market_warehouse_id BIGINT,
            created_at TIMESTAMPTZ NOT NULL DEFAULT NOW()
        ))",
        // Штрихкоды товаров (с коробок производителя или свои). У товара их
        // может быть несколько, но один штрихкод - только у одного товара.
        R"(CREATE TABLE IF NOT EXISTS product_barcodes (
            barcode TEXT PRIMARY KEY,
            product_id INTEGER NOT NULL REFERENCES products(id) ON DELETE CASCADE,
            created_at TIMESTAMPTZ NOT NULL DEFAULT NOW()
        ))",
        "CREATE INDEX IF NOT EXISTS product_barcodes_product_idx ON product_barcodes (product_id)",
        // Наши склады, остатки которых суммируются и передаются в кабинет.
        // Пусто - берутся все склады.
        R"(CREATE TABLE IF NOT EXISTS market_account_warehouses (
            account_id INTEGER NOT NULL REFERENCES market_accounts(id) ON DELETE CASCADE,
            warehouse_id INTEGER NOT NULL REFERENCES warehouses(id) ON DELETE CASCADE,
            PRIMARY KEY (account_id, warehouse_id)
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

QByteArray connInfo(const ServerConfig &config)
{
    QString conninfo;
    conninfo += QLatin1String("host=") + pqQuote(config.host);
    conninfo += QLatin1String(" port=") + QString::number(config.port);
    conninfo += QLatin1String(" dbname=") + pqQuote(config.database);
    conninfo += QLatin1String(" user=") + pqQuote(config.user);
    conninfo += QLatin1String(" password=") + pqQuote(config.password);
    // QPSQLDriver(PGconn*) не делает SET CLIENT_ENCODING (в отличие от обычного
    // open()), а сам всегда кодирует строки в UTF-8. Передаём кодировку в
    // стартовом пакете - это не отдельный запрос, лимит Neon не тратится.
    conninfo += QLatin1String(" client_encoding=UTF8");
    if (config.useSsl) {
        conninfo += QLatin1String(" sslmode=require");
        // Параметр появился только в libpq 17 - старая libpq отвергла бы всю
        // строку подключения как "invalid connection option".
        if (PQlibVersion() >= 170000)
            conninfo += QLatin1String(" sslnegotiation=postgres");
    }
    conninfo += QLatin1String(" connect_timeout=20");
    return conninfo.toUtf8();
}

// Возвращает открытое соединение или nullptr (с текстом ошибки в *error).
PGconn *connectRaw(const ServerConfig &config, QString *error)
{
    PGconn *conn = PQconnectdb(connInfo(config).constData());
    if (PQstatus(conn) != CONNECTION_OK) {
        if (error)
            *error = QString::fromUtf8(PQerrorMessage(conn)).trimmed();
        PQfinish(conn);
        return nullptr;
    }
    return conn;
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
    PGconn *conn = connectRaw(config, error);
    if (!conn)
        return false;

    const QString batch = schemaDdl().join(QLatin1String(";\n")) + QLatin1Char(';');
    const QByteArray batchUtf8 = batch.toUtf8();
    // Иначе libpq на каждом запуске печатает в stderr
    // "NOTICE: relation ... already exists, skipping" по разу на таблицу.
    const PQnoticeProcessor prevNotice =
        PQsetNoticeProcessor(conn, [](void *, const char *) {}, nullptr);
    PGresult *res = PQexec(conn, batchUtf8.constData());
    PQsetNoticeProcessor(conn, prevNotice, nullptr);
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
    if (QSqlDatabase::contains(QSqlDatabase::defaultConnection))
        QSqlDatabase::removeDatabase(QSqlDatabase::defaultConnection);
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

bool Database::testConnection(const ServerConfig &config, QString *error)
{
#ifdef QPSQL_EMBEDDED_AVAILABLE
    // Обычный QSqlDatabase::open() тут не годится - он сам шлёт серию
    // служебных запросов, на которой Neon рвёт соединение.
    PGconn *conn = connectRaw(config, error);
    if (!conn)
        return false;
    PQfinish(conn);
    return true;
#else
    const QString name = QStringLiteral("connection_test");
    bool ok = false;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase("QPSQL", name);
        db.setHostName(config.host);
        db.setPort(config.port);
        db.setDatabaseName(config.database);
        db.setUserName(config.user);
        db.setPassword(config.password);
        if (config.useSsl)
            db.setConnectOptions("sslmode=require;sslnegotiation=postgres;connect_timeout=20");
        ok = db.open();
        if (!ok && error)
            *error = db.lastError().text();
        db.close();
    }
    QSqlDatabase::removeDatabase(name);
    return ok;
#endif
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
