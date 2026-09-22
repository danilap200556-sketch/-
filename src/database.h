#pragma once

#include <QString>
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

    // Только проверяет, что к серверу можно подключиться (без создания схемы
    // и без регистрации соединения для остального приложения).
    static bool testConnection(const ServerConfig &config, QString *error = nullptr);

    enum class MovementType {
        Receipt,        // приход
        WriteOff,       // списание
        TransferOut,    // перемещение (списание с исходного склада)
        TransferIn,     // перемещение (приход на целевой склад)
        InventoryAdjust // корректировка по итогам инвентаризации
    };
    static QString movementTypeLabel(MovementType type);

    // Простое изменение остатка на одном складе (приход/списание).
    // delta может быть отрицательным (списание). Для списания проверяется,
    // что текущего остатка достаточно.
    static bool adjustStock(int productId, int warehouseId, int delta,
                             MovementType type, const QString &comment,
                             QString *error = nullptr);

    // Перемещение между двумя складами внутри одной транзакции.
    static bool transferStock(int productId, int srcWarehouseId, int dstWarehouseId,
                               int quantity, const QString &comment,
                               QString *error = nullptr);

    // Инвентаризация: пользователь вводит фактически посчитанное количество,
    // функция сама считает и применяет разницу с текущим остатком.
    static bool inventoryAdjust(int productId, int warehouseId, int countedQuantity,
                                 const QString &comment, QString *error = nullptr);

    static int currentStock(int productId, int warehouseId);

    static bool setStockLocation(int productId, int warehouseId, std::optional<int> locationId,
                                  QString *error = nullptr);
};
