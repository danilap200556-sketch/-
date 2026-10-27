#pragma once

#include <QDateTime>
#include <QList>
#include <QString>

// Инвентаризация склада по штрихкодам. Подсчёт (stocktakes) открывается на склад и общий для всех, кто считает: каждый скан
// добавляет штуку товару (stocktake_items), посчитанное видно сразу в приложении и на сайте. Когда подсчёт завершают,
// остаток каждого посчитанного товара становится равным посчитанному, а у товаров склада, которых в подсчёте нет, - нулю
// (если отмечено «не посчитанное считать нулевым»). Всё проводится одним документом «Инвентаризация» в одной транзакции:
// его можно открыть в разделе «Документы» и отменить целиком.
namespace Stocktake {

struct Info {
    int id = 0;
    int warehouseId = 0;
    QString warehouse;
    QString status; // open | done | discarded
    QString startedBy;
    QDateTime startedAt;
    int positions = 0;  // товаров в подсчёте
    qint64 units = 0;   // штук посчитано
    int documentId = 0; // документ, которым подсчёт проведён (0 - нет изменений или ещё не проведён)
    bool valid() const { return id > 0; }
};

struct Row {
    int productId = 0;
    QString sku;
    QString name;
    int book = 0;    // сейчас в учёте на складе
    int counted = 0; // посчитано
    int diff() const { return counted - book; }
};

struct Summary {
    int positions = 0;        // позиций посчитано
    qint64 units = 0;         // штук посчитано
    int diffPositions = 0;    // из них с расхождением
    qint64 surplus = 0;       // излишек, шт (посчитано больше, чем в учёте)
    qint64 shortage = 0;      // недостача, шт (посчитано меньше)
    int missingPositions = 0; // числятся на складе, но не посчитаны
    qint64 missingUnits = 0;  // ...и сколько их штук
};

constexpr int kMaxCount = 100'000'000;

// Открытый подсчёт склада; !valid() - такого нет.
Info current(int warehouseId);
Info get(int id);
// Id открытого подсчёта склада; если его нет - создаёт. 0 при ошибке.
int start(int warehouseId, QString *error = nullptr);

// +delta штук товару (delta может быть отрицательным: поправить лишний скан). В *row - новое состояние строки.
bool add(int id, int productId, int delta, Row *row = nullptr, QString *error = nullptr);
// Задаёт посчитанное количество; отрицательное число убирает товар из подсчёта.
bool setCounted(int id, int productId, int counted, Row *row = nullptr, QString *error = nullptr);

QList<Row> countedRows(int id); // посчитанное, последние сканы сверху
QList<Row> missingRows(int id); // числится на складе, в подсчёте нет (по артикулу)
Summary summary(int id);

struct FinishResult {
    int changed = 0;    // у скольких товаров остаток изменился
    int documentId = 0; // 0 - расхождений не было, документа нет
};
// Проводит подсчёт одним документом. Подсчёт, уже завершённый или сброшенный, не проводится повторно.
bool finish(int id, bool zeroMissing, FinishResult *result = nullptr, QString *error = nullptr);
// Сбрасывает подсчёт (остатки не меняются).
bool discard(int id, QString *error = nullptr);

} // namespace Stocktake
