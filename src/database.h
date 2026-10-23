#pragma once

#include <QList>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <optional>

struct ServerConfig;

// Ядро учёта: товары, склады, места хранения, остатки и журнал движений.
// Хранится на общем сервере PostgreSQL (не локально), поэтому одна и та же
// база доступна с любого устройства, на котором запущено приложение.
// Один и тот же артикул может иметь остаток на нескольких складах одновременно —
// таблица stock хранит пару (product_id, warehouse_id) -> quantity,
// а stock_movements - полную историю приход/списание/перемещение/инвентаризации.
class Database
{
public:
    static bool open(const ServerConfig &config, QString *error = nullptr);

    // Несколько SELECT одним обращением к серверу: результаты приходят разом, а не по очереди. База удалённая,
    // и каждое обращение - это ожидание (десятки миллисекунд), поэтому всё, что нужно окну, читаем так.
    // sql - запросы через «;» с метками ?, binds - значения для них по порядку. Результат: по набору строк
    // на каждый запрос (порядок как в sql).
    using Rows = QList<QVariantList>;
    static QList<Rows> queryMulti(const QString &sql, const QVariantList &binds = {}, QString *error = nullptr);

    // Добавляет к тексту ошибки подключения понятную подсказку, что проверить (адрес не находится,
    // сервер не отвечает, неверный пароль, подключились к реплике и т. п.).
    static QString explainConnectionError(const QString &error);

    // Только проверяет, что к серверу можно подключиться (без создания схемы
    // и без регистрации соединения для остального приложения).
    static bool testConnection(const ServerConfig &config, QString *error = nullptr);

    enum class MovementType {
        Receipt,        // приход
        WriteOff,       // списание
        TransferOut,    // перемещение (списание с исходного склада)
        TransferIn,     // перемещение (приход на целевой склад)
        InventoryAdjust, // корректировка по итогам инвентаризации
        Cancel          // отмена другой записи журнала (противоположное изменение)
    };
    static QString movementTypeLabel(MovementType type);

    // Документ - группа записей журнала от одной операции (пакет, перемещение, импорт, правка карточки).
    // Всё, что записано, пока объект жив, получает общий doc_id и отменяется разом. Вложенные документы
    // сливаются в самый внешний. У одиночных операций документа нет: запись отменяется сама по себе.
    class Document
    {
    public:
        Document();
        ~Document();
        Document(const Document &) = delete;
        Document &operator=(const Document &) = delete;

    private:
        bool m_owner;
    };

    // --- Отмена операций. Журнал только дополняется: отмена - новая запись типа cancel с противоположным
    // изменением и ссылкой на отменяемую. Запись отменяется один раз; саму отмену отменить нельзя. Остаток не
    // уходит в минус: если товар уже расходован, отмена отклоняется целиком. Перемещение - всегда парой.
    struct CancelRow {
        int id = 0;
        int productId = 0;
        QString sku;
        QString name;
        int warehouseId = 0;
        QString warehouse;
        int relatedWarehouseId = 0; // 0 - нет
        QString type;               // как в базе: receipt, writeoff, transfer_out, ...
        int delta = 0;
        QString comment;
        qint64 current = 0;         // остаток сейчас
        qint64 after = 0;           // остаток после отмены всех выбранных записей
    };
    struct CancelPlan {
        QList<CancelRow> rows;      // что будет отменено (с парами по перемещению)
        QStringList problems;       // почему отменить нельзя (пусто - можно)
        bool ok() const { return problems.isEmpty() && !rows.isEmpty(); }
    };
    // Что произойдёт при отмене записей (ничего не меняет). lock - внутри открытой транзакции, с блокировкой строк.
    static CancelPlan planCancel(const QList<int> &movementIds, bool lock = false);
    // Все записи документа, к которому относится запись (без отмен). Без документа - только она сама.
    static QList<int> documentMovementIds(int movementId);
    // Отменяет записи одной транзакцией: либо все, либо ни одной. При отказе - причины в *problems.
    static bool cancelMovements(const QList<int> &movementIds, const QString &reason, QStringList *problems,
                                QString *error = nullptr);
    // Русское название типа записи для интерфейса.
    static QString movementTypeTitle(const QString &type);

    // Простое изменение остатка на одном складе (приход/списание).
    // delta может быть отрицательным (списание). Для списания проверяется,
    // что текущего остатка достаточно.
    static bool adjustStock(int productId, int warehouseId, int delta,
                             MovementType type, const QString &comment,
                             QString *error = nullptr);

    // Приход или списание сразу многих позиций одной транзакцией: либо проходят все, либо ни одной.
    // Повторы одного товара складываются. При отказе в *problems - по строке на каждую проблемную
    // позицию (например, не хватает остатка), в *error - общая причина (ошибка базы).
    struct BatchItem {
        int productId = 0;
        qint64 qty = 0; // всегда положительное
    };
    static bool adjustStockBatch(const QList<BatchItem> &items, int warehouseId, bool writeOff,
                                 const QString &comment, QStringList *problems, QString *error = nullptr);

    // Перемещение между двумя складами внутри одной транзакции.
    static bool transferStock(int productId, int srcWarehouseId, int dstWarehouseId,
                               int quantity, const QString &comment,
                               QString *error = nullptr);

    // Инвентаризация: пользователь вводит фактически посчитанное количество,
    // функция сама считает и применяет разницу с текущим остатком.
    static bool inventoryAdjust(int productId, int warehouseId, int countedQuantity,
                                 const QString &comment, QString *error = nullptr);

    // То же, но без собственной транзакции: вызывать только внутри уже открытой
    // (чтобы несколько правок карточки применялись все вместе или ни одной).
    static bool inventorySetInTransaction(int productId, int warehouseId, int countedQuantity,
                                          const QString &comment, QString *error = nullptr);

    static int currentStock(int productId, int warehouseId);

    static bool setStockLocation(int productId, int warehouseId, std::optional<int> locationId,
                                  QString *error = nullptr);
};
