#pragma once

#include <QList>
#include <QSet>
#include <QString>

// Поиск товара по тому, что человек (или сканер) ввёл в строку: штрихкод, артикул,
// артикул на Маркете или часть названия. Один и тот же поиск используется везде:
// в списках, диалогах прихода/списания, этикетках, заказах и сверке с Маркетом.
namespace ProductFinder {

struct Result {
    QList<int> ids;       // найденные товары, по артикулу
    bool exact = false;   // совпал штрихкод / артикул / артикул на Маркете целиком
};

// Сначала точное совпадение (штрихкод, потом артикул, потом артикул на Маркете),
// иначе все товары, где введённый текст встречается в артикуле, названии или штрихкоде.
Result find(const QString &text, int limit = 300);

// Артикулы товаров на Маркете (offerId: артикул на Маркете, а если пуст - наш артикул)
// у товаров, найденных по тексту. Нужна заказам и сверке с Маркетом.
QSet<QString> offerIds(const QString &text);

// offerId <-> id товара
QString offerIdFor(int productId);
int productForOffer(const QString &offerId);

} // namespace ProductFinder
