#pragma once

#include <QHash>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>

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

// Подсказки «по мере ввода»: товары, у которых введённое встречается в артикуле, названии, артикуле на Маркете или
// штрихкоде. Несколько слов через пробел - каждое должно встретиться («nike 42»). Сначала артикул целиком, потом
// артикулы, начинающиеся с введённого, остальные - по артикулу.
struct Suggestion {
    int id = 0;
    QString sku;
    QString name;
    QString marketSku;
    QString label() const { return sku + QStringLiteral(" — ") + name; }
};
QList<Suggestion> suggest(const QString &text, int limit = 12);

// Только точное совпадение (штрихкод, артикул или артикул на Маркете целиком) - для операций,
// которые что-то списывают или добавляют: выбирать за человека из нескольких подходящих нельзя.
// -> id товара, либо -1 и понятная причина в *error.
int findExact(const QString &text, QString *error = nullptr);

// То же для целого списка кодов сразу: несколько запросов на весь список, а не по запросу на код.
// В *ids - найденные товары по каждому (обрезанному по краям) коду, в *errors - причина для остальных.
void findExactMany(const QStringList &keys, QHash<QString, int> *ids, QHash<QString, QString> *errors);

// Артикулы товаров на Маркете (offerId: артикул на Маркете, а если пуст - наш артикул)
// у товаров, найденных по тексту. Нужна заказам и сверке с Маркетом.
QSet<QString> offerIds(const QString &text);

// offerId <-> id товара
QString offerIdFor(int productId);
int productForOffer(const QString &offerId);

} // namespace ProductFinder
