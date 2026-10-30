#pragma once

#include <QList>
#include <QString>
#include <QStringList>

#include <functional>

// Списание заказов со склада: за каждый заказ из нашей базы списывается столько пар, сколько заказано, - и остаток
// уменьшается везде сразу: в приложении, на сайте и (при следующей отправке остатков) на Маркете. Заказы бывают двух
// видов: Маркета (приложение само спрашивает Маркет, см. вкладку «Заказы Маркета») и нашего сайта (сайт кладёт заказ в
// очередь - таблица order_writeoffs, state = pending). Списывает только кнопка «Списать со склада...», ничего автоматически.
//
// Один заказ списывается один раз (запись order_writeoffs уникальна по виду, кабинету и номеру заказа), каждое списание -
// отдельный документ (раздел «Документы», его можно отменить), списание атомарно: если хоть где-то не хватает остатка,
// по заказу не списывается ничего. Если заказ потом отменили, пара возвращается приходом (с того же склада, тем же
// количеством) - тоже только по кнопке.
namespace OrderWriteoff {

struct Line {
    QString key;   // артикул на Маркете (а если его нет - артикул); у заказов сайта ещё и просто артикул или штрихкод
    QString name;
    int qty = 0;
};

struct Order {
    QString source;       // "market" | "site"
    int accountId = 0;    // кабинет Маркета (у заказов сайта 0)
    QString accountName;
    QString ref;          // номер заказа
    QString status;       // Маркет: PROCESSING, DELIVERY, CANCELLED...; сайт: pending | cancel_requested
    QString substatus;
    QString shipment;     // дата отгрузки (для таблицы)
    QString title;        // как назвать заказ в документе («заказ сайта №10001»); пусто - по виду и номеру
    int warehouseId = 0;  // явный склад списания; 0 - по настройкам кабинета (Маркет) или сайта
    QList<Line> lines;
};

enum class Action {
    WriteOff,   // будет списано
    Return,     // будет возвращено (заказ отменили после списания)
    Done,       // уже списан / уже возвращён
    Skip,       // ничего делать не нужно (заказ ещё не подтверждён, отменён до списания...)
    Problem     // списать нельзя: нет товара в базе, не хватает остатка, не выбран склад
};

struct Item {
    int productId = 0;
    QString sku;
    QString name;
    int qty = 0;
    int stock = 0; // остаток на складе списания на момент проверки
};

struct Planned {
    Order order;
    Action action = Action::Skip;
    QString reason;          // что будет сделано или почему нельзя (по-русски)
    int warehouseId = 0;
    QString warehouseName;
    QList<Item> items;
    int recordId = 0;        // order_writeoffs.id, если запись уже есть
    int docId = 0;           // номер документа списания, если уже списан
    bool actionable() const { return action == Action::WriteOff || action == Action::Return; }
};

// Склад списания заказов кабинета (0 - не выбран; выбирается в настройках кабинета) и заказов сайта (общая настройка).
int siteWarehouse();
bool setSiteWarehouse(int warehouseId);

// Заказы сайта, которые ждут списания (pending) или возврата (cancel_requested), старые первыми.
QList<Order> siteQueue(QString *error = nullptr);

// Что будет сделано с каждым заказом (ничего не меняет). Остатки считаются нарастающим итогом: два заказа на последнюю
// пару - первому «будет списано», второму «не хватает остатка».
QList<Planned> plan(const QList<Order> &orders, QString *error = nullptr);

struct Outcome {
    int written = 0;        // заказов списано
    int returned = 0;       // заказов возвращено
    int units = 0;          // штук списано (минус возвращено нет: только списанное)
    int unitsReturned = 0;
    int skipped = 0;        // уже обработаны на другом компьютере
    int failed = 0;
    QStringList problems;   // по строке на каждый заказ, который не удалось обработать
    QList<int> documents;   // номера созданных документов
    bool cancelled = false;
};

// Выполняет отмеченные заказы (только с actionable()). progress(сделано, всего) возвращает false - остановить.
Outcome apply(const QList<Planned> &selected, const std::function<bool(int, int)> &progress = {});

// --- Универсальный вход для других вкладок и модулей (например, заказы интернет-магазина, который работает с той же базой):
// список готовых позиций «товар - штуки», склад, источник и номер заказа. Ключ идемпотентности - пара (source, ref): повторный
// вызов с тем же ключом ничего не списывает и возвращает номер уже созданного документа. Результат - один документ «Списание»
// (раздел «Документы», его можно отменить) и запись в order_writeoffs, по которой потом возвращается пара при отмене заказа.
// source - любая своя метка («webshop»), чтобы ключи разных источников не пересекались; Маркет и очередь сайта занимают
// «market» и «site».
struct Result {
    enum Status {
        Written,         // списано, docId - номер документа
        AlreadyDone,     // по этому ключу уже списано (или возвращено): ничего не изменилось, docId - прежний документ
        NotEnoughStock,  // не хватает остатка: ничего не списано, ключ свободен - после прихода можно повторить
        Failed           // неверные данные, ошибка базы или операция по ключу уже идёт: message объясняет
    };
    Status status = Failed;
    int docId = 0;
    QString message;
    bool ok() const { return status == Written || status == AlreadyDone; }
};

// title - как назвать заказ в комментарии документа («заказ сайта №10001» -> «Заказ сайта №10001»).
Result writeOffItems(const QString &source, const QString &ref, const QString &title, int warehouseId, const QList<Item> &items);
// Вернуть пару за заказ, списанный выше (заказ отменили): приходом, на тот же склад, тем же количеством. Повторный вызов -
// AlreadyDone с номером документа возврата. Заказ, который не списывался, - Failed.
Result returnOrder(const QString &source, const QString &ref);

} // namespace OrderWriteoff
