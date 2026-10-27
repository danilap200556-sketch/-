#include "database.h"
#include "serverconfig.h"

#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
#include <QSqlRecord>
#include <QVariant>
#include <QDebug>
#include <QThread>
#include <QUuid>
#include <QHash>
#include <QMap>
#include <QSet>
#include <algorithm>

#ifdef QPSQL_EMBEDDED_AVAILABLE
#include "thirdparty/qpsql/qsql_psql_p.h"
#include <libpq-fe.h>
#endif

namespace {
// Текущий документ (см. Database::Document): записи журнала, сделанные пока он открыт, группируются.
QString g_document;
QString g_docKind;
QString g_docComment;
QString g_currentUser;
QString g_lastDocKey;
// Пустая строка вместо NULL: колонки документа NOT NULL, а QString() драйвер отправил бы как NULL.
QString nn(const QString &s) { return s.isNull() ? QString(QLatin1String("")) : s; }
} // namespace

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
        // Параметры карточки Яндекс Маркета у самого товара: бренд, страны, габариты и вес упаковки, ТН ВЭД, теги, 18+,
        // категория Маркета и её характеристики (JSON, как в market_cards.params). Из них собираются карточки Маркета.
        "ALTER TABLE products "
        "ADD COLUMN IF NOT EXISTS market_vendor TEXT NOT NULL DEFAULT '', "
        "ADD COLUMN IF NOT EXISTS market_countries TEXT NOT NULL DEFAULT '', "
        "ADD COLUMN IF NOT EXISTS market_length_cm NUMERIC(10,2) NOT NULL DEFAULT 0, "
        "ADD COLUMN IF NOT EXISTS market_width_cm NUMERIC(10,2) NOT NULL DEFAULT 0, "
        "ADD COLUMN IF NOT EXISTS market_height_cm NUMERIC(10,2) NOT NULL DEFAULT 0, "
        "ADD COLUMN IF NOT EXISTS market_weight_kg NUMERIC(10,3) NOT NULL DEFAULT 0, "
        "ADD COLUMN IF NOT EXISTS market_commodity_code TEXT NOT NULL DEFAULT '', "
        "ADD COLUMN IF NOT EXISTS market_tags TEXT NOT NULL DEFAULT '', "
        "ADD COLUMN IF NOT EXISTS market_adult BOOLEAN NOT NULL DEFAULT FALSE, "
        "ADD COLUMN IF NOT EXISTS market_category_id BIGINT, "
        "ADD COLUMN IF NOT EXISTS market_category_name TEXT NOT NULL DEFAULT '', "
        "ADD COLUMN IF NOT EXISTS market_params TEXT NOT NULL DEFAULT '{}'",
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
        // --- Роль "только просмотр": пользователь видит данные, но ничего не меняет.
        // Администратор всегда может редактировать (read_only для него не действует).
        "ALTER TABLE users ADD COLUMN IF NOT EXISTS read_only BOOLEAN NOT NULL DEFAULT FALSE",
        // --- Фото товаров лежат в общей базе (а не в файлах на конкретном компьютере),
        // чтобы их видели все компьютеры и сайт. data - фото до 1280 px, thumb - миниатюра
        // до 240 px; обе всегда JPEG. Первое по position - "обложка" товара.
        R"(CREATE TABLE IF NOT EXISTS product_photos (
            id INTEGER GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
            product_id INTEGER NOT NULL REFERENCES products(id) ON DELETE CASCADE,
            position INTEGER NOT NULL DEFAULT 0,
            data BYTEA NOT NULL,
            thumb BYTEA NOT NULL,
            created_at TIMESTAMPTZ NOT NULL DEFAULT NOW()
        ))",
        "CREATE INDEX IF NOT EXISTS product_photos_product_idx ON product_photos (product_id, position, id)",
        // --- Отмена операций. doc_id - документ: записи журнала, созданные одной операцией (пакет, перемещение,
        // импорт, правка карточки); их можно отменить разом. reverts_id - у записи типа cancel: какую запись она
        // отменяет; уникальный индекс не даёт отменить одну запись дважды (даже одновременно с двух компьютеров).
        "ALTER TABLE stock_movements ADD COLUMN IF NOT EXISTS doc_id TEXT",
        "ALTER TABLE stock_movements ADD COLUMN IF NOT EXISTS reverts_id INTEGER REFERENCES stock_movements(id)",
        "CREATE INDEX IF NOT EXISTS stock_movements_doc_idx ON stock_movements (doc_id) WHERE doc_id IS NOT NULL",
        "CREATE UNIQUE INDEX IF NOT EXISTS stock_movements_reverts_uq ON stock_movements (reverts_id) WHERE reverts_id IS NOT NULL",
        // --- Документы остатков: каждое списание, приход, перемещение, инвентаризация, импорт - отдельный документ
        // с номером (id), видом, автором и комментарием; внутри него записи журнала (stock_movements.doc_id = doc_key).
        // Документ можно открыть и отменить целиком. kind: receipt | writeoff | transfer | inventory | cancel | other.
        R"(CREATE TABLE IF NOT EXISTS stock_documents (
            id INTEGER GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
            doc_key TEXT NOT NULL UNIQUE,
            kind TEXT NOT NULL DEFAULT 'other',
            comment TEXT NOT NULL DEFAULT '',
            username TEXT NOT NULL DEFAULT '',
            created_at TIMESTAMPTZ NOT NULL DEFAULT NOW()
        ))",
        // Документы, созданные до появления этой таблицы (пакеты, импорт, перемещения, отмены): по одной записи на doc_id.
        R"(INSERT INTO stock_documents (doc_key, kind, comment, created_at)
           SELECT g.doc_id,
                  CASE WHEN g.types = ARRAY['receipt']::text[] THEN 'receipt'
                       WHEN g.types = ARRAY['writeoff']::text[] THEN 'writeoff'
                       WHEN g.types <@ ARRAY['transfer_out', 'transfer_in']::text[] THEN 'transfer'
                       WHEN g.types = ARRAY['inventory_adjust']::text[] THEN 'inventory'
                       WHEN g.types = ARRAY['cancel']::text[] THEN 'cancel'
                       ELSE 'other' END,
                  COALESCE(g.first_comment, ''), g.first_at
             FROM (SELECT m.doc_id, array_agg(DISTINCT m.type ORDER BY m.type) AS types,
                          (array_agg(m.comment ORDER BY m.id))[1] AS first_comment, MIN(m.created_at) AS first_at
                     FROM stock_movements m LEFT JOIN stock_documents d ON d.doc_key = m.doc_id
                    WHERE m.doc_id IS NOT NULL AND d.id IS NULL
                    GROUP BY m.doc_id) g
            ORDER BY g.first_at
           ON CONFLICT (doc_key) DO NOTHING)",
        "CREATE INDEX IF NOT EXISTS stock_documents_created_idx ON stock_documents (created_at DESC)",
        // --- Инвентаризация по штрихкодам: подсчёт склада (stocktakes) и посчитанное по товарам (stocktake_items).
        // Подсчёт общий: на складе открыт не больше одного, в него считают с разных компьютеров и с сайта.
        // status: open | done | discarded; document_id - номер документа «Инвентаризация», когда подсчёт проведён.
        R"(CREATE TABLE IF NOT EXISTS stocktakes (
            id INTEGER GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
            warehouse_id INTEGER NOT NULL REFERENCES warehouses(id) ON DELETE CASCADE,
            status TEXT NOT NULL DEFAULT 'open',
            started_by TEXT NOT NULL DEFAULT '',
            started_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
            finished_at TIMESTAMPTZ,
            finished_by TEXT NOT NULL DEFAULT '',
            document_id INTEGER,
            zero_missing BOOLEAN NOT NULL DEFAULT TRUE
        ))",
        "CREATE UNIQUE INDEX IF NOT EXISTS stocktakes_one_open ON stocktakes (warehouse_id) WHERE status = 'open'",
        R"(CREATE TABLE IF NOT EXISTS stocktake_items (
            stocktake_id INTEGER NOT NULL REFERENCES stocktakes(id) ON DELETE CASCADE,
            product_id INTEGER NOT NULL REFERENCES products(id) ON DELETE CASCADE,
            counted INTEGER NOT NULL CHECK (counted >= 0),
            updated_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
            updated_by TEXT NOT NULL DEFAULT '',
            PRIMARY KEY (stocktake_id, product_id)
        ))",
        // --- Ссылки на фото для Маркета: у фото, которое нужно отдать наружу, появляется секретный токен; сайт отдаёт
        // такое фото по адресу /pub/photo/<id>/<токен>.jpg без входа (Маркет скачивает картинки по ссылке).
        "ALTER TABLE product_photos ADD COLUMN IF NOT EXISTS public_token TEXT",
        "CREATE UNIQUE INDEX IF NOT EXISTS product_photos_token_uq ON product_photos (public_token) WHERE public_token IS NOT NULL",
        // --- Карточки товаров для Яндекс Маркета (то, что передаётся в ассортимент): по одной на товар в кабинете.
        // pictures/videos - ссылки, по одной в строке; barcodes/countries/tags - через «;»; params - JSON
        // {"<id характеристики>": [{"value": "...", "valueId": 1, "unitId": 2}]}; status: draft | sent | error.
        R"(CREATE TABLE IF NOT EXISTS market_cards (
            id INTEGER GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
            account_id INTEGER NOT NULL REFERENCES market_accounts(id) ON DELETE CASCADE,
            product_id INTEGER REFERENCES products(id) ON DELETE SET NULL,
            offer_id TEXT NOT NULL,
            name TEXT NOT NULL DEFAULT '',
            description TEXT NOT NULL DEFAULT '',
            vendor TEXT NOT NULL DEFAULT '',
            vendor_code TEXT NOT NULL DEFAULT '',
            category_id BIGINT,
            category_name TEXT NOT NULL DEFAULT '',
            barcodes TEXT NOT NULL DEFAULT '',
            pictures TEXT NOT NULL DEFAULT '',
            videos TEXT NOT NULL DEFAULT '',
            countries TEXT NOT NULL DEFAULT '',
            length_cm NUMERIC(10,2) NOT NULL DEFAULT 0,
            width_cm NUMERIC(10,2) NOT NULL DEFAULT 0,
            height_cm NUMERIC(10,2) NOT NULL DEFAULT 0,
            weight_kg NUMERIC(10,3) NOT NULL DEFAULT 0,
            commodity_code TEXT NOT NULL DEFAULT '',
            tags TEXT NOT NULL DEFAULT '',
            price NUMERIC(12,2) NOT NULL DEFAULT 0,
            adult BOOLEAN NOT NULL DEFAULT FALSE,
            params TEXT NOT NULL DEFAULT '{}',
            status TEXT NOT NULL DEFAULT 'draft',
            market_status TEXT NOT NULL DEFAULT '',
            market_sku BIGINT,
            last_result TEXT NOT NULL DEFAULT '',
            sent_at TIMESTAMPTZ,
            updated_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
            UNIQUE (account_id, offer_id)
        ))",
        "CREATE INDEX IF NOT EXISTS market_cards_product_idx ON market_cards (product_id)",
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

// Обычный QPSQLDriver на каждый QSqlQuery::prepare()/exec() отправляет серверу три отдельных запроса
// (PREPARE, EXECUTE, а потом DEALLOCATE) - это три обращения по сети вместо одного, и на удалённой
// базе (десятки миллисекунд на обращение) именно они съедают время открытия вкладок и сохранения.
// Значения всё равно подставляются на стороне клиента (так делает и сам драйвер при EXECUTE), поэтому
// отключаем серверные подготовленные запросы: QSqlResult подставит значения в текст и отправит ОДИН запрос.
class PlainQueryDriver : public QPSQLDriver
{
public:
    using QPSQLDriver::QPSQLDriver;
    bool hasFeature(DriverFeature f) const override
    {
        return f == PreparedQueries ? false : QPSQLDriver::hasFeature(f);
    }
};

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
    auto *driver = new PlainQueryDriver(conn);
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

    // Вся схема одним запросом (PQexec выполняет несколько команд через «;»): одно обращение к серверу вместо
    // десятков.
    QSqlQuery q(db);
    return execOrFail(q, schemaDdl().join(QLatin1String(";\n")), error);
}

#endif

} // namespace

QList<Database::Rows> Database::queryMulti(const QString &sql, const QVariantList &binds, QString *error)
{
    QList<Rows> result;
    QSqlQuery q;
    q.prepare(sql);
    for (const QVariant &b : binds)
        q.addBindValue(b);
    if (!q.exec()) {
        if (error)
            *error = q.lastError().text();
        return result;
    }
    do {
        Rows rows;
        const int cols = q.record().count();
        while (q.next()) {
            QVariantList row;
            row.reserve(cols);
            for (int i = 0; i < cols; ++i)
                row << q.value(i);
            rows << row;
        }
        result << rows;
    } while (q.nextResult());
    return result;
}

QString Database::explainConnectionError(const QString &error)
{
    auto has = [&](const char *needle) { return error.contains(QLatin1String(needle), Qt::CaseInsensitive); };
    QString hint;
    if (has("could not translate host name") || has("Name or service not known") || has("Unknown host")
        || has("No such host") || has("Temporary failure in name resolution")) {
        hint = QStringLiteral(
            "Компьютер не может найти такой адрес сервера - это не пароль и не брандмауэр. Проверьте:\n"
            "• адрес написан без опечаток;\n"
            "• для сервера в Яндекс Облаке у ЭТОГО хоста включён «Публичный доступ» (кластер → Хосты → ⋮ → Изменить). "
            "Настройка своя у каждого хоста: перешли на хост в другой зоне (rc1a → rc1e) - включите её и на нём;\n"
            "• надёжнее указать адрес вида c-<идентификатор кластера>.rw.mdb.yandexcloud.net - он всегда ведёт на текущий "
            "основной хост и не меняется при переключении между зонами (идентификатор кластера - на странице кластера).");
    } else if (has("timeout") || has("timed out") || has("Connection refused") || has("could not connect to server")) {
        hint = QStringLiteral(
            "Сервер не отвечает. Проверьте: включён ли публичный доступ у хоста, разрешён ли входящий порт (6432 для "
            "Яндекс Облака) в группе безопасности кластера, есть ли интернет, и не мешает ли антивирус или брандмауэр.");
    } else if (has("password authentication failed")) {
        hint = QStringLiteral("Неверное имя пользователя или пароль (проверьте раскладку и регистр).");
    } else if (has("read-only transaction") || has("in a read-only")) {
        hint = QStringLiteral("Вы подключены к реплике (только чтение). Нужен основной хост - адрес c-<идентификатор "
                              "кластера>.rw.mdb.yandexcloud.net.");
    } else if (has("SSL") || has("pg_hba.conf")) {
        hint = QStringLiteral("Проверьте галочку «Использовать SSL»: для облачных серверов она нужна.");
    } else if (has("does not exist") && has("database")) {
        hint = QStringLiteral("Такой базы на сервере нет - проверьте название базы (с учётом регистра).");
    }
    return hint.isEmpty() ? error : error + QStringLiteral("\n\n") + hint;
}

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
    case MovementType::Cancel: return QStringLiteral("cancel");
    }
    return QString();
}

QString Database::movementTypeTitle(const QString &type)
{
    if (type == QLatin1String("receipt")) return QStringLiteral("Приход");
    if (type == QLatin1String("writeoff")) return QStringLiteral("Списание");
    if (type == QLatin1String("transfer_out")) return QStringLiteral("Перемещение (расход)");
    if (type == QLatin1String("transfer_in")) return QStringLiteral("Перемещение (приход)");
    if (type == QLatin1String("inventory_adjust")) return QStringLiteral("Инвентаризация");
    if (type == QLatin1String("cancel")) return QStringLiteral("Отмена");
    return type;
}

Database::Document::Document(const QString &kind, const QString &comment)
    : m_owner(g_document.isEmpty())
{
    if (m_owner) {
        g_document = QUuid::createUuid().toString(QUuid::WithoutBraces);
        g_docKind = kind.isEmpty() ? QStringLiteral("other") : kind;
        g_docComment = nn(comment.trimmed().left(300));
    }
}

Database::Document::~Document()
{
    if (m_owner) {
        g_lastDocKey = g_document;
        g_document.clear();
        g_docKind.clear();
        g_docComment.clear();
    }
}

int Database::lastDocumentId()
{
    if (g_lastDocKey.isEmpty())
        return 0;
    QSqlQuery q;
    q.prepare("SELECT id FROM stock_documents WHERE doc_key = ?");
    q.addBindValue(g_lastDocKey);
    return q.exec() && q.next() ? q.value(0).toInt() : 0;
}

void Database::setCurrentUser(const QString &username)
{
    g_currentUser = nn(username.trimmed().left(100));
}

QString Database::currentUser()
{
    return g_currentUser;
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

struct Movement {
    int productId = 0;
    int warehouseId = 0;
    int delta = 0;
    QString type;
    std::optional<int> related;
    QString comment;
};

// Позиция, на которую не хватило остатка.
struct Shortage {
    QString sku;
    qint64 have = 0;
    qint64 delta = 0; // итоговое изменение по товару на складе (отрицательное)
};

enum class MovementResult { Ok, Insufficient, Failed };

// Применяет несколько проводок (запись в журнал + агрегированный остаток) ОДНИМ запросом. Всё или ничего:
// если хоть где-то не хватает остатка, не записывается ничего, а в *shortages попадают нехватки. Раньше
// каждая проводка была отдельной транзакцией из 4-5 обращений к серверу; на удалённой базе это секунды ожидания
// на списке из десятков позиций.
MovementResult applyMovements(const QList<Movement> &mvs, QList<Shortage> *shortages, QString *error)
{
    if (mvs.isEmpty())
        return MovementResult::Ok;
    QStringList rows;
    QVariantList binds;
    for (int i = 0; i < mvs.size(); ++i) {
        const Movement &m = mvs[i];
        rows << (i == 0 ? QStringLiteral("(?::int, ?::int, ?::int, ?::int, ?::text, ?::int, ?::text)")
                        : QStringLiteral("(?, ?, ?, ?, ?, ?, ?)"));
        binds << i << m.productId << m.warehouseId << m.delta << m.type
              << (m.related ? QVariant(*m.related) : QVariant(QMetaType(QMetaType::Int))) << m.comment;
    }
    // Документ (номер, вид, автор) создаётся вместе с первой записью - тем же запросом, и только если проводка прошла.
    QString docCte;
    if (!g_document.isEmpty()) {
        docCte = QStringLiteral("doc AS (INSERT INTO stock_documents (doc_key, kind, comment, username) "
                                "SELECT ?::text, ?::text, ?::text, ?::text FROM ok WHERE ok.good "
                                "ON CONFLICT (doc_key) DO NOTHING RETURNING id), ");
        binds << g_document << g_docKind << nn(g_docComment) << nn(g_currentUser);
    }
    binds << (g_document.isEmpty() ? QVariant(QMetaType(QMetaType::QString)) : QVariant(g_document));

    const QString sql = QStringLiteral(
        "WITH items(n, product_id, warehouse_id, delta, type, related, comment) AS (VALUES %1), "
        "agg AS (SELECT product_id, warehouse_id, SUM(delta)::int AS delta FROM items GROUP BY product_id, warehouse_id), "
        "chk AS (SELECT a.product_id, a.warehouse_id, a.delta, COALESCE(s.quantity, 0) AS have FROM agg a "
        "        LEFT JOIN stock s ON s.product_id = a.product_id AND s.warehouse_id = a.warehouse_id), "
        "ok AS (SELECT NOT EXISTS (SELECT 1 FROM chk WHERE have + delta < 0) AS good), "
        "%2"
        "mv AS (INSERT INTO stock_movements (product_id, warehouse_id, related_warehouse_id, type, delta, comment, doc_id) "
        "       SELECT i.product_id, i.warehouse_id, i.related, i.type, i.delta, i.comment, ?::text "
        "       FROM items i, ok WHERE ok.good ORDER BY i.n RETURNING id), "
        "st AS (INSERT INTO stock (product_id, warehouse_id, quantity) "
        "       SELECT a.product_id, a.warehouse_id, a.delta FROM agg a, ok WHERE ok.good "
        "       ORDER BY a.product_id, a.warehouse_id "
        "       ON CONFLICT (product_id, warehouse_id) DO UPDATE SET quantity = stock.quantity + EXCLUDED.quantity "
        "       WHERE stock.quantity + EXCLUDED.quantity >= 0 RETURNING 1) "
        // Если между проверкой и записью остаток успели списать другим окном, деление на ноль откатывает весь запрос
        // (делитель - не константа, иначе планировщик посчитал бы 1/0 ещё до выполнения, даже в неисполняемой ветке CASE).
        "SELECT ok.good, CASE WHEN ok.good AND (SELECT count(*) FROM st) <> (SELECT count(*) FROM agg) "
        "                     THEN 1 / (SELECT count(*) - count(*) FROM st) ELSE 0 END, "
        "       sh.sku, sh.have, sh.delta "
        "FROM ok LEFT JOIN (SELECT p.sku, c.have, c.delta FROM chk c JOIN products p ON p.id = c.product_id "
        "                   WHERE c.have + c.delta < 0) sh ON TRUE").arg(rows.join(QLatin1Char(',')), docCte);

    QSqlQuery q;
    q.prepare(sql);
    for (const QVariant &b : binds)
        q.addBindValue(b);
    if (!q.exec()) {
        const QString code = q.lastError().nativeErrorCode();
        if (error) {
            if (code == QLatin1String("23503"))
                *error = QStringLiteral("Товар или склад уже удалён (возможно, с другого компьютера).");
            else if (code == QLatin1String("22012"))
                *error = QStringLiteral("Остаток изменили в другом окне, пока шла запись. Повторите.");
            else if (code == QLatin1String("22003"))
                *error = QStringLiteral("Остаток получится слишком большим.");
            else
                *error = q.lastError().text();
        }
        return MovementResult::Failed;
    }
    bool good = true;
    while (q.next()) {
        good = q.value(0).toBool();
        if (!good && shortages)
            shortages->append({q.value(2).toString(), q.value(3).toLongLong(), q.value(4).toLongLong()});
    }
    return good ? MovementResult::Ok : MovementResult::Insufficient;
}

// Выполняет одну проводку (запись в журнал + обновление агрегированного остатка).
bool applyMovementInternal(int productId, int warehouseId, int delta,
                            Database::MovementType type,
                            std::optional<int> relatedWarehouseId,
                            const QString &comment, QString *error)
{
    QList<Shortage> shortages;
    const MovementResult r = applyMovements({{productId, warehouseId, delta, Database::movementTypeLabel(type),
                                              relatedWarehouseId, comment}}, &shortages, error);
    if (r == MovementResult::Insufficient) {
        if (error)
            *error = QStringLiteral("Недостаточно остатка: на складе %1 шт, требуется списать %2 шт")
                         .arg(shortages.isEmpty() ? 0 : shortages.first().have)
                         .arg(shortages.isEmpty() ? -delta : -shortages.first().delta);
        return false;
    }
    return r == MovementResult::Ok;
}

} // namespace

bool Database::adjustStock(int productId, int warehouseId, int delta, MovementType type,
                            const QString &comment, QString *error)
{
    // Одна проводка - один запрос, транзакция не нужна. Но и одиночная операция - это документ (с номером и автором):
    // его видно в разделе «Документы» и можно отменить целиком.
    QString kind = QStringLiteral("other");
    switch (type) {
    case MovementType::Receipt: kind = QStringLiteral("receipt"); break;
    case MovementType::WriteOff: kind = QStringLiteral("writeoff"); break;
    case MovementType::TransferOut:
    case MovementType::TransferIn: kind = QStringLiteral("transfer"); break;
    case MovementType::InventoryAdjust: kind = QStringLiteral("inventory"); break;
    case MovementType::Cancel: kind = QStringLiteral("cancel"); break;
    }
    Document doc(kind, comment);
    return applyMovementInternal(productId, warehouseId, delta, type, std::nullopt, comment, error);
}

bool Database::adjustStockBatch(const QList<BatchItem> &items, int warehouseId, bool writeOff,
                                const QString &comment, QStringList *problems, QString *error)
{
    if (problems)
        problems->clear();
    // Повторы складываем, порядок - по возрастанию товара: две партии одновременно не зайдут в тупик.
    QMap<int, qint64> total;
    for (const BatchItem &it : items) {
        if (it.qty <= 0 || it.productId <= 0) {
            if (error)
                *error = QStringLiteral("Количество должно быть больше нуля");
            return false;
        }
        total[it.productId] += it.qty;
    }
    if (total.isEmpty()) {
        if (error)
            *error = QStringLiteral("Список пуст");
        return false;
    }

    auto refuse = [&](const QStringList &bad) {
        if (problems)
            *problems = bad;
        if (error)
            *error = QStringLiteral("Ничего не выполнено: %1").arg(bad.join(QStringLiteral("; ")));
        return false;
    };
    // Названия артикулов для сообщений (читаем только когда есть что объяснять).
    auto skuOf = [](const QList<int> &ids) {
        QHash<int, QString> sku;
        if (ids.isEmpty())
            return sku;
        QStringList marks(ids.size(), QStringLiteral("?"));
        QSqlQuery q;
        q.prepare(QStringLiteral("SELECT id, sku FROM products WHERE id IN (%1)").arg(marks.join(QLatin1Char(','))));
        for (int id : ids)
            q.addBindValue(id);
        if (q.exec())
            while (q.next())
                sku.insert(q.value(0).toInt(), q.value(1).toString());
        return sku;
    };

    QList<int> tooBig;
    for (auto it = total.cbegin(); it != total.cend(); ++it)
        if (it.value() > 100'000'000)
            tooBig << it.key();
    if (!tooBig.isEmpty()) {
        const auto sku = skuOf(tooBig);
        QStringList bad;
        for (int id : tooBig)
            bad << QStringLiteral("%1: слишком большое количество (%2)").arg(sku.value(id, QStringLiteral("товар №%1").arg(id))).arg(total[id]);
        return refuse(bad);
    }

    const MovementType type = writeOff ? MovementType::WriteOff : MovementType::Receipt;
    QList<Movement> mvs;
    for (auto it = total.cbegin(); it != total.cend(); ++it)
        mvs.append({it.key(), warehouseId, int(writeOff ? -it.value() : it.value()), movementTypeLabel(type), std::nullopt, comment});

    // Все позиции - один документ: отменяется разом; и один запрос к серверу на весь список.
    Document doc(writeOff ? QStringLiteral("writeoff") : QStringLiteral("receipt"), comment);
    QList<Shortage> shortages;
    QString err;
    const MovementResult r = applyMovements(mvs, &shortages, &err);
    if (r == MovementResult::Ok)
        return true;
    if (r == MovementResult::Insufficient) {
        QStringList bad;
        for (const Shortage &sh : shortages)
            bad << QStringLiteral("%1: на складе %2 шт, а списать нужно %3 шт").arg(sh.sku).arg(sh.have).arg(-sh.delta);
        return refuse(bad);
    }
    // Не прошло: если часть товаров успели удалить с другого компьютера, называем их.
    QList<int> ids = total.keys();
    const auto existing = skuOf(ids);
    QStringList gone;
    for (int id : ids)
        if (!existing.contains(id))
            gone << QStringLiteral("товар №%1 уже удалён").arg(id);
    if (!gone.isEmpty())
        return refuse(gone);
    if (error)
        *error = err;
    return false;
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

    // Расход и приход - один документ (отменяются только вместе) и один запрос к серверу.
    Document doc(QStringLiteral("transfer"), comment);
    QList<Shortage> shortages;
    const MovementResult r = applyMovements(
        {{productId, srcWarehouseId, -quantity, movementTypeLabel(MovementType::TransferOut), dstWarehouseId, comment},
         {productId, dstWarehouseId, quantity, movementTypeLabel(MovementType::TransferIn), srcWarehouseId, comment}},
        &shortages, error);
    if (r == MovementResult::Insufficient) {
        if (error)
            *error = QStringLiteral("Недостаточно остатка: на складе %1 шт, требуется списать %2 шт")
                         .arg(shortages.isEmpty() ? 0 : shortages.first().have).arg(quantity);
        return false;
    }
    return r == MovementResult::Ok;
}

bool Database::inventorySetInTransaction(int productId, int warehouseId, int countedQuantity,
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

    QString fullComment = comment;
    if (!fullComment.isEmpty())
        fullComment += " ";
    fullComment += QStringLiteral("(инвентаризация: было %1, стало %2)").arg(current).arg(countedQuantity);
    return applyMovementInternal(productId, warehouseId, delta, MovementType::InventoryAdjust, std::nullopt,
                                 fullComment, error);
}

bool Database::setStockLevelsInTransaction(int warehouseId, const QList<StockLevel> &levels, bool zeroOthers,
                                            const QString &comment, int *changed, QString *error)
{
    if (changed)
        *changed = 0;
    // Остатки склада блокируем на время проводки: пока идёт запись, их не изменит другое окно, поэтому «было» верно.
    QHash<int, int> have;
    {
        QSqlQuery q;
        q.prepare("SELECT product_id, quantity FROM stock WHERE warehouse_id = ? FOR UPDATE");
        q.addBindValue(warehouseId);
        if (!q.exec()) {
            if (error)
                *error = q.lastError().text();
            return false;
        }
        while (q.next())
            have.insert(q.value(0).toInt(), q.value(1).toInt());
    }
    QList<Movement> mvs;
    QSet<int> seen;
    auto setTo = [&](int productId, int counted) {
        const int current = have.value(productId, 0);
        if (counted == current)
            return;
        QString full = comment;
        if (!full.isEmpty())
            full += QLatin1Char(' ');
        full += QStringLiteral("(инвентаризация: было %1, стало %2)").arg(current).arg(counted);
        mvs.append({productId, warehouseId, counted - current, movementTypeLabel(MovementType::InventoryAdjust), std::nullopt, full});
    };
    for (const StockLevel &l : levels) {
        if (l.counted < 0 || l.counted > 100'000'000) {
            if (error)
                *error = QStringLiteral("Посчитанное количество должно быть от 0 до 100 000 000");
            return false;
        }
        if (seen.contains(l.productId))
            continue;
        seen.insert(l.productId);
        setTo(l.productId, l.counted);
    }
    if (zeroOthers) {
        QList<int> others;
        for (auto it = have.cbegin(); it != have.cend(); ++it)
            if (it.value() != 0 && !seen.contains(it.key()))
                others << it.key();
        std::sort(others.begin(), others.end());
        for (int id : std::as_const(others))
            setTo(id, 0);
    }
    // Пачками, чтобы запрос не разрастался; все пачки - один документ.
    constexpr int kChunk = 400;
    for (int start = 0; start < mvs.size(); start += kChunk) {
        QList<Shortage> shortages;
        QString err;
        if (applyMovements(mvs.mid(start, kChunk), &shortages, &err) != MovementResult::Ok) {
            if (error)
                *error = err.isEmpty() ? QStringLiteral("Остаток изменили в другом окне, пока шла запись. Повторите.") : err;
            return false;
        }
    }
    if (changed)
        *changed = int(mvs.size());
    return true;
}

bool Database::inventoryAdjust(int productId, int warehouseId, int countedQuantity,
                                const QString &comment, QString *error)
{
    // Пересчёт: один запрос на чтение текущего остатка и один на запись (проводка сама атомарна, транзакция
    // с BEGIN/COMMIT добавляла бы ещё два обращения к серверу).
    Document doc(QStringLiteral("inventory"), comment);
    return inventorySetInTransaction(productId, warehouseId, countedQuantity, comment, error);
}

namespace {

// Строка журнала для отмены: всё нужное для проверки и показа.
const char kCancelRowSql[] =
    "SELECT m.id, m.product_id, p.sku, p.name, m.warehouse_id, w.name, COALESCE(m.related_warehouse_id, 0), "
    "       m.type, m.delta, COALESCE(m.comment, ''), "
    "       COALESCE((SELECT r.id FROM stock_movements r WHERE r.reverts_id = m.id), 0) "
    "FROM stock_movements m JOIN products p ON p.id = m.product_id JOIN warehouses w ON w.id = m.warehouse_id ";

struct LoadedRow {
    Database::CancelRow row;
    int revertedBy = 0;
};

LoadedRow readCancelRow(const QSqlQuery &q)
{
    LoadedRow r;
    r.row.id = q.value(0).toInt();
    r.row.productId = q.value(1).toInt();
    r.row.sku = q.value(2).toString();
    r.row.name = q.value(3).toString();
    r.row.warehouseId = q.value(4).toInt();
    r.row.warehouse = q.value(5).toString();
    r.row.relatedWarehouseId = q.value(6).toInt();
    r.row.type = q.value(7).toString();
    r.row.delta = q.value(8).toInt();
    r.row.comment = q.value(9).toString();
    r.revertedBy = q.value(10).toInt();
    return r;
}

} // namespace

QList<int> Database::documentMovementIds(int movementId)
{
    QList<int> ids;
    QSqlQuery q;
    q.prepare("SELECT m.id FROM stock_movements m "
              "WHERE m.type <> 'cancel' AND (m.id = ? OR m.doc_id = (SELECT doc_id FROM stock_movements WHERE id = ?)) "
              "ORDER BY m.id");
    q.addBindValue(movementId);
    q.addBindValue(movementId);
    if (q.exec())
        while (q.next())
            ids << q.value(0).toInt();
    if (ids.isEmpty())
        ids << movementId;
    return ids;
}

Database::CancelPlan Database::planCancel(const QList<int> &movementIds, bool lock)
{
    CancelPlan plan;
    QList<int> want;
    {
        QSet<int> seen;
        for (int id : movementIds)
            if (id > 0 && !seen.contains(id)) {
                seen.insert(id);
                want << id;
            }
    }
    if (want.isEmpty()) {
        plan.problems << QStringLiteral("Не выбрано ни одной записи журнала");
        return plan;
    }
    const QString forUpdate = lock ? QStringLiteral(" FOR UPDATE OF m") : QString();

    QMap<int, LoadedRow> found;
    {
        QStringList idList;
        for (int id : std::as_const(want))
            idList << QString::number(id);
        QSqlQuery q;
        if (!q.exec(QString::fromLatin1(kCancelRowSql) + QStringLiteral("WHERE m.id IN (%1) ORDER BY m.id%2")
                        .arg(idList.join(QLatin1Char(',')), forUpdate))) {
            plan.problems << q.lastError().text();
            return plan;
        }
        while (q.next()) {
            const LoadedRow r = readCancelRow(q);
            found.insert(r.row.id, r);
        }
    }
    for (int id : std::as_const(want))
        if (!found.contains(id))
            plan.problems << QStringLiteral("Запись №%1 не найдена (возможно, товар или склад уже удалены)").arg(id);

    // Перемещение - это две записи: расход и приход. Отменяются только вместе. Время сравнивается в базе
    // (с точностью до микросекунд) - так находится пара и у старых записей, сделанных до появления документов.
    QStringList transferIds;
    for (const LoadedRow &r : std::as_const(found))
        if (r.row.type == QLatin1String("transfer_out") || r.row.type == QLatin1String("transfer_in"))
            transferIds << QString::number(r.row.id);
    if (!transferIds.isEmpty()) {
        QSqlQuery q;
        // Одним запросом на все перемещения из списка (раньше - по запросу на запись).
        if (q.exec(QString::fromLatin1(kCancelRowSql) +
                   QStringLiteral("JOIN stock_movements o ON o.id IN (%1) "
                                  "WHERE m.id <> o.id AND m.type IN ('transfer_out', 'transfer_in') AND ("
                                  "  (o.doc_id IS NOT NULL AND m.doc_id = o.doc_id) "
                                  "  OR (o.doc_id IS NULL AND m.doc_id IS NULL AND m.product_id = o.product_id "
                                  "      AND m.created_at = o.created_at AND m.type <> o.type "
                                  "      AND m.warehouse_id = o.related_warehouse_id AND m.related_warehouse_id = o.warehouse_id "
                                  "      AND m.delta = -o.delta)) "
                                  "ORDER BY m.id%2").arg(transferIds.join(QLatin1Char(',')), forUpdate))) {
            while (q.next()) {
                const LoadedRow mate = readCancelRow(q);
                found.insert(mate.row.id, mate);
            }
        }
    }

    QList<CancelRow> ok;
    for (const LoadedRow &r : std::as_const(found)) {
        if (r.row.type == QLatin1String("cancel"))
            plan.problems << QStringLiteral("Запись №%1 сама является отменой - отменить её нельзя. Если нужно вернуть "
                                            "как было, проведите операцию заново").arg(r.row.id);
        else if (r.revertedBy)
            plan.problems << QStringLiteral("Запись №%1 (%2, %3) уже отменена записью №%4")
                                 .arg(r.row.id).arg(r.row.sku, movementTypeTitle(r.row.type)).arg(r.revertedBy);
        else
            ok << r.row;
    }

    // Остаток не должен уйти в минус: итог по каждому товару и складу.
    struct Key { int pid; int wid; bool operator<(const Key &o) const { return pid != o.pid ? pid < o.pid : wid < o.wid; } };
    QMap<Key, qint64> net;
    QMap<Key, QPair<QString, QString>> names;
    for (const CancelRow &r : std::as_const(ok)) {
        const Key k{r.productId, r.warehouseId};
        net[k] -= r.delta;
        names[k] = {r.sku, r.warehouse};
    }
    QMap<Key, QPair<qint64, qint64>> levels; // сейчас / станет
    QMap<Key, qint64> current;
    if (!net.isEmpty()) {
        QStringList pairs;
        QVariantList binds;
        for (auto it = net.cbegin(); it != net.cend(); ++it) {
            pairs << QStringLiteral("(?, ?)");
            binds << it.key().pid << it.key().wid;
        }
        QSqlQuery q;
        q.prepare(QStringLiteral("SELECT product_id, warehouse_id, quantity FROM stock WHERE (product_id, warehouse_id) IN (%1)%2")
                      .arg(pairs.join(QLatin1Char(',')), lock ? QStringLiteral(" FOR UPDATE") : QString()));
        for (const QVariant &b : binds)
            q.addBindValue(b);
        if (q.exec())
            while (q.next())
                current[Key{q.value(0).toInt(), q.value(1).toInt()}] = q.value(2).toLongLong();
    }
    for (auto it = net.cbegin(); it != net.cend(); ++it) {
        const qint64 cur = current.value(it.key(), 0);
        const qint64 next = cur + it.value();
        levels[it.key()] = {cur, next};
        const auto &nm = names[it.key()];
        if (next < 0)
            plan.problems << QStringLiteral("%1, склад «%2»: отмена уберёт %3 шт, а на складе сейчас %4 шт - "
                                            "товар уже расходован после этой операции").arg(nm.first, nm.second).arg(-it.value()).arg(cur);
        else if (next > 2'000'000'000LL)
            plan.problems << QStringLiteral("%1, склад «%2»: остаток получится слишком большим").arg(nm.first, nm.second);
    }
    for (CancelRow r : std::as_const(ok)) {
        const auto lv = levels.value(Key{r.productId, r.warehouseId});
        r.current = lv.first;
        r.after = lv.second;
        plan.rows << r;
    }
    return plan;
}

bool Database::cancelMovements(const QList<int> &movementIds, const QString &reason, QStringList *problems, QString *error)
{
    if (problems)
        problems->clear();
    QSqlDatabase db = QSqlDatabase::database();
    if (!db.transaction()) {
        if (error)
            *error = db.lastError().text();
        return false;
    }
    auto refuse = [&](const QStringList &list, const QString &single) {
        db.rollback();
        if (problems)
            *problems = list;
        if (error)
            *error = list.isEmpty() ? single : (list.size() == 1 ? list.first() : QStringLiteral("Отменить нельзя:\n- ") + list.join(QStringLiteral("\n- ")));
        return false;
    };

    const CancelPlan plan = planCancel(movementIds, true);
    if (!plan.problems.isEmpty() || plan.rows.isEmpty())
        return refuse(plan.problems, QStringLiteral("Нечего отменять"));

    const QString text = reason.trimmed().left(200);
    const QString comment = text.isEmpty() ? QStringLiteral("Отмена") : QStringLiteral("Отмена: ") + text;
    Document doc(QStringLiteral("cancel"), comment); // все записи отмены - тоже один документ

    // Сначала остатки (суммой по товару и складу), потом записи журнала: порядок строк не важен.
    struct Key { int pid; int wid; bool operator<(const Key &o) const { return pid != o.pid ? pid < o.pid : wid < o.wid; } };
    QMap<Key, qint64> net;
    for (const CancelRow &r : plan.rows)
        net[Key{r.productId, r.warehouseId}] -= r.delta;
    {
        QStringList rows;
        QSqlQuery q;
        for (auto it = net.cbegin(); it != net.cend(); ++it)
            rows << QStringLiteral("(?, ?, ?)");
        q.prepare(QStringLiteral("INSERT INTO stock (product_id, warehouse_id, quantity) VALUES %1 "
                                 "ON CONFLICT(product_id, warehouse_id) DO UPDATE SET quantity = stock.quantity + excluded.quantity")
                      .arg(rows.join(QLatin1Char(','))));
        for (auto it = net.cbegin(); it != net.cend(); ++it) {
            q.addBindValue(it.key().pid);
            q.addBindValue(it.key().wid);
            q.addBindValue(it.value());
        }
        if (!q.exec())
            return refuse({}, q.lastError().text());
    }
    {
        // Документ отмены (номер, автор) - вместе с первой записью, в той же транзакции.
        QSqlQuery d;
        d.prepare("INSERT INTO stock_documents (doc_key, kind, comment, username) VALUES (?, 'cancel', ?, ?) ON CONFLICT (doc_key) DO NOTHING");
        d.addBindValue(g_document);
        d.addBindValue(nn(g_docComment));
        d.addBindValue(nn(g_currentUser));
        if (!d.exec())
            return refuse({}, d.lastError().text());
        QStringList rows;
        QSqlQuery q;
        for (int i = 0; i < plan.rows.size(); ++i)
            rows << QStringLiteral("(?, ?, ?, 'cancel', ?, ?, ?, ?)");
        q.prepare(QStringLiteral("INSERT INTO stock_movements (product_id, warehouse_id, related_warehouse_id, type, delta, comment, doc_id, reverts_id) "
                                 "VALUES %1").arg(rows.join(QLatin1Char(','))));
        for (const CancelRow &r : plan.rows) {
            q.addBindValue(r.productId);
            q.addBindValue(r.warehouseId);
            if (r.relatedWarehouseId)
                q.addBindValue(r.relatedWarehouseId);
            else
                q.addBindValue(QVariant(QMetaType(QMetaType::Int)));
            q.addBindValue(-r.delta);
            q.addBindValue(comment);
            q.addBindValue(g_document);
            q.addBindValue(r.id);
        }
        if (!q.exec()) {
            // Две отмены одной записи одновременно: вторую останавливает уникальный индекс reverts_id.
            if (q.lastError().nativeErrorCode() == QLatin1String("23505"))
                return refuse({QStringLiteral("Эту запись уже отменили (возможно, на другом компьютере). Обновите журнал")}, QString());
            return refuse({}, q.lastError().text());
        }
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
