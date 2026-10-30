#pragma once

#include <QDate>
#include <QDateTime>
#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

// Цены и скидки, которые видят покупатели на САЙТЕ магазина (проект web). Свои, отдельные от цены склада
// (products.price): цену склада используют учёт и Яндекс Маркет, на сайте её меняют независимо.
//
// Цена задаётся на МОДЕЛЬ, а не на размер: в базе каждый размер - отдельный товар с артикулом «МОДЕЛЬ_РАЗМЕР»
// (DD1391-100_42.5), и цена модели действует на все её размеры, в том числе на те, что появятся позже.
// Если у модели цены сайта нет, сайт берёт цену склада.
namespace SitePrices {

// Ключ модели - артикул без размера: DD1391-100_42.5 -> DD1391-100. Размер - то, что идёт после последнего «_» и
// похоже на размер (число 30-59 с долей, S/M/L/XL..., ONE SIZE), дальше может стоять пометка в скобках: 44(2E).
// Правило ОБЯЗАНО совпадать с сайтом (web/src/model.js, splitSku); сверка на всех артикулах - tools/check_model_key.
QString modelKey(const QString &sku);

struct Entry {
    QString modelKey;
    double price = 0;                // обычная цена на сайте, ₽
    std::optional<double> salePrice; // цена со скидкой, ₽ (нет - скидки нет)
    QDate saleFrom;                  // скидка действует с этого дня включительно (пусто - сразу)
    QDate saleTo;                    // и по этот день включительно (пусто - без срока)
};

enum class SaleState {
    None,      // скидки нет
    Active,    // действует сейчас
    Scheduled, // начнётся позже
    Ended,     // срок прошёл
};
SaleState saleState(const Entry &entry, const QDate &today);
QString saleStateText(SaleState state);

// Скидка в процентах относительно обычной цены (0 - скидки нет).
int discountPercent(double price, double salePrice);
// Цена со скидкой: price минус percent%, округлённая до step рублей (1, 10 или 100). Результат не больше price.
double priceByPercent(double price, double percent, int step = 1);
// Цена, которую увидит покупатель сегодня.
double effectivePrice(const Entry &entry, const QDate &today);

// Одна строка вкладки: модель, что о ней известно на складе и что задано для сайта.
struct ModelRow {
    QString key;
    QString name;
    int sizes = 0;               // сколько размеров (товаров) в модели
    qint64 stock = 0;            // суммарный остаток по всем складам
    double warehousePrice = 0;   // цена склада: наименьшая положительная среди размеров
    bool hasSite = false;        // цена сайта задана
    Entry site;
    QString updatedBy;
    QDateTime updatedAt;
    bool orphan = false;         // цена есть, а товаров с таким ключом больше нет
};

// Все модели склада вместе с ценами сайта (одно обращение к базе).
QList<ModelRow> loadModels(QString *error = nullptr);

// Записывает цены одной транзакцией: либо все, либо ни одной. user - кто правил (для истории).
bool save(const QList<Entry> &entries, const QString &user, QString *error = nullptr);
// Убирает цену сайта: сайт вернётся к цене склада.
bool remove(const QStringList &modelKeys, QString *error = nullptr);

} // namespace SitePrices
