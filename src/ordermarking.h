#pragma once

#include <QList>
#include <QString>
#include <QStringList>

// Печать кодов «Честного знака» к заказам Маркета: по каждой паре в заказе подбирается один свободный код этого товара. ИП и склад - два
// независимых признака кода, склады к ИП не привязаны: ИП берётся из кабинета Маркета (настройки кабинета → «ИП (для кодов «Честного знака»)»), склад - склад
// списания кабинета. Сначала берутся коды именно этого склада, затем коды без склада; коды другого склада не трогаются. У кабинета без
// склада списания подходят коды любого склада, у кабинета без ИП - только коды без ИП. Подобранные коды печатаются на термоэтикетках, после
// печати каждый записывается на свой заказ («market:<кабинет>:<номер заказа>»), поэтому повторное открытие не берёт для того же заказа
// новые коды, а показывает уже выданные (их можно напечатать ещё раз, например после замятия).
namespace OrderMarking {

struct Item {
    QString offerId;   // артикул на Маркете
    QString name;
    int count = 0;
};

struct OrderInput {
    int accountId = 0;          // кабинет Маркета (из него берутся ИП и склад списания)
    QString accountName;
    QString orderNumber;
    QString status;             // отменённые заказы пропускаются
    QList<Item> items;
};

struct Line {
    QString ref;                // market:<кабинет>:<номер>
    QString accountName, orderNumber;
    QString offerId, name, sku;
    int productId = 0;
    int ownerId = 0;            // ИП кабинета (0 - без ИП)
    QString ownerName;
    int warehouseId = 0;        // склад списания кабинета (0 - не задан)
    QString warehouseName;
    int need = 0;               // пар в заказе
    int issued = 0;             // кодов уже выдано под этот заказ (напечатано раньше)
    QList<int> issuedIds;       // их номера (для повторной печати)
    QList<int> picked;          // подобранные свободные коды
    QString problem;            // что мешает (товар не найден, неоднозначный артикул...)
    int missing() const { return problem.isEmpty() ? qMax(0, need - issued - picked.size()) : 0; }
};

struct Plan {
    QList<Line> lines;
    int skippedOrders = 0;      // отменённые и пустые заказы
    int ordersCount = 0;        // заказов в плане
    int need() const;           // всего пар
    int pickedCount() const;    // подобрано новых кодов
    int issuedCount() const;    // уже выдано
    int missingCount() const;   // не хватает кодов
    QList<int> pickedIds() const;                  // новые коды в порядке заказов
    QList<int> issuedIds() const;                  // ранее выданные
    // Пары «ИП - склад», по которым не хватает кодов (для заказа недостающих: у каждой пары свой заказ в СУЗ).
    QList<QPair<int, int>> targetsWithMissing() const;
};

// Строит план (ничего не меняет в базе, кроме чтения). Свободные коды раздаются заказам по очереди.
Plan build(const QList<OrderInput> &orders);

// После печати: записывает коды на заказы (только те, что действительно напечатаны). Возвращает, сколько записано.
int markIssued(const Plan &plan);

QString orderRef(int accountId, const QString &orderNumber);

} // namespace OrderMarking
